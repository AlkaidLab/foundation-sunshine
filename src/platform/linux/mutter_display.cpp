#include "src/platform/linux/mutter_display.h"

#include "src/logging.h"
#include "src/platform/linux/sdbus_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <unistd.h>

using namespace std::chrono_literals;
using namespace std::string_view_literals;

namespace platf::mutter {

#if defined(SUNSHINE_HAVE_SDBUS)

  namespace {
    /// Guards the probe cooldown. Queries happen from the display-settings
    /// thread, but a tray-triggered status read can race with them.
    std::mutex state_mu;
    std::chrono::steady_clock::time_point cooldown_until {};
    bool unavailable_logged = false;

    /// Owns a user-session bus for the duration of one call.
    class bus_t {
    public:
      bus_t():
          handle(platf::sdbus::open_user_bus()) {
        if (handle) {
          // Bound every blocking call: Mutter answers from memory, and a dead
          // or wedged compositor must not stall the settings thread for the
          // sd-bus default of 25 s.
          sd_bus_set_method_call_timeout(handle, static_cast<std::uint64_t>(kProbeTimeout.count() * 1000));
        }
      }

      ~bus_t() {
        if (handle) {
          sd_bus_unref(handle);
        }
      }

      bus_t(const bus_t &) = delete;
      bus_t &operator=(const bus_t &) = delete;

      explicit operator bool() const {
        return handle != nullptr;
      }

      sd_bus *get() const {
        return handle;
      }

    private:
      sd_bus *handle;
    };

    /**
     * @brief Read one `a{sv}` map of scalars, keeping only the keys we know.
     * @details Unknown keys are skipped: Mutter adds properties over time and a
     *          new one must not make the whole state unreadable.
     */
    bool
    read_properties(sd_bus_message *message, monitor_t &monitor) {
      int rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
      if (rc < 0) {
        return false;
      }

      while ((rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *key = nullptr;
        if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &key) < 0 || !key) {
          return false;
        }

        const std::string name { key };
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, nullptr) < 0) {
          return false;
        }

