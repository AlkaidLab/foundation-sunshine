/**
 * @file src/launch_splash/launch_splash.cpp
 * @brief The host side of the launch splash.
 */
#include "launch_splash.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#include "src/client_interface.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/display_device/display_device.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "shown_gate.h"
#include "src/tray/tray_http.h"
#include "src/tray/tray_state.h"

namespace launch_splash {

  using namespace std::literals;

  namespace {
    using sys_clock = std::chrono::system_clock;
    constexpr auto poll_interval = std::chrono::milliseconds(150);

    std::mutex mutex;
    std::condition_variable wake;
    tracker_t tracker;
    std::filesystem::path status_file;
    std::thread watcher;
    std::uint64_t generation = 0;  // a new launch makes the watcher of the old one leave
    std::string display_name;
    shown_gate_t gate;  // the app start waits on it for the Panel's answer; its own lock, so the wait never holds the splash's
    bool announced = false;  // a launch is announced and begin() has not taken it over
    std::chrono::steady_clock::time_point announced_at;
    std::chrono::steady_clock::time_point ready_at;

    std::int64_t
    since_ms(const std::chrono::steady_clock::time_point from, const std::chrono::steady_clock::time_point to) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    }

    std::int64_t
    now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(sys_clock::now().time_since_epoch()).count();
    }

    std::filesystem::path
    launch_dir() {
      return platf::appdata() / "launch";
    }

    tray_state::launch_t
    to_tray(const view_t &view, const std::string &display) {
      tray_state::launch_t out;
      out.id = view.id;
      out.active = view.active;
      out.kind = std::string { kind_key(view.kind) };
      out.app_id = view.app_id;
      out.app_name = view.app_name;
      out.art = view.art;
      out.stage = std::string { stage_key(view.stage) };
      out.notice = view.notice;
      out.display = display;
      out.display_ready = view.display_ready;
      out.pids = view.pids;
      out.started_at_ms = view.started_at_ms;
      out.deadline_ms = view.deadline_ms;
      return out;
    }

    // call with the mutex held
    void
    publish_locked() {
      tray_state::set_launch(to_tray(tracker.view(), display_name));
    }

    std::string
    read_text(const std::filesystem::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return {};
      }
      std::ostringstream text;
      text << in.rdbuf();
      return text.str();
    }

    void
    remove_file(const std::filesystem::path &path) {
      if (path.empty()) {
        return;
      }
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }

    void
    watch(const std::uint64_t mine) {
      std::filesystem::file_time_type last_write {};
      std::uintmax_t last_size = 0;
      std::unique_lock lock { mutex };
      while (generation == mine && tracker.view().active) {
        bool changed = tracker.tick(now_ms());
        if (!status_file.empty()) {
          std::error_code ec;
          const auto written = std::filesystem::last_write_time(status_file, ec);
          const auto size = ec ? 0 : std::filesystem::file_size(status_file, ec);
          if (!ec && (written != last_write || size != last_size)) {
            last_write = written;
            last_size = size;
            // the file is replaced whole by the launcher (written beside, then moved over); half a write is no JSON and is read again next time
            if (const auto status = parse_launcher_status(read_text(status_file)); status && tracker.apply(*status, now_ms())) {
              changed = true;
              display_name = display_device::get_display_name(config::video.output_name);
            }
          }
        }
        if (changed) {
          publish_locked();
          BOOST_LOG(debug) << "Launch splash: stage "sv << stage_key(tracker.view().stage) << (tracker.view().active ? "" : " (over)");
        }
        wake.wait_for(lock, poll_interval);
      }
      if (generation == mine) {
        remove_file(status_file);
        status_file.clear();
      }
    }

    void
    stop_watcher_locked(std::unique_lock<std::mutex> &lock) {
      ++generation;
      wake.notify_all();
      if (watcher.joinable()) {
        auto old = std::move(watcher);
        lock.unlock();
        old.join();
        lock.lock();
      }
    }
  }  // namespace

  namespace {
    /// The picture behind the splash: the hero if the app has one, else its poster. Only files the host would serve as art, and only ones that exist.
    std::string
    art_of(const proc::ctx_t &app) {
      const auto readable = [](const std::filesystem::path &path) {
        std::error_code ec;
        return client_interface::is_art_extension(path.extension().string()) && std::filesystem::is_regular_file(path, ec);
      };
      if (!app.fs_facts.hero_path.empty()) {
        const auto configured = file_handler::path_from_utf8(app.fs_facts.hero_path);
        const std::filesystem::path candidates[] = {
          file_handler::path_from_utf8(SUNSHINE_ASSETS_DIR) / configured,
          platf::appdata() / "covers" / configured,
          configured,
        };
        for (const auto &path : candidates) {
          if (readable(path)) {
            return file_handler::path_to_utf8(path);
          }
        }
      }
      const auto poster = proc::validate_app_image_path(app.image_path);
      if (!poster.empty()) {
        const auto path = file_handler::path_from_utf8(poster);
        if (readable(path)) {
          return file_handler::path_to_utf8(path);
        }
      }
      return {};
    }
  }  // namespace

  bool
  command_reports_stages(const std::string &cmd) {
    std::string lower = cmd;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("sparxie-launch") != std::string::npos;
  }

  namespace {
    // call with the lock; ready is whether the display the stream captures is already set up
    launcher_env_t
    start_locked(std::unique_lock<std::mutex> &lock, const proc::ctx_t &app, const kind_e kind, const bool ready) {
      stop_watcher_locked(lock);
      remove_file(status_file);
      announced = false;
      announced_at = ready_at = std::chrono::steady_clock::now();
      const auto id = tracker.begin(app.id, app.name, art_of(app), kind, now_ms(), ready);
      gate.arm(id);
      std::error_code ec;
      const auto dir = launch_dir();
      std::filesystem::create_directories(dir, ec);
      status_file = ec ? std::filesystem::path {} : dir / ("status-" + id + ".json");
      // the display is the one the stream captures; the virtual display of this launch is up before the app starts
      display_name = ready ? display_device::get_display_name(config::video.output_name) : std::string {};
      publish_locked();
      const auto mine = ++generation;
      watcher = std::thread([mine] { watch(mine); });
      BOOST_LOG(info) << "Launch splash: "sv << app.name << " ("sv << kind_key(kind) << ", "sv << id << (ready ? "" : ", announced before the display setup") << ')';
      return launcher_env_t { id, status_file };
    }
  }  // namespace

  bool
  announce(const proc::ctx_t &app) {
    if (!config::stream.launch_splash || app.cmd.empty()) {
      return false;
    }
    std::unique_lock lock { mutex };
    start_locked(lock, app, kind_e::launch, false);
    announced = true;
    return true;
  }

  void
  display_ready() {
    std::lock_guard lock { mutex };
    if (tracker.set_display_ready()) {
      display_name = display_device::get_display_name(config::video.output_name);
      ready_at = std::chrono::steady_clock::now();
      publish_locked();
      BOOST_LOG(info) << "Launch splash: display ready "sv << since_ms(announced_at, ready_at) << " ms after the request, the Panel may show it"sv;
    }
  }

  void
  cancel_announced() {
    std::unique_lock lock { mutex };
    if (!announced) {
      return;
    }
    announced = false;
    if (tracker.end()) {
      publish_locked();
      stop_watcher_locked(lock);
      remove_file(status_file);
      status_file.clear();
      gate.withdraw();
    }
  }

  bool
  note_shown(const std::string &id, const std::int64_t panel_ms, const std::int64_t prepare_ms) {
    return gate.note(id, panel_ms, prepare_ms);
  }

  void
  wait_shown() {
    std::string id;
    {
      std::lock_guard lock { mutex };
      const auto &view = tracker.view();
      if (!view.active || view.kind != kind_e::launch || !config::stream.launch_splash) {
        return;
      }
      id = view.id;
    }
    const auto outcome = gate.wait(id, std::chrono::milliseconds(shown_wait_ms), tray_http::launch_splash_ack_listeners() > 0);
    switch (outcome.how) {
      case shown_gate_t::how_e::answered:
        BOOST_LOG(info) << "Launch splash: on screen, first frame painted "sv << outcome.panel_ms << " ms after the display was ready (its window was ready "sv << outcome.prepare_ms
                        << " ms after the request); the app start waited "sv << outcome.waited_ms << " ms"sv;
        break;
      case shown_gate_t::how_e::timed_out:
        BOOST_LOG(warning) << "Launch splash: not on screen "sv << shown_wait_ms << " ms after the display was ready, the app starts anyway"sv;
        break;
      case shown_gate_t::how_e::no_listener:
        BOOST_LOG(info) << "Launch splash: no Panel that can answer is connected, the app starts without waiting"sv;
        break;
      case shown_gate_t::how_e::withdrawn:
        break;
    }
  }

  std::optional<launcher_env_t>
  begin(const proc::ctx_t &app, const kind_e kind) {
    if (!config::stream.launch_splash) {
      return std::nullopt;
    }
    std::unique_lock lock { mutex };
    // a reconnecting client resumes an app whose launch is still being shown: that one goes on
    if (kind == kind_e::resume && tracker.view().active && tracker.view().app_id == app.id) {
      return std::nullopt;
    }
    // the request announced this launch before the display setup: it goes on under the same id
    if (kind == kind_e::launch && announced && tracker.view().active && tracker.view().app_id == app.id) {
      announced = false;
      if (tracker.set_display_ready()) {
        display_name = display_device::get_display_name(config::video.output_name);
        ready_at = std::chrono::steady_clock::now();
        publish_locked();
      }
      return launcher_env_t { tracker.id(), status_file };
    }
    return start_locked(lock, app, kind, true);
  }

  void
  note_started(const std::uint32_t pid, const bool launcher_reports) {
    std::lock_guard lock { mutex };
    if (!tracker.view().active) {
      return;
    }
    tracker.add_pid(pid);
    if (!launcher_reports) {
      launcher_status_t status;
      status.launch = tracker.id();
      status.stage = stage_e::waiting_window;
      tracker.apply(status, now_ms());
    }
    publish_locked();
  }

  void
  fail(const std::string &reason) {
    std::lock_guard lock { mutex };
    if (tracker.fail(reason, now_ms())) {
      publish_locked();
    }
  }

  void
  end() {
    std::unique_lock lock { mutex };
    if (announced) {
      return;  // the request is still being prepared: the proc's clean-slate terminate() must not end it; cancel_announced() does
    }
    if (!tracker.view().active || tracker.view().stage == stage_e::failed) {
      return;  // nothing showing, or a failure the person is still reading: its own time ends it
    }
    tracker.end();
    gate.withdraw();
    publish_locked();
    stop_watcher_locked(lock);
    remove_file(status_file);
    status_file.clear();
  }

}  // namespace launch_splash
