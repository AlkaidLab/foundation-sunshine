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

}  // namespace sunshinesvc
