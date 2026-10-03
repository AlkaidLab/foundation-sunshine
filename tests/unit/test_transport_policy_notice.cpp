#include "src/transport_policy_notice.h"
#include "src/transport_policy_json.h"
#include <gtest/gtest.h>
#include <limits>

namespace {
  TPS_STATUS_NOTICE notice() {
    return { TPS_APPLIED_KNOWN | TPS_FIRST_SENT_KNOWN | TPS_ENCODER_READY,
      0, 0, 0xffffffffu, 0xffffffffffffffffull, 1, 1, 7, 6, 5, 0xffffffffffffffffull };
  }
  transport::frame_policy_t initial() {
    transport::frame_policy_t p;
    p.connection_epoch = 42;
    p.budget.total_kbps = 40000;
    p.encoder_kbps = 32000;
    p.fec_base = p.fec_key = p.fec_recovery = 20;
    return p;
  }
  void compare_api(const transport::policy_snapshot_t &s, const TPS_STATUS_NOTICE &n) {
    const auto api = transport::policy_status_json(s, n.sessionId);
    EXPECT_EQ(api["connectionEpoch"], std::to_string(n.connectionEpoch));
    EXPECT_EQ(api["acceptedRevision"], std::to_string(n.acceptedRevision));
    EXPECT_EQ(api["controlEpoch"], std::to_string(n.controlEpoch));
    EXPECT_EQ(api["pending"].get<bool>(), bool(n.flags & TPS_PENDING));
    EXPECT_EQ(api["encoderReady"].get<bool>(), bool(n.flags & TPS_ENCODER_READY));
    EXPECT_EQ(api["stopped"].get<bool>(), bool(n.flags & TPS_STOPPED));
    EXPECT_EQ(api["encoderAppliedRevision"].is_null(), !(n.flags & TPS_APPLIED_KNOWN));
    if (n.flags & TPS_APPLIED_KNOWN) {
      EXPECT_EQ(api["encoderAppliedRevision"], std::to_string(n.encoderAppliedRevision));
    }
    if (n.flags & TPS_FIRST_SENT_KNOWN) {
      bool found = false;
      for (const auto &r : api["receipts"]) {
        if (r["revision"] == std::to_string(n.firstSentRevision)) {
          EXPECT_TRUE(r["encoderApplied"].get<bool>());
          EXPECT_EQ(r["firstSentFrame"], std::to_string(n.firstSentFrame));
          found = true;
        }
      }
      EXPECT_TRUE(found);
    }
  }
}

