/**
 * @file tests/unit/test_sunshinesvc_state.cpp
 * @brief Tests for the GUI agent lifecycle policy used by sunshinesvc.
 */
#include "src/config_session_binding.h"
#include <tools/sunshinesvc_state.h>

#include <array>
#include <gtest/gtest.h>
#include <vector>

TEST(SessionBinding, MissingOrInvalidSettingUsesConsoleCapture) {
  using namespace config_session_binding;
  EXPECT_EQ(default_mode, mode_e::console);
  EXPECT_EQ(parse_or_default(""), mode_e::console);
  EXPECT_EQ(parse_or_default("invalid"), mode_e::console);
  EXPECT_EQ(parse_or_default("0"), mode_e::automatic);
  EXPECT_EQ(parse_or_default("1"), mode_e::console);
  EXPECT_EQ(parse_or_default("\"0\""), mode_e::automatic);
  EXPECT_EQ(parse_or_default("00"), mode_e::automatic);
  EXPECT_EQ(parse_or_default("0x0"), mode_e::automatic);
  EXPECT_EQ(parse_or_default("0x1"), mode_e::console);
  EXPECT_EQ(parse_or_default("2"), mode_e::console);
}

TEST(SessionBinding, RdpGuiDoesNotMoveConsoleCaptureOrItsUserToken) {
  using namespace config_session_binding;
  constexpr std::uint32_t console = 3;
  constexpr std::uint32_t rdp = 1;
  const auto targets = select_sessions(mode_e::console, console, rdp);
  const auto default_targets = select_sessions(default_mode, console, rdp);
  EXPECT_EQ(default_targets.capture, console);
  EXPECT_EQ(default_targets.gui, rdp);
  EXPECT_EQ(targets.capture, console);
  EXPECT_EQ(targets.gui, rdp);
  EXPECT_EQ(user_token_session(user_token_purpose_e::capture, mode_e::console, console, rdp), console);
  EXPECT_EQ(user_token_session(user_token_purpose_e::gui, mode_e::console, console, rdp), rdp);
}

TEST(SessionBinding, GuiLoginAndLogoutDoNotChangeConsoleCapture) {
  using namespace config_session_binding;
  const auto before = select_sessions(mode_e::console, 3, invalid_session_id);
  const auto connected = select_sessions(mode_e::console, 3, 1);
  const auto disconnected = select_sessions(mode_e::console, 3, invalid_session_id);
  EXPECT_EQ(before.capture, connected.capture);
  EXPECT_EQ(connected.capture, disconnected.capture);
  EXPECT_EQ(before.gui, invalid_session_id);
  EXPECT_EQ(connected.gui, 1U);
  EXPECT_EQ(disconnected.gui, invalid_session_id);
  EXPECT_EQ(user_token_session(user_token_purpose_e::gui, mode_e::console, 3, invalid_session_id), invalid_session_id);

  const auto connect = update_sessions(mode_e::console, 3, invalid_session_id, 3, 1, false);
  EXPECT_TRUE(connect.update_gui);
  EXPECT_FALSE(connect.restart_capture);
  EXPECT_FALSE(connect.retry);
  const auto disconnect = update_sessions(mode_e::console, 3, 1, 3, invalid_session_id, false);
  EXPECT_TRUE(disconnect.update_gui);
  EXPECT_FALSE(disconnect.restart_capture);
  EXPECT_FALSE(disconnect.retry);
}

TEST(SessionBinding, TransientQueriesDoNotMoveTheWrongSession) {
  using namespace config_session_binding;
  const auto console = update_sessions(mode_e::console, 3, 1, 3, invalid_session_id, true);
  EXPECT_FALSE(console.update_gui);
  EXPECT_FALSE(console.restart_capture);
  EXPECT_TRUE(console.retry);

  const auto changed_console = update_sessions(mode_e::console, 3, 1, 4, invalid_session_id, true);
  EXPECT_TRUE(changed_console.restart_capture);
  EXPECT_FALSE(changed_console.update_gui);
  EXPECT_TRUE(changed_console.retry);

  const auto automatic = update_sessions(mode_e::automatic, 1, 1, 3, invalid_session_id, true);
  EXPECT_FALSE(automatic.update_gui);
  EXPECT_FALSE(automatic.restart_capture);
  EXPECT_TRUE(automatic.retry);

  const auto no_console = update_sessions(mode_e::console, 3, 1, invalid_session_id, 1, false);
  EXPECT_FALSE(no_console.restart_capture);
  EXPECT_TRUE(no_console.retry);
}

TEST(SessionBinding, ExplicitAutomaticCaptureFollowsTheInteractiveSession) {
  using namespace config_session_binding;
  const auto connected = select_sessions(mode_e::automatic, 3, 1);
  EXPECT_EQ(connected.capture, 1U);
  EXPECT_EQ(connected.gui, 1U);
  EXPECT_EQ(user_token_session(user_token_purpose_e::capture, mode_e::automatic, 3, 1), 1U);
  const auto rdp_connected = update_sessions(mode_e::automatic, 3, invalid_session_id, 3, 1, false);
  EXPECT_TRUE(rdp_connected.update_gui);
  EXPECT_TRUE(rdp_connected.restart_capture);
  EXPECT_FALSE(rdp_connected.retry);

  const auto no_user = select_sessions(mode_e::automatic, 3, invalid_session_id);
  EXPECT_EQ(no_user.capture, 3U);
  EXPECT_EQ(no_user.gui, invalid_session_id);
}

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

