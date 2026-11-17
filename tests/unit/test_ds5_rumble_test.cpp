/**
 * @file tests/unit/test_ds5_rumble_test.cpp
 * @brief The short test rumble of the gamepad test tool: limits, outcomes, and what a pad is given back.
 */
#include "src/ds5/rumble_test.h"

#include <gtest/gtest.h>

#include <vector>

namespace {
  namespace rt = ds5::rumble_test;

  struct call_t {
    int left, right;
    bool restore;
  };
  std::vector<call_t> calls;
  unsigned pads_alive = 0;

  unsigned
  fake_pads(std::uint8_t left, std::uint8_t right, bool restore) {
    calls.push_back({ left, right, restore });
    return pads_alive;
  }

  class Ds5RumbleTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      calls.clear();
      pads_alive = 0;
      rt::handler().store(nullptr);
    }
    void
    TearDown() override {
      rt::handler().store(nullptr);
    }
  };

  TEST_F(Ds5RumbleTest, DurationStaysInsideTheLimits) {
    EXPECT_EQ(rt::clamp_duration(0, false), rt::default_ms);
    EXPECT_EQ(rt::clamp_duration(-5), rt::default_ms);
    EXPECT_EQ(rt::clamp_duration(1), rt::min_ms);
    EXPECT_EQ(rt::clamp_duration(400), 400u);
    EXPECT_EQ(rt::clamp_duration(60000), rt::max_ms);
  }

  TEST_F(Ds5RumbleTest, WithoutANativePathItIsUnsupported) {
    const auto pulse = rt::run(100, [](auto) {});
    EXPECT_EQ(pulse.outcome, rt::outcome_e::unsupported);
    EXPECT_STREQ(rt::to_string(pulse.outcome), "unsupported");
  }

  TEST_F(Ds5RumbleTest, NoLivePadSendsNothingAndDoesNotWait) {
    rt::handler().store(&fake_pads);
    bool waited = false;
    const auto pulse = rt::run(100, [&](auto) { waited = true; });
    EXPECT_EQ(pulse.outcome, rt::outcome_e::no_pad);
    EXPECT_FALSE(waited);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_FALSE(calls[0].restore);
  }

  TEST_F(Ds5RumbleTest, OnePulseThenTheRestoreForEveryLivePad) {
    rt::handler().store(&fake_pads);
    pads_alive = 2;
    std::chrono::milliseconds waited { 0 };
    const auto pulse = rt::run(300, [&](auto d) { waited = d; });
    EXPECT_EQ(pulse.outcome, rt::outcome_e::sent);
    EXPECT_EQ(pulse.pads, 2u);
    EXPECT_EQ(waited.count(), 300);
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].left, rt::strength);
    EXPECT_EQ(calls[0].right, rt::strength);
    EXPECT_FALSE(calls[0].restore);
    EXPECT_TRUE(calls[1].restore);
  }

  TEST_F(Ds5RumbleTest, ARunningGameKeepsItsRumble) {
    EXPECT_EQ(rt::restore_values(true, { 90, 30 }), (std::array<std::uint8_t, 2> { 90, 30 }));
    EXPECT_EQ(rt::restore_values(false, { 90, 30 }), (std::array<std::uint8_t, 2> { 0, 0 }));
    EXPECT_EQ(rt::restore_values(true, { 0, 0 }), (std::array<std::uint8_t, 2> { 0, 0 }));
  }

  TEST_F(Ds5RumbleTest, ASecondPulseWhileOneRunsIsBusy) {
    rt::handler().store(&fake_pads);
    pads_alive = 1;
    rt::pulse_t inner;
    const auto outer = rt::run(100, [&](auto) { inner = rt::run(100, [](auto) {}); });
    EXPECT_EQ(outer.outcome, rt::outcome_e::sent);
    EXPECT_EQ(inner.outcome, rt::outcome_e::busy);
  }
}  // namespace
