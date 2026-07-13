/**
 * @file tests/unit/test_rtsp.cpp
 * @brief Tests for launch-ticket lifecycle and RTSP claim routing.
 */

#include <array>

#include "src/launch_session_manager.h"
#include "src/rtsp.h"

#include "../tests_common.h"

using namespace std::chrono_literals;

namespace {
  std::shared_ptr<rtsp_stream::launch_session_t>
  make_session(std::uint32_t id,
               std::string cert,
               std::string peer,
               const crypto::aes_t *key = nullptr) {
    auto session = std::make_shared<rtsp_stream::launch_session_t>();
    session->id = id;
    session->unique_id = "client-" + std::to_string(id);
    session->client_cert_uuid = std::move(cert);
    session->rtsp_peer_address = std::move(peer);
    if (key) {
      session->gcm_key = *key;
      session->rtsp_cipher.emplace(*key, false);
    }
    return session;
  }
}  // namespace

TEST(LaunchSessionManager, RoutesConcurrentPlaintextClientsAndPreservesSingletonFallback) {
  rtsp_stream::launch_session_manager_t manager;
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();

  EXPECT_EQ(manager.register_session(make_session(1, "cert-a", "192.0.2.10"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  EXPECT_EQ(manager.register_session(make_session(2, "cert-b", "192.0.2.20"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);

  auto first = manager.claim_plaintext("192.0.2.10", now);
  auto second = manager.claim_plaintext("192.0.2.20", now);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->id, 1U);
  EXPECT_EQ(second->id, 2U);

  rtsp_stream::launch_session_manager_t singleton_manager;
  ASSERT_EQ(singleton_manager.register_session(make_session(3, "cert-c", "198.51.100.10"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  auto fallback = singleton_manager.claim_plaintext("198.51.100.20", now);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback->id, 3U);
}

TEST(LaunchSessionManager, ReplacesPendingRetryButDoesNotReplaceClaimedHandshake) {
  rtsp_stream::launch_session_manager_t manager;
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();

  ASSERT_EQ(manager.register_session(make_session(10, "same-cert", "203.0.113.10"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  EXPECT_EQ(manager.register_session(make_session(11, "same-cert", "203.0.113.20"), 10s, now),
            rtsp_stream::launch_ticket_register_e::replaced);
  EXPECT_EQ(manager.size(now), 1U);

  auto claimed = manager.claim_plaintext("203.0.113.20", now);
  ASSERT_TRUE(claimed);
  EXPECT_EQ(claimed->id, 11U);
  EXPECT_EQ(manager.register_session(make_session(12, "same-cert", "203.0.113.30"), 10s, now),
            rtsp_stream::launch_ticket_register_e::client_busy);

  EXPECT_TRUE(manager.release(11, 10s, now));
  auto reclaimed = manager.claim_plaintext("203.0.113.20", now);
  ASSERT_TRUE(reclaimed);
  EXPECT_EQ(reclaimed->id, 11U);
  EXPECT_TRUE(manager.activate(11));
  EXPECT_EQ(manager.size(now), 0U);
}

TEST(LaunchSessionManager, EnforcesCapacityAndExpiresTickets) {
  rtsp_stream::launch_session_manager_t manager { 2, 1 };
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();

  ASSERT_EQ(manager.register_session(make_session(20, "cert-20", "192.0.2.20"), 1s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_EQ(manager.register_session(make_session(21, "cert-21", "192.0.2.21"), 1s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  EXPECT_EQ(manager.register_session(make_session(22, "cert-22", "192.0.2.22"), 1s, now),
            rtsp_stream::launch_ticket_register_e::global_limit);

  EXPECT_EQ(manager.prune(now + 2s), 2U);
  EXPECT_EQ(manager.size(now + 2s), 0U);

  rtsp_stream::launch_session_manager_t legacy_manager { 4, 2 };
  auto legacy = make_session(23, {}, "192.0.2.23");
  legacy->unique_id = "legacy-retry";
  ASSERT_EQ(legacy_manager.register_session(legacy, 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  auto legacy_retry = make_session(24, {}, "192.0.2.23");
  legacy_retry->unique_id = "legacy-retry";
  EXPECT_EQ(legacy_manager.register_session(legacy_retry, 10s, now),
            rtsp_stream::launch_ticket_register_e::replaced);
  auto legacy_claim = legacy_manager.claim_plaintext("192.0.2.23", now);
  ASSERT_TRUE(legacy_claim);
  EXPECT_EQ(legacy_claim->id, 24U);

  rtsp_stream::launch_session_manager_t peer_limited_manager { 4, 2 };
  ASSERT_EQ(peer_limited_manager.register_session(make_session(25, {}, "192.0.2.25"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_EQ(peer_limited_manager.register_session(make_session(26, {}, "192.0.2.25"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  EXPECT_EQ(peer_limited_manager.register_session(make_session(27, {}, "192.0.2.25"), 10s, now),
            rtsp_stream::launch_ticket_register_e::client_limit);
}

TEST(LaunchSessionManager, PreservesClaimedTicketsPastPendingExpiry) {
  rtsp_stream::launch_session_manager_t manager;
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();

  ASSERT_EQ(manager.register_session(make_session(30, "claimed-cert", "192.0.2.30"), 1s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_EQ(manager.register_session(make_session(31, "pending-cert", "192.0.2.31"), 1s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_TRUE(manager.claim_plaintext("192.0.2.30", now));

  EXPECT_EQ(manager.prune(now + 2s), 1U);
  EXPECT_EQ(manager.size(now + 2s), 1U);
  EXPECT_EQ(manager.register_session(make_session(32, "claimed-cert", "192.0.2.32"), 1s, now + 2s),
            rtsp_stream::launch_ticket_register_e::client_busy);

  EXPECT_TRUE(manager.release(30, 1s, now + 2s));
  EXPECT_EQ(manager.prune(now + 4s), 1U);
  EXPECT_EQ(manager.size(now + 4s), 0U);
}

TEST(LaunchSessionManager, ErasesOnlyTicketsOwnedByAuthenticatedClient) {
  rtsp_stream::launch_session_manager_t manager;
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();

  ASSERT_EQ(manager.register_session(make_session(40, "cert-a", "192.0.2.40"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_EQ(manager.register_session(make_session(41, "cert-b", "192.0.2.41"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_EQ(manager.register_session(make_session(42, {}, "192.0.2.42"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);

  EXPECT_EQ(manager.erase_client_sessions({}), 0U);
  EXPECT_EQ(manager.erase_client_sessions("cert-a"), 1U);
  EXPECT_EQ(manager.size(now), 2U);

  EXPECT_FALSE(manager.claim_plaintext("192.0.2.40", now));
  auto other_client = manager.claim_plaintext("192.0.2.41", now);
  ASSERT_TRUE(other_client);
  EXPECT_EQ(other_client->id, 41U);
}

TEST(LaunchSessionManager, SupportsOverlappingRtspConnectionsForOneLaunch) {
  rtsp_stream::launch_session_manager_t manager;
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();

  ASSERT_EQ(manager.register_session(make_session(33, "multi-rtsp-cert", "192.0.2.33"), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);

  auto describe = manager.claim_plaintext("192.0.2.33", now);
  auto announce = manager.claim_plaintext("192.0.2.33", now);
  ASSERT_TRUE(describe);
  ASSERT_TRUE(announce);
  EXPECT_EQ(describe->id, 33U);
  EXPECT_EQ(announce->id, 33U);

  EXPECT_TRUE(manager.release(33, 10s, now));
  EXPECT_EQ(manager.register_session(make_session(34, "multi-rtsp-cert", "192.0.2.34"), 10s, now),
            rtsp_stream::launch_ticket_register_e::client_busy);
  EXPECT_TRUE(manager.release(33, 10s, now));
  EXPECT_EQ(manager.register_session(make_session(34, "multi-rtsp-cert", "192.0.2.34"), 10s, now),
            rtsp_stream::launch_ticket_register_e::replaced);

  rtsp_stream::launch_session_manager_t encrypted_manager;
  const crypto::aes_t key(16, 0x33);
  ASSERT_EQ(encrypted_manager.register_session(make_session(35, "encrypted-cert", "192.0.2.35", &key), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);

  constexpr std::string_view plaintext { "OPTIONS rtsp://host RTSP/1.0\r\nCSeq: 1\r\n\r\n" };
  crypto::aes_t iv(12, 0);
  iv[10] = 'C';
  iv[11] = 'R';
  crypto::cipher::gcm_t client_cipher { key, false };
  std::vector<std::uint8_t> tagged_cipher(
    crypto::cipher::round_to_pkcs7_padded(plaintext.size()) + crypto::cipher::tag_size);
  const auto encrypted_size = client_cipher.encrypt(plaintext, tagged_cipher.data(), &iv);
  ASSERT_GT(encrypted_size, 0);
  const auto encrypted_message = std::string_view {
    reinterpret_cast<const char *>(tagged_cipher.data()),
    static_cast<std::size_t>(encrypted_size) + crypto::cipher::tag_size
  };

  auto encrypted_describe = encrypted_manager.claim_encrypted("192.0.2.35", encrypted_message, iv, now);
  auto encrypted_announce = encrypted_manager.claim_encrypted("192.0.2.35", encrypted_message, iv, now);
  ASSERT_TRUE(encrypted_describe);
  ASSERT_TRUE(encrypted_announce);
  EXPECT_EQ(encrypted_describe.session->id, 35U);
  EXPECT_EQ(encrypted_announce.session->id, 35U);

  EXPECT_TRUE(encrypted_manager.release(35, 10s, now));
  EXPECT_EQ(encrypted_manager.register_session(make_session(36, "encrypted-cert", "192.0.2.36", &key), 10s, now),
            rtsp_stream::launch_ticket_register_e::client_busy);
  EXPECT_TRUE(encrypted_manager.release(35, 10s, now));
  EXPECT_EQ(encrypted_manager.register_session(make_session(36, "encrypted-cert", "192.0.2.36", &key), 10s, now),
            rtsp_stream::launch_ticket_register_e::replaced);
}

TEST(LaunchSessionManager, AuthenticatesEncryptedClaimByGcmTagAcrossRouteChanges) {
  rtsp_stream::launch_session_manager_t manager;
  const auto now = rtsp_stream::launch_session_manager_t::clock_t::now();
  const crypto::aes_t key_a(16, 0x11);
  const crypto::aes_t key_b(16, 0x22);

  ASSERT_EQ(manager.register_session(make_session(30, "cert-a", "192.0.2.30", &key_a), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  ASSERT_EQ(manager.register_session(make_session(31, "cert-b", "192.0.2.30", &key_b), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);

  // Two plaintext clients behind the same visible address are intentionally
  // ambiguous. Encrypted RTSP can still distinguish them cryptographically.
  EXPECT_FALSE(manager.claim_plaintext("192.0.2.30", now));

  rtsp_stream::launch_session_manager_t encrypted_only_manager;
  ASSERT_EQ(encrypted_only_manager.register_session(make_session(32, "cert-c", "192.0.2.32", &key_a), 10s, now),
            rtsp_stream::launch_ticket_register_e::accepted);
  EXPECT_FALSE(encrypted_only_manager.claim_plaintext("192.0.2.32", now));

  constexpr std::string_view plaintext { "OPTIONS rtsp://host RTSP/1.0\r\nCSeq: 1\r\n\r\n" };
  crypto::aes_t iv(12, 0);
  iv[10] = 'C';
  iv[11] = 'R';
  crypto::cipher::gcm_t client_cipher { key_b, false };
  std::vector<std::uint8_t> tagged_cipher(
    crypto::cipher::round_to_pkcs7_padded(plaintext.size()) + crypto::cipher::tag_size);
  const auto encrypted_size = client_cipher.encrypt(
    plaintext, tagged_cipher.data(), &iv);
  ASSERT_GT(encrypted_size, 0);

  const std::array<char, 32> invalid_tagged_cipher {};
  EXPECT_FALSE(manager.claim_encrypted(
    "198.51.100.99",
    std::string_view { invalid_tagged_cipher.data(), invalid_tagged_cipher.size() },
    iv,
    now));

  auto claim = manager.claim_encrypted(
    "198.51.100.99",
    std::string_view { reinterpret_cast<const char *>(tagged_cipher.data()),
                       static_cast<std::size_t>(encrypted_size) + crypto::cipher::tag_size },
    iv,
    now);
  ASSERT_TRUE(claim);
  EXPECT_EQ(claim.session->id, 31U);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(claim.plaintext.data()), claim.plaintext.size()), plaintext);
}
