/**
 * @file src/platform/linux/foreground_app.cpp
 * @brief Foreground window tracking for ABR, Linux side.
 *
 * The Windows backend reads GetForegroundWindow() on demand. A Wayland
 * compositor does not let regular clients query focus, so focus is taken from
 * whichever control interface the running session offers:
 *
 *   KDE Plasma  - a tiny KWin script pushes active-window changes to a private
 *                 D-Bus service hosted here (the tested path, carries a pid).
 *   wlroots     - wlr-foreign-toplevel-management reports activation state for
 *                 every toplevel, which covers sway, river, wayfire and the
 *                 rest of the wlroots family (no pid in the protocol).
 *   niri        - `niri msg --json focused-window` is polled through niri's own
 *                 IPC CLI (niri implements neither KWin's scripting nor the
 *                 wlroots protocol).
 *   X11         - _NET_ACTIVE_WINDOW watched through PropertyNotify, for an X11
 *                 session or XWayland under a compositor with no focus source.
 *
 * detect() serves the cached report. Desktops with none of these keep returning
 * empty info and ABR degrades the same way it does today.
 */
#include "foreground_app.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(SUNSHINE_HAVE_SDBUS)
  #include <systemd/sd-bus.h>
#endif

#ifdef SUNSHINE_BUILD_WAYLAND
  #include <wayland-client.h>

  #include <wlr-foreign-toplevel-management-unstable-v1.h>

  #include "wayland.h"
#endif

#ifdef SUNSHINE_BUILD_X11
  #include <X11/Xatom.h>
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
#endif

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
    // Re-read the foreground window on a timer as well, in case the producer did
    // not deliver an event for the change (missed PropertyNotify, compositor
    // that keeps quiet). Matches ABR's own detection cadence.
    constexpr auto kResyncInterval = std::chrono::seconds { 2 };

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

    // The KWin producer is the only one that needs a session bus: it registers
    // a D-Bus object for the KWin script to call back on. Without libsystemd
    // there is no sd-bus at all, so the whole producer compiles out and the
    // probe below reports it as unavailable.
#if defined(SUNSHINE_HAVE_SDBUS)

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

#else  // !SUNSHINE_HAVE_SDBUS

    /**
     * @brief Whether a KWin instance owns the session-bus name.
     * @details Built without libsystemd there is no bus to ask, so the producer
     *          is reported as unavailable and niri, wlroots or X11 answer
     *          instead. A Plasma session on such a build loses the only
     *          producer that carries a pid, which is why the package depends on
     *          the headers.
     */
    [[maybe_unused]] bool
    kwin_is_running() {
      return false;
    }

#endif  // SUNSHINE_HAVE_SDBUS

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

