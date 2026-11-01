#include "src/streaming/measured_bitrate.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

namespace {
  int
  rate_for(std::uint64_t bytes) {
    return static_cast<int>(bytes * 8 / streaming::measured_bitrate_window_t::window_ms);
  }
}  // namespace

TEST(MeasuredBitrateWindow, EmptyWindowIsZero) {
  streaming::measured_bitrate_window_t window;
  EXPECT_EQ(window.kbps(0), 0);
  EXPECT_EQ(window.kbps(1'500), 0);
  EXPECT_EQ(window.kbps(10'000'000), 0);
  window.add_bytes(0, 1'500);
  EXPECT_EQ(window.kbps(1'500), 0);
}

TEST(MeasuredBitrateWindow, CountsBytesOverAFixedTwoSecondWindow) {
  streaming::measured_bitrate_window_t window;
  constexpr std::uint64_t bytes = 500'000;
  constexpr std::uint64_t at = 5'000;
  window.add_bytes(bytes, at);

  EXPECT_EQ(window.kbps(at), rate_for(bytes));
  EXPECT_EQ(window.kbps(at + streaming::measured_bitrate_window_t::window_ms - 1), rate_for(bytes));
  EXPECT_EQ(rate_for(bytes), 2'000);
}

TEST(MeasuredBitrateWindow, DropsBytesOnceTheWindowExpires) {
  streaming::measured_bitrate_window_t window;
  constexpr std::uint64_t at = 5'000;
  window.add_bytes(500'000, at);

  EXPECT_EQ(window.kbps(at + streaming::measured_bitrate_window_t::window_ms), 0);
  EXPECT_EQ(window.kbps(at + 60'000), 0);
}

TEST(MeasuredBitrateWindow, KeepsOnlyTheBucketsStillInsideTheWindow) {
  streaming::measured_bitrate_window_t window;
  constexpr std::uint64_t older = 100'000;
  constexpr std::uint64_t newer = 100'000;
  window.add_bytes(older, 0);
  window.add_bytes(newer, 1'500);

  EXPECT_EQ(window.kbps(1'999), rate_for(older + newer));
  EXPECT_EQ(window.kbps(2'000), rate_for(newer));
  EXPECT_EQ(window.kbps(1'500 + streaming::measured_bitrate_window_t::window_ms), 0);
}

TEST(MeasuredBitrateWindow, SumsBytesInTheSameBucket) {
  streaming::measured_bitrate_window_t window;
  window.add_bytes(125'000, 8'050);
  window.add_bytes(125'000, 8'099);

  EXPECT_EQ(window.kbps(8'099), rate_for(250'000));
  EXPECT_EQ(rate_for(250'000), 1'000);
}

TEST(MeasuredBitrateWindow, RecycledSlotDoesNotKeepThePreviousEpoch) {
  streaming::measured_bitrate_window_t window;
  constexpr auto span = static_cast<std::uint64_t>(streaming::measured_bitrate_window_t::slot_count) *
                        streaming::measured_bitrate_window_t::bucket_ms;
  window.add_bytes(500'000, 0);
  window.add_bytes(250, span);

  EXPECT_EQ(window.kbps(span), rate_for(250));
  EXPECT_EQ(window.kbps(0), 0);
}

TEST(MeasuredBitrateWindow, SaturatesABucketInsteadOfWrapping) {
  streaming::measured_bitrate_window_t window;
  const auto too_many = static_cast<std::uint64_t>(streaming::measured_bitrate_window_t::max_bucket_bytes) + 1000;
  window.add_bytes(too_many, 4'000);
  window.add_bytes(too_many, 4'010);

  EXPECT_EQ(window.kbps(4'010), rate_for(streaming::measured_bitrate_window_t::max_bucket_bytes));
}

TEST(MeasuredBitrateWindow, IgnoresASampleFromTheFuture) {
  streaming::measured_bitrate_window_t window;
  window.add_bytes(500'000, 5'000);

  EXPECT_EQ(window.kbps(1'000), 0);
  EXPECT_EQ(window.kbps(5'000), rate_for(500'000));
}

TEST(MeasuredBitrateWindow, ConcurrentAddsAreNotLost) {
  streaming::measured_bitrate_window_t window;
  constexpr int threads = 8;
  constexpr int adds = 20'000;
  constexpr std::uint64_t chunk = 25;
  constexpr std::uint64_t at = 12'345;
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&window] {
      for (int n = 0; n < adds; ++n) {
        window.add_bytes(chunk, at);
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }

  const auto total = static_cast<std::uint64_t>(threads) * adds * chunk;
  EXPECT_EQ(window.kbps(at), rate_for(total));
  EXPECT_EQ(rate_for(total), 16'000);
}

TEST(MeasuredBitrateWindow, RegistryReturnsZeroAfterUnpublish) {
  streaming::measured_bitrate_window_t first;
  streaming::measured_bitrate_window_t second;
  first.add_bytes(500'000, 5'000);
  second.add_bytes(250'000, 5'000);

  streaming::measured_bitrate_handle_t first_handle;
  streaming::measured_bitrate_handle_t second_handle;
  first_handle.publish(7, &first);
  second_handle.publish(8, &second);

  EXPECT_EQ(streaming::measured_bitrate_kbps(7, 5'000), rate_for(500'000));
  EXPECT_EQ(streaming::measured_bitrate_kbps(8, 5'000), rate_for(250'000));
  EXPECT_EQ(streaming::measured_bitrate_kbps(9, 5'000), 0);
  EXPECT_EQ(streaming::measured_bitrate_kbps(8, 7'000), 0);

  first_handle.reset();
  EXPECT_EQ(streaming::measured_bitrate_kbps(7, 5'000), 0);
  EXPECT_EQ(streaming::measured_bitrate_kbps(8, 5'000), rate_for(250'000));
  EXPECT_EQ(first.kbps(5'000), rate_for(500'000));

  {
    streaming::measured_bitrate_handle_t replacement;
    streaming::measured_bitrate_window_t replaced;
    replaced.add_bytes(125'000, 5'000);
    replacement.publish(8, &replaced);
    EXPECT_EQ(streaming::measured_bitrate_kbps(8, 5'000), rate_for(125'000));
    // The older handle must not remove a window a newer publish installed.
    second_handle.reset();
    EXPECT_EQ(streaming::measured_bitrate_kbps(8, 5'000), rate_for(125'000));
  }
  EXPECT_EQ(streaming::measured_bitrate_kbps(8, 5'000), 0);
}
