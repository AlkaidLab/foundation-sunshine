/**
 * @file tests/unit/test_launch_splash.cpp
 * @brief The launch splash's stage mapping, the launcher status file and the rules that end the splash.
 */
#include <gtest/gtest.h>

#include <src/launch_splash/launch_splash_model.h>
#include <src/launch_splash/shown_gate.h>

#include <thread>

using namespace launch_splash;

namespace {
  constexpr std::int64_t t0 = 1'000'000;

  launcher_status_t
  status_for(const std::string &id, const char *stage = nullptr) {
    launcher_status_t status;
    status.launch = id;
    if (stage != nullptr) {
      status.stage = stage_from_key(stage);
    }
    return status;
  }
}  // namespace

TEST(LaunchSplashStages, KeysRoundTripAndAreOrdered) {
  const stage_e all[] = { stage_e::starting, stage_e::broker, stage_e::preparing, stage_e::injecting, stage_e::waiting_window, stage_e::failed };
  for (const auto stage : all) {
    EXPECT_EQ(stage_from_key(stage_key(stage)), stage);
  }
  EXPECT_LT(stage_e::starting, stage_e::broker);
  EXPECT_LT(stage_e::broker, stage_e::preparing);
  EXPECT_LT(stage_e::preparing, stage_e::injecting);
  EXPECT_LT(stage_e::injecting, stage_e::waiting_window);
  EXPECT_EQ(stage_key(stage_e::waiting_window), "waiting-window");
  EXPECT_FALSE(stage_from_key("running").has_value());
  EXPECT_EQ(kind_key(kind_e::launch), "launch");
  EXPECT_EQ(kind_key(kind_e::resume), "resume");
}

TEST(LaunchSplashStatusFile, ReadsTheLaunchersFields) {
  const auto status = parse_launcher_status(R"({"launch":"7-1","stage":"preparing","pids":[1234,5678],"notice":"direct"})");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->launch, "7-1");
  EXPECT_EQ(status->stage, stage_e::preparing);
  EXPECT_EQ(status->notice, "direct");
  EXPECT_EQ(status->pids, (std::vector<std::uint32_t> { 1234, 5678 }));
}

TEST(LaunchSplashStatusFile, FailedCarriesAReasonCode) {
  const auto status = parse_launcher_status(R"({"launch":"7-1","stage":"failed","reason":"timed-out"})");
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->stage, stage_e::failed);
  EXPECT_EQ(status->reason, "timed-out");
}

TEST(LaunchSplashStatusFile, RejectsWhatIsNotAStatusAndDropsFreeText) {
  EXPECT_FALSE(parse_launcher_status("").has_value());
  EXPECT_FALSE(parse_launcher_status("not json").has_value());
  EXPECT_FALSE(parse_launcher_status("[1,2]").has_value());
  EXPECT_FALSE(parse_launcher_status("{\"launch\":\"a\",\"stage\":").has_value());  // half a write

  const auto status = parse_launcher_status(R"({"launch":"7-1","stage":"fusing","reason":"The Broker did not answer!","notice":"has space","pids":[0,-5,"x",4294967296,42]})");
  ASSERT_TRUE(status.has_value());
  EXPECT_FALSE(status->stage.has_value());  // a stage it does not know is no stage
  EXPECT_TRUE(status->reason.empty());  // free text is not a code
  EXPECT_TRUE(status->notice.empty());
  EXPECT_EQ(status->pids, (std::vector<std::uint32_t> { 42 }));
}

TEST(LaunchSplashTracker, BeginsAtStartingWithAMinuteToLive) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Genshin Impact", "", kind_e::launch, t0);
  const auto &view = tracker.view();
  EXPECT_TRUE(view.active);
  EXPECT_EQ(view.id, id);
  EXPECT_EQ(view.app_name, "Genshin Impact");
  EXPECT_EQ(view.stage, stage_e::starting);
  EXPECT_EQ(view.started_at_ms, t0);
  EXPECT_EQ(view.deadline_ms, t0 + 60'000);
  EXPECT_NE(tracker.begin("13", "Genshin Impact", "", kind_e::launch, t0), id);  // every launch has its own id
}

