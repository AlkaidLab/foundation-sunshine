/**
 * @file tests/unit/platform/windows/test_d3d12_resource_ring.cpp
 * @brief Tests for the non-blocking D3D12 video resource ring.
 */
#include "src/platform/windows/d3d12/d3d12_resource_ring.h"

#include <gtest/gtest.h>

namespace {
  namespace d3d12 = platf::dxgi::d3d12;

  TEST(D3D12ResourceRing, RequiresAGenerationBeforeAcquiring) {
    d3d12::resource_ring_t ring;

    EXPECT_FALSE(ring.try_acquire(0));
    EXPECT_TRUE(ring.begin_generation(1));
    EXPECT_TRUE(ring.try_acquire(0));
  }

  TEST(D3D12ResourceRing, UsesThreeSlotsWithoutBlocking) {
    d3d12::resource_ring_t ring;
    ASSERT_TRUE(ring.begin_generation(1));

    EXPECT_EQ(*ring.try_acquire(0), 0u);
    EXPECT_EQ(*ring.try_acquire(0), 1u);
    EXPECT_EQ(*ring.try_acquire(0), 2u);
    EXPECT_FALSE(ring.try_acquire(0));
    EXPECT_EQ(ring.high_watermark(), 3u);
  }

  TEST(D3D12ResourceRing, CanCancelAnUnsubmittedCapture) {
    d3d12::resource_ring_t ring;
    ASSERT_TRUE(ring.begin_generation(1));
    const auto acquired = ring.try_acquire(0);
    ASSERT_TRUE(acquired);
    EXPECT_TRUE(ring.cancel_capture(*acquired));
    EXPECT_EQ(
      ring.slot(*acquired).state,
      d3d12::slot_state_e::free);
    EXPECT_FALSE(ring.cancel_capture(*acquired));
  }

  TEST(D3D12ResourceRing, WaitsForEncoderAndReadbackOwnership) {
    d3d12::resource_ring_t ring;
    ASSERT_TRUE(ring.begin_generation(1));
    const auto index = *ring.try_acquire(0);

    ASSERT_TRUE(ring.mark_capture_ready(index, 1));
    ASSERT_TRUE(ring.mark_compute_queued(index, 2, true));
    ASSERT_TRUE(ring.mark_encoder_queued(index, 3));

    ring.retire_completed(3);
    EXPECT_EQ(ring.slot(index).state, d3d12::slot_state_e::encoder_queued);

    ASSERT_TRUE(ring.release_analysis_readback(index, 1));
    ring.retire_completed(3);
    EXPECT_EQ(ring.slot(index).state, d3d12::slot_state_e::free);
  }

  TEST(D3D12ResourceRing, RejectsInvalidTransitionsAndFenceRegression) {
    d3d12::resource_ring_t ring;
    ASSERT_TRUE(ring.begin_generation(1));
    const auto index = *ring.try_acquire(0);

    EXPECT_FALSE(ring.mark_compute_queued(index, 1, false));
    ASSERT_TRUE(ring.mark_capture_ready(index, 2));
    EXPECT_FALSE(ring.mark_compute_queued(index, 2, false));
    ASSERT_TRUE(ring.mark_compute_queued(index, 3, false));
    EXPECT_FALSE(ring.mark_encoder_queued(index, 1));
    EXPECT_TRUE(ring.mark_encoder_queued(index, 4));
  }

  TEST(D3D12ResourceRing, RejectsStaleGenerationReadback) {
    d3d12::resource_ring_t ring;
    ASSERT_TRUE(ring.begin_generation(1));
    const auto index = *ring.try_acquire(0);
    ASSERT_TRUE(ring.mark_capture_ready(index, 1));
    ASSERT_TRUE(ring.mark_compute_queued(index, 2, true));
    ASSERT_TRUE(ring.mark_encoder_queued(index, 3));
    ASSERT_TRUE(ring.release_analysis_readback(index, 1));
    ring.retire_completed(3);
    ASSERT_TRUE(ring.begin_generation(2));

    const auto next = *ring.try_acquire(3);
    ASSERT_TRUE(ring.mark_capture_ready(next, 4));
    ASSERT_TRUE(ring.mark_compute_queued(next, 5, true));
    EXPECT_FALSE(ring.release_analysis_readback(next, 1));
    EXPECT_TRUE(ring.release_analysis_readback(next, 2));
  }
}  // namespace
