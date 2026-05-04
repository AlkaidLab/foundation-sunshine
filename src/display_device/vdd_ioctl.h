/**
 * @file vdd_ioctl.h
 * @brief Self-contained IOCTL transport for the ZakoVDD control channel.
 *
 * This module is the *forward-looking* transport for VDD commands.
 *
 * Co-existing with it (intentionally, transitionally) is the named-pipe
 * code in `vdd_utils.cpp`. Once every Sunshine release in the wild bundles
 * a driver build that exposes the IOCTL device interface, the pipe code
 * can be deleted in a single mechanical pass — every pipe-only call site
 * in `vdd_utils.cpp` is bracketed with `LEGACY-PIPE` markers.
 *
 * Design rationale: the entire IOCTL implementation lives in its own
 * translation unit (header + cpp), with no contamination of `vdd_utils.cpp`,
 * so the eventual cleanup is "delete pipe code from vdd_utils.cpp; collapse
 * the dispatch wrapper". The IOCTL module does not need to be touched.
 */

#pragma once

#include <string>

namespace display_device::vdd_ioctl {

  /**
   * @brief Outcome of an IOCTL transport attempt.
   *
   * Three-state so callers can distinguish "transport unavailable, fall
   * back to the legacy pipe" from "transport reached the driver but the
   * command itself failed". The latter must NOT silently retry on the
   * pipe -- the driver already saw the request and a duplicate could
   * change device state (e.g. CREATEMONITOR twice -> two phantom panels)
   * or just waste the ~6s pipe-connect timeout while the user waits.
   */
  enum class result {
    success,           ///< IOCTL completed with STATUS_SUCCESS.
    interface_missing, ///< No registered device interface (driver too old / not installed). Safe to fall back.
    failed,            ///< Driver was reached but rejected the IOCTL or returned an error. Do NOT fall back.
  };

  /**
   * @brief Send a UTF-16 command buffer to the VDD driver via IOCTL.
   *
   * Performs `SetupDiGetClassDevsW` on `GUID_DEVINTERFACE_ZAKO_VDD_CONTROL`,
   * opens the first interface instance with `CreateFileW`, and issues
   * `IOCTL_VDD_COMMAND`.
   *
   * @param command UTF-16 command string identical in grammar to the
   *                legacy pipe protocol (e.g. `L"RELOAD_DRIVER"`,
   *                `L"CREATEMONITOR {GUID}:[..]"`, `L"DESTROYMONITOR"`).
   * @return tri-state result; see `enum class result`.
   */
  result send_command(const std::wstring &command);

  /**
   * @brief Cheap liveness probe used to decide whether the IOCTL transport
   *        is available at all without paying the cost of a full command.
   *
   * @return `true` if `IOCTL_VDD_PING` round-trips successfully.
   */
  bool ping();

}  // namespace display_device::vdd_ioctl
