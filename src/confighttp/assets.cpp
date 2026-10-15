/**
 * @file src/confighttp/assets.cpp
 * @brief Web UI assets handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "assets.h"
#include "asset_paths.h"
#include "src/confighttp.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
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

      std::string content = file_handler::read_file((std::string(WEB_DIR) + pageName).c_str());
      web::CaseInsensitiveMultimap headers;
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
    getDownloadsPage(resp_https_t response, req_https_t request) {
      getHtmlPage(response, request, "downloads.html");
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
      }
      else if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        response->write(web::StatusCode::not_found);
      }
      else {
        auto relPath = fs::relative(filePath, webDirPath);
        // get the mime type from the file extension mime_types map
        // remove the leading period from the extension
        auto mimeType = mime_types.find(relPath.extension().string().empty() ? "" : relPath.extension().string().substr(1));
        // check if the extension is in the map at the x position
        if (mimeType != mime_types.end()) {
          // if it is, set the content type to the mime type
          web::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", mimeType->second);
          if (!response->write_file(web::StatusCode::ok, filePath, headers)) {
            BOOST_LOG(error) << "Failed to open Web UI asset: " << file_handler::path_to_utf8(filePath);
            response->write(web::StatusCode::internal_server_error);
          }
        }
        // do not return any file if the type is not in the map
      }
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/downloads/?$"]["GET"] = getDownloadsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
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