TEST(HostInstance, OnlyAReadyCoreThatIsNotStoppingIsAdopted) {
  using sunshinesvc::core_action_e;
  using sunshinesvc::core_state_t;
  const auto plan_of = [](std::vector<core_state_t> cores) { return sunshinesvc::plan_cores(cores); };

  EXPECT_FALSE(plan_of({}).keep.has_value());
  EXPECT_TRUE(plan_of({}).actions.empty());

  const auto healthy = plan_of({ { true, false } });
  ASSERT_TRUE(healthy.keep.has_value());
  EXPECT_EQ(*healthy.keep, 0u);

  // The reported case: a core in its shutdown is not adopted, the service starts a fresh one.
  const auto stopping = plan_of({ { true, true } });
  EXPECT_FALSE(stopping.keep.has_value());
  EXPECT_EQ(stopping.actions, std::vector<core_action_e> { core_action_e::wait_for_exit });
  EXPECT_FALSE(plan_of({ { false, true } }).keep.has_value());

  // Still starting, or a build that publishes nothing: replaced, not trusted.
  const auto unknown = plan_of({ { false, false } });
  EXPECT_FALSE(unknown.keep.has_value());
  EXPECT_EQ(unknown.actions, std::vector<core_action_e> { core_action_e::end });

  // The healthy core is found wherever it is in the list, and the others are not kept.
  const auto mixed = plan_of({ { true, true }, { false, false }, { true, false }, { true, false } });
  ASSERT_TRUE(mixed.keep.has_value());
  EXPECT_EQ(*mixed.keep, 2u);
  EXPECT_EQ(mixed.actions,
    (std::vector<core_action_e> { core_action_e::wait_for_exit, core_action_e::end, core_action_e::keep, core_action_e::end }));
}

TEST(HostInstance, TheCoreEventsAreOneNameEachPerProcess) {
  const auto ready = sunshinesvc::core_event_name(sunshinesvc::core_event_e::ready, 4242);
  const auto stopping = sunshinesvc::core_event_name(sunshinesvc::core_event_e::stopping, 4242);
  EXPECT_EQ(ready, L"Global\\FoundationSunshineCoreReady-4242");
  EXPECT_EQ(stopping, L"Global\\FoundationSunshineCoreStopping-4242");
  EXPECT_NE(ready, sunshinesvc::core_event_name(sunshinesvc::core_event_e::ready, 4243));
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

TEST(CoreRestartBackoff, FirstTwoQuickFailuresAreRetriedAtOnce) {
  sunshinesvc::CoreRestartBackoff backoff;

  EXPECT_EQ(backoff.next_delay(1300, false), 0U);
  EXPECT_EQ(backoff.next_delay(1300, false), 0U);
}

TEST(CoreRestartBackoff, FurtherQuickFailuresWaitLongerUpToTheLimit) {
  sunshinesvc::CoreRestartBackoff backoff;
  constexpr std::array expected { 0U, 0U, 5000U, 10000U, 20000U, 40000U, 60000U, 60000U };

  for (std::size_t index = 0; index < expected.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(backoff.next_delay(1300, false), expected[index]);
  }
}

TEST(CoreRestartBackoff, ARunOfAMinuteOrAnAskedForExitStartsTheCountAgain) {
  sunshinesvc::CoreRestartBackoff backoff;

  for (int i = 0; i < 4; ++i) backoff.next_delay(1300, false);
  EXPECT_EQ(backoff.next_delay(sunshinesvc::CORE_STABLE_RUNTIME_MS, false), 0U);
  EXPECT_EQ(backoff.next_delay(1300, false), 0U);
  EXPECT_EQ(backoff.next_delay(1300, false), 0U);
  EXPECT_EQ(backoff.next_delay(1300, false), 5000U);

  EXPECT_EQ(backoff.next_delay(500, true), 0U);
  EXPECT_EQ(backoff.next_delay(1300, false), 0U);
}

TEST(CoreRestartBackoff, AZeroWaitReplacesTheDeadlineOfEarlierFailures) {
  sunshinesvc::CoreRestartBackoff backoff;
  constexpr std::uint64_t now_tick = 100'000;

  // three quick failures put the count past its free retries; a fourth one is told to wait
  for (int i = 0; i < 3; ++i) sunshinesvc::core_restart_wait(backoff, 1300, false, now_tick);
  const auto blocked = sunshinesvc::core_restart_wait(backoff, 1300, false, now_tick);
  EXPECT_GT(blocked.delay_ms, 0U);
  EXPECT_EQ(blocked.not_before_tick, now_tick + blocked.delay_ms);

  // a core that ran a minute resets the count: its end clears the deadline, and the next pass may start the core at once
  const auto adopted = sunshinesvc::core_restart_wait(backoff, sunshinesvc::CORE_STABLE_RUNTIME_MS, false, 200'000);
  EXPECT_EQ(adopted.delay_ms, 0U);
  EXPECT_EQ(adopted.not_before_tick, 200'000U);

  // an end the service asked for does the same
  const auto asked_for = sunshinesvc::core_restart_wait(backoff, 500, true, 300'000);
  EXPECT_EQ(asked_for.delay_ms, 0U);
  EXPECT_EQ(asked_for.not_before_tick, 300'000U);
}
