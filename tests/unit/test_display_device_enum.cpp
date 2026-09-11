/**
 * @file tests/unit/test_display_device_enum.cpp
 * @brief Test the device enumeration semantics of the Linux display backend.
 *
 * The bug this pins: `active` used to mean "the connector is plugged in", so a
 * monitor the user had disabled in the desktop was reported active and the VDD
 * preservation logic switched it back on. Windows reports
 * DISPLAYCONFIG_PATH_ACTIVE, i.e. an assigned/enabled path, and the Linux
 * backend now reads DRM's sysfs `enabled` attribute for the same meaning.
 *
 * The assertion is deliberately one-directional: a connector whose sysfs
 * `enabled` says "disabled" must never be reported active. Whether an
 * unreadable attribute falls back to the connected-based view is left to the
 * implementation, so the test stays valid on kernels without the attribute.
 */
#include <src/display_device/display_device.h>
#include <src/display_device/vdd_utils.h>

#include "../tests_common.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#ifndef _WIN32
namespace {
  /**
   * @brief The DRM sysfs `enabled` value for a connector, if readable.
   */
  std::optional<std::string>
  sysfs_enabled_state(const std::string &connector) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator { "/sys/class/drm", ec }) {
      const auto name = entry.path().filename().string();
      const auto dash = name.find('-');
      if (dash == std::string::npos || name.compare(0, 4, "card") != 0) {
        continue;
      }
      if (name.substr(dash + 1) != connector) {
        continue;
      }

      std::ifstream file { entry.path() / "enabled" };
      std::string value;
      if (std::getline(file, value) && !value.empty()) {
        return value;
      }
    }
    return std::nullopt;
  }
}  // namespace

TEST(DisplayDeviceEnum, ActiveRequiresAnEnabledConnector) {
  const auto devices = display_device::enum_available_devices();
  // The virtual display's path is assigned by the Sunshine backend rather than
  // by the desktop, so it is exempt from the sysfs rule.
  const auto live_vdd = display_device::vdd_utils::live_virtual_display_connector();

  for (const auto &[id, info] : devices) {
    if (id == live_vdd) {
      continue;
    }
    const auto enabled = sysfs_enabled_state(id);
    if (!enabled.has_value()) {
      continue;  // kernel without the attribute: nothing to assert
    }

    if (*enabled != "enabled") {
      EXPECT_NE(info.device_state, display_device::device_state_e::active)
        << id << " is not enabled in sysfs but was reported active";
    }
  }
}

TEST(DisplayDeviceEnum, DisabledConnectorsStayVisibleAsInactive) {
  // A connected-but-disabled connector must still be enumerated (the VDD
  // teardown's headless guard and the offlined-physical checks rely on seeing
  // it), just not as active: the other half of the rule above.
  const auto devices = display_device::enum_available_devices();
  const auto live_vdd = display_device::vdd_utils::live_virtual_display_connector();

  for (const auto &[id, info] : devices) {
    if (id == live_vdd) {
      continue;
    }
    const auto enabled = sysfs_enabled_state(id);
    if (enabled.has_value() && *enabled == "enabled") {
      continue;
    }
    EXPECT_EQ(info.device_state, display_device::device_state_e::inactive)
      << id << " should be visible but inactive";
  }
}
#endif  // !_WIN32
