/**
 * @file src/confighttp/assets.cpp
 * @brief Web UI assets handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "assets.h"
#include "asset_codec.h"
#include "asset_encoding.h"
#include "asset_etag.h"
#include "asset_paths.h"
#include "src/confighttp.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "web_install.h"
#include <array>
#include <boost/filesystem.hpp>
#include <filesystem>
#include <fstream>
#include <map>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::assets {
  namespace fs = std::filesystem;

  using namespace confighttp;

  namespace {
    // A validator and its 200 body must describe the same opened snapshot.
    // Large assets continue to use file streaming and do not get a validator.
    constexpr std::size_t maximum_snapshot_size = 8 * 1024 * 1024;

    // A decoded identity representation is produced in memory; the same bound
    // keeps that path from unbounded output on a damaged file.
    constexpr std::size_t maximum_decoded_size = 8 * 1024 * 1024;

    std::optional<std::string>
    read_snapshot(const fs::path &path, std::size_t limit = maximum_snapshot_size) {
      std::ifstream input(path, std::ios::binary);
      if (!input) return std::nullopt;
      std::array<char, 16384> buffer;
      std::string result;
      while (input) {
        input.read(buffer.data(), buffer.size());
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > limit - result.size()) return std::nullopt;
        result.append(buffer.data(), count);
      }
      if (!input.eof()) return std::nullopt;
      return result;
    }

    bool
    respond_if_cached(resp_https_t response, req_https_t request, web::CaseInsensitiveMultimap &headers,
      const std::string &content) {
      const std::string etag = "\"" + asset_digest(content) + "\"";
      headers.emplace("ETag", etag);
      if (!matches_asset_etag(request->header, etag)) return false;
      response->write(web::StatusCode::not_modified, headers);
      return true;
    }

    // mime types map
    const std::map<std::string, std::string> mime_types = {
      { "css", "text/css" },
      { "gif", "image/gif" },
      { "htm", "text/html" },
      { "html", "text/html" },
      { "ico", "image/x-icon" },
      { "jpeg", "image/jpeg" },
      { "jpg", "image/jpeg" },
      { "js", "application/javascript" },
      { "json", "application/json" },
      { "png", "image/png" },
      { "svg", "image/svg+xml" },
      { "ttf", "font/ttf" },
      { "txt", "text/plain" },
      { "webp", "image/webp" },
      { "woff2", "font/woff2" },
      { "xml", "text/xml" },
    };
    void
    getHtmlPage(resp_https_t response, req_https_t request, const std::string &pageName, bool requireAuth = true) {
      if (requireAuth && !authenticate(response, request)) return;

      print_req(request);

      std::error_code page_error;
      if (!fs::is_regular_file(file_handler::path_from_utf8(std::string(WEB_DIR) + pageName), page_error)) {
        // An empty 200 left the Control Panel's embedded pages blank; name the problem instead.
        BOOST_LOG(error) << "Web UI page is missing from the installation: "sv << pageName;
        web::CaseInsensitiveMultimap missing_headers;
        missing_headers.emplace("Content-Type", "text/html; charset=utf-8");
        missing_headers.emplace("Cache-Control", "no-store");
        response->write(web::StatusCode::service_unavailable, web_install::incomplete_page(), missing_headers);
        return;
      }
      std::string content = file_handler::read_file((std::string(WEB_DIR) + pageName).c_str());
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/html; charset=utf-8");
      headers.emplace("X-Content-Type-Options", "nosniff");
      headers.emplace("Cache-Control", "private, no-cache");
      if (pageName == "apps.html") {
        headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
      }
      if (respond_if_cached(response, request, headers, content)) return;
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
    getDownloadsPage(resp_https_t response, req_https_t request) {
      getHtmlPage(response, request, "downloads.html");
    }

    void
    getConfigPage(resp_https_t response, req_https_t request) {
      getHtmlPage(response, request, "config.html");
    }

    void
    getDualSensePage(resp_https_t response, req_https_t request) {
      getHtmlPage(response, request, "dualsense.html");
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
          return;  // authenticate已经发送了401响应
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
    getStaticResource(resp_https_t response, req_https_t request, const std::string &path, const std::string &contentType) {
      // print_req(request);

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", contentType);
      headers.emplace("Cache-Control", "private, no-cache");
      if (!response->write_file(web::StatusCode::ok, file_handler::path_from_utf8(path), headers)) {
        BOOST_LOG(error) << "Failed to open Web UI resource: " << path;
        response->write(web::StatusCode::internal_server_error);
      }
    }

    void
    getFaviconImage(resp_https_t response, req_https_t request) {
      getStaticResource(response, request, WEB_DIR "images/sunshine.ico", "image/x-icon");
    }

    void
    getSunshineLogoImage(resp_https_t response, req_https_t request) {
      getStaticResource(response, request, WEB_DIR "images/logo-sunshine-256.png", "image/png");
    }

    void
    getAlkaidLabLogoImage(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      getStaticResource(response, request, WEB_DIR "images/logo-alkaidlab.png", "image/png");
    }

    void
    getNatPierceLogoImage(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      getStaticResource(response, request, WEB_DIR "images/logo-natpierce.png", "image/png");
    }

    void
    getBoxArt(resp_https_t response, req_https_t request) {
      print_req(request);

      // Extract image filename from request path
      std::string path = request->path;
      if (path.find("/boxart/") == 0) {
        path = path.substr(8);  // Remove "/boxart/" prefix
      }
      path = web::Percent::decode(path);

      BOOST_LOG(debug) << "getBoxArt: Requested file: " << path;

      const auto assetsRoot = file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR);
      const auto coversRoot = platf::appdata() / "covers";
      const auto resolved = resolve_boxart_path(assetsRoot, coversRoot, file_handler::path_from_utf8(path));
      fs::path finalPath;
      if (resolved) {
        finalPath = *resolved;
      }
      else {
        // Preserve the placeholder for invalid and missing image names.
        const auto fallback = resolve_boxart_path(assetsRoot, assetsRoot, "box.png");
        if (!fallback) {
          response->write(web::StatusCode::not_found, "Image not found");
          return;
        }
        finalPath = *fallback;
      }

      const auto imagePath = file_handler::path_to_utf8(finalPath);

      // Get file size
      std::error_code ec;
      auto fileSize = fs::file_size(finalPath, ec);
      if (ec) {
        BOOST_LOG(warning) << "Failed to get file size for: " << imagePath;
        response->write(web::StatusCode::internal_server_error, "Failed to read image file");
        return;
      }

      // Determine Content-Type from file extension
      std::string ext = finalPath.extension().string();
      if (!ext.empty() && ext[0] == '.') {
        ext = ext.substr(1);
      }

      auto mimeType = mime_types.find(ext);
      std::string contentType = "image/png";  // Default type

      if (mimeType != mime_types.end()) {
        contentType = mimeType->second;
      }

      BOOST_LOG(debug) << "Serving boxart: " << imagePath << " (Content-Type: " << contentType << ", Size: " << fileSize << " bytes)";

      // Return image resource
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", contentType);
      headers.emplace("Content-Length", std::to_string(fileSize));
      headers.emplace("Cache-Control", "max-age=3600");  // Add caching to reduce load

      if (!response->write_file(web::StatusCode::ok, finalPath, headers)) {
        BOOST_LOG(warning) << "Failed to open image file: " << imagePath;
        response->write(web::StatusCode::internal_server_error, "Failed to open image file");
      }
    }

    /**
     * 处理静态资源文件
     */
    void
    getNodeModules(resp_https_t response, req_https_t request) {
      // print_req(request);
      fs::path webDirPath = file_handler::path_from_utf8(WEB_DIR);
      fs::path nodeModulesPath(webDirPath / "assets");

      // .relative_path is needed to shed any leading slash that might exist in the request path
      const auto decodedRequestPath = web::Percent::decode(request->path);
      std::error_code path_error;
      nodeModulesPath = fs::weakly_canonical(nodeModulesPath, path_error);
      if (path_error) {
        response->write(web::StatusCode::bad_request, "Bad Request");
        return;
      }
      auto filePath = fs::weakly_canonical(webDirPath / file_handler::path_from_utf8(decodedRequestPath).relative_path(), path_error);

      // Don't do anything if file does not exist or is outside the assets directory
      if (path_error || !is_child_path(filePath, nodeModulesPath)) {
        BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
        response->write(web::StatusCode::bad_request, "Bad Request");
        return;
      }

      // The URL always names the plain file. What is installed beside that
      // name may be the plain file itself (oldest layout), the plain file
      // with gzip sidecars (the layout that shipped every text twice), or a
      // single precompressed Brotli copy (current: one file per asset).
      std::error_code representation_error;
      const auto representation = [&](const std::string &suffix) -> std::optional<fs::path> {
        auto candidate = filePath;
        candidate += suffix;
        candidate = fs::weakly_canonical(candidate, representation_error);
        if (representation_error || !is_child_path(candidate, nodeModulesPath)) return std::nullopt;
        return candidate;
      };
      std::error_code file_error;
      const bool originalPresent = fs::is_regular_file(filePath, file_error);

      auto relPath = fs::relative(filePath, webDirPath);
      auto extension = relPath.extension().string();
      auto mimeType = mime_types.find(extension.empty() ? "" : extension.substr(1));
      if (mimeType == mime_types.end()) {
        // Do not serve unmapped extensions (including internal .br/.gz sidecars);
        // respond 404 immediately instead of leaving the connection open until timeout_content
        // (RFC 9110 sections 15.5.4 and 15.5.5; authority: fs-rfc9110-http-20261110).
        web::CaseInsensitiveMultimap missing_headers;
        missing_headers.emplace("Cache-Control", "no-store");
        response->write(web::StatusCode::not_found, "", missing_headers);
        return;
      }

      const bool compressible = extension == ".js" || extension == ".css" || extension == ".json";
      const auto brotliPath = compressible ? representation(".br") : std::optional<fs::path> {};
      const auto gzipPath = compressible ? representation(".gz") : std::optional<fs::path> {};
      const bool brotliPresent = brotliPath && fs::is_regular_file(*brotliPath, file_error);
      const bool gzipPresent = gzipPath && fs::is_regular_file(*gzipPath, file_error);
      if (!originalPresent && !brotliPresent && !gzipPresent) {
        web::CaseInsensitiveMultimap missing_headers;
        missing_headers.emplace("Cache-Control", "no-store");
        response->write(web::StatusCode::not_found, "", missing_headers);
        return;
      }

      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", mimeType->second);
      headers.emplace("X-Content-Type-Options", "nosniff");
      const auto cachePolicy = asset_cache_control(relPath);
      headers.emplace("Cache-Control", std::string(cachePolicy));
      const bool fixedUrl = cachePolicy == "private, no-cache";

      // Legacy layout only, and only until the single Brotli copy of a newer
      // install appears: the gzip sidecar may be used while it still matches
      // the plain file installed next to it, and never beside a newer .br,
      // which would serve stale bytes for a current asset. A sidecar and
      // source may be installed at different times, so never serve an old
      // encoding for a newer fixed URL, even during upgrades.
      bool gzipUsable = gzipPresent && originalPresent && !brotliPresent;
      std::optional<std::string> gzipSnapshot;
      if (gzipUsable && fixedUrl) {
        auto digestPath = *gzipPath;
        digestPath += ".sha256";
        std::error_code digest_error;
        digestPath = fs::weakly_canonical(digestPath, digest_error);
        const auto marker = !digest_error && is_child_path(digestPath, nodeModulesPath) ? read_snapshot(digestPath, 256) : std::nullopt;
        auto sourceSnapshot = read_snapshot(filePath);
        gzipSnapshot = read_snapshot(*gzipPath);
        gzipUsable = sourceSnapshot && gzipSnapshot && marker &&
                     *marker == asset_sidecar_marker(*sourceSnapshot, *gzipSnapshot);
      }
      // Current layout: the Brotli file is the only copy of this asset, so
      // there is no second snapshot to cross-check.
      const bool brotliUsable = brotliPresent;

      headers.emplace("Vary", "Accept-Encoding");
      asset_encoding_offers offers;
      offers.brotli = brotliUsable;
      offers.gzip = gzipUsable;
      offers.identity = brotliUsable || originalPresent;
      const auto encoding = select_asset_encoding(request->header, offers);
      if (encoding == asset_encoding::unacceptable) {
        headers.erase("Cache-Control");
        headers.emplace("Cache-Control", "no-store");
        response->write(web::StatusCode::not_acceptable, "", headers);
        return;
      }

      if (encoding == asset_encoding::brotli) {
        // RFC 7932 section 9 registers the HTTP content coding name "br".
        headers.emplace("Content-Encoding", "br");
        if (fixedUrl) {
          auto snapshot = read_snapshot(*brotliPath);
          if (snapshot) {
            if (respond_if_cached(response, request, headers, *snapshot)) return;
            response->write(web::StatusCode::ok, *snapshot, headers);
            return;
          }
        }
        if (!response->write_file(web::StatusCode::ok, *brotliPath, headers)) {
          BOOST_LOG(error) << "Failed to open Web UI asset: " << file_handler::path_to_utf8(*brotliPath);
          response->write(web::StatusCode::internal_server_error);
        }
        return;
      }

      if (encoding == asset_encoding::gzip) {
        headers.emplace("Content-Encoding", "gzip");
        if (fixedUrl && gzipSnapshot) {
          if (respond_if_cached(response, request, headers, *gzipSnapshot)) return;
          response->write(web::StatusCode::ok, *gzipSnapshot, headers);
          return;
        }
        if (!response->write_file(web::StatusCode::ok, *gzipPath, headers)) {
          BOOST_LOG(error) << "Failed to open Web UI asset: " << file_handler::path_to_utf8(*gzipPath);
          response->write(web::StatusCode::internal_server_error);
        }
        return;
      }

      if (!brotliUsable) {
        // The plain file: oldest layout, legacy sidecar layout, or an asset
        // type that is never precompressed.
        auto snapshot = fixedUrl ? read_snapshot(filePath) : std::nullopt;
        if (fixedUrl && snapshot) {
          if (respond_if_cached(response, request, headers, *snapshot)) return;
          response->write(web::StatusCode::ok, *snapshot, headers);
          return;
        }
        if (!response->write_file(web::StatusCode::ok, filePath, headers)) {
          BOOST_LOG(error) << "Failed to open Web UI asset: " << file_handler::path_to_utf8(filePath);
          response->write(web::StatusCode::internal_server_error);
        }
        return;
      }

      // Identity for a client that did not accept the shipped encoding: the
      // host decodes the one copy it has instead of shipping a second one.
      const auto compressed = read_snapshot(*brotliPath);
      const auto decoded = compressed ? brotli_decode(*compressed, maximum_decoded_size) : std::nullopt;
      if (!decoded) {
        BOOST_LOG(error) << "Failed to decode precompressed Web UI asset: " << file_handler::path_to_utf8(*brotliPath);
        response->write(web::StatusCode::internal_server_error);
        return;
      }
      if (fixedUrl) {
        if (respond_if_cached(response, request, headers, *decoded)) return;
      }
      response->write(web::StatusCode::ok, *decoded, headers);
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/downloads/?$"]["GET"] = getDownloadsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/dualsense/?$"]["GET"] = getDualSensePage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-256.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/images/logo-alkaidlab.png$"]["GET"] = getAlkaidLabLogoImage;
    server.resource["^/images/logo-natpierce.png$"]["GET"] = getNatPierceLogoImage;
    server.resource["^/boxart/.+$"]["GET"] = getBoxArt;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
  }

}  // namespace confighttp::assets
