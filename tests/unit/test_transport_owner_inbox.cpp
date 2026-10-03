#include "src/transport_owner_inbox.h"

#include <gtest/gtest.h>
#include <array>
#include <barrier>
#include <future>
#include <thread>

namespace {
  using namespace transport;
  using namespace std::chrono_literals;

  pacing_limits_t limits(std::uint64_t rate = 1000) { return {{rate, 2400, 1200}, {rate, 1200, 600}}; }
  owner_frame_t frame(const owner_flow_ref_t &flow, std::uint64_t id, std::size_t bytes = 100,
    frame_dependency_e dependency = frame_dependency_e::non_reference, std::shared_ptr<void> payload = {}) {
    frame_policy_t value;
    value.connection_epoch = flow->connection_epoch;
    value.revision = id + 1;
    return {flow, id, std::make_shared<const frame_policy_t>(value), bytes, 12345, dependency,
            payload ? std::move(payload) : std::make_shared<int>(static_cast<int>(id))};
  }

  TEST(TransportOwnerInbox, PayloadAndContextLiveUntilExplicitOwnerSettlementAndDrain) {
    owner_inbox_t inbox;
    auto context = std::make_shared<int>(42);
    std::weak_ptr<int> weak_context = context;
    auto flow = inbox.add_flow(7, context, limits());
    ASSERT_TRUE(flow);
    context.reset();
    auto payload = std::make_shared<int>(123);
    std::weak_ptr<int> weak_payload = payload;
    ASSERT_EQ(inbox.submit(frame(flow, 1, 100, frame_dependency_e::reference, payload)), owner_submit_result_e::accepted);
    payload.reset();
    EXPECT_FALSE(weak_payload.expired());
    auto registration = inbox.take_commands();
    ASSERT_EQ(registration.size(), 1);
    EXPECT_TRUE(registration[0].register_flow);
    EXPECT_EQ(registration[0].flow->handle, flow->handle);
    registration.clear();
    ASSERT_TRUE(inbox.close(flow));
    EXPECT_FALSE(inbox.acknowledge_drained(flow));
    auto closing = inbox.take_commands();
    ASSERT_EQ(closing.size(), 1);
    ASSERT_EQ(closing[0].discarded_frames.size(), 1);
    EXPECT_FALSE(weak_payload.expired());
    closing[0].discarded_frames.clear();
    EXPECT_TRUE(weak_payload.expired());
    ASSERT_TRUE(inbox.acknowledge_drained(flow));
    EXPECT_TRUE(flow->is_drained());
    EXPECT_FALSE(weak_context.expired());
    closing.clear();
    flow.reset();
    EXPECT_TRUE(weak_context.expired());
    EXPECT_EQ(inbox.snapshot().flows, 0u);
  }

