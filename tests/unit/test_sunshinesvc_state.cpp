/**
 * @file tests/unit/test_sunshinesvc_state.cpp
 * @brief Tests for the GUI agent restart policy used by sunshinesvc.
 */
#include <tools/sunshinesvc_state.h>

#include <array>
#include <iostream>

namespace {

  bool
  backs_off_repeated_failures() {
    sunshinesvc::GuiRestartBackoff backoff;
    constexpr std::array expected { 1000U, 2000U, 4000U, 8000U, 16000U, 30000U, 30000U };

    return std::ranges::all_of(expected, [&backoff](const auto delay) {
      return backoff.next_delay() == delay;
    });
  }

  bool
  resets_after_stable_runtime() {
    sunshinesvc::GuiRestartBackoff backoff;

    return backoff.next_delay() == 1000U &&
           backoff.next_delay() == 2000U &&
           backoff.next_delay(sunshinesvc::GUI_RESTART_STABLE_RUNTIME_MS) == 1000U &&
           backoff.next_delay() == 2000U;
  }

  bool
  explicit_reset_starts_at_initial_delay() {
    sunshinesvc::GuiRestartBackoff backoff;

    if (backoff.next_delay() != 1000U || backoff.next_delay() != 2000U) {
      return false;
    }
    backoff.reset();
    return backoff.next_delay() == 1000U;
  }

}  // namespace

int
main() {
  if (!backs_off_repeated_failures()) {
    std::cerr << "repeated failure backoff test failed\n";
    return 1;
  }
  if (!resets_after_stable_runtime()) {
    std::cerr << "stable runtime reset test failed\n";
    return 1;
  }
  if (!explicit_reset_starts_at_initial_delay()) {
    std::cerr << "explicit reset test failed\n";
    return 1;
  }
  return 0;
}
