/**
 * @file tests/unit/test_encoder_snapshot.cpp
 * @brief The published encoder snapshot and the probe reuse decision (src/encoder_snapshot.h); no driver, display or GPU is touched.
 */
#include "src/encoder_snapshot.h"

#include <atomic>
#include <bitset>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
  using namespace encoder_snapshot;

  struct fake_encoder_t {
    int flags = 0;
    std::bitset<8> capabilities;
  };

  probe_fingerprint_t
  fingerprint() {
    return { "nvidia:2704:luid1:32.0.15.6", "encoder=;hevc=0;av1=0", "vdd", "ddx", "\\\\.\\DISPLAY3", "id1 2868x1320@120 sdr" };
  }
}  // namespace

// The sequence the old probe produced: it reset the shared pointer to null first and only set it again after the encoders had been tested. A capture that
// starts inside that window reads null. This replays the window deterministically with a barrier instead of waiting for the timing to line up.
TEST(EncoderSnapshot, LegacyResetWindowExposesNullAndSlotDoesNot) {
  fake_encoder_t first;
  first.flags = 2;

  std::atomic<fake_encoder_t *> legacy { &first };
  slot_t<fake_encoder_t> slot;
  slot.publish(std::make_shared<const fake_encoder_t>(first));

  std::promise<void> in_window;
  std::promise<void> leave_window;
  std::thread probe([&] {
    // legacy probe: "chosen_encoder = nullptr", tests encoders ...
    legacy.store(nullptr);
    // new probe: works on private copies, the slot is not touched until the end
    auto copy = std::make_shared<fake_encoder_t>(*slot.get());
    copy->capabilities.set();
    copy->capabilities.reset();
    in_window.set_value();
    leave_window.get_future().wait();
    legacy.store(&first);
    slot.publish(std::const_pointer_cast<const fake_encoder_t>(copy));
  });

  in_window.get_future().wait();
  EXPECT_EQ(legacy.load(), nullptr) << "the old pointer was null while a probe ran";
  ASSERT_NE(slot.get(), nullptr) << "a session starting during a probe still gets an encoder";
  EXPECT_EQ(slot.get()->flags, 2);
  leave_window.set_value();
  probe.join();
  ASSERT_NE(slot.get(), nullptr);
}

TEST(EncoderSnapshot, ReadersNeverSeeNullOrTornFlagsWhileProbesPublish) {
  slot_t<fake_encoder_t> slot;
  {
    auto initial = std::make_shared<fake_encoder_t>();
    initial->flags = 2;
    initial->capabilities.set();
    slot.publish(initial);
  }

  std::atomic<bool> stop { false };
  std::atomic<int> nulls { 0 };
  std::atomic<int> torn { 0 };
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        auto snapshot = slot.get();
        if (!snapshot) {
          ++nulls;
          continue;
        }
        // every published snapshot is either all flags set or all flags clear
        const auto count = snapshot->capabilities.count();
        if (count != 0 && count != 8) {
          ++torn;
        }
      }
    });
  }

  for (int round = 0; round < 20000; ++round) {
    auto next = std::make_shared<fake_encoder_t>();
    next->flags = 2;
    if (round % 2) {
      next->capabilities.set();
    }
    slot.publish(next);
    slot.publish(nullptr);  // a failed probe tries to reset it
  }
  stop = true;
  for (auto &reader : readers) {
    reader.join();
  }
  EXPECT_EQ(nulls.load(), 0);
  EXPECT_EQ(torn.load(), 0);
}

TEST(EncoderSnapshot, ASessionKeepsTheSnapshotItTookAcrossLaterProbes) {
  slot_t<fake_encoder_t> slot;
  auto first = std::make_shared<fake_encoder_t>();
  first->flags = 2;
  first->capabilities.set();
  slot.publish(first);

  const auto held = slot.get();
  auto second = std::make_shared<fake_encoder_t>();
  second->flags = 0;
  slot.publish(second);

  EXPECT_EQ(held->flags, 2);
  EXPECT_EQ(held->capabilities.count(), 8u);
  EXPECT_EQ(slot.get()->flags, 0);
}

TEST(EncoderSnapshot, PublishingNothingKeepsThePreviousSnapshot) {
  slot_t<fake_encoder_t> slot;
  EXPECT_EQ(slot.get(), nullptr);
  slot.publish(nullptr);
  EXPECT_EQ(slot.get(), nullptr);
  slot.publish(std::make_shared<fake_encoder_t>());
  slot.publish(nullptr);
  EXPECT_NE(slot.get(), nullptr);
}

TEST(ProbeReuse, SameFingerprintReuses) {
  EXPECT_EQ(decide_reuse(true, false, fingerprint(), fingerprint()), reuse_e::reuse);
}

TEST(ProbeReuse, EachPartOfTheFingerprintForcesAProbe) {
  const auto base = fingerprint();
  auto check = [&](auto change) {
    auto now = base;
    change(now);
    EXPECT_EQ(decide_reuse(true, false, base, now), reuse_e::changed);
  };
  check([](auto &f) { f.gpus = "nvidia:2704:luid1:32.0.15.7"; });
  check([](auto &f) { f.options = "encoder=nvenc;hevc=0;av1=0"; });
  check([](auto &f) { f.capture_backend = "ddx"; });
  check([](auto &f) { f.probe_capture = "wgc"; });
  check([](auto &f) { f.probe_display = "\\\\.\\DISPLAY4"; });
  check([](auto &f) { f.desktop = "id1 1920x1080@60 sdr"; });
  check([](auto &f) { f.desktop = "id1 2868x1320@120 hdr"; });
}

TEST(ProbeReuse, NothingChosenOrNothingRecordedProbes) {
  EXPECT_EQ(decide_reuse(false, false, fingerprint(), fingerprint()), reuse_e::no_snapshot);
  EXPECT_EQ(decide_reuse(true, true, fingerprint(), fingerprint()), reuse_e::always_reprobe);
  EXPECT_EQ(decide_reuse(true, false, std::nullopt, fingerprint()), reuse_e::no_record);
}

TEST(ProbeReuse, AnUnknownGpuIsNeverProvenUnchanged) {
  auto unknown = fingerprint();
  unknown.gpus.clear();
  EXPECT_EQ(decide_reuse(true, false, unknown, unknown), reuse_e::unknown_gpu);
}

TEST(ProbeReuse, MemoRemembersAndForgets) {
  memo_t memo;
  EXPECT_FALSE(memo.last().has_value());
  memo.remember(fingerprint());
  ASSERT_TRUE(memo.last().has_value());
  EXPECT_EQ(*memo.last(), fingerprint());
  memo.forget();
  EXPECT_FALSE(memo.last().has_value());
}
