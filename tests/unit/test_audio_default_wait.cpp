/**
 * @file tests/unit/test_audio_default_wait.cpp
 * @brief What the host does while the system has no default playback device: one line per reason and session, a wait that a new
 *        device ends at once, and the sink information read when the device is there. No audio API is touched.
 */
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/audio_default_wait.h"

namespace {
  using namespace std::chrono_literals;
  using namespace audio_default_wait;

  // A control of a computer that gets its playback device after a number of looks.
  struct fake_control_t {
    int looks_until_device = 0;
    int looks = 0;
    int waits = 0;
    bool sink_info_fails_once = false;
    std::vector<std::chrono::milliseconds> wait_times;

    bool
    default_output_missing() {
      return ++looks <= looks_until_device;
    }

    std::optional<std::string>
    sink_info() {
      if (sink_info_fails_once) {
        sink_info_fails_once = false;
        return std::nullopt;
      }
      return "扬声器 (USB Audio)";
    }

    bool
    wait_for_default_output(std::chrono::milliseconds timeout) {
      ++waits;
      wait_times.push_back(timeout);
      return false;
    }
  };
}  // namespace

TEST(AudioDefaultWaitOnce, EachReasonIsSaidOncePerSession) {
  once_per_session_t notice;
  EXPECT_TRUE(notice.first(reason::no_default));
  EXPECT_FALSE(notice.first(reason::no_default));
  EXPECT_FALSE(notice.first(reason::no_default));
  // another reason is its own line
  EXPECT_TRUE(notice.first(reason::service));
  EXPECT_FALSE(notice.first(reason::service));
}

TEST(AudioDefaultWaitOnce, ANewSessionSaysItAgain) {
  once_per_session_t notice;
  EXPECT_TRUE(notice.first(reason::no_default));
  notice.reset();
  EXPECT_TRUE(notice.first(reason::no_default));
  EXPECT_FALSE(notice.first(reason::no_default));
}

TEST(AudioDefaultWaitOnce, ManyThreadsAskingStillSayItOnce) {
  once_per_session_t notice;
  std::atomic<int> said = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      for (int n = 0; n < 200; ++n) {
        if (notice.first(reason::no_default)) {
          ++said;
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(said.load(), 1);
}

TEST(AudioDefaultWaitGate, ASignalBeforeTheWaitIsKept) {
  gate_t gate;
  gate.signal();
  EXPECT_TRUE(gate.wait_for(0ms));
  // consumed: the next wait runs into the timeout
  EXPECT_FALSE(gate.wait_for(5ms));
}

TEST(AudioDefaultWaitGate, ASignalFromAnotherThreadEndsTheWaitEarly) {
  gate_t gate;
  const auto start = std::chrono::steady_clock::now();
  std::thread notifier([&] {
    std::this_thread::sleep_for(20ms);
    gate.signal();
  });
  EXPECT_TRUE(gate.wait_for(5s));
  notifier.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
}

TEST(AudioDefaultWaitUntilPresent, ReturnsAtOnceWhenADeviceIsThere) {
  int waited = 0;
  const bool arrived = wait_until_present([] { return true; }, [] { return false; }, [&](std::chrono::milliseconds) { ++waited; });
  EXPECT_TRUE(arrived);
  EXPECT_EQ(waited, 0);
}

TEST(AudioDefaultWaitUntilPresent, WaitsUntilTheDeviceAppears) {
  int looks = 0;
  int waited = 0;
  const bool arrived = wait_until_present([&] { return ++looks > 3; }, [] { return false; }, [&](std::chrono::milliseconds) { ++waited; });
  EXPECT_TRUE(arrived);
  EXPECT_EQ(waited, 3);
}

TEST(AudioDefaultWaitUntilPresent, TheEndOfTheStreamEndsTheWait) {
  int waited = 0;
  const bool arrived = wait_until_present([] { return false; }, [&] { return waited >= 2; }, [&](std::chrono::milliseconds) { ++waited; });
  EXPECT_FALSE(arrived);
  EXPECT_EQ(waited, 2);
}

TEST(AudioDefaultWaitUntilPresent, ADeviceThatArrivesWithTheNotificationStartsAtOnce) {
  std::atomic_bool device = false;
  gate_t gate;
  std::thread notifier([&] {
    std::this_thread::sleep_for(30ms);
    device = true;
    gate.signal();
  });
  const auto start = std::chrono::steady_clock::now();
  // the poll is an hour: only the notification can end the wait in time
  const bool arrived = wait_until_present([&] { return device.load(); }, [] { return false; }, [&](std::chrono::milliseconds poll) { gate.wait_for(poll); }, 1h);
  notifier.join();
  EXPECT_TRUE(arrived);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(AudioDefaultWaitSink, SinkInformationIsReadWhenTheDeviceAppears) {
  fake_control_t control;
  control.looks_until_device = 2;
  const auto sink = wait_for_sink(control, [] { return false; }, 7ms);
  ASSERT_TRUE(sink.has_value());
  EXPECT_EQ(*sink, "扬声器 (USB Audio)");
  EXPECT_EQ(control.waits, 2);
  // every wait is the poll interval: the caller looks again, a notification only makes it earlier
  for (const auto time : control.wait_times) {
    EXPECT_EQ(time, 7ms);
  }
}

TEST(AudioDefaultWaitSink, ASinkReadThatFailsRightAfterTheDeviceAppearedIsTriedAgain) {
  fake_control_t control;
  control.sink_info_fails_once = true;
  const auto sink = wait_for_sink(control, [] { return false; }, 1ms);
  EXPECT_TRUE(sink.has_value());
  EXPECT_EQ(control.waits, 1);
}

TEST(AudioDefaultWaitSink, NoDeviceAndTheStreamEndsGivesNothing) {
  fake_control_t control;
  control.looks_until_device = 1000000;
  int checks = 0;
  const auto sink = wait_for_sink(control, [&] { return ++checks > 3; }, 1ms);
  EXPECT_FALSE(sink.has_value());
}

TEST(AudioDefaultWaitClassify, WhyThereIsNoDefault) {
  // nothing is plugged in or installed
  EXPECT_EQ(classify(0, 0, true), reason::no_endpoint);
  // devices exist but are disabled or unplugged
  EXPECT_EQ(classify(2, 0, true), reason::none_active);
  // active devices, none is the default
  EXPECT_EQ(classify(2, 1, true), reason::no_default);
  // the audio service did not answer: the lists mean nothing
  EXPECT_EQ(classify(0, 0, false), reason::service);
  EXPECT_EQ(classify(3, 2, false), reason::service);
}
