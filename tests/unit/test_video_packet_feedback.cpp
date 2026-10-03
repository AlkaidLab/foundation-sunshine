#include "src/transport/transport_feedback_wire.h"
#include "third-party/moonlight-common-c/src/VideoPacketFeedback.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>

namespace {
  struct client_t {
    VIDEO_NETWORK_OBSERVER observer {};
    VIDEO_PACKET_FEEDBACK feedback {};
    client_t() {
      VnInitialize(&observer, 42, 100000, 30000, 0);
      VfInitialize(&feedback, 42);
    }
    bool ready(const transport::wire_feedback_t &server, int64_t now) {
      const auto offer = server.ready(now);
      return offer && VfAcceptReady(&feedback, &observer, &*offer, now);
    }
    unsigned flush(transport::wire_feedback_t &server, uint64_t now, bool deliver = true) {
      unsigned count = 0;
      TF_PACKET_REPORT report {};
      while (VfPrepareReport(&feedback, &observer, now, &report)) {
        std::array<uint8_t, TF_MAX_REPORT_BYTES> bytes {};
        const auto size = TfEncodeReport(&report, bytes.data(), bytes.size());
        EXPECT_NE(size, 0u);
        if (deliver) {
          EXPECT_EQ(server.apply_wire({bytes.data(), size}, now).result, transport::report_result_e::accepted);
        }
        VfCommitQueued(&feedback, &observer, &report);
        count++;
      }
      return count;
    }
  };
}

TEST(VideoPacketFeedback, IndependentFaultPlanMatchesSuccessfulSubmissionsIncludingWholeFramesAndTail) {
  transport::wire_feedback_t server(42, true);
  auto client = std::make_unique<client_t>();
  uint64_t committed = 0, received = 0, missing = 0;
  // Independent oracle: unsent slots, random loss, whole frame and parity tail.
  for (uint64_t sequence = 0; sequence < 1000; ++sequence) {
    const bool submitted = sequence % 17 != 0;
    const bool dropped = sequence % 11 == 0 || (sequence >= 100 && sequence < 120) || sequence >= 980;
    if (!submitted) continue;
    ASSERT_TRUE(server.commit_success({sequence, 10000 + static_cast<int64_t>(sequence), 1248,
      sequence / 20, 1, sequence % 5 ? transport::packet_kind_e::data : transport::packet_kind_e::fec, {}}));
    committed++;
    if (dropped) missing++;
    else {
      received++;
      ASSERT_TRUE(VfObserveAuthenticated(&client->feedback, &client->observer, 42, sequence, 1220, 100000 + sequence));
    }
  }
  ASSERT_TRUE(client->ready(server, 102000));
  ASSERT_GT(client->flush(server, 150000), 0u);
  auto status = server.snapshot();
  EXPECT_EQ(status.ledger.committed_packets, committed);
  EXPECT_EQ(status.ledger.received_packets, received);
  EXPECT_EQ(status.ledger.missing_declarations, missing);
  EXPECT_EQ(status.ledger.late_corrections, 0u);
  // A late packet changes loss once; repeats and duplicates preserve first arrival.
  ASSERT_TRUE(VfObserveAuthenticated(&client->feedback, &client->observer, 42, 22, 1220, 160000));
  ASSERT_TRUE(VfObserveAuthenticated(&client->feedback, &client->observer, 42, 22, 1220, 160001));
  ASSERT_TRUE(client->ready(server, 200000));
  ASSERT_GT(client->flush(server, 200000), 0u);
  status = server.snapshot();
  EXPECT_EQ(status.ledger.received_packets, received + 1);
  EXPECT_EQ(status.ledger.missing_declarations, missing);
  EXPECT_EQ(status.ledger.late_corrections, 1u);
  EXPECT_EQ(status.rate_limited_reports, 0u);
}

TEST(VideoPacketFeedback, LostBothFeedbackCopiesNeverCreatesServerLoss) {
  transport::wire_feedback_t server(42, true);
  auto client = std::make_unique<client_t>();
  for (uint64_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(server.commit_success({i, 10000, 1248, 1, 1, transport::packet_kind_e::data, {}}));
    ASSERT_TRUE(VfObserveAuthenticated(&client->feedback, &client->observer, 42, i, 1220, 100000 + i));
  }
  ASSERT_TRUE(client->ready(server, 102000));
  ASSERT_EQ(client->flush(server, 150000, false), 2u);
  EXPECT_EQ(client->flush(server, 200000), 0u);
  const auto status = server.snapshot();
  EXPECT_EQ(status.ledger.committed_packets, 3u);
  EXPECT_EQ(status.ledger.received_packets, 0u);
  EXPECT_EQ(status.ledger.missing_declarations, 0u);
}

TEST(VideoPacketFeedback, SustainedReportsHonorSharedRateAndBurstBounds) {
  transport::wire_feedback_t server(42, true, 16384);
  auto client = std::make_unique<client_t>();
  uint64_t sequence = 0, reports = 0;
  // 10,000 real identities per second, above a 60 Mbps video's typical rate.
  for (int cycle = 0; cycle < 40; ++cycle) {
    const int64_t now = 100000 + cycle * 50000;
    for (unsigned i = 0; i < 500; ++i, ++sequence) {
      ASSERT_TRUE(server.commit_success({sequence, now - 1000, 1248, sequence / 20, 1, transport::packet_kind_e::data, {}}));
      ASSERT_TRUE(VfObserveAuthenticated(&client->feedback, &client->observer, 42, sequence, 1220, now + i));
    }
    ASSERT_TRUE(client->ready(server, now + 1000));
    reports += client->flush(server, now + 1000);
  }
  EXPECT_GT(reports, 40u);
  const auto status = server.snapshot();
  EXPECT_EQ(status.rate_limited_reports, 0u);
  EXPECT_EQ(status.ledger.committed_packets, 20000u);
  EXPECT_EQ(status.ledger.received_packets, 20000u);
  EXPECT_EQ(status.ledger.missing_declarations, 0u);
}
