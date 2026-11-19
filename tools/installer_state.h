#pragma once

#include <string_view>

namespace installer_state {

  enum class result {
    success,
    needs_reboot,
    service_failed,
    component_failed,
    cancelled,
    failed,
    unknown,
  };

  inline result
  classify_exit_code(const unsigned long code) {
    switch (code) {
      case 0:
        return result::success;
      case 3010:
        return result::needs_reboot;
      case 100:
        return result::service_failed;
      case 101:
        return result::component_failed;
      case 1223:
        return result::cancelled;
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
        return result::failed;
      default:
        return result::unknown;
    }
  }

  inline std::string_view
  name(const result state) {
    switch (state) {
      case result::success:
        return "success";
      case result::needs_reboot:
        return "needs-reboot";
      case result::service_failed:
        return "service-failed";
      case result::component_failed:
        return "component-failed";
      case result::cancelled:
        return "cancelled";
      case result::failed:
        return "failed";
      case result::unknown:
        return "unknown";
    }
    return "unknown";
  }

}  // namespace installer_state
