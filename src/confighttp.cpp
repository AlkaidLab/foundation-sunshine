/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTP server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "process.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <random>
#include <map>
#include <set>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <boost/algorithm/string.hpp>

#include <boost/asio/ssl/context.hpp>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <boost/asio/ssl/context_base.hpp>

#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device/session.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "platform/run_command.h"
#include "rtsp.h"
#include "src/display_device/display_device.h"
#include "src/display_device/to_string.h"
#include "stream.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "version.h"
#include "webhook.h"

#ifdef _WIN32
  #include <iphlpapi.h>
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  // Prevent saveApp/deleteApp concurrent write to file_apps causing file corruption, non-blocking
  // return busy if not acquired
  static std::atomic<bool> apps_writing { false };

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  enum class op_e {
    ADD,  ///< Add client
    REMOVE  ///< Remove client
  };

  void
  print_req(const req_https_t &request) {
    std::ostringstream log_stream;
    log_stream << "Request - TUNNEL: HTTPS"
               << ", METHOD: " << request->method
               << ", PATH: " << request->path;
    
    // Headers
    if (!request->header.empty()) {
      log_stream << ", HEADERS: ";
      bool first = true;
      for (auto &[name, val] : request->header) {
        if (!first) log_stream << ", ";
        log_stream << name << "=" << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
        first = false;
      }
    }
    
    // Query parameters
    auto query_params = request->parse_query_string();
    if (!query_params.empty()) {
      log_stream << ", PARAMS: ";
      bool first = true;
      for (auto &[name, val] : query_params) {
        if (!first) log_stream << "&";
        log_stream << name << "=" << val;
        first = false;
      }
    }
    
    BOOST_LOG(verbose) << log_stream.str();
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  void
  send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(error) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      { "WWW-Authenticate", R"(Basic realm="Sunshine Gamestream Host", charset="UTF-8")" }
    };
    response->write(SimpleWeb::StatusCode::client_error_unauthorized, headers);
  }

  /**
   * Logout endpoint: for localhost (PC) returns 200 so the browser does not show
   * a password dialog; for other allowed origins returns 401 with WWW-Authenticate
   * so the browser shows the credential prompt. Denied origins receive 403.
   */
  void
  handleLogout(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);

    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- logout denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return;
    }

    if (ip_type == net::PC) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout (local), responding 200"sv;
      response->write(SimpleWeb::StatusCode::success_ok, "");
      return;
    }

    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logout requested, responding 401"sv;
    send_unauthorized(response, request);
  }

  void
  send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(error) << "Web UI: ["sv << address << "] -- not authorized, redirect"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      { "Location", path }
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  bool
  authenticate(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);

    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }

    // If credentials are shown, redirect the user to a /welcome page
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }

    if (ip_type == net::PC) {
      return true;
    }

    auto fg = util::fail_guard([&]() {
      send_unauthorized(response, request);
    });

    auto auth = request->header.find("authorization");
    if (auth == request->header.end()) {
      return false;
    }

    auto &rawAuth = auth->second;
    constexpr auto basicPrefix = "Basic "sv;
    if (rawAuth.length() <= basicPrefix.length() || 
        rawAuth.substr(0, basicPrefix.length()) != basicPrefix) {
      return false;
    }
    
    std::string authData;
    try {
      authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr(basicPrefix.length()));
    }
    catch (const std::exception &e) {
      BOOST_LOG(debug) << "Authentication: Base64 decode failed: " << e.what();
      return false;
    }

    int index = authData.find(':');
    if (index >= authData.size() - 1) {
      return false;
    }

    auto username = authData.substr(0, index);
    auto password = authData.substr(index + 1);
    auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();

    if (!boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }

    fg.disable();
    return true;
  }

  void
  not_found(resp_https_t response, req_https_t request) {
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response << "HTTP/1.1 404 NOT FOUND\r\n"
              << data.str();
  }

  void
  close_connection(resp_https_t response, req_https_t request) {
      *response << "HTTP/1.1 444 No Response\r\n";
      response->close_connection_after_response = true;
      return;
  }

  void
  getHtmlPage(resp_https_t response, req_https_t request, const std::string& pageName, bool requireAuth = true) {
    if (requireAuth && !authenticate(response, request)) return;

    print_req(request);

    std::string content = file_handler::read_file((std::string(WEB_DIR) + pageName).c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    if (pageName == "apps.html") {
      headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
    }
    response->write(content, headers);
  }

  void
  getIndexPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "index.html");
  }

  void
  getPinPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "pin.html");
  }

  void
  getAppsPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "apps.html");
  }

  void
  getClientsPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "clients.html");
  }

  void
  getConfigPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "config.html");
  }

  void
  getPasswordPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "password.html");
  }

  void
  getWelcomePage(resp_https_t response, req_https_t request) {
    // 如果已经有用户名，要求认证后才能访问（防止未授权访问）
    // 认证通过后重定向到首页，认证失败则拒绝访问
    if (!config::sunshine.username.empty()) {
      if (!authenticate(response, request)) {
        return; // authenticate已经发送了401响应
      }
      // 认证通过，重定向到首页
      send_redirect(response, request, "/");
      return;
    }
    // 只有在没有用户名时才显示welcome页面（首次设置）
    getHtmlPage(response, request, "welcome.html", false);
  }

  void
  getTroubleshootingPage(resp_https_t response, req_https_t request) {
    getHtmlPage(response, request, "troubleshooting.html");
  }

  /**
   * 处理静态资源文件
   */
  void
  getStaticResource(resp_https_t response, req_https_t request, const std::string& path, const std::string& contentType) {
    // print_req(request);

    std::ifstream in(path, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", contentType);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  void
  getFaviconImage(resp_https_t response, req_https_t request) {
    getStaticResource(response, request, WEB_DIR "images/sunshine.ico", "image/x-icon");
  }

  void
  getSunshineLogoImage(resp_https_t response, req_https_t request) {
    getStaticResource(response, request, WEB_DIR "images/logo-sunshine-256.png", "image/png");
  }

  /**
   * @brief 检查 child 是否是 parent 目录的子路径（防止路径穿越）
   */
  bool
  isChildPath(fs::path const &child, fs::path const &parent) {
    auto relPath = fs::relative(child, parent);
    return *(relPath.begin()) != fs::path("..");
  }

  void
  getBoxArt(resp_https_t response, req_https_t request) {
    print_req(request);

    // Extract image filename from request path
    std::string path = request->path;
    if (path.find("/boxart/") == 0) {
      path = path.substr(8); // Remove "/boxart/" prefix
    }

    BOOST_LOG(debug) << "getBoxArt: Requested file: " << path;

    static const fs::path assetsRoot = fs::weakly_canonical(fs::path(SUNSHINE_ASSETS_DIR));
    static const fs::path coversRoot = fs::weakly_canonical(platf::appdata() / "covers");

    // First try to find in SUNSHINE_ASSETS_DIR
    fs::path targetPath = fs::weakly_canonical(assetsRoot / path);
    fs::path finalPath;
    bool found = false;

    // Strict check: Allow only files directly in assets root, no subdirectories
    if (targetPath.parent_path() == assetsRoot && fs::exists(targetPath) && fs::is_regular_file(targetPath)) {
      finalPath = targetPath;
      found = true;
      BOOST_LOG(debug) << "Found in boxart: " << finalPath.string();
    }
    
    // If not found in boxart, try covers directory
    if (!found) {
      targetPath = fs::weakly_canonical(coversRoot / path);
      // For covers, we use isChildPath which allows subdirectories but prevents traversal out of root
      if (isChildPath(targetPath, coversRoot) && fs::exists(targetPath) && fs::is_regular_file(targetPath)) {
        finalPath = targetPath;
        found = true;
        BOOST_LOG(debug) << "Found in covers: " << finalPath.string();
      }
    }

    if (!found) {
      // If still not found or invalid path, use default image
      BOOST_LOG(debug) << "Not found or invalid path, using default box.png";
      finalPath = assetsRoot / "box.png";
      // Ensure default file exists, otherwise we might fail later
      if (!fs::exists(finalPath)) {
        BOOST_LOG(warning) << "Default box.png not found at: " << finalPath.string();
        response->write(SimpleWeb::StatusCode::client_error_not_found, "Image not found");
        return;
      }
    }

    std::string imagePath = finalPath.string();

    // Get file size
    std::error_code ec;
    auto fileSize = fs::file_size(imagePath, ec);
    if (ec) {
      BOOST_LOG(warning) << "Failed to get file size for: " << imagePath;
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read image file");
      return;
    }

    // Determine Content-Type from file extension
    std::string ext = fs::path(imagePath).extension().string();
    if (!ext.empty() && ext[0] == '.') {
      ext = ext.substr(1);
    }
    
    auto mimeType = mime_types.find(ext);
    std::string contentType = "image/png"; // Default type
    
    if (mimeType != mime_types.end()) {
      contentType = mimeType->second;
    }
    
    BOOST_LOG(debug) << "Serving boxart: " << imagePath << " (Content-Type: " << contentType << ", Size: " << fileSize << " bytes)";

    // Return image resource
    std::ifstream in(imagePath, std::ios::binary);
    if (!in.is_open()) {
      BOOST_LOG(warning) << "Failed to open image file: " << imagePath;
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to open image file");
      return;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", contentType);
    headers.emplace("Content-Length", std::to_string(fileSize));
    headers.emplace("Cache-Control", "max-age=3600"); // Add caching to reduce load
    
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  void
  getNodeModules(resp_https_t response, req_https_t request) {
    // print_req(request);
    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Bad Request");
    }
    else if (!fs::exists(filePath)) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
    }
    else {
      auto relPath = fs::relative(filePath, webDirPath);
      // get the mime type from the file extension mime_types map
      // remove the leading period from the extension
      auto mimeType = mime_types.find(relPath.extension().string().substr(1));
      // check if the extension is in the map at the x position
      if (mimeType != mime_types.end()) {
        // if it is, set the content type to the mime type
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", mimeType->second);
        std::ifstream in(filePath.string(), std::ios::binary);
        response->write(SimpleWeb::StatusCode::success_ok, in, headers);
      }
      // do not return any file if the type is not in the map
    }
  }

  void
  getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    std::string content = file_handler::read_file(config::stream.file_apps.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(content, headers);
  }

  /**
   * @brief Snapshot of log cache state, swapped atomically via shared_ptr.
   */
  struct LogCacheSnapshot {
    std::shared_ptr<const std::string> content;  ///< Cached log content (nullptr in offset-only mode)
    std::uintmax_t file_size = 0;                ///< Actual file size on disk when snapshot was taken
    std::intmax_t mtime_ns = 0;                  ///< File mtime when snapshot was taken
    std::uintmax_t start_offset = 0;             ///< File byte position where content begins
  };

  /**
   * @brief Try to read only the new tail of the log file and append to existing content.
   * @return New content on success, nullptr on any failure (caller should fall back to full read).
   */
  static std::shared_ptr<const std::string> try_incremental_log_read(
    const std::filesystem::path &log_path,
    std::uintmax_t prev_size,
    std::uintmax_t current_size,
    const std::shared_ptr<const std::string> &old_content) {
    if (current_size <= prev_size || prev_size == 0 || !old_content) {
      return nullptr;
    }
    std::ifstream in(log_path.string(), std::ios::binary);
    if (!in || !in.seekg(static_cast<std::streamoff>(prev_size))) {
      return nullptr;
    }
    const auto tail_len = current_size - prev_size;
    std::string tail(tail_len, '\0');
    if (!in.read(tail.data(), static_cast<std::streamsize>(tail_len))) {
      return nullptr;
    }
    return std::make_shared<const std::string>(*old_content + tail);
  }

  /**
   * @brief Read a specific byte range [offset, offset+length) from a file.
   * @return Content string on success, nullptr on failure.
   */
  static std::shared_ptr<const std::string>
  read_file_range(const std::filesystem::path &path, std::uintmax_t offset, std::uintmax_t length) {
    std::ifstream in(path.string(), std::ios::binary);
    if (!in || !in.seekg(static_cast<std::streamoff>(offset))) {
      return nullptr;
    }
    std::string content(static_cast<std::size_t>(length), '\0');
    if (!in.read(content.data(), static_cast<std::streamsize>(length))) {
      return nullptr;
    }
    return std::make_shared<const std::string>(std::move(content));
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Dual mode via X-Log-Offset header:
   *   - Without header: stream full log file from disk (for download)
   *   - With header:    cached or offset-based incremental support (for live viewer)
   *
   * When the log file is small (≤ 4 MB), the entire file is cached in memory for fast serving.
   * When the log file exceeds 4 MB, no content is cached; reads go directly to disk at the
   * client's offset (offset-only mode), avoiding unbounded memory growth.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void
  getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    //print_req(request);

    const std::filesystem::path log_path(config::sunshine.log_file);

    // --- Mode 1: No X-Log-Offset header → stream full file from disk (download) ---
    auto offset_it = request->header.find("X-Log-Offset");
    if (offset_it == request->header.end()) {
      std::ifstream in(log_path.string(), std::ios::binary);
      if (!in.is_open()) {
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to open log file");
        return;
      }
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain; charset=utf-8");
      headers.emplace("Content-Disposition", "attachment; filename=\"sunshine.log\"");
      // Omit Content-Length: log file is actively written (TOCTOU risk)
      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
      return;
    }

    // --- Mode 2: X-Log-Offset present → cached or offset-only mode for live viewer ---

    // When file ≤ MAX_LOG_CACHE_SIZE: cached in memory.  Otherwise: disk reads only.
    static constexpr std::uintmax_t MAX_LOG_CACHE_SIZE = 4 * 1024 * 1024;   // 4 MB
    static constexpr std::uintmax_t MAX_RESPONSE_SIZE  = 4 * 1024 * 1024;   // 4 MB

    static std::atomic<std::shared_ptr<const LogCacheSnapshot>> log_cache;

    // Check file status
    std::error_code ec;
    auto current_size = std::filesystem::file_size(log_path, ec);
    if (ec) {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
      return;
    }
    auto current_mtime = std::filesystem::last_write_time(log_path, ec);
    if (ec) {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
      return;
    }
    auto current_mtime_ns = current_mtime.time_since_epoch().count();

    // Refresh cache if file changed
    auto snapshot = log_cache.load();
    const bool cache_stale = !snapshot || current_size != snapshot->file_size || current_mtime_ns != snapshot->mtime_ns;
    if (cache_stale) {
      auto new_snap = std::make_shared<LogCacheSnapshot>();
      new_snap->file_size = current_size;
      new_snap->mtime_ns = current_mtime_ns;

      if (current_size <= MAX_LOG_CACHE_SIZE) {
        // --- Cached mode: file fits in memory ---
        std::shared_ptr<const std::string> new_content;
        if (snapshot && snapshot->content && snapshot->file_size > 0 && current_size > snapshot->file_size) {
          new_content = try_incremental_log_read(log_path, snapshot->file_size, current_size, snapshot->content);
        }
        if (!new_content) {
          // Use sampled current_size to avoid TOCTOU unsigned underflow at start_offset
          auto read_len = std::min(current_size, MAX_LOG_CACHE_SIZE);
          auto read_start = current_size - read_len;
          new_content = read_file_range(log_path, read_start, read_len);
        }
        if (!new_content) {
          if (current_size > 0) {
            response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
            return;
          }
          new_content = std::make_shared<const std::string>();
        }
        new_snap->content = std::move(new_content);
        new_snap->start_offset = current_size - new_snap->content->size();
      }
      else {
        // --- Offset-only mode: file too large, don't cache content ---
        new_snap->content = nullptr;
        new_snap->start_offset = 0;
      }

      // CAS publish: avoid overwriting a newer snapshot from a concurrent thread
      if (!log_cache.compare_exchange_strong(snapshot, new_snap)) {
        // CAS failed: snapshot already updated by compare_exchange_strong
      }
      else {
        snapshot = std::move(new_snap);
      }
    }

    // Parse client offset
    std::uintmax_t client_offset = 0;
    try {
      std::string offset_str(offset_it->second);
      boost::algorithm::trim(offset_str);
      if (!offset_str.empty()) {
        client_offset = std::stoull(offset_str);
      }
    }
    catch (const std::invalid_argument &) {
      client_offset = 0;
    }
    catch (const std::out_of_range &) {
      client_offset = 0;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain");
    headers.emplace("X-Log-Size", std::to_string(snapshot->file_size));
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    // No change in logs → 304
    if (client_offset > 0 && client_offset == snapshot->file_size) {
      headers.emplace("X-Log-Range", "unchanged");
      response->write(SimpleWeb::StatusCode::redirection_not_modified, headers);
      return;
    }

    if (snapshot->content) {
      // === Cached mode: serve from memory ===
      if (client_offset > 0 && client_offset >= snapshot->start_offset && client_offset < snapshot->file_size) {
        auto cache_pos = client_offset - snapshot->start_offset;
        headers.emplace("X-Log-Range", "incremental");
        auto delta = snapshot->content->substr(static_cast<std::size_t>(cache_pos));
        response->write(SimpleWeb::StatusCode::success_ok, delta, headers);
      }
      else {
        headers.emplace("X-Log-Range", "full");
        response->write(SimpleWeb::StatusCode::success_ok, *snapshot->content, headers);
      }
    }
    else {
      // === Offset-only mode: read from disk, bounded by snapshot->file_size ===
      if (client_offset > 0 && client_offset < snapshot->file_size) {
        auto delta_size = snapshot->file_size - client_offset;
        if (delta_size <= MAX_RESPONSE_SIZE) {
          auto data = read_file_range(log_path, client_offset, delta_size);
          if (data) {
            headers.emplace("X-Log-Range", "incremental");
            response->write(SimpleWeb::StatusCode::success_ok, *data, headers);
            return;
          }
        }
      }
      // Delta too large, read failed, or first request: return tail up to snapshot->file_size
      auto tail_len = std::min(snapshot->file_size, MAX_RESPONSE_SIZE);
      auto tail_start = snapshot->file_size - tail_len;
      auto tail = read_file_range(log_path, tail_start, tail_len);
      if (!tail) {
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Failed to read log file");
        return;
      }
      headers.emplace("X-Log-Range", "full");
      response->write(SimpleWeb::StatusCode::success_ok, *tail, headers);
    }
  }

  void
  saveApp(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // Prevent concurrent write to file_apps causing file corruption
    bool expected = false;
    if (!apps_writing.compare_exchange_strong(expected, true)) {
      pt::ptree outputTree;
      outputTree.put("status", "false");
      outputTree.put("error", "Another save operation is in progress");
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(SimpleWeb::StatusCode::client_error_conflict, data.str());
      return;
    }
    auto writing_guard = util::fail_guard([]() { apps_writing = false; });

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    pt::ptree inputTree, fileTree;

    try {
      pt::read_json(ss, inputTree);
      pt::read_json(config::stream.file_apps, fileTree);

      auto &apps_node = fileTree.get_child("apps"s);
      auto &input_apps_node = inputTree.get_child("apps"s);
      auto &input_edit_node = inputTree.get_child("editApp"s);

      // Validate app name when editing a specific app
      if (!input_edit_node.empty()) {
        auto app_name = input_edit_node.get<std::string>("name", "");
        if (app_name.empty() || app_name.size() > 256) {
          outputTree.put("status", "false");
          outputTree.put("error", "App name must be 1-256 characters");
          return;
        }
      }

      if (input_edit_node.empty()) {
        fileTree.erase("apps");
        fileTree.push_back(std::make_pair("apps", input_apps_node));
      }
      else {
        auto prep_cmd = input_edit_node.get_child_optional("prep-cmd");
        if (prep_cmd && prep_cmd->empty()) {
          input_edit_node.erase("prep-cmd");
        }

        auto detached = input_edit_node.get_child_optional("detached");
        if (detached && detached->empty()) {
          input_edit_node.erase("detached");
        }

        int index = input_edit_node.get<int>("index");
        input_edit_node.erase("index");

        if (index == -1) {
          apps_node.push_back(std::make_pair("", input_edit_node));
        }
        else {
          // Unfortunately Boost PT does not allow to directly edit the array, copy should do the trick
          pt::ptree newApps;
          int i = 0;
          for (auto &[_, app_node] : input_apps_node) {
            newApps.push_back(std::make_pair("", i == index ? input_edit_node : app_node));
            i++;
          }
          fileTree.erase("apps");
          fileTree.push_back(std::make_pair("apps", newApps));
        }
      }

      pt::write_json(config::stream.file_apps, fileTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();

      outputTree.put("status", "false");
      outputTree.put("error", "Invalid Input JSON");
      return;
    }

    BOOST_LOG(info) << "SaveApp: configuration saved successfully"sv;
    outputTree.put("status", "true");
    proc::refresh(config::stream.file_apps);
  }

  void
  deleteApp(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // Prevent concurrent write to file_apps causing file corruption
    bool expected = false;
    if (!apps_writing.compare_exchange_strong(expected, true)) {
      pt::ptree outputTree;
      outputTree.put("status", "false");
      outputTree.put("error", "Another operation is in progress");
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(SimpleWeb::StatusCode::client_error_conflict, data.str());
      return;
    }
    auto writing_guard = util::fail_guard([]() { apps_writing = false; });

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });
    pt::ptree fileTree;
    try {
      pt::read_json(config::stream.file_apps, fileTree);
      auto &apps_node = fileTree.get_child("apps"s);
      int index = stoi(request->path_match[1]);

      int apps_count = static_cast<int>(apps_node.size());
      if (index < 0 || index >= apps_count) {
        outputTree.put("status", "false");
        outputTree.put("error", "Invalid Index");
        return;
      }
      else {
        // Unfortunately Boost PT does not allow to directly edit the array, copy should do the trick
        pt::ptree newApps;
        int i = 0;
        for (const auto &kv : apps_node) {
          if (i++ != index) {
            newApps.push_back(std::make_pair("", kv.second));
          }
        }
        fileTree.erase("apps");
        fileTree.push_back(std::make_pair("apps", newApps));
      }
      pt::write_json(config::stream.file_apps, fileTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", "Invalid File JSON");
      return;
    }

    BOOST_LOG(info) << "DeleteApp: configuration deleted successfully"sv;
    outputTree.put("status", "true");
    proc::refresh(config::stream.file_apps);
  }

  void
  uploadCover(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    std::stringstream ss;
    std::stringstream configStream;
    ss << request->content.rdbuf();
    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      SimpleWeb::StatusCode code = SimpleWeb::StatusCode::success_ok;
      if (outputTree.get_child_optional("error").has_value()) {
        code = SimpleWeb::StatusCode::client_error_bad_request;
      }

      pt::write_json(data, outputTree);
      response->write(code, data.str());
    });
    pt::ptree inputTree;
    try {
      pt::read_json(ss, inputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", e.what());
      return;
    }

    auto key = inputTree.get("key", "");
    if (key.empty()) {
      outputTree.put("error", "Cover key is required");
      return;
    }
    auto url = inputTree.get("url", "");

    const std::string coverdir = platf::appdata().string() + "/covers/";
    file_handler::make_directory(coverdir);

    std::basic_string path = coverdir + http::url_escape(key) + ".png";
    if (!url.empty()) {
      if (http::url_get_host(url) != "images.igdb.com") {
        outputTree.put("error", "Only images.igdb.com is allowed");
        return;
      }
      if (!http::download_image_with_magic_check(url, path)) {
        outputTree.put("error", "Failed to download cover");
        return;
      }
    }
    else {
      // Limit base64 data size to prevent memory exhaustion
      // (10MB decoded to about 7.5MB, enough for cover images)
      constexpr std::size_t MAX_COVER_BASE64_SIZE = 10 * 1024 * 1024;
      auto base64_str = inputTree.get<std::string>("data");
      if (base64_str.size() > MAX_COVER_BASE64_SIZE) {
        outputTree.put("error", "Cover image too large (max 10MB)");
        return;
      }
      auto data = SimpleWeb::Crypto::Base64::decode(base64_str);

      std::ofstream imgfile(path, std::ios::binary);
      if (!imgfile.is_open()) {
        outputTree.put("error", "Failed to create file");
        return;
      }
      imgfile.write(data.data(), (int) data.size());
      imgfile.close();
      
      if (imgfile.fail()) {
        outputTree.put("error", "Failed to write file");
        return;
      }
    }
    outputTree.put("path", path);
  }

  void
  getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    auto devices { display_device::enum_available_devices() };
    pt::ptree devices_nodes;
    for (const auto &[device_id, data] : devices) {
      pt::ptree devices_node;
      devices_node.put("device_id"s, device_id);
      devices_node.put("data"s, to_string(data));
      devices_nodes.push_back(std::make_pair(""s, devices_node));
    }

    auto adapters { platf::adapter_names() };
    pt::ptree adapters_nodes;
    for (const auto &adapter_name : adapters) {
      pt::ptree adapters_node;
      adapters_node.put("name"s, adapter_name);
      adapters_nodes.push_back(std::make_pair(""s, adapters_node));
    }

    outputTree.add_child("display_devices", devices_nodes);
    outputTree.add_child("adapters", adapters_nodes);
    outputTree.put("status", "true");
    outputTree.put("platform", SUNSHINE_PLATFORM);
    outputTree.put("version", PROJECT_VER);

    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      outputTree.put(std::move(name), std::move(value));
    }

    outputTree.put("pair_name", nvhttp::get_pair_name());
  }

  void
  getLocale(resp_https_t response, req_https_t request) {
    // we need to return the locale whether authenticated or not

    print_req(request);

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    outputTree.put("status", "true");
    outputTree.put("locale", config::sunshine.locale);
  }

  std::vector<std::string>
  split(const std::string &str, char delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0, end = 0;
    while ((end = str.find(delimiter, start)) != std::string::npos) {
      tokens.push_back(str.substr(start, end - start));
      start = end + 1;
    }
    tokens.push_back(str.substr(start));
    return tokens;
  }

  bool
  saveVddSettings(std::string resArray, std::string fpsArray, std::string gpu_name) {
    pt::ptree iddOptionTree;
    pt::ptree global_node;
    pt::ptree resolutions_nodes;

    // prepare resolutions setting for vdd
    boost::regex pattern("\\[|\\]|\\s+");
    char delimiter = ',';

    // 添加全局刷新率到global节点
    for (const auto &fps : split(boost::regex_replace(fpsArray, pattern, ""), delimiter)) {
      global_node.add("g_refresh_rate", fps);
    }

    std::string str = boost::regex_replace(resArray, pattern, "");
    boost::algorithm::trim(str);
    for (const auto &resolution : split(str, delimiter)) {
      auto index = resolution.find('x');
      if(index == std::string::npos) {
        return false;
      }
      pt::ptree res_node;
      res_node.put("width", resolution.substr(0, index));
      res_node.put("height", resolution.substr(index + 1));
      resolutions_nodes.push_back(std::make_pair("resolution"s, res_node));
    }

    // 类似于 config.cpp 中的 path_f 函数逻辑，使用相对路径
    std::filesystem::path idd_option_path = platf::appdata() / "vdd_settings.xml";

    BOOST_LOG(info) << "VDD配置文件路径: " << idd_option_path.string();

    if (!fs::exists(idd_option_path)) {
        return false;
    }

    // 先读取现有配置文件
    pt::ptree existing_root;
    pt::ptree root;

    try {
      pt::read_xml(idd_option_path.string(), existing_root);
      // 如果现有配置文件中已有vdd_settings节点
      if (existing_root.get_child_optional("vdd_settings")) {
        // 复制现有配置
        iddOptionTree = existing_root.get_child("vdd_settings");

        // 更新需要更改的部分
        pt::ptree monitor_node;
        monitor_node.put("count", 1);

        pt::ptree gpu_node;
        gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

        // 替换配置
        iddOptionTree.put_child("monitors", monitor_node);
        iddOptionTree.put_child("gpu", gpu_node);
        iddOptionTree.put_child("global", global_node);
        iddOptionTree.put_child("resolutions", resolutions_nodes);
      } else {
        // 如果没有vdd_settings节点，创建新的
        pt::ptree monitor_node;
        monitor_node.put("count", 1);

        pt::ptree gpu_node;
        gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

        iddOptionTree.add_child("monitors", monitor_node);
        iddOptionTree.add_child("gpu", gpu_node);
        iddOptionTree.add_child("global", global_node);
        iddOptionTree.add_child("resolutions", resolutions_nodes);
      }
    } catch(std::exception &e) {
      // 读取失败，创建新的配置
      BOOST_LOG(warning) << "读取现有VDD配置失败，创建新配置: " << e.what();

      pt::ptree monitor_node;
      monitor_node.put("count", 1);

      pt::ptree gpu_node;
      gpu_node.put("friendlyname", gpu_name.empty() ? "default" : gpu_name);

      iddOptionTree.add_child("monitors", monitor_node);
      iddOptionTree.add_child("gpu", gpu_node);
      iddOptionTree.add_child("global", global_node);
      iddOptionTree.add_child("resolutions", resolutions_nodes);
    }

    root.add_child("vdd_settings", iddOptionTree);
    try {
      // 使用更紧凑的XML格式设置，减少不必要的空白
      auto setting = boost::property_tree::xml_writer_make_settings<std::string>(' ', 2);
      std::ostringstream oss;
      write_xml(oss, root, setting);

      // 清理多余空行，保持XML格式整洁
      std::string xml_content = oss.str();
      boost::regex empty_lines_regex("\\n\\s*\\n");
      xml_content = boost::regex_replace(xml_content, empty_lines_regex, "\n");

      std::ofstream file(idd_option_path.string());
      file << xml_content;
      file.close();

      return true;
    }
    catch(std::exception &e) {
      BOOST_LOG(warning) << "写入VDD配置失败: " << e.what();
      return false;
    }
  }

  void
  saveConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    pt::ptree inputTree;

    try {
      pt::read_json(ss, inputTree);
      std::string resArray = inputTree.get<std::string>("resolutions", "[]");
      std::string fpsArray = inputTree.get<std::string>("fps", "[]");
      std::string gpu_name = inputTree.get<std::string>("adapter_name", "");

      // Validate config field lengths to prevent abuse
      auto sunshine_name = inputTree.get<std::string>("sunshine_name", "");
      if (sunshine_name.size() > 256) {
        outputTree.put("status", "false");
        outputTree.put("error", "sunshine_name too long (max 256)");
        return;
      }
      if (gpu_name.size() > 256) {
        outputTree.put("status", "false");
        outputTree.put("error", "adapter_name too long (max 256)");
        return;
      }

      saveVddSettings(resArray, fpsArray, gpu_name);

      // 将 inputTree 转换为 std::map（保证有序）
      std::map<std::string, std::string> fullConfig;
      for (const auto &kv : inputTree) {
        std::string value = inputTree.get<std::string>(kv.first);
        fullConfig[kv.first] = value;
      }

      // 更新配置
      config::update_full_config(fullConfig);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      outputTree.put("status", "false");
      outputTree.put("error", e.what());
      return;
    }

    outputTree.put("status", "true");
  }

  void
  restart(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // We may not return from this call
    platf::restart();
  }

  void
  boom(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
    lifetime::exit_sunshine(0, false);
  }

  void
  resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    display_device::session_t::get().reset_persistence();
    outputTree.put("status", true);
  }

  void
  savePassword(resp_https_t response, req_https_t request) {
    if (!config::sunshine.username.empty() && !authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    std::stringstream configStream;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      auto username = inputTree.count("currentUsername") > 0 ? inputTree.get<std::string>("currentUsername") : "";
      auto newUsername = inputTree.get<std::string>("newUsername");
      auto password = inputTree.count("currentPassword") > 0 ? inputTree.get<std::string>("currentPassword") : "";
      auto newPassword = inputTree.count("newPassword") > 0 ? inputTree.get<std::string>("newPassword") : "";
      auto confirmPassword = inputTree.count("confirmNewPassword") > 0 ? inputTree.get<std::string>("confirmNewPassword") : "";

      // Validate credential lengths
      if (newUsername.size() > 128) {
        outputTree.put("status", false);
        outputTree.put("error", "Username too long (max 128)");
        return;
      }
      if (newPassword.size() > 256) {
        outputTree.put("status", false);
        outputTree.put("error", "Password too long (max 256)");
        return;
      }

      if (newUsername.length() == 0) newUsername = username;
      if (newUsername.length() == 0) {
        outputTree.put("status", false);
        outputTree.put("error", "Invalid Username");
      }
      else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() || (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword) {
            outputTree.put("status", false);
            outputTree.put("error", "Password Mismatch");
          }
          else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            outputTree.put("status", true);
          }
        }
        else {
          outputTree.put("status", false);
          outputTree.put("error", "Invalid Current Credentials");
        }
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  void
  savePin(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      std::string pin = inputTree.get<std::string>("pin");
      std::string name = inputTree.get<std::string>("name");

      // Validate PIN: must be numeric digits only, 4-8 characters
      if (pin.size() < 4 || pin.size() > 8 || !std::all_of(pin.begin(), pin.end(), ::isdigit)) {
        outputTree.put("status", false);
        outputTree.put("error", "PIN must be 4-8 digits");
        return;
      }
      // Validate client name
      if (name.empty() || name.size() > 256) {
        outputTree.put("status", false);
        outputTree.put("error", "Client name must be 1-256 characters");
        return;
      }

      bool pin_result = nvhttp::pin(pin, name);
      outputTree.put("status", pin_result);

      // Send webhook notification
      webhook::send_event_async(webhook::event_t{
        .type = pin_result ? webhook::event_type_t::CONFIG_PIN_SUCCESS : webhook::event_type_t::CONFIG_PIN_FAILED,
        .alert_type = pin_result ? "config_pair_success" : "config_pair_failed",
        .timestamp = webhook::get_current_timestamp(),
        .client_name = name,
        .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
        .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()),
        .app_name = "",
        .app_id = 0,
        .session_id = "",
        .extra_data = {}
      });
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());

      // Send webhook notification for pairing failure
      webhook::send_event_async(webhook::event_t{
        .type = webhook::event_type_t::CONFIG_PIN_FAILED,
        .alert_type = "config_pair_failed",
        .timestamp = webhook::get_current_timestamp(),
        .client_name = "",
        .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
        .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()),
        .app_name = "",
        .app_id = 0,
        .session_id = "",
        .extra_data = {{"error", e.what()}}
      });
      return;
    }
  }

  void
  getQrPairStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    nlohmann::json j;
    j["status"] = nvhttp::get_qr_pair_status();

    std::string content = j.dump();
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  void
  generateQrPairInfo(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    // Generate a random 4-digit PIN using OpenSSL CSPRNG
    uint16_t random_val;
    RAND_bytes(reinterpret_cast<unsigned char *>(&random_val), sizeof(random_val));
    int pin_num = random_val % 10000;
    char pin_buf[5];
    std::snprintf(pin_buf, sizeof(pin_buf), "%04d", pin_num);
    std::string pin(pin_buf);

    // Set the preset PIN in nvhttp (valid for 120 seconds)
    std::string server_name = config::nvhttp.sunshine_name;
    if (!nvhttp::set_preset_pin(pin, server_name, 120)) {
      outputTree.put("status", false);
      outputTree.put("error", "Failed to set preset PIN");
      return;
    }

    // Get server address info
    auto local_addr = net::addr_to_normalized_string(request->local_endpoint().address());
    auto port = net::map_port(nvhttp::PORT_HTTP);

    // Determine the host address for the QR code URL
    std::string host;
    if (!config::nvhttp.external_ip.empty()) {
      // User explicitly configured an external IP (could be WAN for port-forwarded setups)
      host = config::nvhttp.external_ip;
    }
    else {
      // Detect a usable LAN IP (local_endpoint may be loopback or VPN)
      host = local_addr;
      auto host_net_type = net::from_address(host);
      if (host_net_type != net::LAN) {
        std::string resolved_host;

        // Method 1: UDP connect trick to find default outgoing interface
        try {
          boost::asio::io_context io_ctx;
          boost::asio::ip::udp::socket socket(io_ctx);
          socket.connect(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("8.8.8.8"), 53));
          auto lan_addr = socket.local_endpoint().address();
          socket.close();
          auto candidate = net::addr_to_normalized_string(lan_addr);
          if (net::from_address(candidate) == net::LAN) {
            resolved_host = candidate;
          }
        }
        catch (...) {}

#ifdef _WIN32
        // Method 2 (Windows): enumerate adapters via GetAdaptersAddresses
        if (resolved_host.empty()) {
          ULONG bufLen = 15000;
          std::vector<uint8_t> buf(bufLen);
          auto pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
          if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, pAddresses, &bufLen) == NO_ERROR) {
            for (auto adapter = pAddresses; adapter; adapter = adapter->Next) {
              if (adapter->OperStatus != IfOperStatusUp) continue;
              if (adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP) continue;
              for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto sin = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
                boost::asio::ip::address_v4 addr(ntohl(sin->sin_addr.s_addr));
                auto candidate = addr.to_string();
                if (net::from_address(candidate) == net::LAN) {
                  resolved_host = candidate;
                  break;
                }
              }
              if (!resolved_host.empty()) break;
            }
          }
        }
