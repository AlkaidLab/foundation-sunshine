#include "src/stream_transport_feedback.h"

#include <gtest/gtest.h>

namespace {
  AlkTransportStats make_transport_stats(std::uint32_t channel_kind,
                                          std::uint32_t loss_ppm) {
    AlkTransportStats stats {};
    stats.version = ALK_TRANSPORT_MODULE_VERSION;
    stats.channel_kind = channel_kind;
    stats.rtt_us = 16000;
    stats.jitter_us = 2000;
    stats.packet_loss_ppm = loss_ppm;
    stats.throughput_kbps = 10668;
    stats.reliable_backlog_bytes = 2048;
    return stats;
  }
}

TEST(StreamTransportFeedbackTests, ControlTransportLossDoesNotBecomeMediaPacketLoss) {
  auto stats = make_transport_stats(ALK_TRANSPORT_CHANNEL_CONTROL_RELIABLE, 250000);
  stream_quality::feedback_t feedback;
  feedback.duration_ms = 1000;
  feedback.frames_seen = 60;
  feedback.complete_frames = 60;
  feedback.displayed_frames = 60;

  stream::apply_transport_stats_to_quality_feedback(stats, feedback);

  EXPECT_EQ(feedback.rtt_ms, 16U);
  EXPECT_EQ(feedback.rtt_variance_ms, 2U);
  EXPECT_EQ(feedback.transport_packet_loss_ppm, 250000U);
  EXPECT_EQ(feedback.total_packets, 0U);
  EXPECT_EQ(feedback.missing_packets, 0U);
  EXPECT_EQ(feedback.received_packets, 0U);
}

TEST(StreamTransportFeedbackTests, VideoTransportLossMaySeedMediaPacketLossWhenFeedbackHasNoMediaPackets) {
  auto stats = make_transport_stats(ALK_TRANSPORT_CHANNEL_VIDEO_DATAGRAM, 35000);
  stream_quality::feedback_t feedback;

  stream::apply_transport_stats_to_quality_feedback(stats, feedback);

  EXPECT_EQ(feedback.total_packets, 1000000U);
  EXPECT_EQ(feedback.missing_packets, 35000U);
  EXPECT_EQ(feedback.received_packets, 965000U);
  EXPECT_EQ(feedback.transport_throughput_kbps, 10668U);
  EXPECT_EQ(feedback.transport_reliable_backlog_bytes, 2048U);
}

TEST(StreamTransportFeedbackTests, ExistingMediaPacketWindowWinsOverTransportStats) {
  auto stats = make_transport_stats(ALK_TRANSPORT_CHANNEL_VIDEO_DATAGRAM, 35000);
  stream_quality::feedback_t feedback;
  feedback.total_packets = 2000;
  feedback.missing_packets = 20;
  feedback.received_packets = 1980;

  stream::apply_transport_stats_to_quality_feedback(stats, feedback);

  EXPECT_EQ(feedback.total_packets, 2000U);
  EXPECT_EQ(feedback.missing_packets, 20U);
  EXPECT_EQ(feedback.received_packets, 1980U);
}
