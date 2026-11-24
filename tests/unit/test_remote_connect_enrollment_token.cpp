#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/remote_connect/enrollment_token.h"

namespace {
  using namespace std::chrono_literals;
  using store_t = remote_connect::enrollment_token_store_t;

  store_t::bytes_t
  device_key(std::uint8_t value = 1) {
    store_t::bytes_t key {};
    key[0] = value;
    return key;
  }

  TEST(RemoteEnrollmentToken, ClaimsOnlyOnceAndBindsTheSuppliedKey) {
    store_t store;
    auto token = store.issue(120s);
    ASSERT_TRUE(token);
    const auto key = device_key(7);
    const auto claim = store.claim(*token, key);
    ASSERT_TRUE(claim);
    EXPECT_EQ(claim->device_public_key, key);
    EXPECT_FALSE(store.claim(*token, key));
  }

  TEST(RemoteEnrollmentToken, WrongTokenAndInvalidKeyDoNotConsumeValidToken) {
    store_t store;
    auto token = store.issue(120s);
    ASSERT_TRUE(token);
    auto other_token = *token;
    other_token[0] ^= 0x80;
    EXPECT_FALSE(store.claim(other_token, device_key()));
    EXPECT_FALSE(store.claim(*token, {}));
    EXPECT_TRUE(store.claim(*token, device_key()));
  }

  TEST(RemoteEnrollmentToken, ExpiresAtTheDeadlineUsingMonotonicTime) {
    auto now = store_t::clock_t::time_point {};
    store_t store([&now] { return now; });
    auto token = store.issue(10s);
    ASSERT_TRUE(token);
    now += 10s;
    EXPECT_FALSE(store.claim(*token, device_key()));
    now -= 5s;
    EXPECT_FALSE(store.claim(*token, device_key()));
  }

  TEST(RemoteEnrollmentToken, NewStoreCannotAcceptPriorProcessToken) {
    store_t original;
    auto token = original.issue(120s);
    ASSERT_TRUE(token);
    store_t restarted;
    EXPECT_FALSE(restarted.claim(*token, device_key()));
  }

  TEST(RemoteEnrollmentToken, NewIssueAndCancelInvalidateOldToken) {
    store_t store;
    auto first = store.issue(120s);
    auto second = store.issue(120s);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    EXPECT_FALSE(store.claim(*first, device_key()));
    store.cancel();
    EXPECT_FALSE(store.claim(*second, device_key()));
  }

  TEST(RemoteEnrollmentToken, InvalidLifetimeFailsClosedAndInvalidatesPriorToken) {
    store_t store;
    auto token = store.issue(10s);
    ASSERT_TRUE(token);
    EXPECT_FALSE(store.issue(0s));
    EXPECT_FALSE(store.claim(*token, device_key()));
    EXPECT_FALSE(store.issue(121s));
  }

  TEST(RemoteEnrollmentToken, ClaimsRaceAndOnlyOneDeviceWins) {
    store_t store;
    auto token = store.issue(120s);
    ASSERT_TRUE(token);
    std::atomic<bool> start {false};
    std::atomic<int> claims {0};
    std::atomic<int> bad_bindings {0};
    std::vector<std::thread> threads;
    for (int i = 1; i <= 32; ++i) {
      threads.emplace_back([&, i] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto key = device_key(static_cast<std::uint8_t>(i));
        if (const auto claim = store.claim(*token, key)) {
          if (claim->device_public_key != key) ++bad_bindings;
          ++claims;
        }
      });
    }
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(claims.load(), 1);
    EXPECT_EQ(bad_bindings.load(), 0);
    EXPECT_FALSE(store.claim(*token, device_key()));
  }

  TEST(RemoteEnrollmentToken, CancellationRacingClaimLeavesNoClaimableToken) {
    store_t store;
    auto token = store.issue(120s);
    ASSERT_TRUE(token);
    std::atomic<bool> start {false};
    std::thread claimant([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) store.claim(*token, device_key());
    });
    std::thread canceller([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      store.cancel();
    });
    start.store(true, std::memory_order_release);
    claimant.join();
    canceller.join();
    EXPECT_FALSE(store.claim(*token, device_key()));
  }
}  // namespace
