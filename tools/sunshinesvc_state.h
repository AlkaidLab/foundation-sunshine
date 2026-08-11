/**
 * @file tools/sunshinesvc_state.h
 * @brief Platform-independent lifecycle policy for the Windows GUI agent supervisor.
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace sunshinesvc {

  constexpr std::uint32_t GUI_RESTART_INITIAL_DELAY_MS = 1000;
  constexpr std::uint32_t GUI_RESTART_MAX_DELAY_MS = 30000;
  constexpr std::uint64_t GUI_RESTART_STABLE_RUNTIME_MS = 60000;
  constexpr std::uint32_t GUI_REATTACH_POLL_MS = 3000;
  constexpr std::uint64_t GUI_CRASH_LOG_REMINDER_MS = 300000;

  class GuiRestartBackoff {
  public:
    std::uint32_t
    next_delay(std::uint64_t previous_runtime_ms = 0) {
      if (previous_runtime_ms >= GUI_RESTART_STABLE_RUNTIME_MS) {
        failures_ = 0;
      }

      const auto shift = std::min(failures_, 5U);
      const auto delay = std::min(
        GUI_RESTART_INITIAL_DELAY_MS << shift,
        GUI_RESTART_MAX_DELAY_MS);
      ++failures_;
      return delay;
    }

    void
    reset() {
      failures_ = 0;
    }

  private:
    std::uint32_t failures_ = 0;
  };

  class GuiAgentRestartPolicy {
  public:
    void
    suppress_launch() {
      launch_suppressed_ = true;
    }

    void
    resume_supervision() {
      launch_suppressed_ = false;
    }

    bool
    launch_allowed() const {
      return !launch_suppressed_;
    }

  private:
    bool launch_suppressed_ = false;
  };

  class GuiAgentCrashLogLimiter {
  public:
    bool
    should_log_exit(std::uint32_t exit_code, std::uint32_t retry_delay_ms, std::uint64_t now_ms) {
      const bool changed = !has_last_exit_ ||
                           exit_code != last_exit_code_ ||
                           retry_delay_ms != last_retry_delay_ms_;
      const bool reminder_due = has_last_exit_ &&
                                now_ms - last_exit_log_at_ms_ >= GUI_CRASH_LOG_REMINDER_MS;
      const bool should_log = changed || reminder_due;
      has_last_exit_ = true;
      last_exit_code_ = exit_code;
      last_retry_delay_ms_ = retry_delay_ms;
      if (should_log) {
        last_exit_log_at_ms_ = now_ms;
      }
      acquisition_log_pending_ = should_log;
      return should_log;
    }

    bool
    should_log_acquisition(bool recovered_from_error) {
      const bool should_log = acquisition_log_pending_ || recovered_from_error;
      acquisition_log_pending_ = false;
      return should_log;
    }

    void
    reset() {
      has_last_exit_ = false;
      last_exit_code_ = 0;
      last_retry_delay_ms_ = 0;
      last_exit_log_at_ms_ = 0;
      acquisition_log_pending_ = true;
    }

  private:
    bool has_last_exit_ = false;
    bool acquisition_log_pending_ = true;
    std::uint32_t last_exit_code_ = 0;
    std::uint32_t last_retry_delay_ms_ = 0;
    std::uint64_t last_exit_log_at_ms_ = 0;
  };

}  // namespace sunshinesvc
