#include <gtest/gtest.h>

#include <chrono>

#include "src/platform/windows/ds5/ds5_attach_policy.h"

namespace {
  using platf::ds5::attach_policy::kMaxAttempts;
  using remote_usb::usbip_host_status;

  struct run_t {
    unsigned attempts { 0 };
    std::chrono::milliseconds waited { 0 };
  };

  run_t
  run(usbip_host_status attach, usbip_host_status sweep) {
    run_t result;
    for (unsigned attempt = 0; attempt != kMaxAttempts; ++attempt) {
      ++result.attempts;
      const auto decision = platf::ds5::attach_policy::next(attach, sweep, attempt + 1);
      result.waited += decision.delay;
      if (!decision.retry) break;
    }
    return result;
  }

  constexpr usbip_host_status kAllStatuses[] {
    usbip_host_status::ok,
    usbip_host_status::invalid_argument,
    usbip_host_status::busy,
    usbip_host_status::stopped,
    usbip_host_status::launch_failed,
    usbip_host_status::attach_failed,
    usbip_host_status::detach_failed,
    usbip_host_status::timed_out,
    usbip_host_status::cancelled,
    usbip_host_status::unsupported,
    usbip_host_status::sweep_failed,
  };
}  // namespace

TEST(Ds5AttachPolicy, PermanentAttachFailuresTakeOneAttempt) {
  for (const auto status : {
         usbip_host_status::invalid_argument,
         usbip_host_status::stopped,
         usbip_host_status::launch_failed,
         usbip_host_status::cancelled,
         usbip_host_status::unsupported,
         usbip_host_status::sweep_failed,
       }) {
    const auto outcome = run(status, usbip_host_status::ok);
    EXPECT_EQ(outcome.attempts, 1u) << platf::ds5::attach_policy::name(status);
    EXPECT_EQ(outcome.waited, std::chrono::milliseconds(0)) << platf::ds5::attach_policy::name(status);
  }
}

TEST(Ds5AttachPolicy, MissingHelperOrDriverStopsWithoutTheStalePortRetry) {
  // usbip.exe missing, and the driver missing or refused at install: the helper cannot
  // list imports either, so a second attach cannot succeed and the old backoff was pure loss.
  for (const auto sweep : {
         usbip_host_status::launch_failed,
         usbip_host_status::unsupported,
         usbip_host_status::sweep_failed,
         usbip_host_status::timed_out,
         usbip_host_status::cancelled,
         usbip_host_status::stopped,
       }) {
    for (const auto attach : { usbip_host_status::attach_failed, usbip_host_status::busy, usbip_host_status::timed_out }) {
      const auto outcome = run(attach, sweep);
      EXPECT_EQ(outcome.attempts, 1u) << platf::ds5::attach_policy::name(attach)
                                      << '/' << platf::ds5::attach_policy::name(sweep);
      EXPECT_EQ(outcome.waited, std::chrono::milliseconds(0));
    }
  }
}

TEST(Ds5AttachPolicy, StalePortFailureGetsOneImmediateRetry) {
  for (const auto sweep : { usbip_host_status::ok, usbip_host_status::detach_failed }) {
    for (const auto attach : { usbip_host_status::attach_failed, usbip_host_status::busy, usbip_host_status::timed_out }) {
      const auto outcome = run(attach, sweep);
      EXPECT_EQ(outcome.attempts, 2u) << platf::ds5::attach_policy::name(attach)
                                      << '/' << platf::ds5::attach_policy::name(sweep);
      EXPECT_EQ(outcome.waited, std::chrono::milliseconds(0));
    }
  }
}

TEST(Ds5AttachPolicy, NoOutcomeSleepsOnTheAllocationPath) {
  for (const auto attach : kAllStatuses) {
    for (const auto sweep : kAllStatuses) {
      const auto outcome = run(attach, sweep);
      EXPECT_LE(outcome.attempts, kMaxAttempts);
      EXPECT_EQ(outcome.waited, std::chrono::milliseconds(0))
        << platf::ds5::attach_policy::name(attach) << '/' << platf::ds5::attach_policy::name(sweep);
    }
  }
}

TEST(Ds5AttachPolicy, SecondFailureStops) {
  const auto decision = platf::ds5::attach_policy::next(
    usbip_host_status::attach_failed, usbip_host_status::ok, kMaxAttempts);
  EXPECT_FALSE(decision.retry);
  EXPECT_EQ(decision.delay, std::chrono::milliseconds(0));
}
