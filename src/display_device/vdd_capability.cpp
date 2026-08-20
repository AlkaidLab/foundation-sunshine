#include "vdd_capability.h"

#ifdef _WIN32
  #include "vdd_utils.h"
#endif

namespace display_device::vdd_capability {

  state_e
  query_state() {
#ifndef _WIN32
    return state_e::unsupported_platform;
#else
    const auto status = vdd_utils::get_vdd_status();
    if (!status.installed) {
      return state_e::driver_missing;
    }
    // is_usable() already requires control_available.
    if (status.is_usable()) {
      return state_e::ready;
    }
    return state_e::driver_unreachable;
#endif
  }

  std::string_view
  to_string(state_e state) {
    switch (state) {
      case state_e::ready:
        return "ready";
      case state_e::driver_missing:
        return "driver_missing";
      case state_e::driver_unreachable:
        return "driver_unreachable";
      case state_e::unsupported_platform:
        return "unsupported_platform";
    }

    return "driver_unreachable";
  }

}  // namespace display_device::vdd_capability