TEST(LaunchSplashTracker, TakesOnlyItsOwnLaunchersStatus) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0);
  EXPECT_FALSE(tracker.apply(status_for("an-older-launch", "preparing"), t0 + 10));
  EXPECT_EQ(tracker.view().stage, stage_e::starting);
  EXPECT_TRUE(tracker.apply(status_for(id, "preparing"), t0 + 20));
  EXPECT_EQ(tracker.view().stage, stage_e::preparing);
}

TEST(LaunchSplashTracker, StagesOnlyAdvance) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0);
  EXPECT_TRUE(tracker.apply(status_for(id, "injecting"), t0 + 1));
  EXPECT_FALSE(tracker.apply(status_for(id, "broker"), t0 + 2));  // a late, older write
  EXPECT_FALSE(tracker.apply(status_for(id, "injecting"), t0 + 3));  // the same stage again changes nothing
  EXPECT_EQ(tracker.view().stage, stage_e::injecting);
  EXPECT_TRUE(tracker.apply(status_for(id, "waiting-window"), t0 + 4));
  EXPECT_EQ(tracker.view().stage, stage_e::waiting_window);
}

TEST(LaunchSplashTracker, FailureShowsItsReasonThenGoesAfterFiveSeconds) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0);
  launcher_status_t failed = status_for(id, "failed");
  failed.reason = "broker";
  EXPECT_TRUE(tracker.apply(failed, t0 + 1000));
  EXPECT_EQ(tracker.view().stage, stage_e::failed);
  EXPECT_EQ(tracker.view().notice, "broker");
  EXPECT_EQ(tracker.view().deadline_ms, t0 + 1000 + 5000);
  EXPECT_FALSE(tracker.tick(t0 + 5999));
  EXPECT_TRUE(tracker.view().active);
  EXPECT_TRUE(tracker.tick(t0 + 6000));
  EXPECT_FALSE(tracker.view().active);
}

TEST(LaunchSplashTracker, AFailureAfterTheEndOrTwiceChangesNothing) {
  tracker_t tracker;
  tracker.begin("13", "Game", "", kind_e::launch, t0);
  EXPECT_TRUE(tracker.fail("rpc", t0 + 10));
  EXPECT_FALSE(tracker.fail("lost", t0 + 20));
  EXPECT_EQ(tracker.view().notice, "rpc");
  EXPECT_TRUE(tracker.end());
  EXPECT_FALSE(tracker.fail("lost", t0 + 30));
}

