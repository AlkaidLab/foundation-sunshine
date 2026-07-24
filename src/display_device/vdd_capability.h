#pragma once

#include <string_view>

namespace display_device::vdd_capability {

  inline constexpr int capability_version = 1;

  enum class state_e {
    ready,
    driver_missing,
    driver_unreachable,
    unsupported_platform,
  };

  /**
   * Return the current host-side VDD readiness without creating or destroying
   * a virtual display.
   */
  state_e
  query_state();

  std::string_view
  to_string(state_e state);

}  // namespace display_device::vdd_capability
