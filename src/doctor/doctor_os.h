/**
 * @file src/doctor/doctor_os.h
 * @brief Read-only operating system facts for the health check: time sync, pending restart, crash records,
 *        account, device nodes, audio endpoints, drivers.
 * @details Depends on the standard library and the OS headers only, so the Windows half can be syntax-checked
 *          on its own.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace doctor::os {
  struct time_sync_t {
    bool checked = false;
    bool synced = false;
    std::string source;  ///< "NTP time.windows.com", "kernel NTP"
    std::string detail;  ///< "error ±12 ms", "service stopped (trigger start)"
  };

  time_sync_t
  time_sync();

  /** Codes: "cbs" (Windows component servicing), "wu" (Windows Update), "file_rename", "packages" (Debian). */
  struct pending_reboot_t {
    bool checked = false;
    std::vector<std::string> reasons;
  };

  pending_reboot_t
  pending_reboot();

  struct crashes_t {
    bool checked = false;
    std::string source;  ///< "WER", "systemd-coredump"
    std::vector<std::int64_t> ages_s;  ///< age of each record within the window, newest first
  };

  crashes_t
  recent_crashes(std::int64_t window_s);

  /** "system", "admin", "user" (Windows); "root", "user" (POSIX). Empty when it could not be read. */
  std::string
  account_kind();

  struct access_t {
    bool checked = false;
    bool ok = false;
    std::string error;
  };

  /** Write access to a device node or folder, without opening it for writing. */
  access_t
  writable(const std::string &path);

  struct audio_endpoint_t {
    std::string id;
    std::string name;  ///< friendly name, "Speakers (Realtek(R) Audio)"
    std::string description;  ///< "Speakers"
    std::string adapter;  ///< "Realtek(R) Audio"
    bool is_default = false;
  };

  struct audio_endpoints_t {
    bool checked = false;
    std::string error;
    std::vector<audio_endpoint_t> render;  ///< active render endpoints
  };

  /** Active render endpoints (Windows); the POSIX build reports checked=false. */
  audio_endpoints_t
  audio_endpoints();

  /** True when one field of an endpoint equals the configured name, the way Sunshine matches a sink. */
  bool
  endpoint_matches(const audio_endpoint_t &endpoint, const std::string &configured);

  struct usbip_t {
    bool supported = false;  ///< the platform has a USB forwarding backend
    bool tool_found = false;
    std::string tool_path;
    bool driver_checked = false;
    bool driver_found = false;
    std::string driver;  ///< service name found, "usbip2_ude"
  };

  usbip_t
  usbip();

  /** Driver version of the ZakoDisplay adapter (ROOT\ZAKOVDD); empty when absent or unreadable. */
  std::string
  vdd_driver_version();

  /** Windows build number from RtlGetVersion. */
  std::optional<int>
  windows_build();
}  // namespace doctor::os