        if (name == "display-name") {
          const char *value = nullptr;
          if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &value) >= 0 && value) {
            monitor.display_name = value;
          }
        }
        else if (name == "color-mode") {
          std::uint32_t value = 0;
          if (sd_bus_message_read_basic(message, SD_BUS_TYPE_UINT32, &value) >= 0) {
            monitor.color_mode = value;
          }
        }
        else if (name == "supported-color-modes") {
          monitor.supported_color_modes.clear();
          if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "u") >= 0) {
            std::uint32_t value = 0;
            while (sd_bus_message_read_basic(message, SD_BUS_TYPE_UINT32, &value) > 0) {
              monitor.supported_color_modes.push_back(value);
            }
            sd_bus_message_exit_container(message);
          }
        }
        else {
          sd_bus_message_skip(message, nullptr);
        }

        sd_bus_message_exit_container(message);  // variant
        sd_bus_message_exit_container(message);  // dict entry
      }

      if (rc < 0) {
        return false;
      }
      return sd_bus_message_exit_container(message) >= 0;
    }

    /**
     * @brief Read the `is-current` / `is-preferred` flags of one mode.
     */
    bool
    read_mode_flags(sd_bus_message *message, mode_t &mode) {
      int rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
      if (rc < 0) {
        return false;
      }

      while ((rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *key = nullptr;
        if (sd_bus_message_read_basic(message, SD_BUS_TYPE_STRING, &key) < 0 || !key) {
          return false;
        }

        const std::string name { key };
        if (sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT, nullptr) < 0) {
          return false;
        }

        int value = 0;
        if (name == "is-current") {
          if (sd_bus_message_read_basic(message, SD_BUS_TYPE_BOOLEAN, &value) >= 0) {
            mode.current = value != 0;
          }
        }
        else if (name == "is-preferred") {
          if (sd_bus_message_read_basic(message, SD_BUS_TYPE_BOOLEAN, &value) >= 0) {
            mode.preferred = value != 0;
          }
        }
        else {
          sd_bus_message_skip(message, nullptr);
        }

        sd_bus_message_exit_container(message);
        sd_bus_message_exit_container(message);
      }

      if (rc < 0) {
        return false;
      }
      return sd_bus_message_exit_container(message) >= 0;
    }

    /// Read the `a(ssss)` list of connectors a logical monitor shows.
    bool
    read_logical_connectors(sd_bus_message *message, logical_monitor_t &logical) {
      int rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(ssss)");
      if (rc < 0) {
        return false;
      }

      while ((rc = sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "ssss")) > 0) {
        const char *connector = nullptr;
        const char *vendor = nullptr;
        const char *product = nullptr;
        const char *serial = nullptr;
        if (sd_bus_message_read(message, "ssss", &connector, &vendor, &product, &serial) < 0) {
          return false;
        }
        if (connector && *connector) {
          logical.connectors.emplace_back(connector);
        }
        sd_bus_message_exit_container(message);
      }

      if (rc < 0) {
        return false;
      }
      return sd_bus_message_exit_container(message) >= 0;
    }

    /// Look up the mode a monitor should be configured with when applying.
    const mode_t *
    mode_for_apply(const monitor_t &monitor) {
      if (const auto *mode = current_mode(monitor)) {
        return mode;
      }
      const auto preferred = std::find_if(monitor.modes.begin(), monitor.modes.end(), [](const auto &candidate) {
        return candidate.preferred;
      });
      if (preferred != monitor.modes.end()) {
        return &*preferred;
      }
      return monitor.modes.empty() ? nullptr : &monitor.modes.front();
    }

    bool
    append_connector(sd_bus_message *message, const monitor_t &monitor) {
      const auto *mode = mode_for_apply(monitor);
      if (!mode) {
        BOOST_LOG(warning) << "Mutter: monitor " << monitor.connector << " has no mode to apply";
        return false;
      }

      int rc = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "ssa{sv}");
      if (rc < 0) {
        return false;
      }

      rc = sd_bus_message_append(message, "ss", monitor.connector.c_str(), mode->id.c_str());
      if (rc < 0) {
        return false;
      }

      rc = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
      if (rc < 0) {
        return false;
      }

      // Absence of the property means the default colour mode, so only a
      // non-default one has to be sent. That also covers turning HDR back off:
      // leaving it out resets it.
      if (monitor.color_mode != kColorModeDefault) {
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv");
        if (rc < 0) {
          return false;
        }
        rc = sd_bus_message_append(message, "s", "color-mode");
        if (rc < 0) {
          return false;
        }
        rc = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "u");
        if (rc < 0) {
          return false;
        }
        rc = sd_bus_message_append(message, "u", monitor.color_mode);
        if (rc < 0) {
          return false;
        }
        sd_bus_message_close_container(message);  // variant
        sd_bus_message_close_container(message);  // dict entry
      }

      rc = sd_bus_message_close_container(message);  // a{sv}
      if (rc < 0) {
        return false;
      }
      return sd_bus_message_close_container(message) >= 0;  // struct
    }
  }  // namespace

  bool
  session_supports_mutter() {
    if (!probe_allowed()) {
      return false;
    }

    bus_t bus;
    if (!bus) {
      note_unavailable("no user session bus");
      return false;
    }

    // Ask the bus driver who owns the name instead of activating it: on a
    // non-GNOME session an activation attempt would either spawn nothing or
    // wait for a service that never appears, burning the probe timeout once
    // per query. GetNameOwner fails immediately when nobody owns the name.
    sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    const int rc = sd_bus_call_method(bus.get(), "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetNameOwner", &dbus_error, &reply, "s", kService);
    if (reply) {
      sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&dbus_error);

    if (rc < 0) {
      note_unavailable("org.gnome.Mutter.DisplayConfig is not on the bus");
      return false;
    }

    note_available();
    return true;
  }

  bool
  probe_allowed() {
    std::lock_guard<std::mutex> lock { state_mu };
    return std::chrono::steady_clock::now() >= cooldown_until;
  }

  void
  note_available() {
    std::lock_guard<std::mutex> lock { state_mu };
    cooldown_until = {};
    unavailable_logged = false;
  }

  void
  note_unavailable(const std::string &reason) {
    std::lock_guard<std::mutex> lock { state_mu };
    cooldown_until = std::chrono::steady_clock::now() + kRetryCooldown;
    if (!unavailable_logged) {
      unavailable_logged = true;
      BOOST_LOG(debug) << "Mutter display configuration unavailable (" << reason << ")";
    }
  }

  bool
  read_state(sd_bus_message *reply, state_t &out) {
    if (!reply) {
      return false;
    }

    state_t state;

    // A reply body is a tuple of the out arguments, and sd-bus hands its
    // members out directly: entering the root container is neither needed nor
    // possible. The fields are read in signature order.
    std::uint32_t serial = 0;
    if (sd_bus_message_read(reply, "u", &serial) < 0) {
      return false;
    }
    state.serial = serial;

    int rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "((ssss)a(siiddada{sv})a{sv})");
    if (rc < 0) {
      return false;
    }

    while ((rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "(ssss)a(siiddada{sv})a{sv}")) > 0) {
      monitor_t monitor;

      if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "ssss") < 0) {
        return false;
      }
      const char *connector = nullptr;
      const char *vendor = nullptr;
      const char *product = nullptr;
      const char *monitor_serial = nullptr;
      if (sd_bus_message_read(reply, "ssss", &connector, &vendor, &product, &monitor_serial) < 0) {
        return false;
      }
      monitor.connector = connector ? connector : "";
      monitor.vendor = vendor ? vendor : "";
      monitor.product = product ? product : "";
      monitor.serial = monitor_serial ? monitor_serial : "";
      sd_bus_message_exit_container(reply);

      int modes_rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(siiddada{sv})");
      if (modes_rc < 0) {
        return false;
      }

      while ((modes_rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "siiddada{sv}")) > 0) {
        mode_t mode;

        const char *id = nullptr;
        std::int32_t width = 0;
        std::int32_t height = 0;
        double refresh = 0.0;
        double preferred_scale = 0.0;
        if (sd_bus_message_read(reply, "s", &id) < 0 ||
            sd_bus_message_read(reply, "ii", &width, &height) < 0 ||
            sd_bus_message_read(reply, "dd", &refresh, &preferred_scale) < 0) {
          return false;
        }
        mode.id = id ? id : "";
        mode.width = width;
        mode.height = height;
        mode.refresh = refresh;

        // Supported scales are irrelevant here: Sunshine changes one output's
        // mode, and a scale the compositor insists on shows up as a rejected
        // apply, which the caller detects by re-reading the state.
        if (sd_bus_message_skip(reply, "ad") < 0) {
          return false;
        }
        if (!read_mode_flags(reply, mode)) {
          return false;
        }

        sd_bus_message_exit_container(reply);
        monitor.modes.push_back(std::move(mode));
      }

      if (modes_rc < 0 || sd_bus_message_exit_container(reply) < 0) {
        return false;
      }
      if (!read_properties(reply, monitor)) {
        return false;
      }
      if (sd_bus_message_exit_container(reply) < 0) {
        return false;
      }

      state.monitors.push_back(std::move(monitor));
    }

    if (rc < 0 || sd_bus_message_exit_container(reply) < 0) {
      return false;
    }

    rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(iiduba(ssss)a{sv})");
    if (rc < 0) {
      return false;
    }

    while ((rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "iiduba(ssss)a{sv}")) > 0) {
      logical_monitor_t logical;
      std::int32_t x = 0;
      std::int32_t y = 0;
      double scale = 1.0;
      std::uint32_t transform = 0;
      int primary = 0;
      if (sd_bus_message_read(reply, "iid", &x, &y, &scale) < 0 ||
          sd_bus_message_read(reply, "u", &transform) < 0 ||
          sd_bus_message_read(reply, "b", &primary) < 0) {
        return false;
      }
      logical.x = x;
      logical.y = y;
      logical.scale = scale;
      logical.transform = transform;
      logical.primary = primary != 0;

      if (!read_logical_connectors(reply, logical)) {
        return false;
      }
      // Logical monitor properties ("is-presentation", "is-underscanning")
      // are not used here.
      if (sd_bus_message_skip(reply, "a{sv}") < 0) {
        return false;
      }
      if (sd_bus_message_exit_container(reply) < 0) {
        return false;
      }

      state.logical_monitors.push_back(std::move(logical));
    }

    if (rc < 0 || sd_bus_message_exit_container(reply) < 0) {
      return false;
    }

    // Global properties ("layout-mode", ...) are not used here.
    if (sd_bus_message_skip(reply, "a{sv}") < 0) {
      return false;
    }

    out = std::move(state);
    return true;
  }

  std::optional<state_t>
  query_state() {
    if (!probe_allowed()) {
      return std::nullopt;
    }

    bus_t bus;
    if (!bus) {
      note_unavailable("no user session bus");
      return std::nullopt;
    }

    sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    const int rc = sd_bus_call_method(bus.get(), kService, kPath, kInterface, "GetCurrentState",
      &dbus_error, &reply, nullptr);
    if (rc < 0) {
      note_unavailable(dbus_error.message ? dbus_error.message : std::strerror(-rc));
      sd_bus_error_free(&dbus_error);
      return std::nullopt;
    }

    state_t state;
    const bool ok = read_state(reply, state);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&dbus_error);

    if (!ok) {
      note_unavailable("unparsable GetCurrentState reply");
      return std::nullopt;
    }

    note_available();
    return state;
  }

  bool
  append_apply_args(sd_bus_message *message, const state_t &state) {
    if (!message) {
      return false;
    }

    int rc = sd_bus_message_append(message, "uu", state.serial, kMethodPersistent);
    if (rc < 0) {
      return false;
    }

    rc = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(iiduba(ssa{sv}))");
    if (rc < 0) {
      return false;
    }

    for (const auto &logical : state.logical_monitors) {
      if (logical.connectors.empty()) {
        continue;
      }

      rc = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "iiduba(ssa{sv})");
      if (rc < 0) {
        return false;
      }
      rc = sd_bus_message_append(message, "iidub", logical.x, logical.y, logical.scale,
        logical.transform, static_cast<int>(logical.primary));
      if (rc < 0) {
        return false;
      }

      rc = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(ssa{sv})");
      if (rc < 0) {
        return false;
      }

      for (const auto &connector : logical.connectors) {
        const auto *monitor = find_monitor(state, connector);
        if (!monitor) {
          BOOST_LOG(warning) << "Mutter: logical monitor references unknown connector " << connector;
          return false;
        }
        if (!append_connector(message, *monitor)) {
          return false;
        }
      }

      rc = sd_bus_message_close_container(message);  // a(ssa{sv})
      if (rc < 0) {
        return false;
      }
      rc = sd_bus_message_close_container(message);  // struct
      if (rc < 0) {
        return false;
      }
    }

    rc = sd_bus_message_close_container(message);  // a(iiduba(ssa{sv}))
    if (rc < 0) {
      return false;
    }

    rc = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (rc < 0) {
      return false;
    }
    return sd_bus_message_close_container(message) >= 0;
  }

  bool
  apply(const state_t &state) {
    bus_t bus;
    if (!bus) {
      note_unavailable("no user session bus");
      return false;
    }

    sd_bus_message *call = nullptr;
    int rc = sd_bus_message_new_method_call(bus.get(), &call, kService, kPath, kInterface,
      "ApplyMonitorsConfig");
    if (rc < 0) {
      note_unavailable(std::strerror(-rc));
      return false;
    }

    if (!append_apply_args(call, state)) {
      sd_bus_message_unref(call);
      BOOST_LOG(error) << "Mutter: could not build the ApplyMonitorsConfig arguments";
      return false;
    }

    sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    const int call_rc = sd_bus_call(bus.get(), call, static_cast<std::uint64_t>(kChangeTimeout.count() * 1000),
      &dbus_error, &reply);

    if (reply) {
      sd_bus_message_unref(reply);
    }
    sd_bus_message_unref(call);

    if (call_rc < 0) {
      BOOST_LOG(error) << "Mutter: ApplyMonitorsConfig failed: "
                       << (dbus_error.message ? dbus_error.message : std::strerror(-call_rc));
      sd_bus_error_free(&dbus_error);
      return false;
    }

    sd_bus_error_free(&dbus_error);
    return true;
  }