TEST(PolicyNoticeWire, ExactUnsignedRoundTripAndFrameZero) {
  const auto n = notice();
  std::array<std::uint8_t, TPS_STATUS_BYTES> bytes {};
  ASSERT_EQ(TpsEncodeStatus(&n, bytes.data(), bytes.size()), bytes.size());
  EXPECT_EQ(bytes[0], 0); EXPECT_EQ(bytes[1], 1);
  EXPECT_EQ(bytes[2], 0); EXPECT_EQ(bytes[3], 72);
  for (int i = 16; i < 24; ++i) EXPECT_EQ(bytes[i], 255);
  TPS_STATUS_NOTICE decoded {};
  ASSERT_TRUE(TpsDecodeStatus(bytes.data(), bytes.size(), &decoded));
  EXPECT_EQ(decoded.connectionEpoch, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(decoded.firstSentFrame, std::numeric_limits<std::uint64_t>::max());
  decoded.firstSentFrame = 0;
  ASSERT_EQ(TpsEncodeStatus(&decoded, bytes.data(), bytes.size()), bytes.size());
  ASSERT_TRUE(TpsDecodeStatus(bytes.data(), bytes.size(), &decoded));
  EXPECT_EQ(decoded.firstSentFrame, 0);
  EXPECT_TRUE(decoded.flags & TPS_FIRST_SENT_KNOWN);
}
TEST(PolicyNoticeWire, InvalidLengthsVersionsReservedAndEnumsAreAtomic) {
  auto n = notice();
  std::array<std::uint8_t, TPS_STATUS_BYTES + 1> bytes {};
  ASSERT_EQ(TpsEncodeStatus(&n, bytes.data(), bytes.size()), TPS_STATUS_BYTES);
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    if (size == TPS_STATUS_BYTES) continue;
    auto unchanged = n;
    EXPECT_FALSE(TpsDecodeStatus(bytes.data(), size, &unchanged));
    EXPECT_EQ(unchanged.connectionEpoch, n.connectionEpoch);
    EXPECT_EQ(unchanged.firstSentFrame, n.firstSentFrame);
  }
  for (const auto offset : { 0, 1, 2, 3, 4, 6, 7, 12, 13, 14, 15 }) {
    auto bad = bytes; bad[offset] = 255;
    auto unchanged = n;
    EXPECT_FALSE(TpsDecodeStatus(bad.data(), TPS_STATUS_BYTES, &unchanged)) << offset;
    EXPECT_EQ(unchanged.noticeSequence, n.noticeSequence);
  }
  EXPECT_EQ(TpsEncodeStatus(&n, bytes.data(), TPS_STATUS_BYTES - 1), 0);
  EXPECT_FALSE(TpsDecodeStatus(nullptr, TPS_STATUS_BYTES, &n));
}
TEST(PolicyNoticeWire, UnknownAbsentContradictoryAndZeroIdentitiesRejected) {
  const auto base = notice();
  for (int mutation = 0; mutation < 12; ++mutation) {
    auto n = base;
    switch (mutation) {
      case 0: n.sessionId = 0; break;
      case 1: n.connectionEpoch = 0; break;
      case 2: n.noticeSequence = 0; break;
      case 3: n.controlEpoch = 0; break;
      case 4: n.acceptedRevision = 0; break;
      case 5: n.encoderAppliedRevision = 8; break;
      case 6: n.flags &= ~TPS_APPLIED_KNOWN; break;
      case 7: n.flags &= ~TPS_FIRST_SENT_KNOWN; break;
      case 8: n.firstSentRevision = 7; break;
      case 9: n.flags |= TPS_PENDING; n.failure = 2; break;
      case 10: n.flags |= TPS_STOPPED; break;
      case 11: n.flags |= TPS_PENDING; n.encoderAppliedRevision = n.acceptedRevision; break;
    }
    std::array<std::uint8_t, TPS_STATUS_BYTES> bytes;
    bytes.fill(0xa5);
    EXPECT_EQ(TpsEncodeStatus(&n, bytes.data(), bytes.size()), 0) << mutation;
    EXPECT_EQ(bytes.front(), 0xa5);
  }
}
TEST(PolicyNoticeReceiver, IdentityDuplicatesReorderingAndReset) {
  TPS_STATUS_RECEIVER state;
  auto n = notice();
  TpsInitializeReceiver(&state, n.connectionEpoch);
  ASSERT_TRUE(TpsAcceptStatus(&state, &n));
  EXPECT_FALSE(TpsAcceptStatus(&state, &n));
  auto wrong = n; wrong.noticeSequence++; wrong.connectionEpoch--;
  EXPECT_FALSE(TpsAcceptStatus(&state, &wrong));
  wrong = n; wrong.noticeSequence++; wrong.sessionId--;
  EXPECT_FALSE(TpsAcceptStatus(&state, &wrong));
  n.noticeSequence = 9;
  ASSERT_TRUE(TpsAcceptStatus(&state, &n));
  n.noticeSequence = 2;
  EXPECT_FALSE(TpsAcceptStatus(&state, &n));
  TPS_STATUS_NOTICE copied {};
  ASSERT_TRUE(TpsCopyStatus(&state, &copied));
  EXPECT_EQ(copied.noticeSequence, 9);
  TpsInitializeReceiver(&state, 1);
  EXPECT_FALSE(TpsCopyStatus(&state, &copied));
  EXPECT_FALSE(TpsAcceptStatus(&state, &n));
  n.connectionEpoch = 1;
  ASSERT_TRUE(TpsAcceptStatus(&state, &n));
}
TEST(PolicyNoticeReceiver, ProgressAndFirstSendIdentityCannotRegress) {
  auto base = notice();
  TPS_STATUS_RECEIVER state;
  TpsInitializeReceiver(&state, base.connectionEpoch);
  ASSERT_TRUE(TpsAcceptStatus(&state, &base));
  for (int mutation = 0; mutation < 6; ++mutation) {
    auto n = base; n.noticeSequence++;
    switch (mutation) {
      case 0: n.acceptedRevision--; n.encoderAppliedRevision = 5; break;
      case 1: n.encoderAppliedRevision--; break;
      case 2: n.firstSentRevision--; break;
      case 3: n.firstSentFrame--; break;
      case 4: n.controlSource = 1; break;
      case 5: n.controlEpoch = 0; break;
    }
    EXPECT_FALSE(TpsAcceptStatus(&state, &n)) << mutation;
    EXPECT_EQ(state.latest.noticeSequence, base.noticeSequence);
  }
}
TEST(PolicyNoticeReceiver, StoppedAndExhaustedSequenceCannotRevive) {
  auto n = notice();
  TPS_STATUS_RECEIVER state;
  TpsInitializeReceiver(&state, n.connectionEpoch);
  n.noticeSequence = std::numeric_limits<std::uint64_t>::max();
  n.flags = TPS_APPLIED_KNOWN | TPS_FIRST_SENT_KNOWN | TPS_STOPPED;
  ASSERT_TRUE(TpsAcceptStatus(&state, &n));
  n.noticeSequence = 0;
  EXPECT_FALSE(TpsAcceptStatus(&state, &n));
  n.noticeSequence = 1;
  n.flags |= TPS_ENCODER_READY;
  EXPECT_FALSE(TpsAcceptStatus(&state, &n));
  TpsInitializeReceiver(&state, 0);
  EXPECT_FALSE(TpsAcceptStatus(&state, &n));
  EXPECT_FALSE(TpsCopyStatus(nullptr, &n));
  EXPECT_FALSE(TpsAcceptStatus(nullptr, &n));
}
TEST(PolicyNoticeProjection, ActualArbiterPhasesMatchPairedJsonAndDoNotGrantControl) {
  transport::policy_state_t state(initial(), 80000, true, true);
  auto s = state.snapshot();
  auto n = transport::policy_notice(s, 1, 1);
  ASSERT_TRUE(n); compare_api(s, *n);
  EXPECT_EQ(n->controlSource, 0);
  const auto p = state.begin_encoder_initialization();
  ASSERT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  ASSERT_TRUE(state.acknowledge_first_sent(p, 0));
  s = state.snapshot(); n = transport::policy_notice(s, 1, 2);
  ASSERT_TRUE(n); compare_api(s, *n);
  EXPECT_TRUE(n->flags & TPS_FIRST_SENT_KNOWN);
  EXPECT_EQ(n->firstSentFrame, 0);
  const auto update = state.request_normalized(p->budget, 10, 20, 30, p->revision, p->control_epoch, "notify-test");
  ASSERT_EQ(update.result, transport::policy_request_result_e::accepted);
  s = state.snapshot(); n = transport::policy_notice(s, 1, 3);
  ASSERT_TRUE(n); compare_api(s, *n);
  EXPECT_EQ(n->controlSource, 1);
  EXPECT_GT(n->controlEpoch, 1);
  const auto applying = state.acquire_pending();
  ASSERT_TRUE(state.acknowledge_encoder(applying, transport::policy_failure_e::backend_failure));
  s = state.snapshot(); n = transport::policy_notice(s, 1, 4);
  ASSERT_TRUE(n); compare_api(s, *n);
  EXPECT_EQ(n->failure, 2);
  state.stop(); s = state.snapshot(); n = transport::policy_notice(s, 1, 5);
  ASSERT_TRUE(n); compare_api(s, *n);
  EXPECT_TRUE(n->flags & TPS_STOPPED);
}
TEST(PolicyNoticeSender, QueueFailureDoesNotAdvanceAndHeartbeatIsBounded) {
  transport::policy_state_t state(initial(), 80000);
  transport::policy_notice_sender_t sender;
  const auto first = sender.prepare(state.snapshot(), 1, 0);
  ASSERT_TRUE(first);
  EXPECT_EQ(sender.prepare(state.snapshot(), 1, 10), first);
  sender.queued(*first, 10);
  EXPECT_FALSE(sender.prepare(state.snapshot(), 1, 2000009));
  EXPECT_EQ(sender.prepare(state.snapshot(), 1, 2000010), first);
  auto p = state.begin_encoder_initialization();
  ASSERT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  EXPECT_FALSE(sender.prepare(state.snapshot(), 1, 250009));
  auto changed = sender.prepare(state.snapshot(), 1, 250010);
  ASSERT_TRUE(changed);
  TPS_STATUS_NOTICE n {};
  ASSERT_TRUE(TpsDecodeStatus(changed->data(), changed->size(), &n));
  EXPECT_EQ(n.noticeSequence, 2);
  EXPECT_EQ(sender.prepare(state.snapshot(), 1, 500000), changed);
  sender.queued(*changed, 500000);
  EXPECT_FALSE(sender.prepare(state.snapshot(), 1, 499999));
  EXPECT_FALSE(sender.prepare(state.snapshot(), 1, -1));
}
TEST(PolicyNoticeSender, SequenceExhaustionKeepsHeartbeatWithoutWrapping) {
  transport::policy_state_t state(initial(), 80000);
  transport::policy_notice_sender_t sender;
  const auto n = transport::policy_notice(state.snapshot(), 1, std::numeric_limits<std::uint64_t>::max());
  ASSERT_TRUE(n);
  std::array<std::uint8_t, TPS_STATUS_BYTES> bytes;
  ASSERT_EQ(TpsEncodeStatus(&*n, bytes.data(), bytes.size()), bytes.size());
  sender.queued(bytes, 0);
  EXPECT_EQ(sender.prepare(state.snapshot(), 1, 2000000), bytes);
  const auto p = state.begin_encoder_initialization();
  ASSERT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  EXPECT_FALSE(sender.prepare(state.snapshot(), 1, 2500000));
}

