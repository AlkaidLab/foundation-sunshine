/**
 * @file tests/unit/test_remote_usb_tombstone.cpp
 * @brief Tests for the bounded Remote USB lifecycle tombstone set.
 */

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "src/remote_usb/remote_usb_tombstone.h"

namespace {

  using store_t = remote_usb::bounded_tombstone_set<std::uint64_t>;

  store_t::policy
  policy(std::size_t max_entries = 4,
    std::chrono::milliseconds ttl = std::chrono::seconds(30)) {
    return {
      .max_entries = max_entries,
      .ttl = ttl,
    };
  }

}  // namespace

TEST(RemoteUsbTombstone, ExpiresAtTtlBoundary) {
  store_t store { policy(4, std::chrono::milliseconds { 10 }) };
  const auto now = store_t::clock_t::time_point {};

  ASSERT_TRUE(store.insert(7, now));
  EXPECT_TRUE(store.contains(7, now + std::chrono::milliseconds { 9 }));
  EXPECT_FALSE(store.contains(7, now + std::chrono::milliseconds { 10 }));
  EXPECT_EQ(store.size(now + std::chrono::milliseconds { 10 }), 0u);
}

TEST(RemoteUsbTombstone, EvictsOldestWhenCapacityIsReached) {
  store_t store { policy(2, std::chrono::hours { 1 }) };
  const auto now = store_t::clock_t::time_point {};

  ASSERT_TRUE(store.insert(1, now));
  ASSERT_TRUE(store.insert(2, now + std::chrono::milliseconds { 1 }));
  ASSERT_TRUE(store.insert(3, now + std::chrono::milliseconds { 2 }));

  EXPECT_FALSE(store.contains(1, now + std::chrono::milliseconds { 2 }));
  EXPECT_TRUE(store.contains(2, now + std::chrono::milliseconds { 2 }));
  EXPECT_TRUE(store.contains(3, now + std::chrono::milliseconds { 2 }));
  EXPECT_EQ(store.size(now + std::chrono::milliseconds { 2 }), 2u);
}

TEST(RemoteUsbTombstone, ReinsertionRefreshesWithoutGrowingResidentSet) {
  store_t store { policy(2, std::chrono::milliseconds { 10 }) };
  const auto now = store_t::clock_t::time_point {};

  ASSERT_TRUE(store.insert(9, now));
  ASSERT_TRUE(store.insert(9, now + std::chrono::milliseconds { 9 }));
  EXPECT_EQ(store.size(now + std::chrono::milliseconds { 9 }), 1u);
  EXPECT_TRUE(store.contains(9, now + std::chrono::milliseconds { 18 }));
  EXPECT_FALSE(store.contains(9, now + std::chrono::milliseconds { 19 }));
}

TEST(RemoteUsbTombstone, RefreshKeepsExpiryOrderForSweeping) {
  store_t store { policy(3, std::chrono::milliseconds { 10 }) };
  const auto now = store_t::clock_t::time_point {};

  ASSERT_TRUE(store.insert(1, now));
  ASSERT_TRUE(store.insert(2, now + std::chrono::milliseconds { 1 }));
  ASSERT_TRUE(store.insert(1, now + std::chrono::milliseconds { 2 }));

  /* Key 2 expires first even though key 1 was inserted first originally. */
  EXPECT_EQ(store.size(now + std::chrono::milliseconds { 11 }), 1u);
  EXPECT_FALSE(store.contains(2, now + std::chrono::milliseconds { 11 }));
  EXPECT_TRUE(store.contains(1, now + std::chrono::milliseconds { 11 }));
}

TEST(RemoteUsbTombstone, EraseRemovesQueueMarkers) {
  store_t store { policy(1, std::chrono::hours { 1 }) };
  const auto now = store_t::clock_t::time_point {};

  ASSERT_TRUE(store.insert(11, now));
  EXPECT_EQ(store.erase(11), 1u);
  EXPECT_EQ(store.size(now), 0u);

  /* A stale marker would otherwise consume the only capacity slot. */
  ASSERT_TRUE(store.insert(12, now + std::chrono::milliseconds { 1 }));
  EXPECT_TRUE(store.contains(12, now + std::chrono::milliseconds { 1 }));
}

TEST(RemoteUsbTombstone, ClearDropsAllEntries) {
  store_t store { policy() };
  const auto now = store_t::clock_t::time_point {};

  ASSERT_TRUE(store.insert(1, now));
  ASSERT_TRUE(store.insert(2, now));
  store.clear();
  EXPECT_EQ(store.size(now), 0u);
}
