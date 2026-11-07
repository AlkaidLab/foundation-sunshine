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

TEST(GuiAgentRestartPolicy, CleanExitSuppressesLaunchUntilSupervisionResumes) {
  sunshinesvc::GuiAgentRestartPolicy policy;

  EXPECT_TRUE(policy.launch_allowed());
  policy.suppress_launch();
  EXPECT_FALSE(policy.launch_allowed());
  policy.resume_supervision();
  EXPECT_TRUE(policy.launch_allowed());
}

TEST(GuiAgentRestartPolicy, MissingWebView2StopsAfterThreeShortRuns) {
  sunshinesvc::GuiAgentRestartPolicy policy;

  EXPECT_FALSE(policy.note_webview2_missing(0));
  EXPECT_FALSE(policy.note_webview2_missing(0));
  EXPECT_TRUE(policy.note_webview2_missing(0));
  EXPECT_EQ(sunshinesvc::GUI_WEBVIEW2_MISSING_EXIT_CODE, 0x5352U);
}

TEST(GuiAgentRestartPolicy, StableRuntimeResetsMissingWebView2Count) {
  sunshinesvc::GuiAgentRestartPolicy policy;

  EXPECT_FALSE(policy.note_webview2_missing(0));
  EXPECT_FALSE(policy.note_webview2_missing(0));
  // A run that reached the stable stretch breaks the streak of short
  // missing-WebView2 runs: three more short runs are needed before the
  // restart is suppressed (docs/windows-compatibility.md, WebView2 section).
  EXPECT_FALSE(policy.note_webview2_missing(sunshinesvc::GUI_RESTART_STABLE_RUNTIME_MS));
  EXPECT_FALSE(policy.note_webview2_missing(0));
  EXPECT_TRUE(policy.note_webview2_missing(0));
  policy.reset_webview2_missing();
  EXPECT_FALSE(policy.note_webview2_missing(0));
}

TEST(GuiAgentExitDecision, KilledAgentRelaunchesWhenNothingReplacesIt) {
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(0, false),
    sunshinesvc::GuiAgentExitAction::relaunch);
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(1, false),
    sunshinesvc::GuiAgentExitAction::relaunch);
  // STILL_ACTIVE is not a quit. Callers map it away; if it is passed through it
  // still restores the tray.
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(259, false),
    sunshinesvc::GuiAgentExitAction::relaunch);
}

TEST(GuiAgentExitDecision, DeliberateMenuQuitStaysSuppressed) {
  EXPECT_NE(sunshinesvc::GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE, 0U);
  EXPECT_NE(sunshinesvc::GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE, 1U);
  EXPECT_NE(sunshinesvc::GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE, 259U);
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(sunshinesvc::GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE, false),
    sunshinesvc::GuiAgentExitAction::suppress_launch);
}

TEST(GuiAgentExitDecision, RunningReplacementIsAdopted) {
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(0, true),
    sunshinesvc::GuiAgentExitAction::adopt_replacement);
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(1, true),
    sunshinesvc::GuiAgentExitAction::adopt_replacement);
  EXPECT_EQ(
    sunshinesvc::decide_gui_agent_exit(sunshinesvc::GUI_AGENT_DELIBERATE_QUIT_EXIT_CODE, true),
    sunshinesvc::GuiAgentExitAction::adopt_replacement);
}

TEST(GuiAgentCrashLogLimiter, LogsStateChangesAndPeriodicReminders) {
  sunshinesvc::GuiAgentCrashLogLimiter limiter;

  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_FALSE(limiter.should_log_acquisition(false));

  EXPECT_TRUE(limiter.should_log_exit(1, 1000, 0));
  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_FALSE(limiter.should_log_exit(1, 1000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS - 1));
  EXPECT_FALSE(limiter.should_log_acquisition(false));
  EXPECT_TRUE(limiter.should_log_exit(1, 1000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS));
  EXPECT_TRUE(limiter.should_log_acquisition(false));

  EXPECT_TRUE(limiter.should_log_exit(1, 2000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS + 1));
  EXPECT_TRUE(limiter.should_log_exit(2, 2000, sunshinesvc::GUI_CRASH_LOG_REMINDER_MS + 2));
  EXPECT_TRUE(limiter.should_log_acquisition(false));
  EXPECT_TRUE(limiter.should_log_acquisition(true));

  limiter.reset();
  EXPECT_TRUE(limiter.should_log_acquisition(false));
}

TEST(HostInstance, ASecondProcessIsNotTheOwner) {
  EXPECT_EQ(sunshinesvc::instance_claim(true, 0), sunshinesvc::instance_claim_e::owner);
  EXPECT_EQ(sunshinesvc::instance_claim(true, sunshinesvc::instance_error_already_exists), sunshinesvc::instance_claim_e::other_running);
  EXPECT_EQ(sunshinesvc::instance_claim(false, sunshinesvc::instance_error_access_denied), sunshinesvc::instance_claim_e::other_running);
  EXPECT_EQ(sunshinesvc::instance_claim(false, 8), sunshinesvc::instance_claim_e::unknown);
}

TEST(HostInstance, AnExistingCoreIsSupervisedInsteadOfLaunched) {
  EXPECT_EQ(sunshinesvc::core_start_decision(false), sunshinesvc::core_start_e::launch);
  EXPECT_EQ(sunshinesvc::core_start_decision(true), sunshinesvc::core_start_e::adopt);
}

TEST(HostInstance, TheSameInstallIsOnePathAndOneMutex) {
  EXPECT_TRUE(sunshinesvc::same_install_core(L"C:\\Program Files\\Sunshine\\Sunshine.exe", L"c:/Program Files/Sunshine/Sunshine.exe"));
  EXPECT_FALSE(sunshinesvc::same_install_core(L"C:\\Other\\Sunshine.exe", L"C:\\Program Files\\Sunshine\\Sunshine.exe"));
  const auto first = sunshinesvc::instance_mutex_name(L"C:\\Program Files\\Sunshine\\Sunshine.exe");
  const auto folded = sunshinesvc::instance_mutex_name(L"c:/program files/sunshine/sunshine.exe");
  const auto other = sunshinesvc::instance_mutex_name(L"D:\\Sunshine\\Sunshine.exe");
  EXPECT_EQ(first, folded);
  EXPECT_NE(first, other);
  EXPECT_EQ(first.rfind(L"Global\\FoundationSunshineHost-", 0), 0u);
}
