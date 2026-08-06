/**
 * @file tests/tools/vmouse_platform_support.cpp
 * @brief Minimal Windows platform support for standalone virtual mouse targets.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <memory>

#include "src/platform/common.h"

using namespace std::chrono_literals;

namespace platf {
  namespace {
    class standalone_high_precision_timer final: public high_precision_timer {
    public:
      standalone_high_precision_timer() {
        timer_ = CreateWaitableTimerEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) {
          timer_ = CreateWaitableTimerEx(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
      }

      ~standalone_high_precision_timer() override {
        if (timer_) {
          CloseHandle(timer_);
        }
      }

      void
      sleep_for(const std::chrono::nanoseconds &duration) override {
        if (!timer_ || duration < 0s || duration > 5s) {
          return;
        }

        LARGE_INTEGER due_time {};
        due_time.QuadPart = duration.count() / -100;
        if (SetWaitableTimer(timer_, &due_time, 0, nullptr, nullptr, false)) {
          WaitForSingleObject(timer_, INFINITE);
        }
      }

      operator bool() override {
        return timer_ != nullptr;
      }

    private:
      HANDLE timer_ = nullptr;
    };
  }  // namespace

  void
  adjust_thread_priority(thread_priority_e priority) {
    int win32_priority;

    switch (priority) {
      case thread_priority_e::low:
        win32_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
      case thread_priority_e::normal:
        win32_priority = THREAD_PRIORITY_NORMAL;
        break;
      case thread_priority_e::high:
        win32_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
      case thread_priority_e::critical:
        win32_priority = THREAD_PRIORITY_HIGHEST;
        break;
      default:
        return;
    }

    SetThreadPriority(GetCurrentThread(), win32_priority);
  }

  std::unique_ptr<high_precision_timer>
  create_high_precision_timer() {
    return std::make_unique<standalone_high_precision_timer>();
  }
}  // namespace platf
