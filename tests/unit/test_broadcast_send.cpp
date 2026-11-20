#include "src/streaming/broadcast_send.h"

#include <array>
#include <chrono>
#include <future>

#include <boost/asio/ip/address.hpp>
#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(BroadcastSessions, RemovingSessionDoesNotWaitForInFlightSnapshot) {
  streaming::live_broadcast_sessions_t<int> sessions;
  auto session = std::make_shared<int>(42);
  const auto old_id = sessions.add(session);
  auto snapshot = sessions.snapshot(old_id);
  ASSERT_EQ(snapshot, session);
  session.reset();

  auto removed = std::async(std::launch::async, [&] { sessions.remove(old_id); });
  const auto removed_while_snapshot_held = removed.wait_for(2s);
  EXPECT_EQ(removed_while_snapshot_held, std::future_status::ready);
  EXPECT_EQ(*snapshot, 42);
  snapshot.reset();
  removed.get();

  auto new_session = std::make_shared<int>(43);
  const auto new_id = sessions.add(new_session);
  EXPECT_NE(new_id, old_id);
  EXPECT_FALSE(sessions.snapshot(old_id));
  EXPECT_EQ(sessions.snapshot(new_id), new_session);
}

namespace {
  struct batch_fixture_t {
    std::array<char, 5> headers {};
    std::array<char, 10> payload {};
    std::vector<platf::buffer_descriptor_t> buffers { { payload.data(), 4 }, { payload.data() + 4, 6 } };
    boost::asio::ip::address peer, source;
    platf::batched_send_info_t batch { headers.data(), 1, buffers, 2, 1, 4, 0, peer, 47998, source };
  };
}  // namespace

TEST(BroadcastSend, BatchWouldBlockDropsWithoutFallback) {
  batch_fixture_t f;
  const auto result = streaming::submit_video_batch(f.batch, [] { return true; }, [](auto &batch) {
    batch.would_block = true;
    return false; }, [](auto &) {
    ADD_FAILURE() << "A congested socket must not receive a fallback send";
    return false; });

  EXPECT_EQ(result.sent, 0u);
  EXPECT_EQ(result.dropped_for_backpressure, 4u);
}

TEST(BroadcastSend, FallbackAccountsForAcceptedPacketsAndDropsBlockedTail) {
  batch_fixture_t f;
  std::size_t calls = 0;
  const auto result = streaming::submit_video_batch(f.batch, [] { return true; }, [](auto &) { return false; }, [&](auto &packet) {
      ++calls;
      EXPECT_EQ(packet.header, f.headers.data() + calls);
      EXPECT_EQ(packet.payload, f.payload.data() + 2 * calls);
      EXPECT_EQ(packet.target_port, f.batch.target_port);
      packet.would_block = calls == 2;
      return !packet.would_block; });

  EXPECT_EQ(calls, 2u);
  EXPECT_EQ(result.sent, 1u);
  EXPECT_EQ(result.dropped_for_backpressure, 3u);
}

TEST(BroadcastSend, MatDataAndParityBackpressureCountDropsWithoutLatchingFailure) {
  int accepted = 0;
  int dropped = 0;
  bool mat_send_failed = false;
  const auto account = [&](bool sent, bool would_block) {
    streaming::account_audio_datagram(
      sent, would_block, [&] { ++accepted; }, [&] { ++dropped; }, [&] { mat_send_failed = true; });
  };

  account(false, true);  // data datagram
  account(false, true);  // parity datagram
  EXPECT_EQ(dropped, 2);
  EXPECT_EQ(accepted, 0);
  EXPECT_FALSE(mat_send_failed);

  account(true, false);
  EXPECT_EQ(accepted, 1);
  EXPECT_FALSE(mat_send_failed);

  account(false, false);
  EXPECT_TRUE(mat_send_failed);
  EXPECT_EQ(dropped, 2);
}

TEST(BroadcastSend, StopDuringFallbackPreventsFurtherSends) {
  batch_fixture_t f;
  bool active = true;
  const auto result = streaming::submit_video_batch(f.batch, [&] { return active; }, [](auto &) { return false; }, [&](auto &) {
      EXPECT_TRUE(active);
      active = false;
      return true; });

  EXPECT_EQ(result.sent, 1u);
  EXPECT_EQ(result.dropped_for_backpressure, 0u);
}