TEST(PolicyNoticeSender, BoundedReceiptEvictionPreservesPreviouslyPublishedFirstSend) {
  transport::policy_state_t state(initial(), 80000);
  transport::policy_notice_sender_t sender;
  const auto initial_policy = state.begin_encoder_initialization();
  ASSERT_TRUE(state.acknowledge_encoder(initial_policy, transport::policy_failure_e::none));
  ASSERT_TRUE(state.acknowledge_first_sent(initial_policy, 0));
  const auto first = sender.prepare(state.snapshot(), 1, 0);
  ASSERT_TRUE(first);
  sender.queued(*first, 0);
  TPS_STATUS_NOTICE first_notice {};
  ASSERT_TRUE(TpsDecodeStatus(first->data(), first->size(), &first_notice));
  TPS_STATUS_RECEIVER receiver;
  TpsInitializeReceiver(&receiver, 42);
  ASSERT_TRUE(TpsAcceptStatus(&receiver, &first_notice));
  for (unsigned i = 0; i < 40; ++i) {
    const auto current = state.snapshot().accepted;
    const auto request = state.request_normalized(current->budget, 10 + i % 2, 20, 30,
      current->revision, current->control_epoch, "evict-" + std::to_string(i));
    ASSERT_EQ(request.result, transport::policy_request_result_e::accepted);
    ASSERT_TRUE(state.acknowledge_encoder(state.acquire_pending(), transport::policy_failure_e::none));
  }
  const auto projection = transport::policy_notice(state.snapshot(), 1, 1);
  ASSERT_TRUE(projection);
  ASSERT_FALSE(projection->flags & TPS_FIRST_SENT_KNOWN);
  const auto latest = sender.prepare(state.snapshot(), 1, 1000000);
  ASSERT_TRUE(latest);
  TPS_STATUS_NOTICE latest_notice {};
  ASSERT_TRUE(TpsDecodeStatus(latest->data(), latest->size(), &latest_notice));
  EXPECT_GT(latest_notice.acceptedRevision, first_notice.acceptedRevision);
  EXPECT_EQ(latest_notice.firstSentRevision, first_notice.firstSentRevision);
  EXPECT_EQ(latest_notice.firstSentFrame, 0);
  ASSERT_TRUE(TpsAcceptStatus(&receiver, &latest_notice));
}

