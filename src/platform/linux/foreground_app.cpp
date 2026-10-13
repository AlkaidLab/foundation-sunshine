/**
 * @file src/platform/linux/foreground_app.cpp
 * @brief Foreground window tracking for ABR, Linux side.
 *
 * The Windows backend reads GetForegroundWindow() on demand. A Wayland
 * compositor does not let regular clients query focus, so focus is taken from
 * whichever control interface the running session offers:
 *
 *   KDE Plasma - a tiny KWin script pushes active-window changes to a private
 *                D-Bus service hosted here (the tested path).
 *   niri       - `niri msg --json focused-window` is polled through niri's own
 *                IPC CLI (niri has no KWin, and its socket is per-session).
 *
 * detect() serves the cached report. Desktops with neither keep returning
 * empty info and ABR degrades the same way it does today.
 */
#include "foreground_app.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <thread>

#include <nlohmann/json.hpp>
#include <systemd/sd-bus.h>

#include "sdbus_session.h"
#include "src/display_device/vdd_utils.h"
#include "src/logging.h"

namespace platf::foreground_app {
  using namespace std::string_view_literals;

  namespace {

    constexpr const char *kBusName = "org.sunshine.Abr";
    constexpr const char *kInterface = "org.sunshine.abr";
    constexpr const char *kPluginName = "sunshine_foreground_report";
    constexpr auto kSetupRetryInterval = std::chrono::seconds { 30 };
    constexpr auto kReloadInterval = std::chrono::seconds { 300 };
    // niri pushes focus changes over its event stream, but a polled query keeps
    // this path simple and matches ABR's own 10 s detection interval.
    constexpr auto kNiriPollInterval = std::chrono::seconds { 2 };
    constexpr auto kNiriCommandTimeout = std::chrono::milliseconds { 3000 };

    struct cache_t {
      std::mutex mutex;
      info_t info;
      std::chrono::steady_clock::time_point updated;
      bool ever_reported = false;
    };

    cache_t &
    cache() {
      static cache_t instance;
      return instance;
    }

    std::atomic<bool> worker_started { false };
    std::atomic<bool> worker_running { false };

    /**
     * @brief Record one report, whichever producer supplied it.
     * @details KWin omits the pid for some windows (XWayland clients, transient
     *          dialogs) and niri can as well. Keep the last known pid while the
     *          app class is unchanged so the shared ABR consumer still sees
     *          pid > 0 and can detect app switches; a different class without a
     *          pid reports 0, which the consumer handles through its exe-name
     *          comparison.
     */
    /**
     * @brief Forget the cached window because its producer is gone.
     * @details The cache is otherwise deliberately long-lived: a foreground app
     *          that never changes is normal. When the producer itself dies (the
     *          compositor dropped our script, the niri query stopped working),
     *          the cached window may be long closed, so ABR must fall back to
     *          its launcher-based classification instead of acting on it.
     */
    void
    clear_cache() {
      auto &state = cache();
      std::lock_guard lock { state.mutex };
      state.info = {};
      state.updated = std::chrono::steady_clock::now();
      state.ever_reported = false;
    }

    void
    store_report(std::uint32_t pid, const std::string &exe_name, const std::string &window_title) {
      auto &state = cache();
      std::lock_guard lock { state.mutex };
      if (pid > 0) {
        state.info.pid = pid;
      }
      else if (exe_name.empty() || exe_name != state.info.exe_name) {
        state.info.pid = 0;
      }
      state.info.exe_name = exe_name;
      state.info.window_title = window_title;
      state.updated = std::chrono::steady_clock::now();
      state.ever_reported = true;
    }

    int
    on_report(sd_bus_message *m, void *, sd_bus_error *) {
      std::int32_t pid = 0;
      const char *resource_class = nullptr;
      const char *caption = nullptr;
      if (sd_bus_message_read(m, "iss", &pid, &resource_class, &caption) < 0) {
        return 0;
      }

      store_report(
        pid > 0 ? static_cast<std::uint32_t>(pid) : 0,
        resource_class ? resource_class : "",
        caption ? caption : "");
      return 1;
    }

    const sd_bus_vtable
      foreground_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Report", "iss", nullptr, on_report, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END
      };

