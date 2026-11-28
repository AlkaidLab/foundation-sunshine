#include "vdd_capability.h"

#ifdef _WIN32
  #include "vdd_utils.h"
#endif

namespace display_device::vdd_capability {

  state_e
  query_state() {
    return query_detail().state;
  }

  detail_t
  query_detail() {
    detail_t detail;
#ifndef _WIN32
    detail.state = state_e::unsupported_platform;
#else
    const auto status = vdd_utils::get_vdd_status();
    detail.control_available = status.control_available;
    if (!status.installed) {
      detail.state = state_e::driver_missing;
    }
    // is_usable() already requires control_available.
    else if (status.is_usable()) {
      detail.state = state_e::ready;
    }
    else {
      detail.state = state_e::driver_unreachable;
      detail.unreachable = classify_unreachable(status.running, status.problem_code_valid, status.problem_code);
    }
#endif
    return detail;
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
