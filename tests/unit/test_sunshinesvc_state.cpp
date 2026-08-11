/**
 * @file tests/unit/test_sunshinesvc_state.cpp
 * @brief Tests for the GUI agent lifecycle policy used by sunshinesvc.
 */
#include <tools/sunshinesvc_state.h>

#include <array>
#include <gtest/gtest.h>

TEST(GuiRestartBackoff, BacksOffRepeatedFailures) {
  sunshinesvc::GuiRestartBackoff backoff;
  constexpr std::array expected { 1000U, 2000U, 4000U, 8000U, 16000U, 30000U, 30000U };

  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(backoff.next_delay(), expected[index]);
  }
}

TEST(GuiRestartBackoff, ResetsOnlyAfterStableRuntime) {
  sunshinesvc::GuiRestartBackoff backoff;

  EXPECT_EQ(backoff.next_delay(), 1000U);
  EXPECT_EQ(backoff.next_delay(), 2000U);
  EXPECT_EQ(backoff.next_delay(sunshinesvc::GUI_RESTART_STABLE_RUNTIME_MS - 1), 4000U);
  EXPECT_EQ(backoff.next_delay(sunshinesvc::GUI_RESTART_STABLE_RUNTIME_MS), 1000U);
  EXPECT_EQ(backoff.next_delay(), 2000U);
}

TEST(GuiRestartBackoff, ExplicitResetStartsAtInitialDelay) {
  sunshinesvc::GuiRestartBackoff backoff;

  EXPECT_EQ(backoff.next_delay(), 1000U);
  EXPECT_EQ(backoff.next_delay(), 2000U);
  backoff.reset();
  EXPECT_EQ(backoff.next_delay(), 1000U);
}

TEST(GuiAgentRestartPolicy, CleanExitIsSuppressedUntilNextCoreLifecycle) {
  sunshinesvc::GuiAgentRestartPolicy policy;

  EXPECT_TRUE(policy.restart_allowed());
  policy.suppress_until_core_restart();
  EXPECT_FALSE(policy.restart_allowed());
  policy.begin_core_lifecycle();
  EXPECT_TRUE(policy.restart_allowed());
}

TEST(GuiAgentCrashLogLimiter, LogsOnlyCrashLoopStateChanges) {
  sunshinesvc::GuiAgentCrashLogLimiter limiter;

  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_FALSE(limiter.should_log_acquisition(false));

  EXPECT_TRUE(limiter.should_log_exit(1, 1000));
  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_FALSE(limiter.should_log_exit(1, 1000));
  EXPECT_FALSE(limiter.should_log_acquisition(false));

  EXPECT_TRUE(limiter.should_log_exit(1, 2000));
  EXPECT_TRUE(limiter.should_log_exit(2, 2000));
  EXPECT_TRUE(limiter.should_log_acquisition(true));

  limiter.reset();
  EXPECT_TRUE(limiter.should_log_acquisition(false));
}