#else  // !SUNSHINE_HAVE_SDBUS

  // Without libsystemd there is no sd-bus to reach org.gnome.Mutter.DisplayConfig
  // with, so the whole backend reports "not here" and every caller falls through
  // to the next one in display_device.cpp. The state helpers below stay real:
  // they are pure functions over state_t and carry no bus dependency.

  bool
  session_supports_mutter() {
    return false;
  }

  bool
  probe_allowed() {
    return false;
  }

  void
  note_available() {
  }

  void
  note_unavailable(const std::string &reason) {
    // Say it once, the way the sd-bus half does, so a user on a system without
    // libsystemd can tell why GNOME display control is missing.
    static std::atomic<bool> logged { false };
    if (!logged.exchange(true)) {
      BOOST_LOG(info) << "Mutter: unavailable (" << reason << "); built without libsystemd";
    }
  }

  bool
  read_state(sd_bus_message *reply, state_t &out) {
    (void) reply;
    (void) out;
    return false;
  }

  std::optional<state_t>
  query_state() {
    return std::nullopt;
  }

  bool
  append_apply_args(sd_bus_message *message, const state_t &state) {
    (void) message;
    (void) state;
    return false;
  }

  bool
  apply(const state_t &state) {
    (void) state;
    return false;
  }

#endif  // SUNSHINE_HAVE_SDBUS

  monitor_t *
  find_monitor(state_t &state, const std::string &connector) {
    const auto it = std::find_if(state.monitors.begin(), state.monitors.end(), [&connector](const auto &monitor) {
      return monitor.connector == connector;
    });
    return it == state.monitors.end() ? nullptr : &*it;
  }

  const monitor_t *
  find_monitor(const state_t &state, const std::string &connector) {
    return find_monitor(const_cast<state_t &>(state), connector);
  }

  const logical_monitor_t *
  find_logical_monitor(const state_t &state, const std::string &connector) {
    return find_logical_monitor(const_cast<state_t &>(state), connector);
  }

  logical_monitor_t *
  find_logical_monitor(state_t &state, const std::string &connector) {
    const auto it = std::find_if(state.logical_monitors.begin(), state.logical_monitors.end(),
      [&connector](const auto &logical) {
        return std::find(logical.connectors.begin(), logical.connectors.end(), connector) != logical.connectors.end();
      });
    return it == state.logical_monitors.end() ? nullptr : &*it;
  }

  const mode_t *
  current_mode(const monitor_t &monitor) {
    const auto it = std::find_if(monitor.modes.begin(), monitor.modes.end(), [](const auto &mode) {
      return mode.current;
    });
    return it == monitor.modes.end() ? nullptr : &*it;
  }

  const mode_t *
  matching_mode(const monitor_t &monitor, int width, int height, double refresh) {
    // The compositor's own rate is a double; compare with the tolerance the
    // rest of the display code uses for rates that differ in the last digits.
    constexpr double kToleranceHz = 0.05;
    const auto it = std::find_if(monitor.modes.begin(), monitor.modes.end(),
      [width, height, refresh](const auto &mode) {
        return mode.width == width && mode.height == height &&
               std::fabs(mode.refresh - refresh) <= kToleranceHz;
      });
    return it == monitor.modes.end() ? nullptr : &*it;
  }

}  // namespace platf::mutter