    bool
    install_kwin_script(sd_bus *bus) {
      namespace fs = std::filesystem;

      std::error_code ec;
      const auto script_path = fs::temp_directory_path(ec) / "sunshine-foreground-report.js";
      if (ec) {
        return false;
      }

      static constexpr char script[] =
        R"JS(function sunshineReport() {
  var w = null;
  if (typeof workspace.activeWindow !== "undefined") { w = workspace.activeWindow; }
  else if (typeof workspace.activeClient !== "undefined") { w = workspace.activeClient; }
  if (w) {
    callDBus("org.sunshine.Abr", "/", "org.sunshine.abr", "Report",
      (w.pid ? Number(w.pid) : 0),
      String(w.resourceClass !== undefined ? (w.resourceClass || "") : (w.resourceName || "")),
      String(w.caption || ""));
  }
}
sunshineReport();
try {
  if (workspace.windowActivated) { workspace.windowActivated.connect(sunshineReport); }
  else if (workspace.clientActivated) { workspace.clientActivated.connect(sunshineReport); }
} catch (e) {})JS";

      try {
        std::ofstream out(script_path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
          return false;
        }
        out << script;
        if (!out) {
          return false;
        }
      }
      catch (const std::exception &err) {
        BOOST_LOG(warning) << "foreground: cannot write the KWin script: "sv << err.what();
        return false;
      }

      // Drop any instance left over from a previous run before loading.
      sd_bus_call_method(bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
        "unloadScript", nullptr, nullptr, "s", kPluginName);

      sd_bus_message *reply = nullptr;
      if (sd_bus_call_method(bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
            "loadScript", nullptr, &reply, "ss", script_path.c_str(), kPluginName) < 0) {
        return false;
      }

      int script_id = -1;
      sd_bus_message_read(reply, "i", &script_id);
      sd_bus_message_unref(reply);
      if (script_id < 0) {
        return false;
      }

      const std::string object = std::string { "/Scripting/Script" } + std::to_string(script_id);
      if (sd_bus_call_method(bus, "org.kde.KWin", object.c_str(), "org.kde.kwin.Script",
            "run", nullptr, nullptr, nullptr) < 0) {
        return false;
      }

      // The script reports the active window on load, so a success here means
      // the cache is about to be populated (or KWin reports no active window).
      return true;
    }

    void
    worker() {
      sd_bus *bus = nullptr;
      bool service_up = false;
      auto last_setup_attempt = std::chrono::steady_clock::time_point {};

      while (worker_running.load(std::memory_order_acquire)) {
        if (!service_up) {
          const auto now = std::chrono::steady_clock::now();
          if (now - last_setup_attempt < kSetupRetryInterval) {
            std::this_thread::sleep_for(std::chrono::seconds { 2 });
            continue;
          }
          last_setup_attempt = now;

          bus = sdbus::open_user_bus();
          if (!bus) {
            static std::atomic<bool> logged { false };
            if (!logged.exchange(true)) {
              BOOST_LOG(info) << "foreground: no session bus; KWin tracking unavailable"sv;
            }
            continue;
          }

          if (sd_bus_add_object_vtable(bus, nullptr, "/", kInterface, foreground_vtable, nullptr) < 0 ||
              sd_bus_request_name(bus, kBusName, 0) < 0) {
            BOOST_LOG(debug) << "foreground: cannot register "sv << kBusName;
            sd_bus_unref(bus);
            bus = nullptr;
            continue;
          }

          if (!install_kwin_script(bus)) {
            BOOST_LOG(debug) << "foreground: KWin script installation failed; will retry"sv;
            sd_bus_unref(bus);
            bus = nullptr;
            continue;
          }

          service_up = true;
          BOOST_LOG(info) << "foreground: KWin active-window tracking enabled"sv;
          continue;
        }

        // Service the D-Bus connection.
        int r = 0;
        do {
          r = sd_bus_process(bus, nullptr);
        } while (r > 0);
        if (r < 0) {
          BOOST_LOG(debug) << "foreground: session bus lost; re-registering"sv;
          sd_bus_unref(bus);
          bus = nullptr;
          service_up = false;
          continue;
        }

        pollfd pfd { sd_bus_get_fd(bus), POLLIN, 0 };
        if (poll(&pfd, 1, 500) < 0 && errno != EINTR) {
          sd_bus_unref(bus);
          bus = nullptr;
          service_up = false;
          continue;
        }

        // A KWin restart orphans our script silently; reload when nothing has
        // been heard from it for a while even though the bus is healthy.
        auto &state = cache();
        const auto now = std::chrono::steady_clock::now();
        if (now - last_setup_attempt > kReloadInterval) {
          std::chrono::steady_clock::time_point last_report;
          {
            std::lock_guard lock { state.mutex };
            last_report = state.updated;
          }
          if (now - last_report > kReloadInterval) {
            BOOST_LOG(debug) << "foreground: KWin script silent; reloading"sv;
            if (!install_kwin_script(bus)) {
              clear_cache();
            }
            last_setup_attempt = now;
          }
        }
      }

      if (bus) {
        sd_bus_unref(bus);
      }
    }

    /**
     * @brief Whether a D-Bus name currently has an owner.
     * @details Asked of the bus daemon rather than sd_bus_get_name_owner(),
     *          which the systemd headers do not expose on every version.
     */
    bool
    name_has_owner(const char *name) {
      sd_bus *bus = sdbus::open_user_bus();
      if (!bus) {
        return false;
      }

      sd_bus_error err = SD_BUS_ERROR_NULL;
      sd_bus_message *reply = nullptr;
      bool has_owner = false;

      const int rc = sd_bus_call_method(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        &err,
        &reply,
        "s",
        name);
      if (rc >= 0 && reply) {
        int value = 0;
        if (sd_bus_message_read(reply, "b", &value) >= 0) {
          has_owner = value != 0;
        }
      }

      if (reply) {
        sd_bus_message_unref(reply);
      }
      sd_bus_error_free(&err);
      sd_bus_unref(bus);
      return has_owner;
    }

    /**
     * @brief Whether a KWin instance owns the session-bus name.
     * @details Used only to pick the producer: KDE sessions keep the tested
     *          KWin path even if niri happens to be installed.
     */
    bool
    kwin_is_running() {
      return name_has_owner("org.kde.KWin");
    }

    /**
     * @brief Poll niri for the focused window.
     * @details niri exposes focus through its IPC CLI; the command prints a
     *          JSON object, or `null` when nothing is focused (which leaves the
     *          cache untouched, matching how the KWin script only reports on
     *          activations and how the Windows query keeps the last app on an
     *          empty foreground window).
     */
    void
    niri_worker() {
      BOOST_LOG(info) << "foreground: niri focused-window tracking enabled"sv;

      bool failure_logged = false;
      while (worker_running.load(std::memory_order_acquire)) {
        const auto result = display_device::vdd_utils::run_logged("niri msg --json focused-window", kNiriCommandTimeout);
        if (result.exit_code == 0) {
          failure_logged = false;
          info_t info;
          if (parse_niri_focused_window(result.output, info)) {
            store_report(info.pid, info.exe_name, info.window_title);
          }
        }
        else {
          // The query stopped working (niri restarted, socket gone): drop the
          // cached window so ABR does not keep classifying a closed app.
          clear_cache();
          if (!failure_logged) {
            failure_logged = true;
            BOOST_LOG(warning) << "foreground: `niri msg --json focused-window` failed (exit "sv
                               << result.exit_code << "); ABR app classification degraded"sv;
          }
        }

        // Sleep in small slices so a shutdown is observed promptly.
        const auto deadline = std::chrono::steady_clock::now() + kNiriPollInterval;
        while (worker_running.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
        }
      }
    }

    void
    start_worker() {
      if (worker_started.exchange(true)) {
        return;
      }
      worker_running.store(true, std::memory_order_release);

      // KDE first (the long-tested path); otherwise niri's IPC, probed with the
      // very query the poller uses so an installed-but-not-running niri does
      // not steal the KWin path.
      if (!kwin_is_running() &&
          display_device::vdd_utils::run_logged("niri msg --json focused-window", kNiriCommandTimeout).exit_code == 0) {
        std::thread(niri_worker).detach();
        return;
      }

      std::thread(worker).detach();
    }

  }  // namespace

  bool
  parse_niri_focused_window(const std::string &json_text, info_t &out) {
    if (json_text.empty()) {
      return false;
    }

    nlohmann::json doc;
    try {
      doc = nlohmann::json::parse(json_text);
    }
    catch (const std::exception &e) {
      static std::atomic<bool> logged { false };
      if (!logged.exchange(true)) {
        BOOST_LOG(debug) << "foreground: cannot parse niri window JSON: "sv << e.what();
      }
      return false;
    }

    if (!doc.is_object()) {
      // niri prints `null` when no window is focused.
      return false;
    }

    // Field-by-field and type-checked: a renamed or retyped field must cost that
    // field only, not the whole report.
    const auto string_field = [&doc](const char *key) {
      const auto it = doc.find(key);
      return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string {};
    };
    const auto integer_field = [&doc](const char *key) {
      const auto it = doc.find(key);
      return it != doc.end() && it->is_number() ? it->get<std::int64_t>() : std::int64_t { 0 };
    };

    out.window_title = string_field("title");
    out.exe_name = string_field("app_id");
    const auto pid = integer_field("pid");
    out.pid = pid > 0 ? static_cast<std::uint32_t>(pid) : 0;
    return !out.window_title.empty() || !out.exe_name.empty();
  }

  info_t
  detect() {
    start_worker();

    auto &state = cache();
    std::lock_guard lock { state.mutex };
    if (!state.ever_reported) {
      static std::atomic<bool> logged { false };
      if (!logged.exchange(true)) {
        BOOST_LOG(info) << "foreground: no active-window report yet; ABR app classification degraded"sv;
      }
    }
    return state.info;
  }

}  // namespace platf::foreground_app
