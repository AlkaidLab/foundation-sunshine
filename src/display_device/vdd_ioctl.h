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

#include <cstdint>
#include <string>
#include <vector>

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
   * @brief Host-side view of the sealed frame-channel capability probe.
   *
   * Kept separate from `result` because a driver can fully support the command
   * IOCTL transport while still being an older legacy named-texture frame
   * producer.
   */
  enum class frame_channel_status {
    supported,
    unsupported,
    interface_missing,
    failed,
  };

  struct frame_channel_caps {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t max_shared_slots = 0;
    std::uint32_t metadata_size = 0;
  };

  enum class frame_channel_open_status {
    opened,
    unsupported,
    interface_missing,
    failed,
  };

  struct frame_channel_open_request {
    std::uint32_t monitor_index = 0;
    std::uint32_t required_flags = 0;
    std::uint32_t desired_slots = 0;
    std::uint32_t adapter_luid_low_part = 0;
    std::int32_t adapter_luid_high_part = 0;
  };

  struct frame_channel_slot_handle {
    std::uint64_t texture_handle = 0;
  };

  struct frame_channel_open_response {
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t metadata_size = 0;
    std::uint64_t metadata_handle = 0;
    std::uint64_t frame_ready_event_handle = 0;
    std::vector<frame_channel_slot_handle> slots;
  };

  std::uint32_t required_sealed_frame_channel_flags();

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

  /**
   * @brief Probe whether the installed driver supports the v2 sealed frame
   *        channel negotiation IOCTL.
   *
   * `unsupported` means the control device is present but the driver does not
   * implement `IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS`; callers should keep using
   * the hardened legacy named shared-texture path.
   */
  frame_channel_status query_frame_channel_caps(frame_channel_caps &caps);

  /**
   * @brief Ask a v2 driver to duplicate an unnamed producer-owned frame
   *        channel into this Sunshine process.
   *
   * This is the host side of sealed borrow. Older drivers return
   * `unsupported`, and callers must not treat `query_frame_channel_caps()`
   * support as proof that this open path is usable until this call succeeds.
   */
  frame_channel_open_status open_frame_channel(const frame_channel_open_request &request,
                                               frame_channel_open_response &response);

}  // namespace display_device::vdd_ioctl
