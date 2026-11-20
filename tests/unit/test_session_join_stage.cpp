#include "src/streaming/session_join_stage.h"

#include <gtest/gtest.h>

#include <iterator>
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
