/**
 * @file tests/unit/test_display_bounded_call.cpp
 * @brief A display call that never returns must not hold the caller: the bounded run returns, and the call keeps running by itself.
 */
#include "src/display_device/bounded_call.h"

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;

  TEST(DisplayBoundedCall, ReturnsTrueWhenTheCallFinishesInTime) {
    auto ran = std::make_shared<std::atomic_bool>(false);
    EXPECT_TRUE(display_device::run_bounded([ran] { ran->store(true); }, 5s));
    EXPECT_TRUE(ran->load());
  }

  TEST(DisplayBoundedCall, ReturnsFalseAtTheLimitWhileTheCallIsStillBlocked) {
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    auto finished = std::make_shared<std::atomic_bool>(false);
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_FALSE(display_device::run_bounded([gate, finished] {
      gate.wait();
      finished->store(true);
    },
      100ms));
    const auto waited = std::chrono::steady_clock::now() - begin;
    EXPECT_GE(waited, 90ms);
    EXPECT_LT(waited, 5s);
    EXPECT_FALSE(finished->load());
    // the blocked call is not cancelled: it ends when the driver answers
    release->set_value();
    for (int i = 0; i < 200 && !finished->load(); ++i) std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(finished->load());
  }

  TEST(DisplayBoundedCall, AThrownExceptionDoesNotEscapeAndCountsAsFinished) {
    EXPECT_TRUE(display_device::run_bounded([] { throw std::runtime_error("display stack failed"); }, 5s));
  }
}  // namespace
