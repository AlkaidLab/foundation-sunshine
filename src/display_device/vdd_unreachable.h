#pragma once

#include <cstdint>
#include <string>

namespace display_device::vdd_capability {

  /**
   * Why a ZakoVDD adapter that Windows lists is still not usable. The stream-start preflight used to report every
   * one of these as "unreachable"; the cause decides what the person has to do next.
   */
  enum class unreachable_cause_e {
    reboot_required,  ///< Device Manager problem 14 (CM_PROB_NEED_RESTART): Windows has to restart first.
    not_running,      ///< A nonzero Device Manager problem code: disabled (22), failed to start (10, 43), unsigned (52) ...
    status_unknown,   ///< The adapter is listed but Windows would not give its problem code.
    control_interface,  ///< The adapter runs (problem code 0) but the control interface does not open or does not answer a ping.
  };

  struct unreachable_t {
    unreachable_cause_e cause = unreachable_cause_e::control_interface;
    std::uint32_t problem_code = 0;
  };

  constexpr unreachable_t
  classify_unreachable(bool running, bool problem_code_valid, std::uint32_t problem_code) {
    if (!problem_code_valid) {
      return { unreachable_cause_e::status_unknown, 0 };
    }
    if (!running) {
      return { problem_code == 14 ? unreachable_cause_e::reboot_required : unreachable_cause_e::not_running, problem_code };
    }
    return { unreachable_cause_e::control_interface, 0 };
  }

  struct unreachable_text_t {
    std::string message;
    std::string hint;
    std::string recovery_action;
    const char *cause_word;
  };

  /** What the client is told for each cause (the error code stays VDD_DRIVER_UNREACHABLE). */
  inline unreachable_text_t
  describe_unreachable(const unreachable_t &why) {
    switch (why.cause) {
      case unreachable_cause_e::reboot_required:
        return { "The virtual display driver is installed, but Windows has to restart before it can start.",
          "Restart Windows on the host, then try again.", "restart_windows", "reboot_required" };
      case unreachable_cause_e::not_running:
        if (why.problem_code == 22) {
          return { "The virtual display driver is disabled in Windows (Device Manager error 22).",
            "Enable the ZakoVDD display adapter in Device Manager, or repair ZakoVDD, then try again.", "repair_vdd_driver", "not_running" };
        }
        return { "The virtual display driver is installed but not running (Device Manager error " + std::to_string(why.problem_code) + ").",
          "Restart the ZakoVDD display adapter in Device Manager, or repair ZakoVDD, then try again.", "repair_vdd_driver", "not_running" };
      case unreachable_cause_e::status_unknown:
        return { "Windows did not report the state of the virtual display driver.",
          "Repair ZakoVDD, or restart Windows on the host, then try again.", "repair_vdd_driver", "status_unknown" };
      case unreachable_cause_e::control_interface:
        break;
    }
    return { "The virtual display driver is running, but its control interface does not answer.",
      "Repair or update ZakoVDD so the driver matches this Sunshine, then try again.", "repair_vdd_driver", "control_interface" };
  }

}  // namespace display_device::vdd_capability
