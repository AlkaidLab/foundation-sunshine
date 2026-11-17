/**
 * @file config_runtime_stubs.cpp
 * @brief Stand-ins for the host entry points that the real config.cpp links.
 *
 * The configuration suites compile config.cpp on its own: the loggers, the
 * appdata directory and the few platform calls below are all it reaches.
 */
#include "test_temp_paths.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "src/config.h"
#include "src/display_device/display_device.h"
#include "src/globals.h"
#include "src/platform/common.h"

// config.cpp and file_handler.cpp reference these severity loggers and a few
// platform entry points that the host build supplies; provide stand-ins.
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

// Globals from globals.cpp that config.cpp references; the host values are
// irrelevant to these tests.
bool display_cursor = true;
bool is_running_as_system_user = false;

namespace platf {
  std::filesystem::path
  appdata() {
    // One directory per process: config_runtime_unit_tests and config_upgrade_unit_tests both link this file, and ctest -j runs them together.
    static const test_support::ScopedTempDir directory { "sunshine_config_runtime_tests" };
    return directory.path();
  }

  std::string
  get_host_name() {
    return "config-runtime-tests";
  }

  // config.cpp reads the first option as the default gamepad during static
  // initialization, so the stand-in list must never be empty.
  std::vector<supported_gamepad_t> &
  supported_gamepads(input_t *input) {
    (void) input;
    static std::vector<supported_gamepad_t> options {
      { "auto", true, "" },
      { "x360", true, "" },
      { "ds4", true, "" },
      { "ds5", true, "" },
    };
    return options;
  }

  void
  set_global_gamepad_mode(std::string_view preference) {
    (void) preference;
  }
}  // namespace platf

#ifdef _WIN32
namespace platf {
  std::wstring
  from_utf8(const std::string &string) {
    return std::filesystem::path { std::u8string { string.begin(), string.end() } }.wstring();
  }

  std::string
  to_utf8(const std::wstring &string) {
    const auto utf8 = std::filesystem::path { string }.u8string();
    return { utf8.begin(), utf8.end() };
  }
}  // namespace platf

void
launch_ui() {}

namespace service_ctrl {
  bool
  is_service_running() {
    return false;
  }

  bool
  start_service() {
    return false;
  }

  bool
  wait_for_ui_ready() {
    return false;
  }
}  // namespace service_ctrl

// globals.cpp and device_topology.cpp are not linked here; parsed_config.cpp
// only needs the names and a benign empty enumeration for these paths.
const std::string VDD_NAME = "ZakoHDR";
const std::string ZAKO_NAME = "Zako HDR";

namespace display_device {
  boost::optional<device_info_map_t>
  enum_available_devices_checked() {
    return device_info_map_t {};
  }
}  // namespace display_device
#endif

namespace logging {
  void
  print_help(const char *name) {
    (void) name;
  }
}  // namespace logging
