// local includes
#include "src/display_device/settings.h"
#include "src/display_device/vdd_utils.h"
#include "src/globals.h"

#include <filesystem>
#include <fstream>

namespace display_device {

  device_info_map_t
  enum_available_devices() {
    // Enumerate live DRM connectors from sysfs. The session flow matches
    // displays by connector name ("DP-1", "eDP-1", ...), and the headless /
    // stale-VDD checks rely on physical connectors being visible here.
    namespace fs = std::filesystem;

    device_info_map_t devices;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator { "/sys/class/drm", ec }) {
      const auto name = entry.path().filename().string();
      if (name.rfind("card", 0) != 0) {
        continue;
      }
      const auto card_end = name.find('-');
      if (card_end == std::string::npos) {
        continue;
      }
      const std::string connector = name.substr(card_end + 1);

      std::ifstream status_file { entry.path() / "status" };
      std::string status;
      std::getline(status_file, status);

      // "off" is a connector WE powered down for an exclusive virtual display
      // session. It must stay visible (as inactive) - otherwise the session
      // teardown's headless-host guard sees only the virtual display and
      // skips restoring the physical screen.
      const bool connected = status == "connected";
      const bool forced_off = status == "off";
      if (!connected && !forced_off) {
        continue;
      }

      device_info_t info;
      info.display_name = connector;
      info.friendly_name = connector;
      info.device_state = connected ? device_state_e::active : device_state_e::inactive;
      info.hdr_state = hdr_state_e::unknown;
      devices.emplace(connector, std::move(info));
    }

    return devices;
  }

  std::string
  get_display_name(const std::string &value) {
    // Not implemented, but just passthrough the value
    return value;
  }

  device_display_mode_map_t
  get_current_display_modes(const std::unordered_set<std::string> &) {
    // Not implemented
    return {};
  }

  bool
  set_display_modes(const device_display_mode_map_t &) {
    // Not implemented
    return false;
  }

  bool
  is_primary_device(const std::string &) {
    // Not implemented
    return false;
  }

  bool
  set_as_primary_device(const std::string &) {
    // Not implemented
    return false;
  }

  hdr_state_map_t
  get_current_hdr_states(const std::unordered_set<std::string> &) {
    // Not implemented
    return {};
  }

  bool
  set_hdr_states(const hdr_state_map_t &) {
    // Not implemented
    return false;
  }

  active_topology_t
  get_current_topology() {
    // Not implemented
    return {};
  }

  bool
  is_topology_valid(const active_topology_t &topology) {
    // Not implemented
    return false;
  }

  bool
  is_topology_the_same(const active_topology_t &a, const active_topology_t &b) {
    // Not implemented
    return false;
  }

  bool
  set_topology(const active_topology_t &) {
    // Not implemented
    return false;
  }

  struct settings_t::audio_data_t {
    // Not implemented
  };

  struct settings_t::persistent_data_t {
    // Not implemented
  };

  settings_t::settings_t() {
    // Not implemented
  }

  settings_t::~settings_t() {
    // Not implemented
  }

  bool
  settings_t::is_changing_settings_going_to_fail() const {
    // Not implemented
    return false;
  }

  settings_t::apply_result_t
  settings_t::apply_config(
    const parsed_config_t &,
    const rtsp_stream::launch_session_t &,
    const boost::optional<active_topology_t> &) {
    // Not implemented
    return { apply_result_t::result_e::success };
  }

  void
  settings_t::capture_audio_sink() {
    // Not implemented
  }

  void
  settings_t::release_audio_sink() {
    // Not implemented
  }

  bool
  settings_t::has_persistent_data() const {
    // Not implemented
    return false;
  }

  void
  settings_t::remove_vdd_from_initial_topology(const std::string &) {
    // Not implemented
  }

  void
  settings_t::replace_vdd_id(const std::string &, const std::string &) {
    // Not implemented
  }

  std::string
  find_one_of_the_available_devices(const std::string &device_id) {
    // The session flow may hand us a device id, an OS display name or a
    // friendly name; resolve all three against the live connector list. The
    // ZakoVDD friendly name maps to the virtual display when one is live.
    if (device_id.empty()) {
      return {};
    }
    if (device_id == ZAKO_NAME) {
      return vdd_utils::live_virtual_display_connector();
    }

    const auto devices = enum_available_devices();
    if (devices.count(device_id)) {
      return device_id;
    }
    for (const auto &[id, info] : devices) {
      if (info.display_name == device_id || info.friendly_name == device_id) {
        return id;
      }
    }
    return {};
  }

  std::string
  find_device_by_friendlyname(const std::string &friendly_name) {
    if (friendly_name != ZAKO_NAME) {
      return {};
    }
    return vdd_utils::live_virtual_display_connector();
  }

  bool
  settings_t::revert_settings(revert_reason_e reason, bool skip_vdd_destroy) {
    // Not implemented
    (void)reason;  // Unused parameter
    (void)skip_vdd_destroy;  // Unused parameter
    return true;
  }

  void
  settings_t::reset_persistence() {
    // Not implemented
  }

}  // namespace display_device