TEST(LaunchSplashTracker, AFailureNeverLengthensTheDeadline) {
  tracker_t tracker;
  tracker.begin("13", "Game", "", kind_e::launch, t0);
  ASSERT_TRUE(tracker.fail("timed-out", t0 + 58'000));
  EXPECT_EQ(tracker.view().deadline_ms, t0 + 60'000);  // not 58 s + 5 s
}

TEST(LaunchSplashTracker, ADirectStartNoticeIsOneLineForFiveSecondsAndTheLaunchGoesOn) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0);
  launcher_status_t direct = status_for(id, "waiting-window");
  direct.notice = "direct";
  EXPECT_TRUE(tracker.apply(direct, t0 + 2000));
  EXPECT_EQ(tracker.view().notice, "direct");
  EXPECT_EQ(tracker.view().stage, stage_e::waiting_window);
  EXPECT_FALSE(tracker.apply(direct, t0 + 3000));  // the same notice again is the same line; the five seconds do not restart
  EXPECT_FALSE(tracker.tick(t0 + 6999));
  EXPECT_TRUE(tracker.tick(t0 + 7000));
  EXPECT_TRUE(tracker.view().notice.empty());
  EXPECT_TRUE(tracker.view().active);
}

TEST(LaunchSplashTracker, TimesOutAtSixtySeconds) {
  tracker_t tracker;
  tracker.begin("13", "Game", "", kind_e::launch, t0);
  EXPECT_FALSE(tracker.tick(t0 + 59'999));
  EXPECT_TRUE(tracker.view().active);
  EXPECT_TRUE(tracker.tick(t0 + 60'000));
  EXPECT_FALSE(tracker.view().active);
  EXPECT_FALSE(tracker.tick(t0 + 61'000));  // already over: no second change
}

TEST(LaunchSplashTracker, EndWhenTheStreamOrTheAppEndsKeepsTheIdForThePanel) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0);
  EXPECT_TRUE(tracker.end());
  EXPECT_FALSE(tracker.view().active);
  EXPECT_EQ(tracker.view().id, id);  // the Panel closes the splash of this id
  EXPECT_FALSE(tracker.end());  // nothing was showing
  EXPECT_FALSE(tracker.apply(status_for(id, "preparing"), t0 + 5));  // a launcher still writing cannot bring it back
}

TEST(LaunchSplashTracker, ProcessIdsAreUniqueNonZeroAndBounded) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0);
  tracker.add_pid(0);
  tracker.add_pid(100);
  tracker.add_pid(100);
  EXPECT_EQ(tracker.view().pids, (std::vector<std::uint32_t> { 100 }));
  launcher_status_t status = status_for(id);
  for (std::uint32_t pid = 200; pid < 200 + 40; ++pid) {
    status.pids.push_back(pid);
  }
  EXPECT_TRUE(tracker.apply(status, t0 + 1));
  EXPECT_EQ(tracker.view().pids.size(), max_pids);
  tracker.end();
  tracker.add_pid(7);
  EXPECT_TRUE(tracker.view().pids.empty());
}

TEST(LaunchSplashTracker, ResumeIsMarkedSoThePanelCanWaitBeforeShowingAndNeverStaysLong) {
  tracker_t tracker;
  tracker.begin("13", "Game", "", kind_e::resume, t0);
  EXPECT_EQ(to_json(tracker.view()).at("kind"), "resume");
  EXPECT_EQ(tracker.view().deadline_ms, t0 + 8000);  // the game is already running: a splash that finds no window must not sit over it
  EXPECT_TRUE(tracker.tick(t0 + 8000));
}

TEST(LaunchSplashJson, CarriesEverythingThePanelNeeds) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "原神", "C:/covers/genshin.jpg", kind_e::launch, t0);
  tracker.add_pid(321);
  tracker.apply(status_for(id, "waiting-window"), t0 + 1);
  const auto json = to_json(tracker.view());
  EXPECT_EQ(json.at("id"), id);
  EXPECT_TRUE(json.at("active").get<bool>());
  EXPECT_EQ(json.at("app_id"), "13");
  EXPECT_EQ(json.at("app_name"), "原神");
  EXPECT_EQ(json.at("art"), "C:/covers/genshin.jpg");
  EXPECT_EQ(json.at("stage"), "waiting-window");
  EXPECT_EQ(json.at("notice"), "");
  EXPECT_EQ(json.at("pids"), nlohmann::json::array({ 321 }));
  EXPECT_EQ(json.at("started_at_ms"), t0);
  EXPECT_EQ(json.at("deadline_ms"), t0 + 60'000);
  EXPECT_TRUE(json.at("display_ready").get<bool>());
}

TEST(LaunchSplashTracker, AnAnnouncedLaunchWaitsForTheDisplayAndGoesReadyOnce) {
  tracker_t tracker;
  // announced when the request is accepted, before the display and encoder setup: the Panel readies its window and does not show it
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0, false);
  EXPECT_FALSE(tracker.view().display_ready);
  EXPECT_FALSE(to_json(tracker.view()).at("display_ready").get<bool>());
  // the same launch goes on under the same id when the display is set up
  EXPECT_TRUE(tracker.set_display_ready());
  EXPECT_TRUE(tracker.view().display_ready);
  EXPECT_EQ(tracker.id(), id);
  EXPECT_FALSE(tracker.set_display_ready());
  // a launch begun with its display already up never was waiting
  tracker.begin("13", "Game", "", kind_e::launch, t0 + 5);
  EXPECT_TRUE(tracker.view().display_ready);
  EXPECT_FALSE(tracker.set_display_ready());
}

TEST(LaunchSplashTracker, AnEndedLaunchHasNothingToMakeReady) {
  tracker_t tracker;
  tracker.begin("13", "Game", "", kind_e::launch, t0, false);
  tracker.end();
  EXPECT_FALSE(tracker.set_display_ready());
}

TEST(LaunchSplashTracker, TheStageStillsAdvancesWhileTheDisplayIsNotReady) {
  tracker_t tracker;
  const auto id = tracker.begin("13", "Game", "", kind_e::launch, t0, false);
  EXPECT_TRUE(tracker.apply(status_for(id, "preparing"), t0 + 10));
  EXPECT_FALSE(tracker.view().display_ready);
}

TEST(LaunchSplashWait, TheAppStartWaitsAtMostUnderASecond) {
  EXPECT_GT(shown_wait_ms, 0);
  EXPECT_LE(shown_wait_ms, 1000);
}

namespace {
  using namespace std::chrono_literals;
  using how_e = shown_gate_t::how_e;
}  // namespace

TEST(LaunchSplashShownGate, AnAnswerBeforeTheWaitCostsNothing) {
  shown_gate_t gate;
  gate.arm("a");
  EXPECT_TRUE(gate.note("a", 41, 700));
  const auto out = gate.wait("a", 800ms, true);
  EXPECT_EQ(out.how, how_e::answered);
  EXPECT_EQ(out.panel_ms, 41);
  EXPECT_EQ(out.prepare_ms, 700);
  EXPECT_LT(out.waited_ms, 50);
}

TEST(LaunchSplashShownGate, AnAnswerDuringTheWaitEndsItAtOnce) {
  shown_gate_t gate;
  gate.arm("a");
  std::thread panel([&] {
    std::this_thread::sleep_for(60ms);
    gate.note("a", 55, 0);
  });
  const auto out = gate.wait("a", 5000ms, true);
  panel.join();
  EXPECT_EQ(out.how, how_e::answered);
  EXPECT_GE(out.waited_ms, 40);
  EXPECT_LT(out.waited_ms, 2000);
}

TEST(LaunchSplashShownGate, ASilentPanelCostsExactlyTheBoundAndTheAppStartsAnyway) {
  shown_gate_t gate;
  gate.arm("a");
  const auto out = gate.wait("a", 120ms, true);
  EXPECT_EQ(out.how, how_e::timed_out);
  EXPECT_GE(out.waited_ms, 100);
  EXPECT_LT(out.waited_ms, 1000);
}

TEST(LaunchSplashShownGate, NoPanelThatCanAnswerMeansNoWaitAtAll) {
  shown_gate_t gate;
  gate.arm("a");
  const auto out = gate.wait("a", 5000ms, false);
  EXPECT_EQ(out.how, how_e::no_listener);
  EXPECT_LT(out.waited_ms, 50);
}

TEST(LaunchSplashShownGate, AnEndedLaunchReleasesTheWaiter) {
  shown_gate_t gate;
  gate.arm("a");
  std::thread ender([&] {
    std::this_thread::sleep_for(40ms);
    gate.withdraw();
  });
  const auto out = gate.wait("a", 5000ms, true);
  ender.join();
  EXPECT_EQ(out.how, how_e::withdrawn);
  EXPECT_LT(out.waited_ms, 2000);
}

TEST(LaunchSplashShownGate, OnlyTheArmedLaunchCanBeAnsweredAndOnlyOnce) {
  shown_gate_t gate;
  gate.arm("a");
  EXPECT_FALSE(gate.note("an-older-launch", 1, 1));
  EXPECT_FALSE(gate.note("", 1, 1));
  EXPECT_TRUE(gate.note("a", 10, 20));
  EXPECT_FALSE(gate.note("a", 99, 99));
  EXPECT_EQ(gate.wait("a", 10ms, true).panel_ms, 10);
  // a new launch starts clean: the old answer is not its answer
  gate.arm("b");
  EXPECT_EQ(gate.wait("b", 30ms, true).how, how_e::timed_out);
  EXPECT_EQ(gate.wait("a", 30ms, true).how, how_e::withdrawn);
}
