#include <chrono>

#include <gtest/gtest.h>

#include "src/nvhttp/network_probe_limiter.h"

namespace {
  using namespace std::chrono_literals;
  using nvhttp::network_probe::clock_t;
  using nvhttp::network_probe::limiter_t;
  using nvhttp::network_probe::rejection_e;

  TEST(NetworkProbeValidation, AcceptsOnlyBoundedDecimalPayloadSizes) {
    std::size_t bytes = 0;
    EXPECT_TRUE(nvhttp::network_probe::parse_payload_bytes("65536", bytes));
    EXPECT_EQ(bytes, 65536U);
    EXPECT_TRUE(nvhttp::network_probe::parse_payload_bytes("4194304", bytes));
    EXPECT_FALSE(nvhttp::network_probe::parse_payload_bytes("65535", bytes));
    EXPECT_FALSE(nvhttp::network_probe::parse_payload_bytes("4194305", bytes));
    EXPECT_FALSE(nvhttp::network_probe::parse_payload_bytes("1x", bytes));
    EXPECT_FALSE(nvhttp::network_probe::parse_payload_bytes("", bytes));
  }

  TEST(NetworkProbeValidation, EnforcesNonceAlphabetAndLength) {
    EXPECT_TRUE(nvhttp::network_probe::valid_nonce("550e8400-e29b-41d4-a716-446655440000"));
    EXPECT_TRUE(nvhttp::network_probe::valid_nonce("a_B.c-1"));
    EXPECT_FALSE(nvhttp::network_probe::valid_nonce(""));
    EXPECT_FALSE(nvhttp::network_probe::valid_nonce("bad nonce"));
    EXPECT_FALSE(nvhttp::network_probe::valid_nonce(std::string(1, static_cast<char>(0xff))));
    EXPECT_FALSE(nvhttp::network_probe::valid_nonce(std::string(65, 'a')));
  }

  TEST(NetworkProbeLimiter, SerialSamplesWithOneNonceFitProgressiveBudget) {
    limiter_t limiter;
    const auto start = clock_t::time_point {};
    const std::size_t samples[] = { 64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024 };
    auto now = start;
    for (const auto bytes : samples) {
      const auto admission = limiter.admit("client-a", "probe-a", bytes, now);
      ASSERT_TRUE(admission);
      limiter.complete("client-a", admission.id, now + 10ms);
      now += 20ms;
    }
  }

  TEST(NetworkProbeLimiter, RejectsConcurrentSamplesForOneClient) {
    limiter_t limiter;
    const auto now = clock_t::time_point {};
    const auto first = limiter.admit("client-a", "probe-a", 64 * 1024, now);
    ASSERT_TRUE(first);
    const auto second = limiter.admit("client-a", "probe-a", 64 * 1024, now);
    EXPECT_EQ(second.rejection, rejection_e::client_busy);
  }

  TEST(NetworkProbeLimiter, RejectsFifthGlobalConcurrentSample) {
    limiter_t limiter;
    const auto now = clock_t::time_point {};
    nvhttp::network_probe::admission_t first;
    for (int i = 0; i < 4; ++i) {
      const auto admission = limiter.admit("client-" + std::to_string(i), "probe", 64 * 1024, now);
      ASSERT_TRUE(admission);
      if (i == 0) {
        first = admission;
      }
    }
    EXPECT_EQ(limiter.admit("client-4", "probe", 64 * 1024, now).rejection, rejection_e::global_busy);

    limiter.complete("client-0", first.id, now + 1ms);
    EXPECT_TRUE(limiter.admit("client-4", "probe", 64 * 1024, now + 2ms));
  }

  TEST(NetworkProbeLimiter, RejectsExpiredNonceDuringReplayWindow) {
    limiter_t limiter;
    const auto start = clock_t::time_point {};
    const auto first = limiter.admit("client-a", "probe-a", 64 * 1024, start);
    ASSERT_TRUE(first);
    limiter.complete("client-a", first.id, start + 10ms);

    EXPECT_EQ(limiter.admit("client-a", "probe-a", 64 * 1024, start + 3s).rejection, rejection_e::session_expired);
    EXPECT_EQ(limiter.admit("client-a", "probe-b", 64 * 1024, start + 7s).rejection, rejection_e::cooldown);
    EXPECT_TRUE(limiter.admit("client-a", "probe-b", 64 * 1024, start + 8s));
  }