#ifdef SUNSHINE_BUILD_WAYLAND
    /**
     * @brief One toplevel tracked through wlr-foreign-toplevel-management.
     */
    struct wlr_toplevel_t {
      zwlr_foreign_toplevel_handle_v1 *handle {};
      struct wlr_state_t *owner {};
      std::string title;
      std::string app_id;
      bool activated {};
    };

    struct wlr_state_t {
      zwlr_foreign_toplevel_manager_v1 *manager {};
      std::vector<std::unique_ptr<wlr_toplevel_t>> toplevels;
      std::mutex mutex;
      std::atomic<bool> finished {};
    };

    /**
     * @brief Report whichever toplevel the compositor marks as activated.
     * @details A focus switch is delivered as "old toplevel loses `activated`"
     *          plus "new toplevel gains it", each followed by `done`, so the
     *          scan runs on every `done` and reports the single active entry.
     *          The protocol carries no pid, which is fine: store_report() keeps
     *          app-switch detection working through the exe name alone.
     */
    void
    wlr_publish(wlr_state_t &state) {
      std::lock_guard lock { state.mutex };
      for (const auto &toplevel : state.toplevels) {
        if (!toplevel->activated) {
          continue;
        }
        if (!toplevel->app_id.empty() || !toplevel->title.empty()) {
          store_report(0, toplevel->app_id, toplevel->title);
        }
        return;
      }
    }

    void
    wlr_on_title(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *title) {
      static_cast<wlr_toplevel_t *>(data)->title = title ? title : "";
    }

    void
    wlr_on_app_id(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *app_id) {
      static_cast<wlr_toplevel_t *>(data)->app_id = app_id ? app_id : "";
    }

    void
    wlr_on_state(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *states) {
      auto &toplevel = *static_cast<wlr_toplevel_t *>(data);
      toplevel.activated = false;
      const auto *begin = static_cast<const std::uint32_t *>(states->data);
      const auto *end = begin + states->size / sizeof(std::uint32_t);
      for (const auto *state = begin; state != end; ++state) {
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
          toplevel.activated = true;
        }
      }
    }

    void
    wlr_on_done(void *data, zwlr_foreign_toplevel_handle_v1 *) {
      wlr_publish(*static_cast<wlr_toplevel_t *>(data)->owner);
    }

    void
    wlr_on_closed(void *data, zwlr_foreign_toplevel_handle_v1 *handle) {
      // The listener data lives in the entry that is about to be erased, so the
      // state is reached through the owning pointer it carries.
      auto &state = *static_cast<wlr_toplevel_t *>(data)->owner;
      {
        std::lock_guard lock { state.mutex };
        auto &toplevels = state.toplevels;
        const auto it = std::find_if(toplevels.begin(), toplevels.end(), [handle](const auto &entry) {
          return entry->handle == handle;
        });
        if (it != toplevels.end()) {
          // Destroying the proxy also drops the listeners registered on it.
          auto closed = std::move(*it);
          toplevels.erase(it);
          zwlr_foreign_toplevel_handle_v1_destroy(closed->handle);
        }
      }
      // Focus may have moved to another toplevel by now.
      wlr_publish(state);
    }

    const zwlr_foreign_toplevel_handle_v1_listener kWlrToplevelListener = [] {
      zwlr_foreign_toplevel_handle_v1_listener listener {};
      listener.title = wlr_on_title;
      listener.app_id = wlr_on_app_id;
      listener.state = wlr_on_state;
      listener.done = wlr_on_done;
      listener.closed = wlr_on_closed;
      return listener;
    }();

    void
    wlr_on_toplevel(void *data, zwlr_foreign_toplevel_manager_v1 *, zwlr_foreign_toplevel_handle_v1 *handle) {
      auto &state = *static_cast<wlr_state_t *>(data);
      auto toplevel = std::make_unique<wlr_toplevel_t>();
      toplevel->handle = handle;
      toplevel->owner = &state;
      zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kWlrToplevelListener, toplevel.get());
      std::lock_guard lock { state.mutex };
      state.toplevels.push_back(std::move(toplevel));
    }

    void
    wlr_on_manager_finished(void *data, zwlr_foreign_toplevel_manager_v1 *) {
      static_cast<wlr_state_t *>(data)->finished.store(true, std::memory_order_release);
    }

    const zwlr_foreign_toplevel_manager_v1_listener kWlrManagerListener = [] {
      zwlr_foreign_toplevel_manager_v1_listener listener {};
      listener.toplevel = wlr_on_toplevel;
      listener.finished = wlr_on_manager_finished;
      return listener;
    }();

    struct wlr_registry_t {
      wlr_state_t *state {};
    };

    void
    wlr_on_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {
      auto &state = *static_cast<wlr_registry_t *>(data)->state;
      if (state.manager || std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) != 0) {
        return;
      }
      // Version 3 only adds the `parent` event; nothing here needs more.
      constexpr std::uint32_t kMaxVersion = 3;
      state.manager = static_cast<zwlr_foreign_toplevel_manager_v1 *>(
        wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min(version, kMaxVersion)));
      zwlr_foreign_toplevel_manager_v1_add_listener(state.manager, &kWlrManagerListener, &state);
    }

    void
    wlr_on_global_remove(void *, wl_registry *, std::uint32_t) {}

    const wl_registry_listener kWlrRegistryListener = [] {
      wl_registry_listener listener {};
      listener.global = wlr_on_global;
      listener.global_remove = wlr_on_global_remove;
      return listener;
    }();

    /**
     * @brief Open a connection and bind the toplevel manager if it is offered.
     * @returns true when the compositor implements the protocol.
     */
    bool
    wlr_bind(wl::display_t &display, wlr_state_t &state, wlr_registry_t &registry) {
      if (display.init() < 0) {
        return false;
      }
      if (wl_registry_add_listener(display.registry(), &kWlrRegistryListener, &registry) < 0) {
        return false;
      }
      // The first roundtrip delivers the globals, the second the initial
      // toplevel list, so the cache is populated before the worker idles.
      display.roundtrip();
      display.roundtrip();
      return state.manager != nullptr;
    }

    /**
     * @brief Whether the session compositor implements the generic wlroots
     *        toplevel protocol.
     * @details Probed before spawning the worker so a compositor that answers
     *          nothing falls through to the next producer instead of parking a
     *          thread that can never report. KWin has no wlr protocols, and
     *          niri ships its own IPC instead, so both are served elsewhere.
     *          wayland-protocols' own ext-foreign-toplevel-list is not usable
     *          here: it enumerates toplevels without any activation state, so
     *          it cannot answer which window is in the foreground.
     */
    bool
    wlr_toplevels_available() {
      wl::display_t display;
      wlr_state_t state;
      wlr_registry_t registry { &state };
      const bool available = wlr_bind(display, state, registry);
      if (state.manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(state.manager);
      }
      return available;
    }

    void
    wlr_worker() {
      wl::display_t display;
      wlr_state_t state;
      wlr_registry_t registry { &state };
      if (!wlr_bind(display, state, registry)) {
        clear_cache();
        BOOST_LOG(warning) << "foreground: wlr-foreign-toplevel-management unavailable"sv;
        return;
      }

      BOOST_LOG(info) << "foreground: wlr-foreign-toplevel-management tracking enabled"sv;
      wlr_publish(state);

      while (worker_running.load(std::memory_order_acquire) &&
             !state.finished.load(std::memory_order_acquire)) {
        // dispatch() returns false on a plain timeout; a dead compositor is
        // visible through the display error instead.
        display.dispatch(std::chrono::milliseconds { 500 });
        if (wl_display_get_error(display.get()) != 0) {
          BOOST_LOG(warning) << "foreground: Wayland connection lost; ABR app classification degraded"sv;
          clear_cache();
          return;
        }
      }

      zwlr_foreign_toplevel_manager_v1_destroy(state.manager);
      state.manager = nullptr;
    }
