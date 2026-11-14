/**
 * @file tests/unit/test_host_bitrate_cap.cpp
 * @brief The host bitrate ceiling (tray 码率上限) on a running session: the budget's clamp and the ABR range under it.
 */
#include "src/streaming/bitrate.h"

#include <gtest/gtest.h>

using streaming::abr_range_under_ceiling;
using streaming::bitrate_budget_t;
using streaming::bitrate_range_t;

// ---- the budget: the encoder's total under the ceiling ----

TEST(HostBitrateCap, ALoweredCeilingClampsTheRunningTotalAndTheEncoderSeesItThroughPending) {
  bitrate_budget_t budget(60000, 0, 0, 0, 0, false);  // 自动: no ceiling when the stream started
  EXPECT_EQ(budget.set_maximum(20000), 20000);
  const auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->total_kbps, 20000);
  budget.commit(*next);
  EXPECT_EQ(budget.total(), 20000);
}

TEST(HostBitrateCap, AutoGivesBackWhatTheClientAskedForNotTheHeldValue) {
  bitrate_budget_t budget(60000, 0, 0, 0, 0, false);
  budget.set_maximum(20000);
  // the client asks again while held (the host's bitrate callback makes some clients repeat their own value): the ask is remembered whole, the total is held
  EXPECT_EQ(budget.request(60000), 20000);
  EXPECT_EQ(budget.set_maximum(0), 60000);
  EXPECT_EQ(budget.maximum(), 0);
}

TEST(HostBitrateCap, ARequestAboveTheCeilingIsNotClampedBeforeTheBudgetSeesIt) {
  // stream.cpp hands the budget the raw ask (request_total_bitrate); a pre-clamped ask would be forgotten when the ceiling is raised
  bitrate_budget_t budget(30000, 40000, 0, 0, 0, false);
  EXPECT_EQ(budget.request(90000), 40000);
  EXPECT_EQ(budget.set_maximum(70000), 70000);
  EXPECT_EQ(budget.set_maximum(120000), 90000);
}

TEST(HostBitrateCap, ARaisedCeilingNeverRaisesASessionAboveItsAsk) {
  bitrate_budget_t budget(25000, 0, 0, 0, 0, false);
  EXPECT_EQ(budget.set_maximum(50000), 25000);
  EXPECT_FALSE(budget.pending(1000));
}

TEST(HostBitrateCap, TheNewTotalKeepsTheFecAndReservesInsideIt) {
  bitrate_budget_t budget(60000, 0, 512, 500, 20, false);
  const auto before = budget.applied();
  budget.set_maximum(30000);
  const auto next = budget.pending(1000);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->total_kbps, 30000);
  EXPECT_LT(next->encoder_kbps, 30000);
  EXPECT_LT(next->encoder_kbps, before.encoder_kbps);
  EXPECT_EQ(next->fec_percentage, 20U);
}

// ---- the ABR: its range under the ceiling ----

TEST(HostBitrateCapAbr, ACeilingHoldsTheClientsMaximumAndLiftsItsMinimumDown) {
  EXPECT_EQ(abr_range_under_ceiling(0, 80000, 3000, 60000, 20000), (bitrate_range_t { 3000, 20000 }));
  EXPECT_EQ(abr_range_under_ceiling(30000, 80000, 3000, 60000, 20000), (bitrate_range_t { 20000, 20000 }));  // min never above max
}

TEST(HostBitrateCapAbr, WithoutAClientMaximumTheSessionInheritsTheCeiling) {
  EXPECT_EQ(abr_range_under_ceiling(0, 0, 3000, 60000, 90000), (bitrate_range_t { 3000, 90000 }));
  EXPECT_EQ(abr_range_under_ceiling(0, 0, 3000, 60000, 20000), (bitrate_range_t { 3000, 20000 }));
}

TEST(HostBitrateCapAbr, AutoMeansNoExtraCapTheClientsOrThePresetMaximumApplies) {
  EXPECT_EQ(abr_range_under_ceiling(0, 80000, 3000, 60000, 0), (bitrate_range_t { 3000, 80000 }));
  EXPECT_EQ(abr_range_under_ceiling(0, 0, 3000, 60000, 0), (bitrate_range_t { 3000, 60000 }));
  EXPECT_EQ(abr_range_under_ceiling(0, 0, 3000, 60000, -5), (bitrate_range_t { 3000, 60000 }));
}

TEST(HostBitrateCapAbr, ARaisedOrRemovedCeilingGivesTheClientsOwnRangeBack) {
  const auto at = [](int ceiling) { return abr_range_under_ceiling(5000, 80000, 3000, 60000, ceiling); };
  EXPECT_EQ(at(20000).max_kbps, 20000);
  EXPECT_EQ(at(50000).max_kbps, 50000);
  EXPECT_EQ(at(120000).max_kbps, 80000);  // above what the client asked: what it asked
  EXPECT_EQ(at(0).max_kbps, 80000);
  EXPECT_EQ(at(0).min_kbps, 5000);
}

TEST(HostBitrateCapAbr, TheClientsOwnMinimumBeatsThePreset) {
  EXPECT_EQ(abr_range_under_ceiling(8000, 0, 3000, 60000, 0), (bitrate_range_t { 8000, 60000 }));
}