#endif

        if (!resolved_host.empty()) {
          host = resolved_host;
          BOOST_LOG(info) << "QR pair: resolved to LAN IP " << host;
        }
        else {
          BOOST_LOG(warning) << "QR pair: could not find a LAN IP, using " << host;
        }
      }
    }

    // Build the moonlight:// URL
    std::string url = "moonlight://pair?host=" + host +
                      "&port=" + std::to_string(port) +
                      "&pin=" + pin +
                      "&name=" + server_name;

    outputTree.put("status", true);
    outputTree.put("pin", pin);
    outputTree.put("host", host);
    outputTree.put("port", port);
    outputTree.put("name", server_name);
    outputTree.put("url", url);
    outputTree.put("expires_in", 120);
  }

  void
  cancelQrPair(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    nvhttp::clear_preset_pin();

    pt::ptree outputTree;
    outputTree.put("status", true);

    std::ostringstream data;
    pt::write_json(data, outputTree);
    response->write(data.str());
  }

  void
  unpairAll(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });
    nvhttp::erase_all_clients();
    proc::proc.terminate();
    outputTree.put("status", true);
  }

  void
  unpair(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      std::string uuid = inputTree.get<std::string>("uuid");

      // Validate UUID format (hex + hyphens, reasonable length)
      if (uuid.empty() || uuid.size() > 64) {
        outputTree.put("status", false);
        outputTree.put("error", "Invalid client UUID");
        return;
      }

      outputTree.put("status", nvhttp::unpair_client(uuid));
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  void
  renameClient(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      pt::read_json(ss, inputTree);

      std::string uuid = inputTree.get<std::string>("uuid");
      std::string new_name = inputTree.get<std::string>("name");

      if (new_name.empty()) {
        outputTree.put("status", false);
        outputTree.put("error", "Name cannot be empty");
        return;
      }

      bool result = nvhttp::rename_client(uuid, new_name);
      outputTree.put("status", result);
      if (!result) {
        outputTree.put("error", "Client not found");
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "Rename client: "sv << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
    }
  }

  void
  listClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);
    const nlohmann::json named_certs = nvhttp::get_all_clients();
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
    output_tree["status"] = "true";
    send_response(response, output_tree);
  }

  void
  closeApp(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    pt::ptree outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    proc::proc.terminate();
    outputTree.put("status", true);
  }

  void
  getRuntimeSessions(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // 限制只允许 localhost 访问（增强安全性）
    auto client_address = request->remote_endpoint().address();
    auto address = net::addr_to_normalized_string(client_address);
    auto ip_type = net::from_address(address);
    
    if (ip_type != net::PC) {
      std::ostringstream msg_stream;
      msg_stream << "Access denied when getting runtime sessions. Only localhost requests are allowed. Client IP: " << client_address.to_string();
      BOOST_LOG(warning) << msg_stream.str();
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 403;
      error_json["status_message"] = msg_stream.str();
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
      return;
    }

    try {
      // 获取所有活动会话信息
      auto sessions_info = stream::session::get_all_sessions_info();
      
      json response_json;
      response_json["success"] = true;
      response_json["status_code"] = 200;
      response_json["status_message"] = "Success";
      response_json["total_sessions"] = sessions_info.size();
      
      json sessions_array = json::array();

      for (const auto &session_info : sessions_info) {
        json session_obj;
        session_obj["client_name"] = session_info.client_name;
        session_obj["client_address"] = session_info.client_address;
        session_obj["state"] = session_info.state;
        session_obj["session_id"] = session_info.session_id;
        session_obj["width"] = session_info.width;
        session_obj["height"] = session_info.height;
        session_obj["fps"] = session_info.fps;
        session_obj["bitrate"] = session_info.bitrate;
        session_obj["host_audio"] = session_info.host_audio;
        session_obj["enable_hdr"] = session_info.enable_hdr;
        session_obj["enable_mic"] = session_info.enable_mic;
        session_obj["app_name"] = session_info.app_name;
        session_obj["app_id"] = session_info.app_id;
        
        sessions_array.push_back(session_obj);
      }
      
      response_json["sessions"] = sessions_array;
      
      BOOST_LOG(debug) << "Config API: Runtime sessions info requested, returned " << sessions_info.size() << " sessions";
      
      response->write(response_json.dump());
      response->close_connection_after_response = true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "getRuntimeSessions: " << e.what();
      
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 500;
      error_json["status_message"] = std::string(e.what());
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
    }
    catch (...) {
      BOOST_LOG(error) << "getRuntimeSessions: Unknown exception";
      
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 500;
      error_json["status_message"] = "Unknown error";
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
    }
  }

  void
  changeRuntimeBitrate(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    print_req(request);

    // 限制只允许 localhost 访问
    auto client_address = request->remote_endpoint().address();
    auto address = net::addr_to_normalized_string(client_address);
    auto ip_type = net::from_address(address);
    
    if (ip_type != net::PC) {
      std::ostringstream msg_stream;
      msg_stream << "Access denied. Only localhost requests are allowed. Client IP: " << client_address.to_string();
      BOOST_LOG(warning) << msg_stream.str();
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 403;
      error_json["status_message"] = msg_stream.str();
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
      return;
    }

    try {
      auto args = request->parse_query_string();
      auto bitrate_param = args.find("bitrate");
      auto clientname_param = args.find("clientname");

      // 验证参数
      if (bitrate_param == args.end()) {
        std::ostringstream msg_stream;
        msg_stream << "Missing bitrate parameter when changing bitrate";
        BOOST_LOG(warning) << msg_stream.str();
        json error_json;
        error_json["success"] = false;
        error_json["status_code"] = 400;
        error_json["status_message"] = msg_stream.str();
        response->write(error_json.dump());
        response->close_connection_after_response = true;
        return;
      }

      if (clientname_param == args.end()) {
        std::ostringstream msg_stream;
        msg_stream << "Missing clientname parameter when changing bitrate";
        BOOST_LOG(warning) << msg_stream.str();
        json error_json;
        error_json["success"] = false;
        error_json["status_code"] = 400;
        error_json["status_message"] = msg_stream.str();
        response->write(error_json.dump());
        response->close_connection_after_response = true;
        return;
      }

      // 安全地解析码率参数
      int bitrate = 0;
      try {
        bitrate = std::stoi(bitrate_param->second);
      }
      catch (...) {
        json error_json;
        error_json["success"] = false;
        error_json["status_code"] = 400;
        error_json["status_message"] = "Invalid bitrate parameter format";
        response->write(error_json.dump());
        response->close_connection_after_response = true;
        return;
      }

      std::string client_name = clientname_param->second;

      // 验证码率范围
      if (bitrate <= 0 || bitrate > 800000) {
        std::ostringstream msg_stream;
        msg_stream << "Invalid bitrate value when changing bitrate. Must be between 1 and 800000 Kbps";
        BOOST_LOG(warning) << msg_stream.str();
        json error_json;
        error_json["success"] = false;
        error_json["status_code"] = 400;
        error_json["status_message"] = msg_stream.str();
        response->write(error_json.dump());
        response->close_connection_after_response = true;
        return;
      }

      // 获取所有活动会话以便调试
      std::vector<std::string> available_clients;
      try {
        auto sessions_info = stream::session::get_all_sessions_info();
        for (const auto &session_info : sessions_info) {
          if (session_info.state == "RUNNING") {
            available_clients.push_back(session_info.client_name);
          }
        }
      }
      catch (...) {
        // 继续执行，即使获取会话信息失败，仍然尝试修改码率
      }
      
      BOOST_LOG(info) << "Config API: Attempting to change bitrate for client '" << client_name 
                      << "' to " << bitrate << " Kbps";
      if (!available_clients.empty()) {
        BOOST_LOG(info) << "Available RUNNING clients: " << boost::algorithm::join(available_clients, ", ");
      }
      
      // 调用底层 API 修改码率
      video::dynamic_param_t param;
      param.type = video::dynamic_param_type_e::BITRATE;
      param.value.int_value = bitrate;
      param.valid = true;
      
      bool success = stream::session::change_dynamic_param_for_client(client_name, param);

      json response_json;
      if (success) {
        response_json["success"] = true;
        response_json["status_code"] = 200;
        response_json["status_message"] = "Bitrate change request sent to client session";
        response_json["bitrate"] = bitrate;
        response_json["client_name"] = client_name;
        
        BOOST_LOG(info) << "Config API: Dynamic bitrate change requested for client '" 
                       << client_name << "': " << bitrate << " Kbps";
      } else {
        std::string error_msg = "No active streaming session found for client: " + client_name;
        if (!available_clients.empty()) {
          error_msg += ". Available clients: " + boost::algorithm::join(available_clients, ", ");
        } else {
          error_msg += ". No RUNNING sessions available.";
        }
        
        response_json["success"] = false;
        response_json["status_code"] = 404;
        response_json["status_message"] = error_msg;
        
        BOOST_LOG(warning) << "Config API: Failed to change bitrate - " << error_msg;
      }
      
      response->write(response_json.dump());
      response->close_connection_after_response = true;
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "changeRuntimeBitrate: " << e.what();
      
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 500;
      error_json["status_message"] = std::string(e.what());
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
    }
    catch (...) {
      BOOST_LOG(error) << "changeRuntimeBitrate: Unknown exception";
      
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 500;
      error_json["status_message"] = "Unknown error";
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
    }
  }

  void
  proxySteamApi(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    // 提取请求路径，移除/steam-api前缀
    std::string path = request->path;
    if (path.find("/steam-api") == 0) {
      path = path.substr(10); // 移除"/steam-api"前缀
    }

    // 构建目标URL
    std::string targetUrl = "https://api.steampowered.com" + path;
    
    // 添加查询参数
    if (!request->query_string.empty()) {
      targetUrl += "?" + request->query_string;
    }

    BOOST_LOG(info) << "Steam API proxy request: " << targetUrl;

    // 安全检查：防止SSRF，确保目标主机确实是api.steampowered.com
    if (http::url_get_host(targetUrl) != "api.steampowered.com") {
      BOOST_LOG(warning) << "Blocked Steam API proxy request to unauthorized host";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Invalid Host");
      return;
    }

    // 使用http模块下载数据
    std::string content;
    
    try {
      if (http::fetch_url(targetUrl, content)) {
        // 设置响应头
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("Access-Control-Allow-Origin", "*");
        headers.emplace("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
        
        response->write(SimpleWeb::StatusCode::success_ok, content, headers);
      } else {
        BOOST_LOG(error) << "Steam API request failed: " << targetUrl;
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam API request failed");
      }
    } catch (const std::exception& e) {
      BOOST_LOG(error) << "Steam API proxy exception: " << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam API proxy exception");
    }
  }

  void
  proxySteamStore(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    // 提取请求路径，移除/steam-store前缀
    std::string path = request->path;
    if (path.find("/steam-store") == 0) {
      path = path.substr(12); // 移除"/steam-store"前缀
    }

    // 构建目标URL
    std::string targetUrl = "https://store.steampowered.com" + path;
    
    // 添加查询参数
    if (!request->query_string.empty()) {
      targetUrl += "?" + request->query_string;
    }

    BOOST_LOG(info) << "Steam Store proxy request: " << targetUrl;

    // 安全检查：防止SSRF，确保目标主机确实是store.steampowered.com
    if (http::url_get_host(targetUrl) != "store.steampowered.com") {
      BOOST_LOG(warning) << "Blocked Steam Store proxy request to unauthorized host";
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Invalid Host");
      return;
    }

    // 使用http模块下载数据
    std::string content;
    
    try {
      if (http::fetch_url(targetUrl, content)) {
        // 设置响应头
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        headers.emplace("Access-Control-Allow-Origin", "*");
        headers.emplace("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
        
        response->write(SimpleWeb::StatusCode::success_ok, content, headers);
      } else {
        BOOST_LOG(error) << "Steam Store request failed: " << targetUrl;
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam Store request failed");
      }
    } catch (const std::exception& e) {
      BOOST_LOG(error) << "Steam Store proxy exception: " << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "Steam Store proxy exception");
    }
  }

  // ===== AI LLM Proxy =====

  // 内存缓存 AI 配置，避免每次请求都读文件
  static std::mutex ai_config_mutex;
  static nlohmann::json ai_config_cache;
  static bool ai_config_loaded = false;

  /**
   * @brief 获取 AI 配置文件路径（与 sunshine.conf 同目录）
   */
  static std::string
  getAiConfigPath() {
    auto config_dir = fs::path(config::sunshine.config_file).parent_path();
    return (config_dir / "ai_config.json").string();
  }

  /**
   * @brief 从文件或缓存读取 AI 配置（调用方需持有 ai_config_mutex）
   */
  static nlohmann::json
  loadAiConfigLocked() {
    if (ai_config_loaded) {
      return ai_config_cache;
    }

    auto path = getAiConfigPath();
    try {
      std::string content = file_handler::read_file(path.c_str());
      if (!content.empty()) {
        ai_config_cache = nlohmann::json::parse(content);
        ai_config_loaded = true;
        return ai_config_cache;
      }
    } catch (...) {}

    ai_config_cache = nlohmann::json{
      {"enabled", false},
      {"provider", "openai"},
      {"apiBase", "https://api.openai.com/v1"},
      {"apiKey", ""},
      {"model", "gpt-4o-mini"}
    };
    ai_config_loaded = true;
    return ai_config_cache;
  }

  /**
   * @brief 从文件或缓存读取 AI 配置（线程安全）
   */
  static nlohmann::json
  loadAiConfig() {
    std::lock_guard<std::mutex> lock(ai_config_mutex);
    return loadAiConfigLocked();
  }

  /**
   * @brief 保存 AI 配置并刷新缓存（调用方需持有 ai_config_mutex）
   */
  static bool
  saveAiConfigLocked(const nlohmann::json &cfg) {
    auto path = getAiConfigPath();
    try {
      std::ofstream file(path);
      if (file.is_open()) {
        file << cfg.dump(2);
        ai_config_cache = cfg;
        ai_config_loaded = true;
        return true;
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to save AI config: " << e.what();
    }
    return false;
  }

  /**
   * @brief 检测 provider 是否为 Anthropic（需要不同的 API 格式）
   */
  static bool
  isAnthropicProvider(const nlohmann::json &cfg) {
    std::string provider = cfg.value("provider", "");
    std::string apiBase = cfg.value("apiBase", "");
    return provider == "anthropic" || apiBase.find("anthropic.com") != std::string::npos;
  }

  /**
   * @brief 将 OpenAI 格式的请求转换为 Anthropic 格式
   */
  static std::string
  convertToAnthropicFormat(const std::string &openaiBody, const std::string &model) {
    try {
      auto input = nlohmann::json::parse(openaiBody);
      nlohmann::json anthropic;

      anthropic["model"] = input.value("model", model);
      anthropic["max_tokens"] = input.value("max_tokens", 4096);

      // 提取 system message 和 user/assistant messages
      if (input.contains("messages")) {
        nlohmann::json messages = nlohmann::json::array();
        for (auto &msg : input["messages"]) {
          std::string role = msg.value("role", "");
          if (role == "system") {
            anthropic["system"] = msg.value("content", "");
          } else {
            messages.push_back(msg);
          }
        }
        anthropic["messages"] = messages;
      }

      // 转换 temperature, top_p 等通用参数
      if (input.contains("temperature")) anthropic["temperature"] = input["temperature"];
      if (input.contains("top_p")) anthropic["top_p"] = input["top_p"];
      if (input.contains("stream")) anthropic["stream"] = input["stream"];

      return anthropic.dump();
    } catch (...) {
      return openaiBody;  // 转换失败，原样返回
    }
  }

  /**
   * @brief 将 Anthropic 格式的响应转换回 OpenAI 格式
   */
  static std::string
  convertFromAnthropicFormat(const std::string &anthropicResponse) {
    try {
      auto resp = nlohmann::json::parse(anthropicResponse);
      nlohmann::json openai;

      openai["id"] = resp.value("id", "");
      openai["object"] = "chat.completion";
      openai["model"] = resp.value("model", "");

      // 转换 content blocks
      nlohmann::json choice;
      choice["index"] = 0;
      choice["finish_reason"] = resp.value("stop_reason", "stop");

      std::string content;
      if (resp.contains("content") && resp["content"].is_array()) {
        for (auto &block : resp["content"]) {
          if (block.value("type", "") == "text") {
            content += block.value("text", "");
          }
        }
      }
      choice["message"]["role"] = "assistant";
      choice["message"]["content"] = content;
      openai["choices"] = nlohmann::json::array({choice});

      // 转换 usage
      if (resp.contains("usage")) {
        openai["usage"]["prompt_tokens"] = resp["usage"].value("input_tokens", 0);
        openai["usage"]["completion_tokens"] = resp["usage"].value("output_tokens", 0);
        openai["usage"]["total_tokens"] =
          resp["usage"].value("input_tokens", 0) + resp["usage"].value("output_tokens", 0);
      }

      return openai.dump();
    } catch (...) {
      return anthropicResponse;
    }
  }

  /**
   * @brief GET /api/ai/config — 获取 AI 配置（不返回完整 API key）
   */
  void
  getAiConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    auto cfg = loadAiConfig();

    // 掩码 API key：仅显示前4+后4字符
    if (cfg.contains("apiKey") && cfg["apiKey"].is_string()) {
      std::string key = cfg["apiKey"].get<std::string>();
      if (key.length() > 8) {
        cfg["apiKey"] = key.substr(0, 4) + "****" + key.substr(key.length() - 4);
      } else if (!key.empty()) {
        cfg["apiKey"] = "****";
      }
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, cfg.dump(), headers);
  }

  /**
   * @brief POST /api/ai/config — 保存 AI 配置
   */
  void
  saveAiConfigEndpoint(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    nlohmann::json output;
    try {
      auto input = nlohmann::json::parse(ss.str());

      // 用同一把锁包住 load-modify-save，防止并发写入丢失
      std::lock_guard<std::mutex> lock(ai_config_mutex);
      auto current = loadAiConfigLocked();
      if (input.contains("enabled")) current["enabled"] = input["enabled"].get<bool>();
      if (input.contains("provider")) current["provider"] = input["provider"].get<std::string>();
      if (input.contains("apiBase")) current["apiBase"] = input["apiBase"].get<std::string>();
      if (input.contains("model")) current["model"] = input["model"].get<std::string>();
      if (input.contains("apiKey")) {
        std::string key = input["apiKey"].get<std::string>();
        // 如果前端发来的是掩码（包含****），不覆盖
        if (key.find("****") == std::string::npos) {
          current["apiKey"] = key;
        }
      }

      if (saveAiConfigLocked(current)) {
        output["status"] = "ok";
      } else {
        output["status"] = "error";
        output["error"] = "Failed to write config file";
      }
    } catch (const std::exception &e) {
      output["status"] = "error";
      output["error"] = std::string("Invalid JSON: ") + e.what();
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, output.dump(), headers);
  }

  /**
   * @brief POST /api/ai/chat/completions — OpenAI 兼容的 LLM 代理端点
   *
   * 支持普通请求和 SSE 流式请求。
   * 自动适配 Anthropic API 格式。
   * 客户端无需知道 API key 或实际后端，Sunshine 作为透明代理。
   */
  void
  proxyAiChat(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    std::string requestBody = ss.str();

    // 检测是否请求流式输出
    bool isStream = false;
    try {
      auto reqJson = nlohmann::json::parse(requestBody);
      isStream = reqJson.value("stream", false);
    } catch (...) {}

    if (isStream) {
      bool headerSent = false;
      auto result = processAiChatStream(requestBody, [&](const char *data, size_t len) {
        if (!headerSent) {
          *response << "HTTP/1.1 200 OK\r\n";
          *response << "Content-Type: text/event-stream\r\n";
          *response << "Cache-Control: no-cache\r\n";
          *response << "Connection: keep-alive\r\n";
          *response << "\r\n";
          response->send();
          headerSent = true;
        }
        std::string chunk(data, len);
        *response << chunk;
        response->send();
      });

      if (result.httpCode != 200 && !headerSent) {
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_bad_gateway, result.body, headers);
      }
    } else {
      auto result = processAiChat(requestBody);
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", result.contentType);

      auto statusCode = (result.httpCode == 200)
        ? SimpleWeb::StatusCode::success_ok
        : (result.httpCode == 403)
          ? SimpleWeb::StatusCode::client_error_forbidden
          : (result.httpCode == 400)
            ? SimpleWeb::StatusCode::client_error_bad_request
            : SimpleWeb::StatusCode::server_error_bad_gateway;

      response->write(statusCode, result.body, headers);
    }
  }

  /**
   * @brief OPTIONS handler for CORS preflight on AI endpoints
   */
  void
  handleAiCors(resp_https_t response, req_https_t request) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
    response->write(SimpleWeb::StatusCode::success_no_content, "", headers);
  }

  // ===== AI Proxy shared interface =====

  bool
  isAiEnabled() {
    auto cfg = loadAiConfig();
    return cfg.value("enabled", false) &&
           !cfg.value("apiKey", "").empty() &&
           !cfg.value("apiBase", "").empty();
  }

  /**
   * @brief Prepare AI proxy request: validate config, build URL, headers, convert body.
   * @return empty targetUrl on error (result is filled with error info)
   */
  static bool
  prepareAiRequest(
    const std::string &requestBody,
    std::string &targetUrl,
    std::string &processedBody,
    std::map<std::string, std::string> &proxyHeaders,
    bool &isAnthropic,
    bool &isStream,
    AiProxyResult &result) {

    auto cfg = loadAiConfig();

    if (!cfg.value("enabled", false)) {
      result = {403, R"({"error":{"message":"AI proxy is not enabled","type":"invalid_request_error"}})", "application/json"};
      return false;
    }

    std::string apiBase = cfg.value("apiBase", "");
    std::string apiKey = cfg.value("apiKey", "");
    std::string defaultModel = cfg.value("model", "");

    if (apiBase.empty() || apiKey.empty()) {
      result = {400, R"({"error":{"message":"AI proxy not configured: missing apiBase or apiKey","type":"invalid_request_error"}})", "application/json"};
      return false;
    }

    if (requestBody.empty()) {
      result = {400, R"({"error":{"message":"Empty request body","type":"invalid_request_error"}})", "application/json"};
      return false;
    }

    processedBody = requestBody;
    isStream = false;

    try {
      auto reqJson = nlohmann::json::parse(processedBody);
      isStream = reqJson.value("stream", false);
      if (!reqJson.contains("model") && !defaultModel.empty()) {
        reqJson["model"] = defaultModel;
        processedBody = reqJson.dump();
      }
    } catch (...) {}

    isAnthropic = isAnthropicProvider(cfg);

    while (!apiBase.empty() && apiBase.back() == '/') {
      apiBase.pop_back();
    }
    targetUrl = isAnthropic
      ? apiBase + "/v1/messages"
      : apiBase + "/chat/completions";

    if (isAnthropic) {
      // Anthropic 流式 SSE 格式与 OpenAI 不兼容，强制走非流式以保证响应格式一致
      if (isStream) {
        try {
          auto reqJson = nlohmann::json::parse(processedBody);
          reqJson["stream"] = false;
          processedBody = reqJson.dump();
        } catch (...) {}
        isStream = false;
      }
      processedBody = convertToAnthropicFormat(processedBody, defaultModel);
      proxyHeaders["x-api-key"] = apiKey;
      proxyHeaders["anthropic-version"] = "2023-06-01";
    } else {
      proxyHeaders["Authorization"] = "Bearer " + apiKey;
    }

    BOOST_LOG(info) << "AI proxy forwarding to: " << targetUrl << (isStream ? " (stream)" : "");
    return true;
  }

  AiProxyResult
  processAiChat(const std::string &requestBody) {
    std::string targetUrl, processedBody;
    std::map<std::string, std::string> proxyHeaders;
    bool isAnthropic = false, isStream = false;
    AiProxyResult result;

    if (!prepareAiRequest(requestBody, targetUrl, processedBody, proxyHeaders, isAnthropic, isStream, result)) {
      return result;
    }

    std::string responseBody;
    long httpCode = 0;

    try {
      bool ok = http::post_json(targetUrl, processedBody, proxyHeaders, responseBody, httpCode, 120);
      if (ok) {
        if (isAnthropic && httpCode >= 200 && httpCode < 300) {
          responseBody = convertFromAnthropicFormat(responseBody);
        }
        int statusCode = (httpCode >= 200 && httpCode < 300) ? 200 : 502;
        return {statusCode, responseBody, "application/json"};
      } else {
        return {502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json"};
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "AI proxy exception: " << e.what();
      nlohmann::json err;
      err["error"]["message"] = std::string("AI proxy exception: ") + e.what();
      err["error"]["type"] = "internal_error";
      return {500, err.dump(), "application/json"};
    }
  }

  /**
   * @brief curl write callback for streaming with std::function
   */
  struct StreamCallbackContext {
    std::function<void(const char *, size_t)> callback;
  };

  static size_t
  stream_func_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    auto *ctx = static_cast<StreamCallbackContext *>(userdata);
    ctx->callback(ptr, realsize);
    return realsize;
  }

  AiProxyResult
  processAiChatStream(
    const std::string &requestBody,
    std::function<void(const char *, size_t)> chunkCallback) {

    std::string targetUrl, processedBody;
    std::map<std::string, std::string> proxyHeaders;
    bool isAnthropic = false, isStream = false;
    AiProxyResult result;

    if (!prepareAiRequest(requestBody, targetUrl, processedBody, proxyHeaders, isAnthropic, isStream, result)) {
      return result;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
      return {500, R"({"error":{"message":"Failed to initialize CURL","type":"internal_error"}})", "application/json"};
    }

    StreamCallbackContext ctx;
    ctx.callback = std::move(chunkCallback);

    struct curl_slist *header_list = nullptr;
    header_list = curl_slist_append(header_list, "Content-Type: application/json");
    for (const auto &[key, value] : proxyHeaders) {
      std::string header_line = key + ": " + value;
      header_list = curl_slist_append(header_list, header_line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, targetUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, processedBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(processedBody.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_func_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode curlResult = curl_easy_perform(curl);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (curlResult != CURLE_OK) {
      return {502, R"({"error":{"message":"Failed to connect to upstream LLM API","type":"upstream_error"}})", "application/json"};
    }
    return {200, "", "text/event-stream"};
  }

  /**
   * @brief 计算文件的SHA256哈希值
   * @param filepath 文件路径
   * @return SHA256哈希字符串，如果失败返回空字符串
   */
  std::string
  calculate_file_hash(const std::string &filepath) {
    if (filepath.empty() || !boost::filesystem::exists(filepath)) {
      return "";
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
      return "";
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
      return "";
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
      EVP_MD_CTX_free(mdctx);
      return "";
    }

    char buf[1024 * 16];
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (file.gcount() > 0) {
        if (EVP_DigestUpdate(mdctx, buf, file.gcount()) != 1) {
          EVP_MD_CTX_free(mdctx);
          file.close();
          return "";
        }
      }
    }
    file.close();

    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
      EVP_MD_CTX_free(mdctx);
      return "";
    }
    EVP_MD_CTX_free(mdctx);

    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_len; i++) {
      ss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return ss.str();
  }

  /**
   * @brief 从命令字符串中提取可执行文件路径
   * @param cmd 完整命令字符串
   * @return 可执行文件路径
   */
  std::string
  extract_executable_path(const std::string &cmd) {
    if (cmd.empty()) {
      return "";
    }

    std::string trimmed = cmd;
    // 移除前导空格
    size_t start = trimmed.find_first_not_of(" \t");
    if (start != std::string::npos) {
      trimmed = trimmed.substr(start);
    }

    // 处理引号包裹的路径
    if (!trimmed.empty() && trimmed[0] == '"') {
      size_t end = trimmed.find('"', 1);
      if (end != std::string::npos) {
        return trimmed.substr(1, end - 1);
      }
    }

    // 提取第一个空格前的部分（可执行文件路径）
    size_t space = trimmed.find(' ');
    if (space != std::string::npos) {
      return trimmed.substr(0, space);
    }

    return trimmed;
  }

  void
  testMenuCmd(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    // 安全限制：只允许局域网访问测试命令功能
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    
    if (ip_type != net::PC) {
      BOOST_LOG(warning) << "TestMenuCmd: Access denied from non-local network: " << address;
      pt::ptree outputTree;
      outputTree.put("status", false);
      outputTree.put("error", "Test command feature is only available from local network");
      
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    pt::ptree inputTree, outputTree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    });

    try {
      pt::read_json(ss, inputTree);
      auto cmd = inputTree.get<std::string>("cmd");
      auto working_dir = inputTree.get<std::string>("working_dir", "");
      auto elevated = inputTree.get<bool>("elevated", false);

      // 安全检查：命令不能为空
      if (cmd.empty()) {
        BOOST_LOG(warning) << "TestMenuCmd: Empty command provided";
        outputTree.put("status", false);
        outputTree.put("error", "Command cannot be empty");
        return;
      }

      // 安全检查：命令长度限制（防止过长的命令）
      if (cmd.length() > 4096) {
        BOOST_LOG(warning) << "TestMenuCmd: Command too long (" << cmd.length() << " characters)";
        outputTree.put("status", false);
        outputTree.put("error", "Command exceeds maximum length");
        return;
      }

      // 提取可执行文件路径并计算SHA256哈希值
      std::string executable_path = extract_executable_path(cmd);
      std::string file_hash;
      
      if (!executable_path.empty()) {
        // 如果是相对路径，尝试解析为绝对路径
        boost::filesystem::path exec_path(executable_path);
        if (!exec_path.is_absolute()) {
          // 在PATH中查找或使用工作目录
          if (!working_dir.empty()) {
            exec_path = boost::filesystem::path(working_dir) / exec_path;
          }
        }
        
        file_hash = calculate_file_hash(exec_path.string());
        
        if (file_hash.empty() && boost::filesystem::exists(exec_path)) {
          BOOST_LOG(warning) << "TestMenuCmd: Failed to calculate hash for executable: " << exec_path;
        }
      }

      // 记录详细信息用于审计（包含文件哈希值）
      BOOST_LOG(info) << "Testing menu command from " << address << ": [" << cmd << "]";
      if (!file_hash.empty()) {
        BOOST_LOG(info) << "Executable SHA256: " << file_hash << " (" << executable_path << ")";
      }
      else if (!executable_path.empty()) {
        BOOST_LOG(warning) << "Could not verify executable: " << executable_path;
      }

      std::error_code ec;
      boost::filesystem::path work_dir;
      
      if (!working_dir.empty()) {
        // 验证工作目录是否存在
        if (!boost::filesystem::exists(working_dir) || !boost::filesystem::is_directory(working_dir)) {
          BOOST_LOG(warning) << "TestMenuCmd: Invalid working directory: " << working_dir;
          outputTree.put("status", false);
          outputTree.put("error", "Invalid working directory");
          return;
        }
        work_dir = boost::filesystem::path(working_dir);
      } else {
        work_dir = boost::filesystem::current_path();
      }

      // 执行命令
      auto child = platf::run_command(elevated, true, cmd, work_dir, proc::proc.get_env(), nullptr, ec, nullptr);
      
      if (ec) {
        BOOST_LOG(warning) << "Failed to run menu command [" << cmd << "]: " << ec.message();
        outputTree.put("status", false);
        outputTree.put("error", "Failed to execute command: " + ec.message());
      }
      else {
        BOOST_LOG(info) << "Successfully executed menu command [" << cmd << "]";
        child.detach();
        outputTree.put("status", true);
        outputTree.put("message", "Command executed successfully");
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "TestMenuCmd error: " << e.what();
      outputTree.put("status", false);
      outputTree.put("error", e.what());
      return;
    }
  }

  void
  start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    server.default_resource["GET"] = close_connection;
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/clients/?$"]["GET"] = getClientsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/qr-pair$"]["POST"] = generateQrPairInfo;
    server.resource["^/api/qr-pair/cancel$"]["POST"] = cancelQrPair;
    server.resource["^/api/qr-pair$"]["GET"] = getQrPairStatus;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/logout$"]["GET"] = handleLogout;
    server.resource["^/api/logout$"]["POST"] = handleLogout;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/restart$"]["GET"] = restart;
    server.resource["^/api/boom$"]["GET"] = boom;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = listClients;
    server.resource["^/api/clients/list$"]["POST"] = saveConfig;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/rename$"]["POST"] = renameClient;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/apps/test-menu-cmd$"]["POST"] = testMenuCmd;
    server.resource["^/api/runtime/sessions$"]["GET"] = getRuntimeSessions;
    server.resource["^/api/runtime/bitrate$"]["GET"] = changeRuntimeBitrate;
    server.resource["^/steam-api/.+$"]["GET"] = proxySteamApi;
    server.resource["^/steam-store/.+$"]["GET"] = proxySteamStore;
    server.resource["^/api/ai/config$"]["GET"] = getAiConfig;
    server.resource["^/api/ai/config$"]["POST"] = saveAiConfigEndpoint;
    server.resource["^/api/ai/chat/completions$"]["POST"] = proxyAiChat;
    server.resource["^/api/ai/chat/completions$"]["OPTIONS"] = handleAiCors;
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-256.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/boxart/.+$"]["GET"] = getBoxArt;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.config.reuse_address = true;
    server.config.address = net::get_bind_address(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](https_server_t *server) {
      try {
        server->start([](unsigned short port) {
          BOOST_LOG(debug) << "Configuration UI available at [https://localhost:"sv << port << "]"sv;
        });
      }
      catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
      catch (std::exception &err) {
        BOOST_LOG(fatal) << "Configuration HTTPS server failed to start: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