  TEST(TransportOwnerInbox, AFullFlowDoesNotClearAnotherFlowsOwnership) {
    owner_inbox_bounds_t bounds {2, 3, 300, 1};
    owner_inbox_t inbox(bounds);
    auto a = inbox.add_flow(7, {}, limits());
    auto b = inbox.add_flow(8, {}, limits());
    inbox.take_commands();
    ASSERT_EQ(inbox.submit(frame(a, 1, 50, frame_dependency_e::reference)), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit(frame(b, 1, 80)), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.submit(frame(a, 2, 50, frame_dependency_e::reference)), owner_submit_result_e::full);
    EXPECT_EQ(inbox.snapshot().queued_frames, 2u);
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands[0].flow, a);
    ASSERT_TRUE(commands[0].highest_reference_break);
    EXPECT_EQ(*commands[0].highest_reference_break, 2u);
    EXPECT_EQ(commands[0].drops.full_frames, 1u);
    EXPECT_EQ(commands[0].drops.rejected_bytes, 50u);
    ASSERT_EQ(commands[0].discarded_frames.size(), 1);
    EXPECT_EQ(commands[0].discarded_frames[0].frame_id, 1u);
    EXPECT_EQ(inbox.snapshot().queued_bytes, 80u);
    auto retained = inbox.take_frame();
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->flow, b);
    EXPECT_EQ(retained->owned_bytes, 80u);
  }

  TEST(TransportOwnerInbox, NonReferenceQueueRejectionNeverManufacturesABreak) {
    owner_inbox_t inbox({1, 1, 100, 1});
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.submit(frame(flow, 2)), owner_submit_result_e::full);
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_FALSE(commands[0].highest_reference_break);
    EXPECT_TRUE(commands[0].discarded_frames.empty());
    EXPECT_EQ(commands[0].drops.full_frames, 1u);
    auto kept = inbox.take_frame();
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->frame_id, 1u);
  }

  TEST(TransportOwnerInbox, RecoveryRejectionCullsEveryOlderOwnedFrameBeforePacerMarker) {
    owner_inbox_t inbox({1, 1, 100, 1});
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    ASSERT_EQ(inbox.submit(frame(flow, 95)), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.submit(frame(flow, 100, 100, frame_dependency_e::recovery)), owner_submit_result_e::full);
    EXPECT_FALSE(inbox.take_frame());
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands[0].highest_reference_break, 100u);
    ASSERT_EQ(commands[0].discarded_frames.size(), 1);
    EXPECT_EQ(commands[0].discarded_frames[0].frame_id, 95u);
    EXPECT_EQ(commands[0].drops.discarded_frames, 1u);
    EXPECT_EQ(commands[0].drops.discarded_bytes, 100u);
    EXPECT_EQ(inbox.snapshot().queued_frames, 0u);
  }

  TEST(TransportOwnerInbox, MarkerCullAndMergedLimitsStayWithinOneCommandSlot) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit(frame(flow, 3)), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit(frame(flow, 11)), owner_submit_result_e::accepted);
    for (unsigned i = 0; i < 10000; ++i) ASSERT_TRUE(inbox.update_limits(flow, limits(i + 1)));
    ASSERT_TRUE(inbox.mark_reference_break(flow, 3));
    ASSERT_TRUE(inbox.mark_reference_break(flow, 10));
    ASSERT_TRUE(inbox.mark_reference_break(flow, 8));
    EXPECT_EQ(inbox.snapshot().pending_commands, 1u);
    EXPECT_FALSE(inbox.take_frame());
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_TRUE(commands[0].register_flow);
    EXPECT_EQ(commands[0].initial_limits, limits());
    EXPECT_EQ(commands[0].latest_limits, limits(10000));
    EXPECT_EQ(commands[0].highest_reference_break, 10u);
    ASSERT_EQ(commands[0].discarded_frames.size(), 2);
    EXPECT_EQ(commands[0].discarded_frames[0].frame_id, 1u);
    EXPECT_EQ(commands[0].discarded_frames[1].frame_id, 3u);
    auto future = inbox.take_frame();
    ASSERT_TRUE(future);
    EXPECT_EQ(future->frame_id, 11u);
    EXPECT_EQ(inbox.snapshot().queued_bytes, 0u);
  }

  TEST(TransportOwnerInbox, DelayedOldFrameAfterMarkerIsExplicitlyStaleAndNotRetained) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    ASSERT_TRUE(inbox.mark_reference_break(flow, 10));
    inbox.take_commands();
    auto payload = std::make_shared<int>(8);
    std::weak_ptr<int> weak = payload;
    EXPECT_EQ(inbox.submit(frame(flow, 8, 100, frame_dependency_e::reference, payload)), owner_submit_result_e::stale);
    payload.reset();
    EXPECT_TRUE(weak.expired());
    auto delta = inbox.take_commands();
    ASSERT_EQ(delta.size(), 1);
    EXPECT_FALSE(delta[0].highest_reference_break);
    EXPECT_EQ(delta[0].drops.stale_frames, 1u);
    EXPECT_EQ(inbox.snapshot().queued_bytes, 0u);
    EXPECT_EQ(inbox.submit(frame(flow, 11)), owner_submit_result_e::accepted);
  }

  TEST(TransportOwnerInbox, CloseBeforeRegistrationSupersedesOtherCommandsAndStillDrains) {
    owner_inbox_t inbox({1, 2, 200, 2});
    auto flow = inbox.add_flow(7, {}, limits());
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    ASSERT_TRUE(inbox.update_limits(flow, limits(5)));
    ASSERT_TRUE(inbox.mark_reference_break(flow, 2));
    ASSERT_TRUE(inbox.close(flow));
    ASSERT_TRUE(inbox.close(flow));
    EXPECT_FALSE(inbox.acknowledge_drained(flow));
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_TRUE(commands[0].close);
    EXPECT_FALSE(commands[0].register_flow);
    EXPECT_FALSE(commands[0].latest_limits);
    EXPECT_FALSE(commands[0].highest_reference_break);
    EXPECT_EQ(commands[0].discarded_frames.size(), 1u);
    EXPECT_TRUE(inbox.take_commands().empty());
    commands[0].discarded_frames.clear();
    ASSERT_TRUE(inbox.acknowledge_drained(flow));
    EXPECT_TRUE(inbox.acknowledge_drained(flow));
    EXPECT_TRUE(inbox.close(flow));
  }

  TEST(TransportOwnerInbox, NoSlotReuseUntilAcknowledgementAndIdentityNeverReuses) {
    owner_inbox_t inbox({1, 2, 200, 2});
    auto first = inbox.add_flow(7, {}, limits());
    EXPECT_FALSE(inbox.add_flow(8, {}, limits()));
    ASSERT_TRUE(inbox.close(first));
    EXPECT_FALSE(inbox.add_flow(8, {}, limits()));
    inbox.take_commands();
    EXPECT_FALSE(inbox.add_flow(8, {}, limits()));
    ASSERT_TRUE(inbox.acknowledge_drained(first));
    auto fresh = inbox.add_flow(8, {}, limits());
    ASSERT_TRUE(fresh);
    EXPECT_GT(fresh->handle, first->handle);
    EXPECT_EQ(inbox.submit(frame(first, 9)), owner_submit_result_e::closed);
    EXPECT_FALSE(fresh->is_closed());
    EXPECT_EQ(inbox.snapshot().flows, 1u);
  }

  TEST(TransportOwnerInbox, RoundRobinPreservesPerFlowArrivalOrder) {
    owner_inbox_t inbox;
    std::array<owner_flow_ref_t, 3> flows;
    for (unsigned i = 0; i < flows.size(); ++i) flows[i] = inbox.add_flow(7 + i, {}, limits());
    inbox.take_commands();
    for (const auto &flow : flows) for (unsigned id = 0; id < 3; ++id) ASSERT_EQ(inbox.submit(frame(flow, id)), owner_submit_result_e::accepted);
    for (unsigned i = 0; i < 9; ++i) {
      auto taken = inbox.take_frame();
      ASSERT_TRUE(taken);
      EXPECT_EQ(taken->flow, flows[i % 3]);
      EXPECT_EQ(taken->frame_id, i / 3);
      EXPECT_EQ(taken->deadline_origin_us, 12345);
      EXPECT_EQ(taken->policy->revision, i / 3 + 1);
    }
    EXPECT_FALSE(inbox.take_frame());
  }

  TEST(TransportOwnerInbox, ClosingAnAlreadyTakenFrameImmediatelySealsItsGate) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    auto owner_frame = inbox.take_frame();
    ASSERT_TRUE(owner_frame);
    ASSERT_FALSE(owner_frame->flow->is_closed());
    ASSERT_TRUE(inbox.close(flow));
    EXPECT_TRUE(owner_frame->flow->is_closed());
    unsigned attempted_datagrams = 0;
    if (!owner_frame->flow->is_closed()) ++attempted_datagrams;
    EXPECT_EQ(attempted_datagrams, 0u);
    auto command = inbox.take_commands();
    ASSERT_EQ(command.size(), 1);
    EXPECT_TRUE(command[0].discarded_frames.empty());
    owner_frame.reset();  // Explicitly settle the owner's previously taken work.
    EXPECT_TRUE(inbox.acknowledge_drained(flow));
  }

  TEST(TransportOwnerInbox, GlobalStopClosesEveryFlowWakesOwnerAndPreservesAllDrainBarriers) {
    owner_inbox_t inbox;
    auto a = inbox.add_flow(7, {}, limits());
    auto b = inbox.add_flow(8, {}, limits());
    ASSERT_EQ(inbox.submit(frame(a, 1)), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit(frame(b, 1)), owner_submit_result_e::accepted);
    auto generation = inbox.wake_generation();
    inbox.stop();
    inbox.stop();
    EXPECT_TRUE(a->is_closed());
    EXPECT_TRUE(b->is_closed());
    EXPECT_EQ(inbox.wait_until(generation, std::chrono::steady_clock::now() + 1s), owner_wait_result_e::stopped);
    EXPECT_FALSE(inbox.take_frame());
    EXPECT_FALSE(inbox.add_flow(9, {}, limits()));
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 2);
    for (auto &command : commands) {
      EXPECT_TRUE(command.close);
      EXPECT_FALSE(command.register_flow);
      EXPECT_EQ(command.discarded_frames.size(), 1u);
      command.discarded_frames.clear();
      ASSERT_TRUE(inbox.acknowledge_drained(command.flow));
    }
    EXPECT_TRUE(a->is_drained());
    EXPECT_TRUE(b->is_drained());
    EXPECT_EQ(inbox.snapshot().flows, 0u);
    EXPECT_EQ(inbox.snapshot().queued_bytes, 0u);
    EXPECT_EQ(inbox.snapshot().drops.discarded_frames, 2u);
  }

  TEST(TransportOwnerInbox, DrainWaitOnlyCompletesAfterOwnerAcknowledgement) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    ASSERT_TRUE(inbox.close(flow));
    inbox.take_commands();
    auto wait = std::async(std::launch::async, [&] { return flow->wait_drained_until(std::chrono::steady_clock::now() + 500ms); });
    EXPECT_EQ(wait.wait_for(20ms), std::future_status::timeout);
    EXPECT_FALSE(flow->is_drained());
    EXPECT_TRUE(inbox.acknowledge_drained(flow));
    EXPECT_TRUE(wait.get());
    EXPECT_TRUE(flow->wait_drained_until(std::chrono::steady_clock::now()));
  }

  TEST(TransportOwnerInbox, GenerationClosesTheProcessingToWaitLostWakeWindow) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    const auto generation = inbox.wake_generation();
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    auto handled = inbox.take_frame();
    ASSERT_TRUE(handled);
    EXPECT_EQ(inbox.wait_until(generation, std::chrono::steady_clock::now() + 100ms), owner_wait_result_e::woken);
    const auto fresh = inbox.wake_generation();
    EXPECT_EQ(inbox.wait_until(fresh, std::chrono::steady_clock::now() - 1ms), owner_wait_result_e::deadline);
  }

  TEST(TransportOwnerInbox, RepeatedNotificationsCannotRestartAnAbsoluteDeadline) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    // An already-expired absolute deadline makes this oracle independent of
    // scheduler latency. Each recorded notification wins once; recapturing
    // its generation immediately restores the same expired deadline.
    const auto deadline = std::chrono::steady_clock::now() - 1ms;
    for (unsigned i = 1; i <= 10; ++i) {
      const auto generation = inbox.wake_generation();
      ASSERT_TRUE(inbox.update_limits(flow, limits(i)));
      EXPECT_EQ(inbox.wait_until(generation, deadline), owner_wait_result_e::woken);
      const auto fresh_generation = inbox.wake_generation();
      inbox.take_commands();
      EXPECT_EQ(inbox.wait_until(fresh_generation, deadline), owner_wait_result_e::deadline);
    }
  }

  TEST(TransportOwnerInbox, FourProducersObserveExactBoundsWithoutClearingAcceptedFrames) {
    owner_inbox_t inbox({4, 16, 16, 4});
    std::array<owner_flow_ref_t, 4> flows;
    for (unsigned i = 0; i < flows.size(); ++i) flows[i] = inbox.add_flow(7 + i, {}, limits());
    std::barrier start(4);
    std::array<std::thread, 4> producers;
    std::array<unsigned, 4> accepted {};
    for (unsigned i = 0; i < producers.size(); ++i) producers[i] = std::thread([&, i] {
      start.arrive_and_wait();
      for (unsigned id = 0; id < 128; ++id) {
        const auto result = inbox.submit(frame(flows[i], id, 1));
        if (result == owner_submit_result_e::accepted) ++accepted[i];
        else EXPECT_EQ(result, owner_submit_result_e::full);
      }
    });
    for (auto &producer : producers) producer.join();
    for (auto count : accepted) EXPECT_EQ(count, 4u);
    EXPECT_EQ(inbox.snapshot().queued_frames, 16u);
    EXPECT_EQ(inbox.snapshot().queued_bytes, 16u);
    EXPECT_EQ(inbox.snapshot().drops.full_frames, 496u);
    EXPECT_EQ(inbox.snapshot().pending_commands, 4u);
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 4);
    for (const auto &command : commands) {
      EXPECT_EQ(command.drops.full_frames, 124u);
      EXPECT_FALSE(command.highest_reference_break);
      EXPECT_TRUE(command.discarded_frames.empty());
    }
    for (unsigned i = 0; i < 16; ++i) {
      auto taken = inbox.take_frame();
      ASSERT_TRUE(taken);
      EXPECT_EQ(taken->flow, flows[i % 4]);
      EXPECT_EQ(taken->frame_id, i / 4);
    }
  }

  TEST(TransportOwnerInbox, ConcurrentCloseAndLimitsCannotLoseAcceptedOwnership) {
    owner_inbox_t inbox({1, 8, 800, 8});
    auto flow = inbox.add_flow(7, {}, limits());
    std::barrier start(3);
    std::atomic<unsigned> accepted {0};
    auto producer = std::thread([&] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 1000; ++i) if (inbox.submit(frame(flow, i)) == owner_submit_result_e::accepted) ++accepted;
    });
    auto updates = std::thread([&] {
      start.arrive_and_wait();
      for (unsigned i = 0; i < 1000; ++i) inbox.update_limits(flow, limits(i));
    });
    auto closing = std::thread([&] { start.arrive_and_wait(); inbox.close(flow); });
    producer.join(); updates.join(); closing.join();
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_TRUE(commands[0].close);
    EXPECT_FALSE(commands[0].register_flow);
    EXPECT_FALSE(commands[0].latest_limits);
    EXPECT_EQ(commands[0].discarded_frames.size(), accepted.load());
    EXPECT_EQ(commands[0].drops.discarded_bytes, accepted.load() * 100u);
    EXPECT_EQ(commands[0].drops.full_frames + commands[0].drops.closed_frames + accepted.load(), 1000u);
    EXPECT_EQ(inbox.snapshot().queued_bytes, 0u);
    commands[0].discarded_frames.clear();
    EXPECT_TRUE(inbox.acknowledge_drained(flow));
  }

  TEST(TransportOwnerInbox, ForeignIdentityWithEqualHandleCannotCloseOrMutateAFlow) {
    owner_inbox_t a, b;
    auto flow_a = a.add_flow(7, {}, limits());
    auto flow_b = b.add_flow(7, {}, limits());
    EXPECT_EQ(flow_a->handle, flow_b->handle);
    EXPECT_FALSE(b.close(flow_a));
    EXPECT_FALSE(b.update_limits(flow_a, limits(1)));
    EXPECT_FALSE(b.mark_reference_break(flow_a, 9));
    EXPECT_FALSE(b.acknowledge_drained(flow_a));
    EXPECT_EQ(b.submit(frame(flow_a, 1)), owner_submit_result_e::unknown_flow);
    EXPECT_FALSE(flow_a->is_closed());
    EXPECT_FALSE(flow_b->is_closed());
  }

  TEST(TransportOwnerInbox, FeedbackSizeAndReceiptValidationCountsDropsWithoutLossMarkers) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    EXPECT_EQ(inbox.submit_feedback(flow, std::vector<std::uint8_t>(885), 1), owner_submit_result_e::invalid);
    EXPECT_EQ(inbox.submit_feedback(flow, {}, 1), owner_submit_result_e::invalid);
    EXPECT_EQ(inbox.submit_feedback(flow, {1}, -1), owner_submit_result_e::invalid);
    auto drops = inbox.take_commands();
    ASSERT_EQ(drops.size(), 1);
    EXPECT_EQ(drops[0].drops.dropped_feedback, 3u);
    EXPECT_EQ(drops[0].drops.dropped_feedback_bytes, 886u);
    EXPECT_FALSE(drops[0].highest_reference_break);
    EXPECT_EQ(drops[0].drops.full_frames, 0u);
    EXPECT_EQ(inbox.snapshot().queued_feedback_messages, 0u);
  }

  TEST(TransportOwnerInbox, FeedbackPerFlowLimitDoesNotConsumeAnotherFlowsCapacity) {
    owner_inbox_t inbox;
    auto a = inbox.add_flow(7, {}, limits());
    auto b = inbox.add_flow(8, {}, limits());
    inbox.take_commands();
    for (unsigned i = 0; i < 8; ++i) ASSERT_EQ(inbox.submit_feedback(a, std::vector<std::uint8_t>(884, i), i), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.submit_feedback(a, {9}, 9), owner_submit_result_e::full);
    ASSERT_EQ(inbox.submit_feedback(b, {10}, 10), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.snapshot().queued_feedback_messages, 9u);
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 7073u);
    auto drops = inbox.take_commands();
    ASSERT_EQ(drops.size(), 1);
    EXPECT_EQ(drops[0].drops.dropped_feedback, 1u);
    EXPECT_FALSE(drops[0].highest_reference_break);
    auto first = inbox.take_feedback();
    auto second = inbox.take_feedback();
    ASSERT_TRUE(first); ASSERT_TRUE(second);
    EXPECT_EQ(first->flow, a);
    EXPECT_EQ(second->flow, b);
  }

  TEST(TransportOwnerInbox, FeedbackGlobalCountAndBytesAreExactAndReleasedByTake) {
    owner_inbox_t inbox({17, 32, 3200, 2});
    std::array<owner_flow_ref_t, 17> flows;
    for (unsigned i = 0; i < flows.size(); ++i) flows[i] = inbox.add_flow(7 + i, {}, limits());
    inbox.take_commands();
    for (unsigned flow = 0; flow < 16; ++flow) for (unsigned i = 0; i < 8; ++i) ASSERT_EQ(inbox.submit_feedback(flows[flow], std::vector<std::uint8_t>(884), i), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.snapshot().queued_feedback_messages, 128u);
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 113152u);
    EXPECT_EQ(inbox.submit_feedback(flows[16], {1}, 1), owner_submit_result_e::full);
    auto first = inbox.take_feedback();
    ASSERT_TRUE(first);
    EXPECT_EQ(inbox.snapshot().queued_feedback_messages, 127u);
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 112268u);
    EXPECT_EQ(inbox.submit_feedback(flows[16], std::vector<std::uint8_t>(884), 2), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 113152u);
  }

  TEST(TransportOwnerInbox, FeedbackIsOwnedRoundRobinAndPreservesOriginalReceiptTime) {
    owner_inbox_t inbox;
    auto a = inbox.add_flow(7, {}, limits());
    auto b = inbox.add_flow(8, {}, limits());
    inbox.take_commands();
    ASSERT_EQ(inbox.submit_feedback(a, {1, 2, 3}, 100), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit_feedback(a, {4, 5}, 101), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit_feedback(b, {6}, 200), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit_feedback(b, {7, 8, 9, 10}, 201), owner_submit_result_e::accepted);
    const std::array<owner_flow_ref_t, 4> expected {a, b, a, b};
    const std::array<std::int64_t, 4> received {100, 200, 101, 201};
    const std::array<std::vector<std::uint8_t>, 4> data {{{1, 2, 3}, {6}, {4, 5}, {7, 8, 9, 10}}};
    for (unsigned i = 0; i < 4; ++i) {
      auto feedback = inbox.take_feedback();
      ASSERT_TRUE(feedback);
      EXPECT_EQ(feedback->flow, expected[i]);
      EXPECT_EQ(feedback->received_at_us, received[i]);
      EXPECT_EQ(feedback->authenticated_bytes, data[i]);
    }
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 0u);
    EXPECT_FALSE(inbox.take_feedback());
  }

  TEST(TransportOwnerInbox, ClosePurgesFeedbackWithExactDropCountAndDrainAcknowledgement) {
    owner_inbox_t inbox;
    auto a = inbox.add_flow(7, {}, limits());
    auto b = inbox.add_flow(8, {}, limits());
    inbox.take_commands();
    ASSERT_EQ(inbox.submit_feedback(a, std::vector<std::uint8_t>(7), 1), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit_feedback(a, std::vector<std::uint8_t>(9), 2), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit_feedback(b, std::vector<std::uint8_t>(5), 3), owner_submit_result_e::accepted);
    ASSERT_TRUE(inbox.close(a));
    EXPECT_FALSE(inbox.acknowledge_drained(a));
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_TRUE(commands[0].close);
    EXPECT_EQ(commands[0].drops.dropped_feedback, 2u);
    EXPECT_EQ(commands[0].drops.dropped_feedback_bytes, 16u);
    EXPECT_EQ(inbox.snapshot().queued_feedback_messages, 1u);
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 5u);
    auto other = inbox.take_feedback();
    ASSERT_TRUE(other);
    EXPECT_EQ(other->flow, b);
    EXPECT_EQ(inbox.submit_feedback(a, {1, 2, 3, 4}, 4), owner_submit_result_e::closed);
    auto closed_drop = inbox.take_commands();
    ASSERT_EQ(closed_drop.size(), 1);
    EXPECT_FALSE(closed_drop[0].close);
    EXPECT_EQ(closed_drop[0].drops.dropped_feedback, 1u);
    EXPECT_EQ(inbox.snapshot().drops.dropped_feedback, 3u);
    EXPECT_EQ(inbox.snapshot().drops.dropped_feedback_bytes, 20u);
    EXPECT_TRUE(inbox.acknowledge_drained(a));
  }

  TEST(TransportOwnerInbox, FeedbackNotificationUsesSameWakeGenerationContract) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    const auto generation = inbox.wake_generation();
    ASSERT_EQ(inbox.submit_feedback(flow, {1}, 123), owner_submit_result_e::accepted);
    EXPECT_EQ(inbox.wait_until(generation, std::chrono::steady_clock::now() + 100ms), owner_wait_result_e::woken);
    auto feedback = inbox.take_feedback();
    ASSERT_TRUE(feedback);
    inbox.stop();
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands[0].drops.dropped_feedback, 0u);
    feedback.reset();
    EXPECT_TRUE(inbox.acknowledge_drained(flow));
  }

  TEST(TransportOwnerInbox, FeedbackCannotHideAnOversizedAllocationBehindASmallWireSize) {
    owner_inbox_t inbox;
    auto flow = inbox.add_flow(7, {}, limits());
    inbox.take_commands();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(65536);
    bytes.push_back(1);
    EXPECT_EQ(inbox.submit_feedback(flow, std::move(bytes), 1), owner_submit_result_e::invalid);
    EXPECT_EQ(inbox.snapshot().queued_feedback_bytes, 0u);
    auto command = inbox.take_commands();
    ASSERT_EQ(command.size(), 1);
    EXPECT_EQ(command[0].drops.dropped_feedback, 1u);
    EXPECT_FALSE(command[0].highest_reference_break);
  }

  TEST(TransportOwnerInbox, ZeroFrameMarkerIsPresentAndClosedCommandsPrecedeOtherRegistrations) {
    owner_inbox_t inbox;
    auto a = inbox.add_flow(7, {}, limits());
    auto b = inbox.add_flow(8, {}, limits());
    ASSERT_EQ(inbox.submit(frame(a, 0)), owner_submit_result_e::accepted);
    ASSERT_TRUE(inbox.mark_reference_break(a, 0));
    ASSERT_TRUE(inbox.close(b));
    auto commands = inbox.take_commands();
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands[0].flow, b);
    EXPECT_TRUE(commands[0].close);
    EXPECT_EQ(commands[1].flow, a);
    ASSERT_TRUE(commands[1].highest_reference_break);
    EXPECT_EQ(*commands[1].highest_reference_break, 0u);
    ASSERT_EQ(commands[1].discarded_frames.size(), 1);
    EXPECT_EQ(commands[1].discarded_frames[0].frame_id, 0u);
    EXPECT_EQ(inbox.submit(frame(a, 0)), owner_submit_result_e::stale);
  }
  TEST(TransportOwnerInbox, EmergencyDrainRejectsRunningOwnerWithoutTouchingGatesOrPayloads) {
    owner_inbox_t inbox;
    const auto flow = inbox.add_flow(7, {}, limits());
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    ASSERT_FALSE(inbox.emergency_drained_after_owner_abort());
    EXPECT_FALSE(flow->is_closed());
    EXPECT_FALSE(flow->is_drained());
    EXPECT_EQ(inbox.snapshot().queued_frames, 1u);
    EXPECT_EQ(inbox.snapshot().flows, 1u);
  }

  TEST(TransportOwnerInbox, EmergencyDrainReleasesRetainedIngressAndCountsEncodedDiscardsOnly) {
    owner_inbox_t inbox;
    const auto a = inbox.add_flow(7, {}, limits());
    const auto b = inbox.add_flow(8, {}, limits());
    auto payload = std::make_shared<int>(42);
    std::weak_ptr<int> weak = payload;
    ASSERT_EQ(inbox.submit(frame(a, 1, 101, frame_dependency_e::reference, payload)), owner_submit_result_e::accepted);
    ASSERT_EQ(inbox.submit(frame(b, 2, 202)), owner_submit_result_e::accepted);
    payload.reset();
    ASSERT_EQ(inbox.submit_feedback(a, {1, 2, 3}, 100), owner_submit_result_e::accepted);
    inbox.stop();
    ASSERT_TRUE(inbox.emergency_drained_after_owner_abort());
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(a->is_closed());
    EXPECT_TRUE(a->is_drained());
    EXPECT_TRUE(b->is_drained());
    const auto state = inbox.snapshot();
    EXPECT_TRUE(state.stopped);
    EXPECT_EQ(state.flows, 0u);
    EXPECT_EQ(state.queued_frames, 0u);
    EXPECT_EQ(state.queued_bytes, 0u);
    EXPECT_EQ(state.queued_feedback_messages, 0u);
    EXPECT_EQ(state.queued_feedback_bytes, 0u);
    EXPECT_EQ(state.drops.discarded_frames, 2u);
    EXPECT_EQ(state.drops.discarded_bytes, 303u);
    EXPECT_EQ(state.drops.dropped_feedback, 1u);
    EXPECT_EQ(state.drops.dropped_feedback_bytes, 3u);
    EXPECT_EQ(inbox.submit(frame(a, 3)), owner_submit_result_e::closed);
    ASSERT_TRUE(inbox.emergency_drained_after_owner_abort());
    EXPECT_EQ(inbox.snapshot().drops.discarded_frames, 2u);
    EXPECT_FALSE(inbox.add_flow(9, {}, limits()));
  }

  TEST(TransportOwnerInbox, EmergencyDrainAcknowledgesDeliveredCloseAfterTakenOwnersAreSettled) {
    owner_inbox_t inbox;
    const auto flow = inbox.add_flow(7, {}, limits());
    ASSERT_EQ(inbox.submit(frame(flow, 1)), owner_submit_result_e::accepted);
    inbox.close(flow);
    auto command = inbox.take_commands();
    ASSERT_EQ(command.size(), 1u);
    ASSERT_EQ(command[0].discarded_frames.size(), 1u);
    command.clear(); // Owner catches an exception only after releasing taken work.
    EXPECT_FALSE(flow->is_drained());
    inbox.stop();
    ASSERT_TRUE(inbox.emergency_drained_after_owner_abort());
    EXPECT_TRUE(flow->is_drained());
    EXPECT_EQ(inbox.snapshot().drops.discarded_frames, 1u);
    EXPECT_TRUE(inbox.acknowledge_drained(flow));
  }

  TEST(TransportOwnerInbox, EmergencyDrainWakesAWaitingSessionJoin) {
    owner_inbox_t inbox;
    const auto flow = inbox.add_flow(7, {}, limits());
    std::promise<void> started;
    auto ready = started.get_future();
    std::atomic<bool> drained {false};
    std::thread joiner([&] {
      started.set_value();
      drained.store(flow->wait_drained_until(std::chrono::steady_clock::now() + 1s));
    });
    ready.wait();
    inbox.stop();
    const auto cleaned = inbox.emergency_drained_after_owner_abort();
    // No fatal gtest assertion can leave a joinable thread on this path.
    const auto completed = flow->wait_drained_until(std::chrono::steady_clock::now() + 1s);
    joiner.join();
    EXPECT_TRUE(cleaned);
    EXPECT_TRUE(completed);
    EXPECT_TRUE(drained.load());
  }
}  // namespace