  TEST(NetworkProbeLimiter, EnforcesSixMiBSessionLimit) {
    limiter_t limiter;
    const auto start = clock_t::time_point {};
    const auto first = limiter.admit("client-a", "probe-a", 4 * 1024 * 1024, start);
    ASSERT_TRUE(first);
    limiter.complete("client-a", first.id, start + 1ms);
    EXPECT_EQ(limiter.admit("client-a", "probe-a", 3 * 1024 * 1024, start + 2ms).rejection, rejection_e::session_too_large);
  }

  TEST(NetworkProbeLimiter, StaleCompletionCannotReleaseNewAdmission) {
    limiter_t limiter;
    const auto now = clock_t::time_point {};
    const auto first = limiter.admit("client-a", "probe-a", 64 * 1024, now);
    ASSERT_TRUE(first);
    limiter.complete("client-a", first.id, now + 1ms);

    const auto second = limiter.admit("client-a", "probe-a", 64 * 1024, now + 2ms);
    ASSERT_TRUE(second);
    limiter.complete("client-a", first.id, now + 3ms);
    EXPECT_EQ(limiter.admit("client-a", "probe-a", 64 * 1024, now + 4ms).rejection, rejection_e::client_busy);
  }

  TEST(NetworkProbeLimiter, EnforcesPerClientMinuteQuota) {
    limiter_t limiter;
    const auto start = clock_t::time_point {};
    for (int session = 0; session < 4; ++session) {
      const auto now = start + session * 8s;
      const auto admission = limiter.admit("client-a", "probe-" + std::to_string(session), 4 * 1024 * 1024, now);
      ASSERT_TRUE(admission);
      limiter.complete("client-a", admission.id, now + 1ms);
    }
    EXPECT_EQ(limiter.admit("client-a", "probe-4", 64 * 1024, start + 32s).rejection, rejection_e::client_quota);
  }

  TEST(NetworkProbeLimiter, EnforcesGlobalMinuteQuota) {
    limiter_t limiter;
    const auto now = clock_t::time_point {};
    for (int client = 0; client < 16; ++client) {
      const auto name = "client-" + std::to_string(client);
      const auto admission = limiter.admit(name, "probe", 4 * 1024 * 1024, now);
      ASSERT_TRUE(admission);
      limiter.complete(name, admission.id, now + 1ms);
    }
    EXPECT_EQ(limiter.admit("client-16", "probe", 64 * 1024, now + 2ms).rejection, rejection_e::global_quota);
  }

  TEST(NetworkProbeLimiter, RetainsReplayProtectionForQuotaWindow) {
    limiter_t limiter;
    const auto start = clock_t::time_point {};
    const auto first = limiter.admit("client-a", "probe-a", 64 * 1024, start);
    ASSERT_TRUE(first);
    limiter.complete("client-a", first.id, start + 1ms);

    EXPECT_EQ(limiter.admit("client-a", "probe-a", 64 * 1024, start + 3s).rejection, rejection_e::session_expired);
    EXPECT_EQ(limiter.admit("client-a", "probe-a", 64 * 1024, start + 59s).rejection, rejection_e::session_expired);
    EXPECT_TRUE(limiter.admit("client-a", "probe-a", 64 * 1024, start + 63s));
  }

  TEST(NetworkProbeLimiter, CleanupNeverRemovesInflightClient) {
    limiter_t limiter;
    const auto start = clock_t::time_point {};
    ASSERT_TRUE(limiter.admit("client-a", "probe-a", 64 * 1024, start));

    ASSERT_TRUE(limiter.admit("client-b", "probe-b", 64 * 1024, start + 61s));
    EXPECT_EQ(limiter.admit("client-a", "probe-a", 64 * 1024, start + 61s).rejection, rejection_e::client_busy);
  }
}  // namespace
