#include "src/sparxie/runtime.h"
#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/sparxie/platform.h"
#include "src/sparxie/user_session.h"
#include "src/sparxie/vendors.h"
#include "src/sparxie/zzz_file.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <objbase.h>
  #include <shlobj.h>
  #include <windows.h>
#endif
namespace sparxie {
  namespace {
    // The display this host streams: the configured output when it is active, else the primary, else any active device.
    std::optional<display_device::device_display_mode_map_t::value_type>
    streamed_display_mode() {
      try {
        const auto devices = display_device::enum_available_devices();
        std::string chosen;
        const std::string &configured = config::video.output_name;
        if (!configured.empty()) {
          for (const auto &[id, info] : devices)
            if (id == configured || info.display_name == configured) chosen = id;
        }
        if (chosen.empty())
          for (const auto &[id, info] : devices)
            if (info.device_state == display_device::device_state_e::primary) chosen = id;
        if (chosen.empty())
          for (const auto &[id, info] : devices)
            if (info.device_state == display_device::device_state_e::active) chosen = id;
        if (chosen.empty()) return std::nullopt;
        const auto modes = display_device::get_current_display_modes({ chosen });
        const auto mode = modes.find(chosen);
        if (mode == modes.end()) return std::nullopt;
        return *mode;
      }
      catch (const std::exception &) {
        return std::nullopt;
      }
    }
    // The refresh rate of the display this host streams. The Sparxie tier fps "display" resolves to this number at launch.
    int
    streamed_display_refresh() {
      const auto mode = streamed_display_mode();
      if (!mode || mode->second.refresh_rate.denominator == 0) return 0;
      const auto hz = (mode->second.refresh_rate.numerator + mode->second.refresh_rate.denominator / 2) / mode->second.refresh_rate.denominator;
      return static_cast<int>(std::clamp(hz, 10u, 1000u));
    }
    // The size of that display, the one a game lists its resolutions from; {0, 0} when it cannot be read.
    std::pair<int, int>
    streamed_display_size() {
      const auto mode = streamed_display_mode();
      if (!mode) return { 0, 0 };
      return { static_cast<int>(mode->second.resolution.width), static_cast<int>(mode->second.resolution.height) };
    }
    fs::path
    executable_dir() {
#if defined(_WIN32)
      wchar_t buffer[MAX_PATH];
      const DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
      if (size == 0) return {};
      return fs::path(buffer).parent_path();
#else
      std::error_code ec;
      const fs::path self = fs::read_symlink("/proc/self/exe", ec);
      return ec ? fs::path {} : self.parent_path();
#endif
    }
    fs::path
    local_sparxie_dir() {
#if defined(_WIN32)
      PWSTR path = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) || !path) return {};
      const fs::path dir = fs::path(path) / "Sparxie";
      CoTaskMemFree(path);
      return dir;
#else
      const char *home = std::getenv("HOME");
      return fs::path(home ? home : "") / ".local" / "share" / "sparxie";
#endif
    }
    ArtSchedule
    load_schedule(const fs::path &path) {
      ArtSchedule schedule;
      bool jitter_set = false;
      std::ifstream in(path);
      if (in) {
        try {
          json body;
          in >> body;
          const auto fetch = body.value("last_fetch", static_cast<long long>(0));
          const auto check = body.value("last_check", static_cast<long long>(0));
          if (fetch > 0) schedule.last_fetch = std::chrono::system_clock::time_point { std::chrono::seconds(fetch) };
          if (check > 0) schedule.last_check = std::chrono::system_clock::time_point { std::chrono::seconds(check) };
          jitter_set = body.value("jitterSet", false);
          if (jitter_set) schedule.jitter = std::chrono::minutes(body.value("jitter", 0));
        }
        catch (const std::exception &) {
        }
      }
      // Each process performs the host-start refresh once. The saved flag
      // must not suppress the next process start.
      schedule.host_started = false;
      if (!jitter_set) {
        const std::size_t mixed = std::hash<std::string> {}(path.string());
        schedule.jitter = std::chrono::minutes(static_cast<int>(mixed % 30));
      }
      return schedule;
    }
    void
    save_schedule(const fs::path &path, const ArtSchedule &schedule) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      std::ofstream out(path);
      const auto fetch = std::chrono::duration_cast<std::chrono::seconds>(schedule.last_fetch.time_since_epoch()).count();
      const auto check = std::chrono::duration_cast<std::chrono::seconds>(schedule.last_check.time_since_epoch()).count();
      json body;
      body["last_fetch"] = fetch;
      body["last_check"] = check;
      body["jitter"] = static_cast<int>(schedule.jitter.count());
      body["jitterSet"] = true;
      body["started"] = false;
      out << body.dump(2);
    }
    void
    spawn_refresh(HostEnv env) {
      std::thread([env = std::move(env)]() mutable {
        try {
          refresh_official(env, host_http_get, [](std::string_view bytes) { return host_decode_image(bytes); }, host_http_post_json);
        }
        catch (const std::exception &) {
        }
      }).detach();
    }
    void
    consider(ArtReason reason) {
      try {
        static std::mutex gate;
        std::lock_guard<std::mutex> lock(gate);
        HostEnv env = make_host_env();
        const fs::path path = env.host_data / "art-schedule.json";
        ArtSchedule schedule = load_schedule(path);
        if (!art_due(schedule, std::chrono::system_clock::now(), reason)) return;
        save_schedule(path, schedule);
        spawn_refresh(std::move(env));
      }
      catch (const std::exception &) {
      }
    }
  }  // namespace
  HostEnv
  make_host_env() {
    HostEnv env;
    env.apps_file = config::stream.file_apps;
    env.host_data = platf::appdata() / "sparxie";
    env.covers_dir = platf::appdata() / "covers";
    env.install_root = executable_dir();
    env.component_dir = platf::appdata() / "components" / "sparxie";
    if (const char *home = std::getenv("SPARXIE_HOME")) env.sparxie_exe = find_sparxie_exe(home);
    if (env.sparxie_exe.empty()) env.sparxie_exe = find_sparxie_exe(env.component_dir);
    if (env.sparxie_exe.empty()) env.sparxie_exe = find_sparxie_exe(env.install_root);
    fs::path override_dir;
    if (const char *dir = std::getenv("SPARXIE_DATA_DIR")) override_dir = dir;
    env.data_dir = resolve_data_dir(env.sparxie_exe, override_dir, local_sparxie_dir());
    env.data_dir_provisional = true;
    static std::once_flag runner_once;
    std::call_once(runner_once, [] {
      if (auto runner = make_user_runner()) set_user_runner(std::move(runner));
    });
    env.launch_gui = launch_in_user_session;
    env.warn = [](const std::string &message) { BOOST_LOG(warning) << "Sparxie: " << message; };
    env.registry = &host_registry();
    env.process_running = host_process_running;
    env.display_refresh = streamed_display_refresh;
    env.raise_priority = host_raise_game_priority;
    env.display_size = streamed_display_size;
    env.stream_running = [] { return proc::proc.running() > 0; };
    std::vector<fs::path> magic_roots { env.install_root / "assets", fs::path("assets") };
#ifdef SUNSHINE_ASSETS_DIR
    magic_roots.push_back(fs::path(SUNSHINE_ASSETS_DIR));
#endif
    env.zzz_magics = load_zzz_magics(env.host_data, magic_roots);
    // The default install locations of the official Hypergryph launcher; the
    // launcher keeps a version folder with Launcher.exe inside each root.
    for (const char *name : { "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA" }) {
      if (const char *dir = std::getenv(name)) {
        env.hypergryph_roots.push_back(fs::path(dir) / "Hypergryph Launcher");
        if (std::string(name) == "LOCALAPPDATA") env.hypergryph_roots.push_back(fs::path(dir) / "Programs" / "Hypergryph Launcher");
      }
    }
    // MaaEnd resolves the three official launcher channels from their
    // uninstall records. InstallLocation is the documented Windows Installer
    // value [ms-uninstall-registry-20261111]; read both machine views and the
    // signed-in user's hive so custom and per-user installs launch as well.
    // The key table is the one detection uses (vendors.cpp), so there is one list.
    for (const auto &root : host_uninstall_locations(hypergryph_uninstall_keys())) env.hypergryph_roots.push_back(root);
    std::ifstream templ(env.host_data / "sparxie-host.json");
    if (templ) {
      try {
        json body;
        templ >> body;
        if (body.contains("launchTemplate") && body["launchTemplate"].is_string()) env.launch_template = body["launchTemplate"].get<std::string>();
      }
      catch (const std::exception &) {
      }
    }
    return env;
  }
  void
  note_host_started() {
    consider(ArtReason::host_start);
    // a session that was cut off by a host restart left its values written: put them back now (a game that is running keeps them until it quits)
    std::thread([] {
      try {
        HostEnv env = make_host_env();
        recover_pending_settings(env);
      }
      catch (const std::exception &) {
      }
    }).detach();
  }
  void
  note_app_list_opened() {
    consider(ArtReason::app_list);
    consider(ArtReason::interval);
  }
  namespace {
    // the launch the next after_session belongs to
    std::atomic<std::uint64_t> g_session_generation { 0 };
  }  // namespace
  void
  after_session(const std::string &app_name, const std::string &cmd) {
    const std::uint64_t started = g_session_generation.load();
    std::thread([app_name, cmd, started] {
      try {
        HostEnv env = make_host_env();
        if (finish_session(env, app_name, cmd, started, std::chrono::minutes(2), std::chrono::seconds(1))) BOOST_LOG(info) << "Put back the game settings of [" << app_name << "] after the session";
        else BOOST_LOG(warning) << "The game of [" << app_name << "] was still running 2 minutes after the session; its settings are put back at the next launch or host start";
      }
      catch (const std::exception &) {
      }
    }).detach();
  }
  std::string
  before_launch(const std::string &app_name, const std::string &cmd, const SessionMode &session) {
    std::string arguments;
    try {
      HostEnv env = make_host_env();
      const Status status = prepare_launch(env, app_name, cmd, session, &arguments);
      g_session_generation = launch_generation();
      // 提升游戏优先级: the game starts after this call returns (directly, or through Sparxie), so a thread of its own waits for the process and raises it
      std::thread([env, app_name, cmd]() mutable {
        try {
          const int raised = run_launch_boost(env, app_name, cmd, std::chrono::minutes(5));
          if (raised > 0) BOOST_LOG(info) << "Raised the priority of [" << app_name << "] to above normal";
        }
        catch (const std::exception &) {
        }
      }).detach();
      if (!status.ok && status.error_code == "game_running") {
        BOOST_LOG(info) << "Sparxie left game settings unchanged because the game is running";
      }
      else if (!status.ok) {
        BOOST_LOG(warning) << "Sparxie did not apply game settings: " << status.error;
      }
    }
    catch (const std::exception &) {
    }
    return arguments;
  }
}  // namespace sparxie
