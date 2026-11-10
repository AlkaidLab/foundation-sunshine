/**
 * @file src/confighttp/apps_scan.cpp
 * @brief Host game library discovery endpoint.
 *
 * The endpoint only reads the host and returns candidates. Adding a game stays
 * with the existing application save API, so a scan cannot change configuration
 * as a side effect.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "apps_scan.h"
#include "src/app_scan/scanner.h"
#include "src/confighttp/auth.h"
#include "src/platform/common.h"
#include "src/sparxie/host.h"
#include "src/sparxie/user_session.h"
#ifdef _WIN32
  #include "src/platform/windows/misc.h"
  #include <shlobj.h>
#endif
#include "src/confighttp/common.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/utility.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::apps_scan {
  namespace {
    // A host scan is bounded in time, but it still reads the disk. One request
    // at a time keeps repeated clicks from stacking reads.
    std::atomic<bool> scan_running { false };

    std::optional<std::filesystem::path>
    environment_path(const char *name) {
      const char *value = std::getenv(name);
      if (!value || *value == '\0') {
        return std::nullopt;
      }
      return app_scan::from_utf8(value);
    }

    void
    scan_apps(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      json body;
      if (!read_json_body(response, request, body)) return;
      if (!body.is_object()) {
        write_json_error(response, web::StatusCode::bad_request, "Request body must be a JSON object");
        return;
      }

      app_scan::ScanRequest scan_request;
      if (body.contains("sources")) {
        if (!body["sources"].is_array()) {
          write_json_error(response, web::StatusCode::bad_request, "sources must be an array");
          return;
        }
        for (const json &entry : body["sources"]) {
          if (!entry.is_string()) {
            write_json_error(response, web::StatusCode::bad_request, "sources must contain strings");
            return;
          }
          app_scan::Source source {};
          const std::string name = entry.get<std::string>();
          if (!app_scan::parse_source(name, source)) {
            write_json_error(response, web::StatusCode::bad_request, "Unknown scan source: " + name);
            return;
          }
          if (std::find(scan_request.sources.begin(), scan_request.sources.end(), source) == scan_request.sources.end()) {
            scan_request.sources.push_back(source);
          }
        }
      }
      else {
        scan_request.sources = {
          app_scan::Source::steam,
          app_scan::Source::epic,
          app_scan::Source::gog,
          app_scan::Source::xbox,
          app_scan::Source::ea,
          app_scan::Source::ubisoft,
          app_scan::Source::battlenet,
          app_scan::Source::riot,
          app_scan::Source::hoyoplay,
          app_scan::Source::wegame,
          app_scan::Source::sparxie,
        };
      }

      if (body.contains("directory") && !body["directory"].is_null()) {
        if (!body["directory"].is_string()) {
          write_json_error(response, web::StatusCode::bad_request, "directory must be a string");
          return;
        }
        const std::string directory = body["directory"].get<std::string>();
        if (!directory.empty()) {
          const std::filesystem::path path = app_scan::from_utf8(directory);
          if (const auto error = app_scan::validate_scan_root(path)) {
            write_json_error(response, web::StatusCode::bad_request, *error);
            return;
          }
          scan_request.directory = path;
        }
      }

      if (scan_request.sources.empty() && !scan_request.directory) {
        write_json_error(response, web::StatusCode::bad_request, "No scan source selected");
        return;
      }

      bool expected = false;
      if (!scan_running.compare_exchange_strong(expected, true)) {
        write_json_error(response, web::StatusCode::conflict, "Another scan is in progress");
        return;
      }
      auto running_guard = util::fail_guard([]() { scan_running = false; });

      app_scan::HostContext context;
      context.platform = app_scan::detect_host_platform();
      context.home = environment_path("USERPROFILE");
      if (!context.home) {
        context.home = environment_path("HOME");
      }
      context.program_data = environment_path("ProgramData");
      context.local_app_data = environment_path("LOCALAPPDATA");
      context.roaming_app_data = environment_path("APPDATA");
#ifdef _WIN32
      // The service token is not the signed-in person. Known folders come from
      // that person's token; the scan itself runs while impersonating them.
      if (HANDLE token = platf::retrieve_users_token(false)) {
        const auto folder = [&](REFKNOWNFOLDERID id) -> std::optional<std::filesystem::path> {
          PWSTR wide = nullptr;
          if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, token, &wide)) || !wide) return std::nullopt;
          std::filesystem::path path(wide);
          CoTaskMemFree(wide);
          return path;
        };
        if (auto profile = folder(FOLDERID_Profile)) context.home = profile;
        if (auto local = folder(FOLDERID_LocalAppData)) context.local_app_data = local;
        if (auto roaming = folder(FOLDERID_RoamingAppData)) context.roaming_app_data = roaming;
        CloseHandle(token);
        const auto windows_apps = app_scan::from_utf8("C:\Program Files\WindowsApps");
        std::error_code ec;
        if (std::filesystem::is_directory(windows_apps, ec)) context.package_roots.push_back(windows_apps);
      }
#endif
      std::filesystem::path sparxie_exe;
      if (const char *home = std::getenv("SPARXIE_HOME")) sparxie_exe = sparxie::find_sparxie_exe(app_scan::from_utf8(home));
      if (sparxie_exe.empty()) sparxie_exe = sparxie::find_sparxie_exe(platf::appdata() / "components" / "sparxie");
      if (!sparxie_exe.empty() && std::filesystem::is_regular_file(sparxie_exe)) {
        context.sparxie_exe = sparxie_exe;
        std::filesystem::path data = sparxie_exe.parent_path();
        if (data.filename().string().rfind("app-", 0) == 0) data = data.parent_path();
        if (const char *override_dir = std::getenv("SPARXIE_DATA_DIR")) data = app_scan::from_utf8(override_dir);
        context.sparxie_data = data;
      }
      context.covers_dir = platf::appdata() / "covers";
      const auto registry = app_scan::make_native_registry();
      context.registry = registry.get();

      const auto start = std::chrono::steady_clock::now();
      app_scan::ScanResult result;
      const auto run_scan = [&] { result = app_scan::scan(scan_request, context); };
      const auto runner = sparxie::make_user_runner();
      const bool as_user = runner ? runner(run_scan) : (run_scan(), true);
      if (!as_user) {
        run_scan();
        result.warnings.emplace_back("user.absent");
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

      std::string masked_directory = "-"s;
      if (scan_request.directory) {
        masked_directory = app_scan::mask_user_path(app_scan::to_utf8(*scan_request.directory), context.home);
      }
      BOOST_LOG(info) << "App scan: sources="sv << scan_request.sources.size()
                      << ", directory="sv << masked_directory
                      << ", games="sv << result.games.size()
                      << ", warnings="sv << result.warnings.size()
                      << ", elapsed_ms="sv << elapsed.count();

      json response_body;
      response_body["games"] = json::array();
      for (const app_scan::Game &game : result.games) {
        json item;
        item["platform"] = game.platform;
        item["source"] = game.platform;
        item["name"] = game.name;
        item["id"] = game.id;
        item["install_dir"] = game.install_dir;
        item["launch"] = game.launch;
        item["sources"] = game.sources.empty() ? json::array({ game.platform }) : json::array();
        if (item["sources"].empty()) {
          for (const std::string &source : game.sources) item["sources"].push_back(source);
        }
        if (!game.exe.empty()) {
          item["exe"] = game.exe;
        }
        if (!game.cover_hint.empty()) {
          item["cover_hint"] = game.cover_hint;
        }
        if (!game.region.empty()) item["region"] = game.region;
        if (!game.profile.empty()) item["profile"] = game.profile;
        if (!game.launches.empty()) {
          item["launches"] = json::array();
          for (const app_scan::LaunchChoice &choice : game.launches) {
            item["launches"].push_back({ { "id", choice.id }, { "launch", choice.launch } });
          }
        }
        response_body["games"].push_back(std::move(item));
      }
      response_body["warnings"] = result.warnings;

      write_json(response, web::StatusCode::ok, response_body);
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/apps/scan$"]["POST"] = scan_apps;
  }
}  // namespace confighttp::apps_scan
