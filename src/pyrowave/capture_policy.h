#pragma once

#include <cstddef>

namespace pyrowave {
  // The shared capture owner serializes admission and fallback with the same
  // lock. A per-session GPU failure must not change another encoder's input.
  class capture_policy_t {
  public:
    bool
    attach(bool accepts_system_capture) noexcept {
      if (system_capture_ && !accepts_system_capture) {
        return false;
      }
      ++sessions_;
      return true;
    }

    void
    detach() noexcept {
      if (sessions_ != 0) {
        --sessions_;
      }
      // The display can still exist between the last detach and capture
      // teardown. Its memory type changes only with a new capture owner.
    }

    bool
    request_system_capture() noexcept {
      if (sessions_ == 0 || (!system_capture_ && sessions_ != 1)) {
        return false;
      }
      system_capture_ = true;
      return true;
    }

    [[nodiscard]] bool
    system_capture() const noexcept {
      return system_capture_;
    }

  private:
    std::size_t sessions_ = 0;
    bool system_capture_ = false;
  };
}  // namespace pyrowave
