#include "src/transport/transport_budget.h"

#include <gtest/gtest.h>
#include <limits>

TEST(TransportBudget, UsesParityRelativeToDataRatherThanTotal) {
  const auto allocation = transport::allocate_budget({ .total_kbps = 40000, .fec_numerator = 20 });
  ASSERT_TRUE(allocation);
  EXPECT_EQ(allocation->encoder_kbps, 33333);
  EXPECT_EQ(allocation->primary_video_kbps, 40000);
}

TEST(TransportBudget, ChargesAllReservationsBeforeEncoding) {
  const auto allocation = transport::allocate_budget({
    .total_kbps = 40000,
    .other_kbps = 1000,
    .repair_kbps = 2000,
    .probe_kbps = 1000,
    .video_overhead_kbps = 600,
    .fec_numerator = 20,
  });
  ASSERT_TRUE(allocation);
  EXPECT_EQ(allocation->primary_video_kbps, 36000);
  EXPECT_EQ(allocation->encoder_kbps, 29500);
}

TEST(TransportBudget, RejectsInvalidInputsAndNeverCreatesNegativeRates) {
  EXPECT_FALSE(transport::allocate_budget({ .total_kbps = -1 }));
  EXPECT_FALSE(transport::allocate_budget({ .total_kbps = 2000, .other_kbps = -1 }));
  EXPECT_FALSE(transport::allocate_budget({ .total_kbps = 2000, .fec_denominator = 0 }));
  const auto exhausted = transport::allocate_budget({ .total_kbps = 2000, .other_kbps = 3000 });
  ASSERT_TRUE(exhausted);
  EXPECT_EQ(exhausted->encoder_kbps, 0);
  EXPECT_EQ(exhausted->primary_video_kbps, 0);
}

TEST(TransportBudget, BoundsExtremeIntegerInputs) {
  const auto max_int = std::numeric_limits<int>::max();
  const auto allocation = transport::allocate_budget({
    .total_kbps = max_int,
    .fec_numerator = std::numeric_limits<std::uint32_t>::max(),
    .fec_denominator = std::numeric_limits<std::uint32_t>::max(),
  });
  ASSERT_TRUE(allocation);
  EXPECT_EQ(allocation->encoder_kbps, max_int / 2);
  const auto exhausted = transport::allocate_budget({ .total_kbps = max_int, .other_kbps = max_int, .repair_kbps = max_int });
  ASSERT_TRUE(exhausted);
  EXPECT_EQ(exhausted->encoder_kbps, 0);
}

TEST(TransportBudget, IncreasingProtectionCannotIncreaseEncoderBudget) {
  int previous = 40000;
  for (unsigned percentage = 0; percentage <= 255; ++percentage) {
    const auto allocation = transport::allocate_budget({ .total_kbps = 40000, .fec_numerator = percentage });
    ASSERT_TRUE(allocation);
    EXPECT_LE(allocation->encoder_kbps, previous);
    EXPECT_LE(static_cast<std::uint64_t>(allocation->encoder_kbps) * (100 + percentage), 40000U * 100U);
    previous = allocation->encoder_kbps;
  }
}

TEST(TransportFec, DisabledFecIgnoresMinimumParity) {
  const auto block = transport::plan_fec_block(5, 0, 10);
  ASSERT_TRUE(block);
  EXPECT_EQ(block->parity_shards, 0);
  EXPECT_EQ(block->encoded_percentage, 0);
}

TEST(TransportFec, MinimumParityRoundTripsThroughActualHeaderPercentage) {
  // Existing floor(100*5/200) advertises 2%, which the client decodes as four
  // parity shards although five were generated. The plan must be coherent.
  const auto block = transport::plan_fec_block(200, 1, 5);
  ASSERT_TRUE(block);
  EXPECT_GE(block->parity_shards, 5);
  EXPECT_EQ(block->parity_shards, (200 * block->encoded_percentage + 99) / 100);
}

TEST(TransportFec, RejectsUnrepresentableMinimumAndOversizedRsBlocks) {
  EXPECT_FALSE(transport::plan_fec_block(0, 20, 0));
  EXPECT_FALSE(transport::plan_fec_block(1, 20, 3));
  EXPECT_FALSE(transport::plan_fec_block(255, 20, 0));
  EXPECT_FALSE(transport::plan_fec_block(100, 256, 0));
  EXPECT_FALSE(transport::plan_fec_block(1024, 0, 0));
}

TEST(TransportFec, ProtectedFrameBalancesNonemptyBlocks) {
  const auto frame = transport::plan_fec_frame(700, 20, 2);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->fec_skipped);
  EXPECT_EQ(frame->block_count, 4U);
  EXPECT_EQ(frame->data_shards(), 700U);
  for (std::size_t i = 0; i < frame->block_count; ++i) {
    EXPECT_GT(frame->blocks[i].data_shards, 0);
    EXPECT_LE(frame->blocks[i].total_shards(), transport::max_rs_shards);
  }
}

TEST(TransportFec, OversizedFrameExplicitlyFallsBackWithoutForgingProtection) {
  const auto frame = transport::plan_fec_frame(900, 20, 0);
  ASSERT_TRUE(frame);
  EXPECT_TRUE(frame->fec_skipped);
  EXPECT_EQ(frame->data_shards(), 900U);
  EXPECT_EQ(frame->parity_shards(), 0U);
  EXPECT_EQ(frame->block_count, 4U);
}

TEST(TransportFec, RejectsFramesOutsideTheExistingIndexSpace) {
  EXPECT_FALSE(transport::plan_fec_frame(0, 20, 0));
  EXPECT_FALSE(transport::plan_fec_frame(4093, 0, 0));
  ASSERT_TRUE(transport::plan_fec_frame(4092, 0, 0));
}

TEST(TransportFec, ExhaustivePlansRespectHeaderAndShardConstraints) {
  for (const unsigned minimum : {0U, 2U, 5U, 32U}) {
    for (const unsigned percentage : {0U, 1U, 5U, 20U, 50U, 100U, 255U}) {
      for (std::size_t data = 1; data <= 4092; ++data) {
        const auto frame = transport::plan_fec_frame(data, percentage, minimum);
        ASSERT_TRUE(frame);
        ASSERT_GE(frame->block_count, 1U);
        ASSERT_LE(frame->block_count, 4U);
        EXPECT_EQ(frame->data_shards(), data);
        for (std::size_t i = 0; i < frame->block_count; ++i) {
          const auto &block = frame->blocks[i];
          EXPECT_GT(block.data_shards, 0);
          EXPECT_LE(block.data_shards, transport::max_unprotected_shards);
          EXPECT_EQ(block.parity_shards, (block.data_shards * block.encoded_percentage + 99) / 100);
          if (block.encoded_percentage != 0) {
            EXPECT_GE(block.parity_shards, minimum);
            EXPECT_LE(block.total_shards(), transport::max_rs_shards);
          }
        }
      }
    }
  }
}

TEST(TransportBudget, CountsIpAndUdpHeadersConsistently) {
  EXPECT_EQ(transport::ip_datagram_bytes(1400, false), 1428U);
  EXPECT_EQ(transport::ip_datagram_bytes(1400, true), 1448U);
  EXPECT_FALSE(transport::ip_datagram_bytes(std::numeric_limits<std::uint64_t>::max(), true));
}
