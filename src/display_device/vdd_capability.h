#pragma once

#include <string_view>

#include "vdd_unreachable.h"

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

  struct detail_t {
    state_e state = state_e::unsupported_platform;
    unreachable_t unreachable;  ///< Only meaningful when state is driver_unreachable.
    bool control_available = false;
  };

  /** The state together with what made it driver_unreachable (the preflight tells the person which). */
  detail_t
  query_detail();

  std::string_view
  to_string(state_e state);

}  // namespace display_device::vdd_capability
