#include "stream_transport_feedback.h"

#include <algorithm>

namespace stream {
  void
  apply_transport_stats_to_quality_feedback(const AlkTransportStats &stats,
                                            stream_quality::feedback_t &feedback) {
    feedback.transport_throughput_kbps = stats.throughput_kbps;
    feedback.transport_packet_loss_ppm = stats.packet_loss_ppm;
    feedback.transport_reliable_backlog_bytes = stats.reliable_backlog_bytes;

    if (stats.rtt_us != 0) {
      feedback.rtt_ms = stats.rtt_us / 1000U;
    }
    if (stats.jitter_us != 0) {
      feedback.rtt_variance_ms = stats.jitter_us / 1000U;
    }

    const bool transport_loss_is_media_video =
      stats.channel_kind == ALK_TRANSPORT_CHANNEL_VIDEO_DATAGRAM;
    if (transport_loss_is_media_video &&
        stats.packet_loss_ppm != 0 &&
        feedback.total_packets == 0) {
      feedback.total_packets = 1000000U;
      feedback.missing_packets = std::min<std::uint32_t>(stats.packet_loss_ppm, 1000000U);
      feedback.received_packets = 1000000U - feedback.missing_packets;
    }

    const bool input_latency_reported =
      feedback.input_send_latency_us > 0U ||
      feedback.input_ack_latency_us > 0U;
    if (stats.reliable_backlog_packets != 0U && input_latency_reported) {
      feedback.input_queue_depth = std::max(feedback.input_queue_depth,
                                            stats.reliable_backlog_packets);
    }
  }
}
