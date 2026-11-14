#include "src/confighttp/sparxie.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/sparxie/install.h"
#include "src/sparxie/platform.h"
#include "src/sparxie/runtime.h"
#include <fstream>
#include <iterator>
using json = nlohmann::json;
// This namespace is also named sparxie, so the component namespace is qualified from the global scope.
namespace confighttp::sparxie {
  namespace {
    namespace fs = ::sparxie::fs;
    // The route being answered on this thread, for the log line of a failure
    thread_local std::string current_route;
    void
    finish(resp_https_t response, ::sparxie::ApiResult result) {
      json body = std::move(result.body);
      if (!result.status.ok) {
        BOOST_LOG(warning) << "Sparxie API " << current_route << " failed: " << result.status.error_code << ": " << result.status.error;
        body["ok"] = false;
        if (!body.contains("error")) body["error"] = result.status.error;
        if (!body.contains("error_code")) body["error_code"] = result.status.error_code;
        write_json(response, static_cast<web::StatusCode>(result.status.http), body);
        return;
      }
      write_json(response, web::StatusCode::ok, body);
    }
    void
    finish(resp_https_t response, ::sparxie::InstallOutcome result) {
      ::sparxie::ApiResult wrapped;
      wrapped.status = std::move(result.status);
      wrapped.body = std::move(result.body);
      finish(response, std::move(wrapped));
    }
    // The host keeps the application list in memory: a request that rewrote apps.json makes it reload once, the way the plain application routes do, so clients see the change without a restart
    void
    reload_apps(const ::sparxie::HostEnv &env) {
      if (env.apps_writes > 0) proc::refresh(config::stream.file_apps);
    }
    bool
    ready(resp_https_t response, req_https_t request, bool with_body) {
      if (with_body && !check_content_type(response, request, "application/json")) return false;
      return authenticate(response, request);
    }
    void
    get_status(resp_https_t response, req_https_t request) {
      if (!ready(response, request, false)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::component_status(env));
    }
    void
    get_games(resp_https_t response, req_https_t request) {
      if (!ready(response, request, false)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::list_games(env));
    }
    void
    get_profiles(resp_https_t response, req_https_t request) {
      if (!ready(response, request, false)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::list_profiles(env));
    }
    void
    post_identify(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::identify_path(env, body.value("path", std::string()), body.value("game", std::string()), body.value("region", std::string())));
    }
    void
    post_scan(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::scan_folder(env, body));
    }
    void
    post_browse(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::browse_path(env, body));
    }
    void
    post_open(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::open_component(env));
    }
    // The host is a service: a dialog opened here would land in the service's own profile, not in the
    // person's session. The Panel opens its own picker and the Web UI uses /api/sparxie/browse.
    void
    post_pick(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json err;
      err["ok"] = false;
      err["error"] = "The host does not open file dialogs";
      err["error_code"] = "picker_unavailable";
      write_json(response, web::StatusCode::conflict, err);
    }
    void
    post_create(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      auto result = ::sparxie::create_app(env, body);
      reload_apps(env);
      finish(response, std::move(result));
    }
    void
    put_app(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      auto result = ::sparxie::update_app(env, std::stoi(request->path_match[1]), body);
      reload_apps(env);
      finish(response, std::move(result));
    }
    void
    post_import(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      auto result = ::sparxie::import_apps(env, body);
      reload_apps(env);
      finish(response, std::move(result));
    }
    void
    post_warning(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      if (!body.value("acknowledged", false)) {
        json err;
        err["ok"] = false;
        err["error"] = "acknowledged must be true";
        err["error_code"] = "invalid_input";
        write_json(response, web::StatusCode::bad_request, err);
        return;
      }
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::acknowledge_warning(env));
    }
    void
    get_settings(resp_https_t response, req_https_t request) {
      if (!ready(response, request, false)) return;
      const auto query = request->parse_query_string();
      const auto found = query.find("install");
      const auto exe = query.find("exe");
      const auto region = query.find("region");
      auto env = ::sparxie::make_host_env();
      finish(response, ::sparxie::game_settings_for(env, found == query.end() ? std::string() : found->second, exe == query.end() ? std::string() : exe->second, region == query.end() ? std::string() : region->second));
    }
    void
    post_settings(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      auto result = ::sparxie::write_game_settings_for(env, body);
      reload_apps(env);
      finish(response, std::move(result));
    }
    void
    put_profile(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      ::sparxie::ProfileDraft draft;
      draft.display_name = body.value("name", std::string());
      draft.executable = body.value("exe", std::string());
      draft.game = body.value("game", std::string("genshin"));
      if (body.contains("options") && body["options"].is_object()) draft.options = body["options"];
      ::sparxie::StoreHooks hooks;
      hooks.running = ::sparxie::host_process_running({ "Sparxie.exe", "Sparxie.Desktop.exe", "Sparxie.Launcher.exe" });
      if (hooks.running) {
        json err;
        err["ok"] = false;
        err["error"] = "Sparxie is running, close it before writing";
        err["error_code"] = "sparxie_running";
        write_json(response, web::StatusCode::conflict, err);
        return;
      }
      const auto stored = ::sparxie::update_profile(env.data_dir, std::string(request->path_match[1]), draft, hooks);
      if (!stored.status.ok) {
        json err;
        err["ok"] = false;
        err["error"] = stored.status.error;
        err["error_code"] = stored.status.error_code;
        err["write_back"]["state"] = stored.state;
        write_json(response, static_cast<web::StatusCode>(stored.status.http), err);
        return;
      }
      json ok;
      ok["ok"] = true;
      ok["profile"] = stored.profile;
      ok["write_back"]["state"] = stored.state;
      ok["write_back"]["backup"] = stored.backup;
      ok["write_back"]["dataDirProvisional"] = true;
      write_json(response, web::StatusCode::ok, ok);
    }
    void
    put_cover(resp_https_t response, req_https_t request) {
      if (!ready(response, request, true)) return;
      json body;
      if (!read_json_body(response, request, body)) return;
      auto env = ::sparxie::make_host_env();
      auto result = ::sparxie::set_cover(env, std::stoi(request->path_match[1]), body.value("cover", std::string()));
      reload_apps(env);
      finish(response, std::move(result));
    }
    void
    post_refresh(resp_https_t response, req_https_t request) {
      if (!ready(response, request, false)) return;
      auto env = ::sparxie::make_host_env();
      auto result = ::sparxie::refresh_official(env, ::sparxie::host_http_get, [](std::string_view bytes) { return ::sparxie::host_decode_image(bytes); }, ::sparxie::host_http_post_json);
      reload_apps(env);
      finish(response, std::move(result));
    }
    // The official art of an install. `badge` asks for the variant the host drew the Sparxie mark on (the poster a touch application hands to the clients): covers/sparxie-<install>-<version>-badge<rev>.png
    // beside the plain cover, posters/<install>/hero-badge.jpg beside hero.jpg. The icon and the banner have no badged variant.
    fs::path
    art_file(const ::sparxie::HostEnv &env, const std::string &slot, const std::string &id, const std::string &version, bool badge) {
      if (slot == "icon") return env.host_data / "posters" / id / "icon.png";
      if (slot == "banner") return env.host_data / "posters" / id / "banner.png";
      if (slot == "hero") return env.host_data / "posters" / id / (badge ? "hero-badge.jpg" : "hero.jpg");
      std::error_code ec;
      fs::path plain;
      if (!version.empty()) {
        plain = env.covers_dir / ("sparxie-" + id + "-" + version + ".png");
      }
      else {
        fs::file_time_type stamp;
        for (const auto &entry : fs::directory_iterator(env.covers_dir, ec)) {
          const std::string name = entry.path().filename().string();
          if (name.rfind("sparxie-" + id + "-", 0) != 0) continue;
          // the badged copy of a cover is the poster of a touch application, not the official art the pages show
          if (entry.path().stem().string().find("-badge") != std::string::npos) continue;
          if (plain.empty() || entry.last_write_time() > stamp) {
            plain = entry.path();
            stamp = entry.last_write_time();
          }
        }
      }
      if (!badge || plain.empty()) return plain;
      // the newest badged sibling of that cover (a later badge revision wins)
      fs::path badged;
      fs::file_time_type stamp;
      const std::string prefix = plain.stem().string() + "-badge";
      for (const auto &entry : fs::directory_iterator(env.covers_dir, ec)) {
        if (entry.path().extension() != ".png" || entry.path().stem().string().rfind(prefix, 0) != 0) continue;
        if (badged.empty() || entry.last_write_time() > stamp) {
          badged = entry.path();
          stamp = entry.last_write_time();
        }
      }
      return badged;
    }
    void
    get_art(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      const auto query = request->parse_query_string();
      const auto version = query.find("v");
      const auto badge = query.find("badge");
      auto env = ::sparxie::make_host_env();
      const fs::path file = art_file(env, std::string(request->path_match[1]), std::string(request->path_match[2]), version == query.end() ? std::string() : version->second,
        badge != query.end() && badge->second == "1");
      std::ifstream in(file, std::ios::binary);
      if (!in) {
        json err;
        err["ok"] = false;
        err["error"] = "No cached image";
        err["error_code"] = "art_unavailable";
        write_json(response, web::StatusCode::not_found, err);
        return;
      }
      std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", file.extension() == ".jpg" ? "image/jpeg" : "image/png");
      headers.emplace("Cache-Control", "public, max-age=3600");
      response->write(bytes, headers);
    }
  }  // namespace

  void
  get_manifest(resp_https_t response, req_https_t request) {
    if (!ready(response, request, false)) return;
    json body = ::sparxie::manifest_json();
    body["ok"] = true;
    write_json(response, web::StatusCode::ok, body);
  }
  void
  post_install(resp_https_t response, req_https_t request) {
    if (!ready(response, request, true)) return;
    json body;
    if (!read_json_body(response, request, body)) return;
    auto env = ::sparxie::make_host_env();
    ::sparxie::InstallRequest req;
    req.zip = body.value("path", std::string());
    req.dest = env.component_dir;
    req.confirm_unverified = body.value("confirm_unverified", false);
    req.acknowledged = body.value("acknowledged", false);
    req.running = ::sparxie::host_process_running({ "Sparxie.exe", "Sparxie.Desktop.exe", "Sparxie.Launcher.exe" });
    if (body.value("wait", false)) {
      finish(response, ::sparxie::install_from_file(env, std::move(req)));
      return;
    }
    const std::string id = ::sparxie::start_install_job(std::move(env), std::move(req));
    if (id.empty()) {
      json err = { { "ok", false }, { "error", "Another install is still running" }, { "error_code", "busy" } };
      write_json(response, web::StatusCode::conflict, err);
      return;
    }
    json ok = { { "ok", true }, { "job", id }, { "done", false }, { "stage", "dependencies" }, { "percent", 0 } };
    write_json(response, web::StatusCode::ok, ok);
  }
  void
  get_install_job(resp_https_t response, req_https_t request) {
    if (!ready(response, request, false)) return;
    const json body = ::sparxie::install_job(std::string(request->path_match[1]));
    const int http = body.value("error_code", std::string()) == "not_found" && !body.value("done", false) ? 404 : 200;
    write_json(response, static_cast<web::StatusCode>(http), body);
  }
  void
  post_uninstall(resp_https_t response, req_https_t request) {
    if (!ready(response, request, true)) return;
    json body;
    if (!read_json_body(response, request, body)) return;
    auto env = ::sparxie::make_host_env();
    ::sparxie::UninstallRequest req;
    req.dest = env.component_dir;
    req.data_dir = env.data_dir;
    req.running = ::sparxie::host_process_running({ "Sparxie.exe", "Sparxie.Desktop.exe", "Sparxie.Launcher.exe" });
    if (body.contains("delete_config") && body["delete_config"].is_boolean()) {
      req.delete_config_set = true;
      req.delete_config = body["delete_config"].get<bool>();
    }
    finish(response, ::sparxie::uninstall_component(req));
  }
  // Every Sparxie route notes itself for the log and turns an exception into an answer the page can show: the host writes a warning line (route, code, message) for every failure
  template<class Handler>
  auto
  guarded(Handler handler) {
    return [handler](resp_https_t response, req_https_t request) {
      current_route = request->method + " " + request->path;
      try {
        handler(response, request);
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Sparxie API " << current_route << " failed: internal_error: " << e.what();
        json err = { { "ok", false }, { "error", e.what() }, { "error_code", "internal_error" } };
        write_json(response, web::StatusCode::internal_server_error, err);
      }
    };
  }
  void
  register_routes(https_server_t &server) {
    server.resource["^/api/sparxie/status$"]["GET"] = guarded(get_status);
    server.resource["^/api/sparxie/manifest$"]["GET"] = guarded(get_manifest);
    server.resource["^/api/sparxie/install$"]["POST"] = guarded(post_install);
    server.resource["^/api/sparxie/install/([0-9a-f]+)$"]["GET"] = guarded(get_install_job);
    server.resource["^/api/sparxie/uninstall$"]["POST"] = guarded(post_uninstall);
    server.resource["^/api/sparxie/games$"]["GET"] = guarded(get_games);
    server.resource["^/api/sparxie/profiles$"]["GET"] = guarded(get_profiles);
    server.resource["^/api/sparxie/identify$"]["POST"] = guarded(post_identify);
    server.resource["^/api/sparxie/scan$"]["POST"] = guarded(post_scan);
    server.resource["^/api/sparxie/browse$"]["POST"] = guarded(post_browse);
    server.resource["^/api/sparxie/open$"]["POST"] = guarded(post_open);
    server.resource["^/api/sparxie/pick$"]["POST"] = guarded(post_pick);
    server.resource["^/api/sparxie/apps$"]["POST"] = guarded(post_create);
    server.resource["^/api/sparxie/apps/([0-9]+)$"]["PUT"] = guarded(put_app);
    server.resource["^/api/sparxie/import$"]["POST"] = guarded(post_import);
    server.resource["^/api/sparxie/warning$"]["POST"] = guarded(post_warning);
    server.resource["^/api/sparxie/game-settings$"]["GET"] = guarded(get_settings);
    server.resource["^/api/sparxie/game-settings$"]["POST"] = guarded(post_settings);
    server.resource["^/api/sparxie/profiles/([^/]+)$"]["PUT"] = guarded(put_profile);
    server.resource["^/api/sparxie/apps/([0-9]+)/cover$"]["PUT"] = guarded(put_cover);
    server.resource["^/api/sparxie/art/refresh$"]["POST"] = guarded(post_refresh);
    server.resource["^/api/sparxie/art/(icon|card|banner|hero)/([^/]+)$"]["GET"] = guarded(get_art);
    // applications made after the last poster refresh have no poster yet: give them the one on disk now, not at the next refresh
    {
      auto env = ::sparxie::make_host_env();
      if (::sparxie::attach_official_covers(env) > 0) reload_apps(env);
      // profiles that failed saves of an earlier build left in Sparxie's config go (its config is backed up first)
      if (const int removed = ::sparxie::remove_orphan_profiles(env); removed > 0) BOOST_LOG(info) << "Sparxie: removed " << removed << " profile(s) that no application uses";
    }
    ::sparxie::note_host_started();
  }
}  // namespace confighttp::sparxie