TEST(PolicyNoticeReceiver, EncoderRebuildReadinessIsAGaugeWhileAppliedReceiptIsHistorical) {
  transport::policy_state_t state(initial(), 80000);
  const auto p = state.begin_encoder_initialization();
  ASSERT_TRUE(state.acknowledge_encoder(p, transport::policy_failure_e::none));
  ASSERT_TRUE(state.acknowledge_first_sent(p, 0));
  TPS_STATUS_RECEIVER receiver;
  TpsInitializeReceiver(&receiver, 42);
  auto ready = transport::policy_notice(state.snapshot(), 1, 1);
  ASSERT_TRUE(ready);
  ASSERT_TRUE(TpsAcceptStatus(&receiver, &*ready));
  const auto rebuilding = state.begin_encoder_initialization();
  ASSERT_EQ(rebuilding, p);
  auto rebuilding_notice = transport::policy_notice(state.snapshot(), 1, 2);
  ASSERT_TRUE(rebuilding_notice);
  EXPECT_FALSE(rebuilding_notice->flags & TPS_ENCODER_READY);
  EXPECT_EQ(rebuilding_notice->encoderAppliedRevision, ready->encoderAppliedRevision);
  EXPECT_TRUE(TpsAcceptStatus(&receiver, &*rebuilding_notice));
  ASSERT_TRUE(state.acknowledge_encoder(rebuilding, transport::policy_failure_e::backend_failure));
  auto failed = transport::policy_notice(state.snapshot(), 1, 3);
  ASSERT_TRUE(failed);
  EXPECT_TRUE(TpsAcceptStatus(&receiver, &*failed));
  ASSERT_TRUE(state.acknowledge_encoder(state.begin_encoder_initialization(), transport::policy_failure_e::none));
  auto recovered = transport::policy_notice(state.snapshot(), 1, 4);
  ASSERT_TRUE(recovered);
  EXPECT_TRUE(recovered->flags & TPS_ENCODER_READY);
  EXPECT_TRUE(TpsAcceptStatus(&receiver, &*recovered));
}
