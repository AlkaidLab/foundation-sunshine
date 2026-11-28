/**
 * @file tests/unit/test_ds5_pad_test.cpp
 * @brief The pad tests of the Web UI: their sequences, timings and the block of the trigger test.
 */
#include <gtest/gtest.h>

#include <vector>

#include "src/ds5/pad_test.h"
#include "src/ds5/tuning_trigger.h"

namespace {
  using namespace ds5::pad_test;
  using namespace std::chrono_literals;

  std::vector<step_e> steps;
  unsigned pads_reached = 1;

  unsigned
  record(step_e step) {
    steps.push_back(step);
    return pads_reached;
  }

  struct fixture_t {
    std::vector<std::chrono::milliseconds> waits;
    fixture_t() {
      steps.clear();
      pads_reached = 1;
      handler().store(&record);
    }
    ~fixture_t() { handler().store(nullptr); }
    auto
    waiter() {
      return [this](std::chrono::milliseconds wait) { waits.push_back(wait); };
    }
    std::chrono::milliseconds
    total() const {
      std::chrono::milliseconds sum { 0 };
      for (const auto wait : waits) sum += wait;
      return sum;
    }
  };
}  // namespace

TEST(Ds5PadTest, ActionsAreReadFromTheirNames) {
  EXPECT_EQ(parse_action("identify"), action_e::identify);
  EXPECT_EQ(parse_action("trigger_left"), action_e::trigger_left);
  EXPECT_EQ(parse_action("trigger_right"), action_e::trigger_right);
  EXPECT_EQ(parse_action("stop"), action_e::stop);
  EXPECT_FALSE(parse_action("").has_value());
  EXPECT_FALSE(parse_action("haptics").has_value());
}

TEST(Ds5PadTest, IdentifyFlashesTheLightBarThreeTimesAndGivesTheOldColourBack) {
  fixture_t fixture;
  const auto result = run(action_e::identify, fixture.waiter());
  EXPECT_EQ(result.outcome, outcome_e::sent);
  EXPECT_EQ(result.pads, 1u);
  const std::vector<step_e> expected { step_e::lightbar_white, step_e::lightbar_off, step_e::lightbar_white, step_e::lightbar_off,
    step_e::lightbar_white, step_e::lightbar_off, step_e::lightbar_restore };
  EXPECT_EQ(steps, expected);
  EXPECT_EQ(fixture.total(), 3 * 400ms) << "three flashes of 250 ms on and 150 ms off";
  for (const auto wait : fixture.waits) EXPECT_LE(wait, 25ms);
}

TEST(Ds5PadTest, ATriggerTestHoldsTheEffectAndThenReleasesThatTriggerOnly) {
  fixture_t fixture;
  EXPECT_EQ(run(action_e::trigger_left, fixture.waiter()).outcome, outcome_e::sent);
  EXPECT_EQ(steps, (std::vector<step_e> { step_e::trigger_left_on, step_e::trigger_left_off }));
  EXPECT_EQ(fixture.total(), 1500ms);
  steps.clear();
  EXPECT_EQ(run(action_e::trigger_right, fixture.waiter()).outcome, outcome_e::sent);
  EXPECT_EQ(steps, (std::vector<step_e> { step_e::trigger_right_on, step_e::trigger_right_off }));
}

TEST(Ds5PadTest, StopEndsEverythingAtOnceWithoutWaiting) {
  fixture_t fixture;
  EXPECT_EQ(run(action_e::stop, fixture.waiter()).outcome, outcome_e::sent);
  EXPECT_EQ(steps, (std::vector<step_e> { step_e::stop_all }));
  EXPECT_TRUE(fixture.waits.empty());
}

TEST(Ds5PadTest, StopEndsATestThatIsWaitingWithinAFewMillisecondsAndItsEffectIsGivenBack) {
  fixture_t fixture;
  int slices = 0;
  const auto result = run(action_e::trigger_left, [&](std::chrono::milliseconds) {
    if (++slices == 4) EXPECT_EQ(run(action_e::stop, [](auto) {}).outcome, outcome_e::sent) << "stop does not wait for the test that runs";
  });
  EXPECT_EQ(result.outcome, outcome_e::sent);
  EXPECT_EQ(slices, 4) << "the waiting test heard stop at its next slice";
  EXPECT_EQ(steps, (std::vector<step_e> { step_e::trigger_left_on, step_e::stop_all, step_e::trigger_left_off }));
}

TEST(Ds5PadTest, NoPadMeansNothingIsSentAndNothingWaits) {
  fixture_t fixture;
  pads_reached = 0;
  for (const auto action : { action_e::identify, action_e::trigger_left, action_e::trigger_right, action_e::stop }) {
    steps.clear();
    EXPECT_EQ(run(action, fixture.waiter()).outcome, outcome_e::no_pad);
    EXPECT_EQ(steps.size(), 1u);
  }
  EXPECT_TRUE(fixture.waits.empty());
}

TEST(Ds5PadTest, WithoutANativeClientThePathIsUnsupportedAndWhileOneRunsAnotherIsBusy) {
  handler().store(nullptr);
  EXPECT_EQ(run(action_e::identify, [](auto) {}).outcome, outcome_e::unsupported);
  handler().store([](step_e) -> unsigned { return 1; });
  outcome_e inner = outcome_e::sent;
  const auto outer = run(action_e::trigger_left, [&](std::chrono::milliseconds) { inner = run(action_e::identify, [](auto) {}).outcome; });
  EXPECT_EQ(outer.outcome, outcome_e::sent);
  EXPECT_EQ(inner, outcome_e::busy);
  handler().store(nullptr);
}

TEST(Ds5PadTest, TheTriggerTestBlockIsAFeedbackBlockThatTheTriggerRewriteUnderstands) {
  const auto block = trigger_test_block();
  EXPECT_EQ(block[0], 0x21);
  EXPECT_EQ(block[1] | (block[2] << 8), 0x03fc) << "zones 2 to 9";
  const auto packed = static_cast<std::uint32_t>(block[3]) | (static_cast<std::uint32_t>(block[4]) << 8) | (static_cast<std::uint32_t>(block[5]) << 16) |
                      (static_cast<std::uint32_t>(block[6]) << 24);
  for (std::uint32_t zone = 2; zone < 10; ++zone) EXPECT_EQ((packed >> (3u * zone)) & 7u, 4u) << zone;
  EXPECT_EQ(ds5::tuning::trigger::classify(block[0]), ds5::tuning::trigger::Class::feedback);
  EXPECT_FALSE(ds5::tuning::trigger::is_off(block));
}

TEST(Ds5PadTest, TheTrackerSendsAHeldBlockAgainAfterItForgotWhatWasSent) {
  using namespace ds5::tuning::trigger;
  tracker_t tracker;
  config_t config;
  std::array<std::uint8_t, 64> payload {};
  const auto block = trigger_test_block();
  std::copy(block.begin(), block.end(), payload.begin() + 22);
  const auto first = tracker.plan(payload.data(), 0x08, config);
  EXPECT_NE(first.flags, 0);
  EXPECT_EQ(tracker.plan(nullptr, 0, config).flags, 0) << "an unchanged block is not sent twice";
  tracker.forget_sent();
  const auto again = tracker.plan(nullptr, 0, config);
  EXPECT_EQ(again.flags & 0x08, 0x08);
  EXPECT_EQ(again.left, first.left);
}
