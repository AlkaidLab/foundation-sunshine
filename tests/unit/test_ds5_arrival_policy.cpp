/**
 * @file tests/unit/test_ds5_arrival_policy.cpp
 * @brief A controller arrival replaces a pad made without metadata and never tears down a pad it already made.
 */
#include "src/ds5/arrival_policy.h"

#include <gtest/gtest.h>

namespace {
  using ds5::arrival::action_e;
  using ds5::arrival::decide;
  using ds5::arrival::meta_t;

  const meta_t pad { 2, 251, 0x3ff };
}  // namespace

TEST(Ds5ArrivalPolicy, FreeSlotAllocates) {
  EXPECT_EQ(decide(false, false, {}, pad), action_e::allocate);
}

TEST(Ds5ArrivalPolicy, PadMadeWithoutMetadataIsReplacedByTheRealArrival) {
  // The ghost case: a multi-controller packet held slot 0 before the real pad connected mid-stream.
  EXPECT_EQ(decide(true, false, {}, pad), action_e::replace);
}

TEST(Ds5ArrivalPolicy, SamePadAnnouncedAgainIsIgnored) {
  EXPECT_EQ(decide(true, true, pad, pad), action_e::ignore_duplicate);
}

TEST(Ds5ArrivalPolicy, OtherDataForAPadThatWasNotRemovedIsIgnored) {
  // A repeating arrival with reduced capabilities (an arrival flood seen in testing) must not rebuild the pad.
  EXPECT_EQ(decide(true, true, pad, meta_t { 2, 11, 0x3ff }), action_e::ignore_conflict);
}
