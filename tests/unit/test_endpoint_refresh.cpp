#include "src/endpoint_refresh.h"
#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace {
  class EndpointRefresh: public testing::Test {
  protected:
    using protocol = stream::endpoint_refresh_t;
    const boost::asio::ip::address address = boost::asio::ip::make_address("127.0.0.1");
    const std::string id = "0123456789abcdef";
    crypto::aes_t ri = crypto::aes_t(16, 0x11);
    std::array<unsigned char, 32> key {};
    protocol server { address, LI_ER_VIDEO, id, ri };
    protocol::clock::time_point now = protocol::clock::time_point {} + 10s;
    protocol::endpoint_t old_peer { address, 40000 }, new_peer { address, 40001 };

    void SetUp() override {
      ASSERT_TRUE(server.ready());
      ASSERT_TRUE(LiErDeriveKey(ri.data(), reinterpret_cast<const unsigned char *>(id.data()), key.data()));
    }

    protocol::packet_t packet(uint8_t type, uint64_t sequence, const unsigned char *nonce = nullptr) {
      protocol::packet_t result {};
      EXPECT_TRUE(LiErEncode(result.data(), type, LI_ER_VIDEO,
        reinterpret_cast<const unsigned char *>(id.data()), key.data(), sequence, nonce));
      return result;
    }

    std::optional<protocol::packet_t> send(const protocol::packet_t &request, const protocol::endpoint_t &peer) {
      return server.receive(request.data(), request.size(), peer, now);
    }

    LI_ER_MESSAGE decode(const protocol::packet_t &response) {
      LI_ER_MESSAGE result {};
      EXPECT_TRUE(LiErDecode(response.data(), response.size(), LI_ER_VIDEO,
        reinterpret_cast<const unsigned char *>(id.data()), key.data(), &result));
      return result;
    }

    void connect() {
      auto challenge = send(packet(LI_ER_PROBE, 1), old_peer);
      ASSERT_TRUE(challenge);
      auto message = decode(*challenge);
      ASSERT_EQ(message.type, LI_ER_CHALLENGE);
      ASSERT_TRUE(send(packet(LI_ER_RESPONSE, message.sequence, message.nonce), old_peer));
      ASSERT_EQ(server.port(), old_peer.port());
    }
  };
}

TEST_F(EndpointRefresh, MigrationRequiresFreshProofFromTheCandidatePort) {
  connect();
  now += 500ms;
  auto challenge = send(packet(LI_ER_PROBE, 2), new_peer);
  ASSERT_TRUE(challenge);
  auto message = decode(*challenge);
  auto response = packet(LI_ER_RESPONSE, message.sequence, message.nonce);
  EXPECT_EQ(server.port(), old_peer.port());
  EXPECT_FALSE(send(response, old_peer));
  EXPECT_FALSE(send(response, { boost::asio::ip::make_address("127.0.0.2"), new_peer.port() }));
  auto tampered = response;
  tampered.back() ^= 1;
  EXPECT_FALSE(send(tampered, new_peer));
  EXPECT_TRUE(send(response, new_peer));
  EXPECT_EQ(server.port(), new_peer.port());
  EXPECT_FALSE(send(response, new_peer));
  EXPECT_FALSE(send(packet(LI_ER_PROBE, 2), old_peer));
}

TEST_F(EndpointRefresh, ExpiredChallengeCannotReplaceTheWorkingEndpoint) {
  connect();
  now += 500ms;
  auto challenge = send(packet(LI_ER_PROBE, 2), new_peer);
  ASSERT_TRUE(challenge);
  auto message = decode(*challenge);
  now += 2001ms;
  EXPECT_FALSE(send(packet(LI_ER_RESPONSE, message.sequence, message.nonce), new_peer));
  EXPECT_EQ(server.port(), old_peer.port());
  auto fresh = send(packet(LI_ER_PROBE, 3), new_peer);
  ASSERT_TRUE(fresh);
  auto renewed = decode(*fresh);
  EXPECT_NE(std::memcmp(message.nonce, renewed.nonce, LI_ER_NONCE_SIZE), 0);
  EXPECT_TRUE(send(packet(LI_ER_RESPONSE, renewed.sequence, renewed.nonce), new_peer));
}

TEST_F(EndpointRefresh, LostCommitAckCanBeRecoveredWithoutAnotherMigration) {
  connect();
  now += 500ms;
  auto challenge = send(packet(LI_ER_PROBE, 2), new_peer);
  ASSERT_TRUE(challenge);
  auto message = decode(*challenge);
  ASSERT_TRUE(send(packet(LI_ER_RESPONSE, message.sequence, message.nonce), new_peer));
  // Drop that ACK; the next PROBE must repeat proof of the committed nonce.
  now += 500ms;
  auto ack = send(packet(LI_ER_PROBE, 3), new_peer);
  ASSERT_TRUE(ack);
  auto confirmation = decode(*ack);
  EXPECT_EQ(confirmation.type, LI_ER_ACK);
  EXPECT_EQ(confirmation.sequence, 3U);
  EXPECT_EQ(std::memcmp(confirmation.nonce, message.nonce, LI_ER_NONCE_SIZE), 0);
  EXPECT_EQ(server.port(), new_peer.port());
}

TEST_F(EndpointRefresh, RejectsOtherSessionsStreamsAndUnvalidatedInitialTraffic) {
  auto request = packet(LI_ER_PROBE, 1);
  auto wrong = request;
  wrong[8] ^= 1;
  EXPECT_FALSE(send(wrong, old_peer));
  EXPECT_FALSE(server.receive("PING", 4, old_peer, now));
  EXPECT_TRUE(LiErEncode(wrong.data(), LI_ER_PROBE, LI_ER_AUDIO,
    reinterpret_cast<const unsigned char *>(id.data()), key.data(), 1, nullptr));
  EXPECT_FALSE(send(wrong, old_peer));
  EXPECT_FALSE(send(request, { address, 0 }));
  EXPECT_EQ(server.port(), 0);
  EXPECT_TRUE(send(request, old_peer));
  now += 10ms;
  EXPECT_FALSE(send(packet(LI_ER_PROBE, 2), new_peer)); // Challenge rate limit.
  EXPECT_EQ(server.port(), 0);
}

TEST_F(EndpointRefresh, AuthenticatedProbeFloodCannotProduceAnAckForEveryPacket) {
  connect();
  for (uint64_t sequence = 2; sequence < 102; sequence++) {
    EXPECT_FALSE(send(packet(LI_ER_PROBE, sequence), old_peer));
  }
  now += 250ms;
  EXPECT_TRUE(send(packet(LI_ER_PROBE, 102), old_peer));
  EXPECT_FALSE(send(packet(LI_ER_PROBE, 103), old_peer));
}
