/**
 * @file tests/unit/test_mic_queue.cpp
 * @brief Test the bounded microphone staging queue (Linux-only source).
 *
 * The queue is what keeps the shared mixer thread off the blocking
 * pa_simple_write(): a full queue must report backpressure, stop() must free a
 * waiting consumer, and reset() must re-arm the queue for a reinitialized sink.
 */
#include <src/platform/linux/mic_queue.h>

#include "../tests_common.h"

#ifndef _WIN32
namespace {
  using platf::mic_queue::queue_t;

  std::vector<std::int16_t>
  frame_of(std::int16_t value, std::size_t samples = 4) {
    return std::vector<std::int16_t>(samples, value);
  }
}  // namespace

TEST(MicQueue, PushPopPreservesOrderAndPayload) {
  queue_t queue { 4 };
  const auto first = frame_of(11);
  const auto second = frame_of(22);

  ASSERT_TRUE(queue.push(first.data(), first.size()));
  ASSERT_TRUE(queue.push(second.data(), second.size()));
  EXPECT_EQ(queue.size(), 2u);

  std::vector<std::int16_t> out;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, first);
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, second);
  EXPECT_EQ(queue.size(), 0u);
}

TEST(MicQueue, FullQueueReportsBackpressure) {
  queue_t queue { 2 };
  const auto frame = frame_of(7);

  ASSERT_TRUE(queue.push(frame.data(), frame.size()));
  ASSERT_TRUE(queue.push(frame.data(), frame.size()));
  EXPECT_FALSE(queue.push(frame.data(), frame.size())) << "a full queue must report the drop";
  EXPECT_EQ(queue.size(), queue.capacity());

  std::vector<std::int16_t> out;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_TRUE(queue.push(frame.data(), frame.size())) << "popping must free a slot";
}

TEST(MicQueue, EmptyFrameIsNotABackpressureCondition) {
  queue_t queue { 1 };
  EXPECT_TRUE(queue.push(nullptr, 4));
  EXPECT_TRUE(queue.push(frame_of(1).data(), 0));
  EXPECT_EQ(queue.size(), 0u);
}

TEST(MicQueue, StopWakesAParkedConsumer) {
  queue_t queue { 2 };
  std::vector<std::int16_t> out;
  std::atomic<bool> returned { true };
  std::atomic<bool> parked { false };

  std::thread consumer([&] {
    parked.store(true);
    returned.store(queue.pop(out));
  });

  while (!parked.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds { 30 });

  queue.stop();
  consumer.join();

  EXPECT_FALSE(returned.load()) << "stop() must end a consumer waiting on an empty queue";
}

TEST(MicQueue, StopDiscardsPendingFrames) {
  queue_t queue { 2 };
  const auto frame = frame_of(3);
  ASSERT_TRUE(queue.push(frame.data(), frame.size()));
  ASSERT_TRUE(queue.push(frame.data(), frame.size()));
  ASSERT_EQ(queue.size(), 2u);

  queue.stop();

  EXPECT_EQ(queue.size(), 0u) << "pending frames are discarded on stop";
  EXPECT_FALSE(queue.push(frame.data(), frame.size()));
}

TEST(MicQueue, PopWakesOnPush) {
  queue_t queue { 2 };
  std::vector<std::int16_t> out;
  std::atomic<bool> popped { false };

  std::thread consumer([&] {
    if (queue.pop(out)) {
      popped.store(true);
    }
  });

  // Give the consumer a moment to park on the condition variable, then feed it.
  std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
  const auto frame = frame_of(42);
  ASSERT_TRUE(queue.push(frame.data(), frame.size()));

  for (int i = 0; i < 100 && !popped.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
  }

  // stop() guarantees the thread can exit even if the wake-up failed, so the
  // test can never hang the suite.
  queue.stop();
  consumer.join();

  EXPECT_TRUE(popped.load());
  EXPECT_EQ(out, frame);
}

TEST(MicQueue, ResetRearmsAfterStop) {
  queue_t queue { 1 };
  queue.stop();

  const auto frame = frame_of(5);
  EXPECT_FALSE(queue.push(frame.data(), frame.size())) << "a stopped queue refuses pushes";

  queue.reset();
  EXPECT_TRUE(queue.push(frame.data(), frame.size()));

  std::vector<std::int16_t> out;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, frame);
}
#endif  // !_WIN32
