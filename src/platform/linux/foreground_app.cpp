/**
 * @file src/platform/linux/foreground_app.cpp
 * @brief Foreground window tracking for ABR, Linux side.
 *
 * The Windows backend reads GetForegroundWindow() on demand. A Wayland
 * compositor does not let regular clients query focus, so on KDE Plasma we
 * load a tiny KWin script once and let it push active-window changes to a
 * private D-Bus service hosted here; detect() serves the cached report.
 * Desktops without KWin keep returning empty info and ABR degrades the same
 * way it does today.
 */
#include "foreground_app.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <poll.h>
#include <thread>

#include <systemd/sd-bus.h>

#include "sdbus_session.h"
#include "src/logging.h"

namespace platf::foreground_app {
  using namespace std::string_view_literals;

  namespace {

    constexpr const char *kBusName = "org.sunshine.Abr";
    constexpr const char *kInterface = "org.sunshine.abr";
    constexpr const char *kPluginName = "sunshine_foreground_report";
    constexpr auto kSetupRetryInterval = std::chrono::seconds { 30 };
    constexpr auto kReloadInterval = std::chrono::seconds { 300 };

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

    int
    on_report(sd_bus_message *m, void *, sd_bus_error *) {
      std::int32_t pid = 0;
      const char *resource_class = nullptr;
      const char *caption = nullptr;
      if (sd_bus_message_read(m, "iss", &pid, &resource_class, &caption) < 0) {
        return 0;
      }

      auto &state = cache();
      std::lock_guard lock { state.mutex };
      const std::string exe_name = resource_class ? resource_class : "";
      // KWin omits the pid for some windows (XWayland clients, transient
      // dialogs). Keep the last known pid while the app class is unchanged so
      // the shared ABR consumer still sees pid > 0 and can detect app switches;
      // a different class without a pid reports 0, which the consumer handles
      // through its exe-name comparison.
      if (pid > 0) {
        state.info.pid = static_cast<std::uint32_t>(pid);
      }
      else if (exe_name.empty() || exe_name != state.info.exe_name) {
        state.info.pid = 0;
      }
      state.info.exe_name = exe_name;
      state.info.window_title = caption ? caption : "";
      state.updated = std::chrono::steady_clock::now();
      state.ever_reported = true;
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
            install_kwin_script(bus);
            last_setup_attempt = now;
          }
        }
      }

      if (bus) {
        sd_bus_unref(bus);
      }
    }

    void
    start_worker() {
      if (worker_started.exchange(true)) {
        return;
      }
      worker_running.store(true, std::memory_order_release);
      std::thread(worker).detach();
    }

  }  // namespace

  info_t
  detect() {
    start_worker();

    auto &state = cache();
    std::lock_guard lock { state.mutex };
    if (!state.ever_reported) {
      static std::atomic<bool> logged { false };
      if (!logged.exchange(true)) {
        BOOST_LOG(info) << "foreground: no KWin report yet; ABR app classification degraded"sv;
      }
    }
    return state.info;
  }

}  // namespace platf::foreground_app
