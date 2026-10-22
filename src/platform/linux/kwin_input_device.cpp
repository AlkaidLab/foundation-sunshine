#include "src/platform/linux/kwin_input_device.h"

#include "src/logging.h"
#include "src/platform/linux/sdbus_session.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace std::string_view_literals;

namespace platf::kwin_input {

  namespace {
    /// Read a /sys attribute, dropping the trailing newline. Empty on failure.
    std::string
    read_attr(const std::filesystem::path &path) {
      std::ifstream file { path };
      if (!file) {
        return {};
      }
      std::string value;
      std::getline(file, value);
      while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
        value.pop_back();
      }
      return value;
    }

    /// "card1-DP-2" -> "DP-2". Anything without a dash is returned unchanged.
    std::string
    connector_from_entry(const std::string &entry) {
      const auto dash = entry.find('-');
      return dash == std::string::npos ? entry : entry.substr(dash + 1);
    }

    /// Name of the first /sys/class/input entry whose device name matches.
    std::vector<std::pair<std::string, std::string>>
    scan_input_nodes() {
      std::vector<std::pair<std::string, std::string>> nodes;
      std::error_code ec;
      const std::filesystem::path root { "/sys/class/input" };
      for (const auto &entry : std::filesystem::directory_iterator { root, ec }) {
        const auto name = entry.path().filename().string();
        if (name.rfind("event", 0) != 0) {
          continue;
        }
        nodes.emplace_back(name, read_attr(entry.path() / "device/name"));
      }
      return nodes;
    }
  }  // namespace

  mapping_t
  plan_mapping(const std::vector<std::string> &enabled_outputs, const std::string &streamed_output) {
    // Windows parity: absolute touch input is expressed over the whole virtual desktop,
    // so the device must not be confined to a single output.
    mapping_t mapping;
    mapping.map_to_workspace = true;

    if (!streamed_output.empty() &&
        std::find(enabled_outputs.begin(), enabled_outputs.end(), streamed_output) != enabled_outputs.end()) {
      mapping.output_name = streamed_output;
    } else if (!enabled_outputs.empty()) {
      // The name only has to point at *an* enabled output: with map_to_workspace the
      // coordinates span the workspace anyway, but KWin drops touch events outright when
      // the device has no output at all.
      mapping.output_name = enabled_outputs.front();
    }
    return mapping;
  }

  std::vector<std::string>
  collect_matching_nodes(const std::vector<std::pair<std::string, std::string>> &nodes, std::string_view device_name) {
    std::vector<std::string> matched;
    for (const auto &[node, name] : nodes) {
      if (name == device_name) {
        matched.push_back(node);
      }
    }
    std::sort(matched.begin(), matched.end());
    return matched;
  }

  std::vector<std::string>
  enabled_outputs() {
    std::vector<std::string> outputs;
    std::error_code ec;
    const std::filesystem::path root { "/sys/class/drm" };
    for (const auto &entry : std::filesystem::directory_iterator { root, ec }) {
      const auto name = entry.path().filename().string();
      if (name.rfind("card", 0) != 0 || name.find('-') == std::string::npos) {
        continue;
      }
      if (read_attr(entry.path() / "status") != "connected" || read_attr(entry.path() / "enabled") != "enabled") {
        continue;
      }
      outputs.push_back(connector_from_entry(name));
    }
    std::sort(outputs.begin(), outputs.end());
    return outputs;
  }

  std::vector<std::string>
  find_event_nodes(std::string_view device_name) {
    return collect_matching_nodes(scan_input_nodes(), device_name);
  }

#if defined(SUNSHINE_HAVE_SDBUS)

  namespace {
    /// Owns a user-session bus for the duration of one call. sd-bus is not thread safe,
    /// so every call gets its own connection (same pattern as the clipboard host).
    struct bus_t {
      sd_bus *bus = nullptr;
      bus_t(): bus { sdbus::open_user_bus() } {}
      ~bus_t() {
        if (bus) {
          sd_bus_unref(bus);
        }
      }
      explicit operator bool() const {
        return bus != nullptr;
      }
    };

    /// Write one property on a KWin input-device object.
    bool
    set_property(sd_bus *bus, const std::string &path, const char *property, const char *signature, auto value, std::string *error) {
      sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
      const int rc = sd_bus_set_property(bus, "org.kde.KWin", path.c_str(), "org.kde.KWin.InputDevice", property, &dbus_error, signature, value);
      if (rc < 0) {
        if (error) {
          *error = std::string { property } + ": " +
                   (dbus_error.message ? dbus_error.message : std::strerror(-rc));
        }
        sd_bus_error_free(&dbus_error);
        return false;
      }
      sd_bus_error_free(&dbus_error);
      return true;
    }
  }  // namespace

  bool
  available() {
    bus_t session;
    if (!session) {
      return false;
    }

    sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    // sd-bus has no "name is owned" helper; asking the bus daemon is the documented way.
    const int rc = sd_bus_call_method(session.bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                      "org.freedesktop.DBus", "GetNameOwner", &dbus_error, &reply, "s", "org.kde.KWin");
    if (reply) {
      sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&dbus_error);
    return rc >= 0;
  }

  bool
  apply(const std::string &event_node, const mapping_t &mapping, std::string *error) {
    bus_t session;
    if (!session) {
      if (error) {
        *error = "no session bus"sv;
      }
      return false;
    }

    const std::string path = "/org/kde/KWin/InputDevice/" + event_node;
    bool ok = true;
    if (!mapping.output_name.empty()) {
      ok = set_property(session.bus, path, "outputName", "s", mapping.output_name.c_str(), error) && ok;
    }
    ok = set_property(session.bus, path, "mapToWorkspace", "b", static_cast<int>(mapping.map_to_workspace), error) && ok;
    return ok;
  }

  void
  sync_our_touch_devices(const std::string &streamed_output) {
    const auto nodes = find_event_nodes(kTouchDeviceName);
    if (nodes.empty()) {
      // The device is created per client; nothing to do before the first session.
      return;
    }

    if (!available()) {
      BOOST_LOG(debug) << "KWin touch mapping: compositor not on the session bus, leaving the device alone"sv;
      return;
    }

    const auto outputs = enabled_outputs();
    const auto mapping = plan_mapping(outputs, streamed_output);
    for (const auto &node : nodes) {
      std::string error;
      if (apply(node, mapping, &error)) {
        BOOST_LOG(info) << "KWin touch mapping: "sv << node << " -> output "sv
                        << mapping.output_name << " (workspace-wide coordinates)"sv;
      } else {
        // Not fatal: an older KWin may not expose the property, and the session still
        // works for pointer input.
        BOOST_LOG(debug) << "KWin touch mapping: could not configure "sv << node << ": "sv << error;
      }
    }
  }

#else

  bool
  available() {
    return false;
  }

  bool
  apply(const std::string &event_node, const mapping_t &mapping, std::string *error) {
    (void) event_node;
    (void) mapping;
    if (error) {
      *error = "built without sd-bus"sv;
    }
    return false;
  }

  void
  sync_our_touch_devices(const std::string &streamed_output) {
    // Built without libsystemd: there is no session bus to talk to KWin over.
    (void) streamed_output;
  }

#endif

}  // namespace platf::kwin_input
