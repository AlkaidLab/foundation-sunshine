#include "src/streaming/capture_target.h"

#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

namespace {
  using namespace streaming;
  using namespace std::chrono_literals;

  TEST(CurrentCaptureTarget, StartsUnavailableUntilActualCaptureOpens) {
    capture_target_t target;
    const auto current = target.lock();
    EXPECT_TRUE(current.display_name().empty());
    EXPECT_TRUE(current.device_id().empty());
    EXPECT_FALSE(current.stopped());
  }

  TEST(CurrentCaptureTarget, ReinitializationClearsBothIdentitiesBeforePublishingNewCapture) {
    capture_target_t target;
    target.publish("DISPLAY2", "monitor-A");
    {
      const auto current = target.lock();
      EXPECT_EQ(current.display_name(), "DISPLAY2");
      EXPECT_EQ(current.device_id(), "monitor-A");
    }
    target.invalidate();
    {
      const auto current = target.lock();
      EXPECT_TRUE(current.display_name().empty());
      EXPECT_TRUE(current.device_id().empty());
      EXPECT_FALSE(current.stopped());
    }
    target.publish("DISPLAY3", "monitor-B");
    const auto current = target.lock();
    EXPECT_EQ(current.display_name(), "DISPLAY3");
    EXPECT_EQ(current.device_id(), "monitor-B");
  }

  TEST(CurrentCaptureTarget, StoppedSessionCannotBeRevivedByLateCaptureOrSharedJoin) {
    capture_target_t target;
    target.publish("DISPLAY2", "monitor-A");
    target.stop();
    target.invalidate();
    target.publish("DISPLAY3", "monitor-B");
    const auto current = target.lock();
    EXPECT_TRUE(current.stopped());
    EXPECT_TRUE(current.display_name().empty());
    EXPECT_TRUE(current.device_id().empty());
  }

  TEST(CurrentCaptureSelection, UsesCertificateIdentityAndOnlyRunningVideoSessions) {
    auto actual = std::make_shared<capture_target_t>();
    auto other = std::make_shared<capture_target_t>();
    const std::vector<capture_session_t> sessions {
      { "cert-B", true, false, other },
      { "cert-A", false, false, other },
      { "cert-A", true, true, other },
      { "cert-A", true, false, actual },
    };
    const auto result = select_current_capture(sessions, "cert-A");
    EXPECT_EQ(result.error, capture_target_error_e::none);
    EXPECT_EQ(result.target, actual);
  }

  TEST(CurrentCaptureSelection, MissingOrEmptyCertificateNeverFallsBackToAnotherClient) {
    auto target = std::make_shared<capture_target_t>();
    const std::vector<capture_session_t> sessions {
      { "cert-B", true, false, target },
      { "", true, false, target },
      { "cert-A", false, false, target },
      { "cert-A", true, true, target },
    };
    EXPECT_EQ(select_current_capture(sessions, "cert-A").error, capture_target_error_e::no_active_stream);
    EXPECT_EQ(select_current_capture(sessions, "").error, capture_target_error_e::no_active_stream);
    EXPECT_EQ(select_current_capture({}, "cert-A").error, capture_target_error_e::no_active_stream);
  }

  TEST(CurrentCaptureSelection, MultipleVideoSessionsAreAmbiguousEvenWhenTheyShareCapture) {
    auto shared_capture = std::make_shared<capture_target_t>();
    const std::vector<capture_session_t> sessions {
      { "cert-A", true, false, shared_capture },
      { "cert-A", true, false, shared_capture },
    };
    const auto result = select_current_capture(sessions, "cert-A");
    EXPECT_EQ(result.error, capture_target_error_e::ambiguous_stream);
    EXPECT_FALSE(result.target);
  }

  TEST(CurrentCaptureSelection, CaptureReadinessDoesNotHideSessionAmbiguity) {
    const std::vector<capture_session_t> sessions {
      { "cert-A", true, false, {} },
      { "cert-A", true, false, std::make_shared<capture_target_t>() },
    };
    EXPECT_EQ(select_current_capture(sessions, "cert-A").error, capture_target_error_e::ambiguous_stream);

    const auto initializing = select_current_capture({ { "cert-A", true, false, {} } }, "cert-A");
    EXPECT_EQ(initializing.error, capture_target_error_e::none);
    EXPECT_FALSE(initializing.target);
  }

  TEST(CurrentCaptureIdentity, RequiresStableIdentityAndRejectsGdiReuseOrRetargeting) {
    EXPECT_TRUE(matches_capture_identity("monitor-A", "DISPLAY2", "monitor-A", "DISPLAY2"));
    EXPECT_TRUE(matches_capture_identity("monitor-A", "", "monitor-A", "DISPLAY2"));
    EXPECT_FALSE(matches_capture_identity("", "DISPLAY2", "monitor-A", "DISPLAY2"));
    EXPECT_FALSE(matches_capture_identity("monitor-A", "DISPLAY2", "", "DISPLAY2"));
    EXPECT_FALSE(matches_capture_identity("monitor-A", "DISPLAY2", "monitor-B", "DISPLAY2"));
    EXPECT_FALSE(matches_capture_identity("monitor-A", "DISPLAY2", "monitor-A", "DISPLAY3"));
  }

  TEST(CurrentCaptureTarget, SwitchingWaitsUntilTheDisplayOperationReleasesItsLock) {
    capture_target_t target;
    target.publish("DISPLAY2", "monitor-A");
    std::promise<void> started;
    std::promise<void> finished;
    auto completion = finished.get_future();
    std::thread switcher;
    {
      const auto operation = target.lock();
      switcher = std::thread([&]() {
        started.set_value();
        target.invalidate();
        target.publish("DISPLAY3", "monitor-B");
        finished.set_value();
      });
      started.get_future().wait();
      // A bounded completion wait checks serialization without sleeping the
      // worker or relying on when the scheduler starts the thread.
      EXPECT_EQ(completion.wait_for(50ms), std::future_status::timeout);
      EXPECT_EQ(operation.display_name(), "DISPLAY2");
      EXPECT_EQ(operation.device_id(), "monitor-A");
    }
    switcher.join();
    EXPECT_EQ(completion.wait_for(0ms), std::future_status::ready);
    const auto current = target.lock();
    EXPECT_EQ(current.display_name(), "DISPLAY3");
    EXPECT_EQ(current.device_id(), "monitor-B");
  }

  TEST(CurrentCaptureTarget, StoppingWaitsForAnAlreadyRunningDisplayOperation) {
    capture_target_t target;
    target.publish("DISPLAY2", "monitor-A");
    std::promise<void> started;
    std::promise<void> finished;
    auto completion = finished.get_future();
    std::thread stopper;
    {
      const auto operation = target.lock();
      stopper = std::thread([&]() {
        started.set_value();
        target.stop();
        finished.set_value();
      });
      started.get_future().wait();
      EXPECT_EQ(completion.wait_for(50ms), std::future_status::timeout);
      EXPECT_FALSE(operation.stopped());
    }
    stopper.join();
    target.publish("DISPLAY3", "monitor-B");
    const auto current = target.lock();
    EXPECT_TRUE(current.stopped());
    EXPECT_TRUE(current.device_id().empty());
    EXPECT_TRUE(current.display_name().empty());
  }
}  // namespace
