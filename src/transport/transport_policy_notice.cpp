#include "transport_policy_notice.h"
#include <algorithm>
#include <limits>

namespace transport {
  std::optional<TPS_STATUS_NOTICE>
  policy_notice(const policy_snapshot_t &s, std::uint32_t id, std::uint64_t sequence) {
    if (!s.accepted) return {};
    TPS_STATUS_NOTICE n {};
    n.sessionId = id;
    n.connectionEpoch = s.accepted->connection_epoch;
    n.noticeSequence = sequence;
    n.controlEpoch = s.accepted->control_epoch;
    n.acceptedRevision = s.accepted->revision;
    switch (s.accepted->control_source) {
      case control_source_e::legacy: n.controlSource = 0; break;
      case control_source_e::manual: n.controlSource = 1; break;
      case control_source_e::googcc: n.controlSource = 2; break;
      case control_source_e::local: n.controlSource = 3; break;
    }
    for (const auto &r : s.receipts) {
      if (!r.policy) return {};
      if (r.policy == s.applied && r.encoder_applied) {
        n.flags |= TPS_APPLIED_KNOWN;
        n.encoderAppliedRevision = r.policy->revision;
      }
      if (r.encoder_applied && r.first_sent_frame && r.policy->revision >= n.firstSentRevision) {
        n.flags |= TPS_FIRST_SENT_KNOWN;
        n.firstSentRevision = r.policy->revision;
        n.firstSentFrame = *r.first_sent_frame;
      }
      if (r.policy == s.accepted) {
        switch (r.failure) {
          case policy_failure_e::none: n.failure = 0; break;
          case policy_failure_e::unsupported: n.failure = 1; break;
          case policy_failure_e::backend_failure: n.failure = 2; break;
          case policy_failure_e::superseded: n.failure = 3; break;
          case policy_failure_e::stopped: n.failure = 4; break;
        }
        if (!r.encoder_applied && !n.failure && !s.stopped) n.flags |= TPS_PENDING;
      }
    }
    if (s.stopped) n.flags |= TPS_STOPPED;
    else {
      if (s.encoder_initialized) n.flags |= TPS_ENCODER_READY;
      if (s.experimental_packet_control_negotiated) n.flags |= TPS_PACKET_CONTROL;
      if (s.experimental_video_pacer_enabled) n.flags |= TPS_VIDEO_PACER;
    }
    std::array<std::uint8_t, TPS_STATUS_BYTES> wire;
    if (!TpsEncodeStatus(&n, wire.data(), wire.size())) return {};
    return n;
  }

  std::optional<std::array<std::uint8_t, TPS_STATUS_BYTES>>
  policy_notice_sender_t::prepare(const policy_snapshot_t &s, std::uint32_t id, std::int64_t now_us) const {
    if (now_us < 0 || (last_queued_us_ >= 0 && now_us < last_queued_us_)) return {};
    TPS_STATUS_NOTICE previous {};
    if (last_ && !TpsDecodeStatus(last_->data(), last_->size(), &previous)) return {};
    if (last_ && now_us - last_queued_us_ < 250000) return {};
    auto n = policy_notice(s, id, last_ ? previous.noticeSequence : 1);
    if (!n) return {};
    // The HTTPS receipt history is bounded. Preserve a first send already
    // observed by this publisher after its old receipt leaves that history.
    if (last_ && previous.firstSentRevision > n->firstSentRevision) {
      n->flags |= TPS_FIRST_SENT_KNOWN;
      n->firstSentRevision = previous.firstSentRevision;
      n->firstSentFrame = previous.firstSentFrame;
    }
    std::array<std::uint8_t, TPS_STATUS_BYTES> wire;
    if (!TpsEncodeStatus(&*n, wire.data(), wire.size())) return {};
    if (last_ && wire != *last_) {
      if (previous.noticeSequence == std::numeric_limits<std::uint64_t>::max()) return {};
      n->noticeSequence++;
      TpsEncodeStatus(&*n, wire.data(), wire.size());
    }
    else if (last_ && now_us - last_queued_us_ < 2000000) return {};
    return wire;
  }
  void
  policy_notice_sender_t::queued(const std::array<std::uint8_t, TPS_STATUS_BYTES> &body, std::int64_t now_us) {
    TPS_STATUS_NOTICE n {};
    if (now_us < 0 || (last_queued_us_ >= 0 && now_us < last_queued_us_) ||
        !TpsDecodeStatus(body.data(), body.size(), &n)) return;
    last_ = body;
    last_queued_us_ = now_us;
  }
}  // namespace transport