#endif  // SUNSHINE_BUILD_WAYLAND

#ifdef SUNSHINE_BUILD_X11
    /**
     * @brief Read a byte-string X11 property.
     * @details Trailing NUL padding is stripped, but an embedded NUL is kept:
     *          WM_CLASS is exactly that, a pair of NUL separated strings.
     */
    std::string
    x11_string_property(Display *dpy, Window window, Atom property, Atom type) {
      if (property == None || type == None) {
        return {};
      }

      Atom actual_type = None;
      int actual_format = 0;
      unsigned long count = 0;
      unsigned long remaining = 0;
      unsigned char *data = nullptr;
      if (XGetWindowProperty(dpy, window, property, 0, 1024, False, type,
            &actual_type, &actual_format, &count, &remaining, &data) != Success ||
          !data) {
        return {};
      }

      std::string value;
      if (actual_format == 8) {
        value.assign(reinterpret_cast<const char *>(data), count);
      }
      XFree(data);

      const auto last = value.find_last_not_of('\0');
      value.erase(last == std::string::npos ? 0 : last + 1);
      return value;
    }

    std::uint32_t
    x11_cardinal_property(Display *dpy, Window window, Atom property) {
      if (property == None) {
        return 0;
      }

      Atom actual_type = None;
      int actual_format = 0;
      unsigned long count = 0;
      unsigned long remaining = 0;
      unsigned char *data = nullptr;
      std::uint32_t value = 0;
      if (XGetWindowProperty(dpy, window, property, 0, 1, False, XA_CARDINAL,
            &actual_type, &actual_format, &count, &remaining, &data) == Success &&
          data) {
        if (actual_format == 32 && count >= 1) {
          value = static_cast<std::uint32_t>(reinterpret_cast<long *>(data)[0]);
        }
        XFree(data);
      }
      return value;
    }

    /**
     * @brief Foreground window on X11, from the EWMH active-window property.
     * @details _NET_ACTIVE_WINDOW names the focused window; from there
     *          _NET_WM_PID, WM_CLASS and _NET_WM_NAME give the same triple the
     *          KWin script reports. Property names use intern-only lookups so a
     *          missing atom is not created just to be searched for.
     */
    bool
    x11_read_foreground(Display *dpy, info_t &out) {
      const Atom active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
      if (active_window == None) {
        return false;
      }

      Atom actual_type = None;
      int actual_format = 0;
      unsigned long count = 0;
      unsigned long remaining = 0;
      unsigned char *data = nullptr;
      if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), active_window, 0, 1, False, XA_WINDOW,
            &actual_type, &actual_format, &count, &remaining, &data) != Success ||
          !data) {
        return false;
      }
      const Window window = *reinterpret_cast<Window *>(data);
      XFree(data);
      if (!window) {
        return false;
      }

      out = {};
      out.pid = x11_cardinal_property(dpy, window, XInternAtom(dpy, "_NET_WM_PID", True));

      // WM_CLASS holds "instance\0class"; KWin reports the class as the resource
      // class, so the class is what the shared consumer compares against.
      auto wm_class = x11_string_property(dpy, window, XA_WM_CLASS, XA_STRING);
      const auto separator = wm_class.find('\0');
      if (separator != std::string::npos) {
        wm_class.erase(0, separator + 1);
      }
      out.exe_name = wm_class;

      out.window_title = x11_string_property(dpy, window, XInternAtom(dpy, "_NET_WM_NAME", True),
        XInternAtom(dpy, "UTF8_STRING", True));
      if (out.window_title.empty()) {
        out.window_title = x11_string_property(dpy, window, XA_WM_NAME, XA_STRING);
      }

      return !out.window_title.empty() || !out.exe_name.empty();
    }

    /**
     * @brief Track the X11 foreground window from PropertyNotify events.
     * @details Covers an X11 session and, on Wayland, XWayland clients under a
     *          compositor with no focus interface of its own. The property is
     *          re-read on a timer as well: a compositor that forgets the
     *          notification would otherwise freeze the cached window forever.
     */
    void
    x11_worker() {
      Display *dpy = XOpenDisplay(nullptr);
      if (!dpy) {
        clear_cache();
        BOOST_LOG(warning) << "foreground: cannot open the X11 display"sv;
        return;
      }

      const Window root = DefaultRootWindow(dpy);
      XSelectInput(dpy, root, PropertyChangeMask);
      const Atom active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
      BOOST_LOG(info) << "foreground: X11 active-window tracking enabled"sv;

      info_t last;
      const auto publish = [&]() {
        info_t current;
        if (!x11_read_foreground(dpy, current)) {
          return;
        }
        if (current.pid == last.pid && current.exe_name == last.exe_name &&
            current.window_title == last.window_title) {
          return;
        }
        last = current;
        store_report(current.pid, current.exe_name, current.window_title);
      };
      publish();

      const int fd = ConnectionNumber(dpy);
      auto next_resync = std::chrono::steady_clock::now() + kResyncInterval;
      while (worker_running.load(std::memory_order_acquire)) {
        pollfd pfd { fd, POLLIN, 0 };
        const int rc = poll(&pfd, 1, 500);
        if (rc < 0 && errno != EINTR) {
          break;
        }

        if (rc > 0 && (pfd.revents & POLLIN)) {
          while (XPending(dpy) > 0) {
            XEvent event {};
            XNextEvent(dpy, &event);
            if (event.type == PropertyNotify && event.xproperty.window == root &&
                event.xproperty.atom == active_window) {
              publish();
            }
          }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_resync) {
          publish();
          next_resync = now + kResyncInterval;
        }
      }

      XCloseDisplay(dpy);
    }
