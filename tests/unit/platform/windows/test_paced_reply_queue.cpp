/**
 * @file tests/unit/platform/windows/test_paced_reply_queue.cpp
 * @brief The Windows reply queue: interrupt replies follow new data, iso replies keep their clock.
 *
 * The interrupt heartbeat is set to 300 ms so "answered by new data" and "answered by the
 * heartbeat" are far apart even on a loaded runner; only lower bounds are used where a late
 * timer would otherwise make a check flaky.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "src/platform/windows/paced_reply_queue.h"

namespace {
  using namespace std::chrono_literals;
  using queue_t = platf::paced_reply_queue;
  using clock_t_ = queue_t::clock;

  struct recorder_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::uint8_t, clock_t_::time_point>> sent;

    bool
    send(std::vector<std::uint8_t> wire) {
      std::lock_guard lock(mutex);
      sent.emplace_back(wire.empty() ? 0 : wire[0], clock_t_::now());
      changed.notify_all();
      return true;
    }

    bool
    wait_for(std::size_t count, std::chrono::milliseconds limit) {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, limit, [&] { return sent.size() >= count; });
    }

    clock_t_::time_point
    at(std::size_t index) {
      std::lock_guard lock(mutex);
      return sent.at(index).second;
    }
  };

  queue_t::interrupt_config
  slow_heartbeat() { return { 1ms, 300ms }; }
}  // namespace

TEST(PacedReplyQueue, FirstInterruptReplyIsImmediate) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  const auto start = clock_t_::now();
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 1, [] { return std::vector<std::uint8_t> { 1 }; });
  ASSERT_TRUE(rec.wait_for(1, 200ms));
  EXPECT_LT(rec.at(0) - start, 150ms);
}

TEST(PacedReplyQueue, IdleInterruptReplyWaitsForTheHeartbeat) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 1, [] { return std::vector<std::uint8_t> { 1 }; });
  ASSERT_TRUE(rec.wait_for(1, 200ms));
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 2, [] { return std::vector<std::uint8_t> { 2 }; });
  ASSERT_TRUE(rec.wait_for(2, 2000ms));
  EXPECT_GE(rec.at(1) - rec.at(0), 280ms);
}

TEST(PacedReplyQueue, NewDataAnswersAHeldInterruptReplyAtOnce) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 1, [] { return std::vector<std::uint8_t> { 1 }; });
  ASSERT_TRUE(rec.wait_for(1, 200ms));
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 2, [] { return std::vector<std::uint8_t> { 2 }; });
  queue.notify_interrupt_data(queue_t::lane::interrupt_in);
  ASSERT_TRUE(rec.wait_for(2, 250ms));
  EXPECT_GE(rec.at(1) - rec.at(0), 900us);  // never closer than the floor
  EXPECT_LT(rec.at(1) - rec.at(0), 250ms);  // and well before the heartbeat
}

TEST(PacedReplyQueue, DataBeforeTheNextUrbAnswersItWithoutTheHeartbeat) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 1, [] { return std::vector<std::uint8_t> { 1 }; });
  ASSERT_TRUE(rec.wait_for(1, 200ms));
  queue.notify_interrupt_data(queue_t::lane::interrupt_in);
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 2, [] { return std::vector<std::uint8_t> { 2 }; });
  ASSERT_TRUE(rec.wait_for(2, 250ms));
  EXPECT_GE(rec.at(1) - rec.at(0), 900us);
}

TEST(PacedReplyQueue, TheReplyCarriesTheStateOfTheMomentItLeaves) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 1, [] { return std::vector<std::uint8_t> { 1 }; });
  ASSERT_TRUE(rec.wait_for(1, 200ms));
  std::mutex state_mutex;
  std::uint8_t state = 10;
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 2, [&] {
    std::lock_guard lock(state_mutex);
    return std::vector<std::uint8_t> { state };
  });
  {
    std::lock_guard lock(state_mutex);
    state = 42;
  }
  queue.notify_interrupt_data(queue_t::lane::interrupt_in);
  ASSERT_TRUE(rec.wait_for(2, 250ms));
  std::lock_guard lock(rec.mutex);
  EXPECT_EQ(rec.sent[1].first, 42);
}

TEST(PacedReplyQueue, NotifyWithNothingHeldDoesNotAnswerAnything) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  queue.notify_interrupt_data(queue_t::lane::interrupt_in);
  EXPECT_FALSE(rec.wait_for(1, 50ms));
  EXPECT_EQ(queue.held(), 0u);
}

TEST(PacedReplyQueue, ADataNotificationDoesNotTouchIsochronousReplies) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  const auto start = clock_t_::now();
  queue.schedule_iso(queue_t::lane::iso_out, 1, 40, { 7 });
  queue.notify_interrupt_data(queue_t::lane::interrupt_in);
  queue.notify_interrupt_data(queue_t::lane::iso_out);
  EXPECT_FALSE(rec.wait_for(1, 20ms));
  ASSERT_TRUE(rec.wait_for(1, 500ms));
  EXPECT_GE(rec.at(0) - start, 38ms);  // 40 packets of one millisecond
}

TEST(PacedReplyQueue, CancelRemovesAHeldInterruptReply) {
  recorder_t rec;
  queue_t queue { [&](auto wire) { return rec.send(std::move(wire)); }, slow_heartbeat() };
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 1, [] { return std::vector<std::uint8_t> { 1 }; });
  ASSERT_TRUE(rec.wait_for(1, 200ms));
  queue.schedule_interrupt(queue_t::lane::interrupt_in, 2, [] { return std::vector<std::uint8_t> { 2 }; });
  EXPECT_TRUE(queue.cancel(2));
  queue.notify_interrupt_data(queue_t::lane::interrupt_in);
  EXPECT_FALSE(rec.wait_for(2, 50ms));
}
