/**
 * @file src/display_device/display_intent_policy.h
 * @brief Pure policy for resolving a requested display from an enumeration result.
 */
#pragma once

#include "display_device.h"

#include <string>

namespace display_device {

  inline bool
  display_enumeration_is_reliable(const device_enumeration_result_t &enumeration) {
    return enumeration.status == device_enumeration_result_t::status_e::success;
  }

  enum class requested_display_resolution_e {
    preserve_physical,
    unavailable,
    fallback_primary,
    vdd
  };

  inline requested_display_resolution_e
  classify_requested_display(
    const device_enumeration_result_t &enumeration,
    const std::string &device_id,
    bool client_named_display,
    bool explicit_vdd,
    const std::string &vdd_friendly_name) {
    if (explicit_vdd) {
      return requested_display_resolution_e::vdd;
    }

    if (device_id.empty() || enumeration.status == device_enumeration_result_t::status_e::failed) {
      return requested_display_resolution_e::preserve_physical;
    }

    const auto device_it = enumeration.devices.find(device_id);
    if (device_it == enumeration.devices.end()) {
      return client_named_display ?
               requested_display_resolution_e::unavailable :
               requested_display_resolution_e::fallback_primary;
    }

    if (!vdd_friendly_name.empty() && device_it->second.friendly_name == vdd_friendly_name) {
      return requested_display_resolution_e::vdd;
    }

    return requested_display_resolution_e::preserve_physical;
  }

}  // namespace display_device
