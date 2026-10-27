#include "src/streaming/capture_target.h"

#include <gtest/gtest.h>

namespace {
  using namespace streaming;

  TEST(CurrentCaptureTarget, TracksLatestPublishedDisplay) {
    capture_target_t target;
    EXPECT_TRUE(target.display_name().empty());
    target.set("DISPLAY2");
    EXPECT_EQ(target.display_name(), "DISPLAY2");
    target.set("DISPLAY3");
    EXPECT_EQ(target.display_name(), "DISPLAY3");
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
    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.target, actual);
  }

  TEST(CurrentCaptureSelection, NeverFallsBackToAnotherClient) {
    auto target = std::make_shared<capture_target_t>();
    const std::vector<capture_session_t> sessions {
      { "cert-B", true, false, target },
      { "", true, false, target },
      { "cert-A", false, false, target },
      { "cert-A", true, true, target },
    };
    EXPECT_FALSE(select_current_capture(sessions, "cert-A").found);
    EXPECT_FALSE(select_current_capture(sessions, "").found);
    EXPECT_FALSE(select_current_capture({}, "cert-A").found);
  }
}  // namespace
