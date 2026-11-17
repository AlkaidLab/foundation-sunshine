#pragma once
// The failed step and the error code of an install, as fixed words, for install-result.json (stage (e) error capture; the host reads the file at its next start and reports it, see
// telemetry::installer_failure_report). Header only, std only: the installer shell and its tests both include it.
//
// A word is built from the step and a number, never from text of the machine: no path, no message, no name can enter. Steps: payload (the package could not be unpacked), engine (the setup engine
// exited with a code of its own, 1..8 are its documented ones and 100/101 are the shell's), webview2 (the runtime bootstrapper).
#include <string>
#include <string_view>

namespace installer_failure {

  enum class step {
    payload,
    engine,
    webview2,
  };

  inline std::string_view
  step_word(const step s) {
    switch (s) {
      case step::payload:
        return "payload";
      case step::engine:
        return "engine";
      case step::webview2:
        return "webview2";
    }
    return "engine";
  }

  /** `payload_invalid`, `engine_exit_101`, `webview2_exit_5`. */
  inline std::string
  code_word(const step s, const unsigned long code) {
    if (s == step::payload) return "payload_invalid";
    return std::string(step_word(s)) + "_exit_" + std::to_string(code);
  }

  /** The two fields appended to install-result.json for a failed install: `,"failed_step":"engine","error_code":"engine_exit_101"`. */
  inline std::string
  result_fields(const step s, const unsigned long code) {
    return ",\"failed_step\":\"" + std::string(step_word(s)) + "\",\"error_code\":\"" + code_word(s, code) + "\"";
  }

  /** Which results are failures worth a report: not a success, not a restart request, not the person pressing Cancel. */
  inline bool
  is_failure_state(const std::string_view state) {
    return state == "failed" || state == "service-failed" || state == "component-failed" || state == "unknown";
  }

}  // namespace installer_failure
