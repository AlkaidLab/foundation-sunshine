#include "src/streaming/session_join_stage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>

using streaming::join_stage_e;

namespace {
  constexpr join_stage_e all_stages[] = {
    join_stage_e::video_thread,
    join_stage_e::audio_thread,
    join_stage_e::live_session,
    join_stage_e::control_end,
    join_stage_e::input_reset,
    join_stage_e::display_restore,
    join_stage_e::platform_stop,
    join_stage_e::bookkeeping,
  };
}  // namespace

TEST(SessionJoinStage, EveryStageHasItsOwnName) {
  std::set<std::string> names;
  for (const auto stage : all_stages) {
    const auto name = streaming::join_stage_name(stage);
    EXPECT_FALSE(name.empty());
    EXPECT_NE(name, "unknown");
    names.emplace(name);
  }
  EXPECT_EQ(names.size(), streaming::join_stage_count);
  EXPECT_EQ(std::size(all_stages), streaming::join_stage_count);
}

TEST(SessionJoinStage, ThreadStepsKeepTheShortLimit) {
  using namespace std::chrono_literals;
  for (const auto stage : { join_stage_e::video_thread, join_stage_e::audio_thread, join_stage_e::live_session, join_stage_e::control_end }) {
    EXPECT_TRUE(streaming::join_stage_waits_for_thread(stage));
    EXPECT_EQ(streaming::join_stage_budget(stage), 10s);
  }
}

TEST(SessionJoinStage, SlowButNotStuckStepsGetALongerLimit) {
  using namespace std::chrono_literals;
  for (const auto stage : { join_stage_e::input_reset, join_stage_e::display_restore, join_stage_e::platform_stop, join_stage_e::bookkeeping }) {
    EXPECT_FALSE(streaming::join_stage_waits_for_thread(stage));
    EXPECT_EQ(streaming::join_stage_budget(stage), 60s);
  }
}

TEST(SessionJoinProgress, WatchdogOfTheCurrentStepReportsThatStep) {
  streaming::join_progress_t progress;
  const auto generation = progress.enter(join_stage_e::audio_thread);
  const auto hung = progress.hung_stage(generation);
  ASSERT_TRUE(hung.has_value());
  EXPECT_EQ(*hung, join_stage_e::audio_thread);
}

TEST(SessionJoinProgress, EveryStageChangeStartsANewGeneration) {
  streaming::join_progress_t progress;
  std::set<std::uint64_t> generations;
  for (const auto stage : all_stages) {
    generations.insert(progress.enter(stage));
  }
  EXPECT_EQ(generations.size(), streaming::join_stage_count);
}

TEST(SessionJoinProgress, WatchdogOfAnEarlierStageDoesNotBlameTheNewOne) {
  streaming::join_progress_t progress;
  const auto video = progress.enter(join_stage_e::video_thread);
  const auto audio = progress.enter(join_stage_e::audio_thread);
  EXPECT_FALSE(progress.hung_stage(video).has_value());
  EXPECT_EQ(progress.hung_stage(audio), join_stage_e::audio_thread);

  // the same step entered again is a new generation too: the watchdog armed before it must not fire for it
  const auto audio_again = progress.enter(join_stage_e::audio_thread);
  EXPECT_FALSE(progress.hung_stage(audio).has_value());
  EXPECT_EQ(progress.hung_stage(audio_again), join_stage_e::audio_thread);
}

TEST(SessionJoinProgress, EachLaterStepKeepsItsOwnFullBudget) {
  using namespace std::chrono_literals;
  streaming::join_progress_t progress;
  // input-reset, display-restore, platform-stop and bookkeeping are armed one by one with 60 s each; only the last one is ever live
  std::uint64_t previous = progress.enter(join_stage_e::input_reset);
  for (const auto stage : { join_stage_e::display_restore, join_stage_e::platform_stop, join_stage_e::bookkeeping }) {
    EXPECT_EQ(streaming::join_stage_budget(stage), 60s);
    const auto generation = progress.enter(stage);
    EXPECT_FALSE(progress.hung_stage(previous).has_value());
    EXPECT_EQ(progress.hung_stage(generation), stage);
    previous = generation;
  }
}

TEST(SessionJoinProgress, ReturnedSessionIgnoresPendingWatchdogs) {
  streaming::join_progress_t progress;
  const auto generation = progress.enter(join_stage_e::bookkeeping);
  progress.finish();
  EXPECT_FALSE(progress.hung_stage(generation).has_value());
}

TEST(SessionJoinProgress, TwoSessionsEndedAtOnceDoNotSeeEachOther) {
  auto a = std::make_shared<streaming::join_progress_t>();
  auto b = std::make_shared<streaming::join_progress_t>();
  const auto a_generation = a->enter(join_stage_e::video_thread);
  const auto b_generation = b->enter(join_stage_e::video_thread);
  b->enter(join_stage_e::bookkeeping);
  // b moved on; a still sits in its video-thread wait and is blamed for that, not for b's step
  EXPECT_EQ(a->hung_stage(a_generation), join_stage_e::video_thread);
  EXPECT_FALSE(b->hung_stage(b_generation).has_value());
  b->finish();
  EXPECT_EQ(a->hung_stage(a_generation), join_stage_e::video_thread);
}

TEST(SessionJoinProgress, CallbackKeepsItsProgressAliveAfterTheSessionReturned) {
  std::weak_ptr<streaming::join_progress_t> weak;
  std::uint64_t generation = 0;
  std::function<std::optional<join_stage_e>()> callback;
  {
    auto progress = std::make_shared<streaming::join_progress_t>();
    weak = progress;
    generation = progress->enter(join_stage_e::platform_stop);
    callback = [progress, generation]() { return progress->hung_stage(generation); };
    progress->finish();
  }
  EXPECT_FALSE(weak.expired());
  EXPECT_FALSE(callback().has_value());
}