#endif  // SUNSHINE_BUILD_X11

    void
    start_worker() {
      if (worker_started.exchange(true)) {
        return;
      }
      worker_running.store(true, std::memory_order_release);

      // KDE first: its KWin script is the only Wayland producer that also
      // carries a pid, and KWin does not implement the wlroots protocols, so
      // the generic probe below cannot serve a Plasma session anyway.
#if defined(SUNSHINE_HAVE_SDBUS)
      if (kwin_is_running()) {
        std::thread(worker).detach();
        return;
      }
#endif

#ifdef SUNSHINE_BUILD_WAYLAND
      // Generic wlroots path first: sway, river, wayfire and friends all speak
      // it, and it is the only protocol that says which toplevel is active.
      if (wlr_toplevels_available()) {
        std::thread(wlr_worker).detach();
        return;
      }
#endif

      // niri ships its own IPC instead of wlr-foreign-toplevel-management; the
      // query is probed so an installed-but-not-running niri does not win.
      if (display_device::vdd_utils::run_logged("niri msg --json focused-window", kNiriCommandTimeout).exit_code == 0) {
        std::thread(niri_worker).detach();
        return;
      }

#ifdef SUNSHINE_BUILD_X11
      // Last resort: an X11 session, or XWayland on a compositor that offers no
      // focus interface at all.
      if (std::getenv("DISPLAY")) {
        std::thread(x11_worker).detach();
        return;
      }
#endif

      // Nothing recognised. Keep the KWin worker trying: a Plasma session that
      // starts later still gets picked up by its retry loop.
#if defined(SUNSHINE_HAVE_SDBUS)
      std::thread(worker).detach();
#endif
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
