#pragma once

#include <windows.h>

namespace display_device::hdr_state_api {

  // Keep the Win32 ABI local so older Windows SDKs can still build the host.
  // These requests are probed at runtime; no OS version check is needed.
  struct advanced_color_info_2_t {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 flags;
    DISPLAYCONFIG_COLOR_ENCODING color_encoding;
    UINT32 bits_per_color_channel;
    UINT32 active_color_mode;
  };

  struct hdr_state_packet_t {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 enable_hdr;
  };

  static_assert(sizeof(advanced_color_info_2_t) == 36);
  static_assert(sizeof(hdr_state_packet_t) == 24);

  struct state_t {
    LONG status;
    bool supported;
    bool enabled;
    bool dedicated_hdr_api;
  };

  template <class GetInfo>
  state_t
  query(const DISPLAYCONFIG_PATH_TARGET_INFO &target, GetInfo get_info) {
    advanced_color_info_2_t info {};
    info.header = { static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15), sizeof(info), target.adapterId, target.id };
    const LONG status = get_info(&info.header);
    if (status == ERROR_SUCCESS) {
      // Persist the user's HDR switch, even if policy temporarily prevents HDR
      // from becoming active. WCG/ACM is an independent SDR color mode.
      return { status, (info.flags & (1u << 4)) != 0, (info.flags & (1u << 5)) != 0, true };
    }

    // Older systems reject the newer request. Do not hide access, topology or
    // driver failures by retrying them through an API with different semantics.
    if (status != ERROR_INVALID_PARAMETER && status != ERROR_NOT_SUPPORTED) {
      return { status, false, false, true };
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy {};
    legacy.header = { DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(legacy), target.adapterId, target.id };
    const LONG legacy_status = get_info(&legacy.header);
    return { legacy_status, legacy.advancedColorSupported != 0, legacy.advancedColorEnabled != 0, false };
  }

  template <class GetInfo, class SetInfo>
  LONG
  set(const DISPLAYCONFIG_PATH_TARGET_INFO &target, bool enabled, GetInfo get_info, SetInfo set_info) {
    const auto state = query(target, get_info);
    if (state.status != ERROR_SUCCESS) {
      return state.status;
    }
    if (!state.supported) {
      return ERROR_NOT_SUPPORTED;
    }

    if (state.dedicated_hdr_api) {
      hdr_state_packet_t packet {};
      packet.header = { static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16), sizeof(packet), target.adapterId, target.id };
      packet.enable_hdr = enabled ? 1u : 0u;
      // A failed HDR write must not fall back to changing WCG/advanced color.
      return set_info(&packet.header);
    }

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE legacy {};
    legacy.header = { DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE, sizeof(legacy), target.adapterId, target.id };
    legacy.enableAdvancedColor = enabled ? 1u : 0u;
    return set_info(&legacy.header);
  }

}  // namespace display_device::hdr_state_api
