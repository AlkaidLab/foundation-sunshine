#include "src/display_device/vdd_unreachable.h"

#include <gtest/gtest.h>

namespace {
  using namespace display_device::vdd_capability;
}

TEST(VddUnreachableCause, SplitsTheStatesThePreflightUsedToMerge) {
  // installed, problem code unreadable
  EXPECT_EQ(classify_unreachable(false, false, 0).cause, unreachable_cause_e::status_unknown);
  // Device Manager problem 14: only a Windows restart helps
  EXPECT_EQ(classify_unreachable(false, true, 14).cause, unreachable_cause_e::reboot_required);
  // disabled (22), failed to start (10), unsigned (52): the adapter exists but does not run
  const auto disabled = classify_unreachable(false, true, 22);
  EXPECT_EQ(disabled.cause, unreachable_cause_e::not_running);
  EXPECT_EQ(disabled.problem_code, 22u);
  EXPECT_EQ(classify_unreachable(false, true, 10).cause, unreachable_cause_e::not_running);
  // running with problem code 0 but the control interface does not answer
  EXPECT_EQ(classify_unreachable(true, true, 0).cause, unreachable_cause_e::control_interface);
}

TEST(VddUnreachableText, SaysWhatToDoForEachCause) {
  const auto reboot = describe_unreachable(classify_unreachable(false, true, 14));
  EXPECT_EQ(reboot.recovery_action, "restart_windows");
  EXPECT_NE(reboot.hint.find("Restart Windows"), std::string::npos);

  const auto disabled = describe_unreachable(classify_unreachable(false, true, 22));
  EXPECT_NE(disabled.message.find("disabled"), std::string::npos);
  EXPECT_NE(disabled.hint.find("Enable"), std::string::npos);

  const auto failed = describe_unreachable(classify_unreachable(false, true, 43));
  EXPECT_NE(failed.message.find("error 43"), std::string::npos);

  const auto control = describe_unreachable(classify_unreachable(true, true, 0));
  EXPECT_NE(control.message.find("control interface"), std::string::npos);
  EXPECT_NE(control.hint.find("matches this Sunshine"), std::string::npos);

  const auto unknown = describe_unreachable(classify_unreachable(false, false, 0));
  EXPECT_STREQ(unknown.cause_word, "status_unknown");
  // no cause reuses the old catch-all "unreachable" sentence
  for (const auto &t : { reboot, disabled, failed, control, unknown }) {
    EXPECT_EQ(t.message.find("currently unreachable"), std::string::npos);
  }
}
