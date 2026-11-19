/**
 * @file tests/unit/test_ds5_mic_route.cpp
 * @brief The client microphone on the DualSense microphone endpoint: ring, clock servo, silence and idle rules.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numeric>
#include <vector>

#include "src/ds5/mic_route.h"

namespace {
  using namespace std::chrono_literals;
  using ds5::mic_route::ring_t;
  using clock_type = std::chrono::steady_clock;

  const clock_type::time_point t0 = clock_type::time_point {} + 1000s;

  std::vector<std::int16_t>
  ramp(std::size_t count, std::int32_t from) {
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) samples[i] = static_cast<std::int16_t>((from + static_cast<std::int32_t>(i)) % 20000);
    return samples;
  }
}  // namespace

TEST(Ds5MicRoute, NothingIsQueuedWhileNoApplicationRecords) {
  ring_t ring;
  const auto block = ramp(960, 0);
  EXPECT_EQ(ring.write(block.data(), block.size(), false, t0), 0u);
  EXPECT_EQ(ring.stats().frames_idle_discarded, 960u);
  EXPECT_EQ(ring.buffered_frames(), 0u);
}

TEST(Ds5MicRoute, AnswersSilenceUntilTheStartFillThenPlaysTheSamplesUnchanged) {
  ring_t ring;
  std::vector<std::int16_t> out(48, 7);
  const auto first = ramp(960, 0);
  ring.write(first.data(), first.size(), true, t0);
  ring.serve_packet(out.data(), out.size(), t0);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](auto v) { return v == 0; })) << "below the 30 ms start fill nothing plays";
  const auto second = ramp(480, 960);
  ring.write(second.data(), second.size(), true, t0);
  ring.serve_packet(out.data(), out.size(), t0 + 1ms);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[47], 47) << "inside the dead band the samples are copied, not resampled";
  ring.serve_packet(out.data(), out.size(), t0 + 2ms);
  EXPECT_EQ(out[0], 48);
  EXPECT_EQ(ring.stats().ratio, 1.0);
}

TEST(Ds5MicRoute, ARingThatRunsDryCountsOneUnderrunWhileTheRemoteSideTalksAndRecovers) {
  ring_t ring;
  std::vector<std::int16_t> out(48);
  const auto block = ramp(960, 0);
  ring.write(block.data(), block.size(), true, t0);
  ring.write(block.data(), block.size(), true, t0);
  auto now = t0;
  /* 1920 frames last 40 packets; after that the ring is dry while the remote side spoke 50 ms ago */
  for (int packet = 0; packet < 50; ++packet) ring.serve_packet(out.data(), out.size(), now += 1ms);
  EXPECT_EQ(ring.stats().underruns, 1u);
  EXPECT_GT(ring.stats().underrun_frames, 0u);
  /* the remote side is quiet for a second: silence is not an underrun */
  const auto before = ring.stats().underrun_frames;
  now += 1s;
  for (int packet = 0; packet < 10; ++packet) ring.serve_packet(out.data(), out.size(), now += 1ms);
  EXPECT_EQ(ring.stats().underrun_frames, before);
  /* and it plays again after the refill */
  ring.write(block.data(), block.size(), true, now);
  ring.write(block.data(), block.size(), true, now);
  ring.serve_packet(out.data(), out.size(), now += 1ms);
  EXPECT_EQ(out[10], 10);
}

TEST(Ds5MicRoute, AFillAboveTheHighWaterMarkIsTrimmedBackToTheTarget) {
  ring_t ring;
  std::vector<std::int16_t> out(48);
  for (int block = 0; block < 4; ++block) {
    const auto samples = ramp(960, block * 960);
    ring.write(samples.data(), samples.size(), true, t0);
  }
  ring.serve_packet(out.data(), out.size(), t0 + 1ms);
  EXPECT_EQ(ring.stats().frames_trimmed, 3840u - 960u);
  EXPECT_LE(ring.buffered_frames(), 960u);
}

TEST(Ds5MicRoute, AFullRingDropsTheRestAndSaysSo) {
  ds5::mic_route::tuning_t tuning;
  tuning.ring_frames = 1024;
  ring_t ring(tuning);
  const auto samples = ramp(1500, 0);
  EXPECT_EQ(ring.write(samples.data(), samples.size(), true, t0), 1024u);
  EXPECT_EQ(ring.stats().frames_dropped, 476u);
}

TEST(Ds5MicRoute, TwoClocksThatDifferByOneTenthOfAPercentRunTenSecondsWithoutAnUnderrun) {
  ring_t ring;
  std::vector<std::int16_t> out(48);
  /* the producer delivers 20 ms blocks at 48048 Hz (0.1 percent fast), the bus asks for 48 frames per millisecond at 48000 Hz */
  const double producer_rate = 48048.0;
  double produced = 0;
  std::uint64_t written = 0;
  auto now = t0;
  const auto block = ramp(960, 0);
  /* prime with the start fill so playback starts at once */
  for (int i = 0; i < 2; ++i) {
    ring.write(block.data(), block.size(), true, now);
    written += block.size();
  }
  for (int ms = 1; ms <= 10000; ++ms) {
    now = t0 + std::chrono::milliseconds(ms);
    produced = 1920.0 + producer_rate * ms / 1000.0;  // the blocks continue after the two written to start
    while (static_cast<double>(written) + 960.0 <= produced) {
      ring.write(block.data(), block.size(), true, now);
      written += 960;
    }
    ring.serve_packet(out.data(), out.size(), now);
  }
  const auto stats = ring.stats();
  EXPECT_EQ(stats.underruns, 0u);
  EXPECT_EQ(stats.frames_trimmed, 0u) << "the servo keeps up with 0.1 percent without discarding audio";
  EXPECT_GT(stats.fill_average, 900.0);
  EXPECT_LT(stats.fill_average, 1600.0);
  EXPECT_NEAR(stats.ratio, 1.001, 0.0005) << "it settles on the 0.1 percent the clocks differ by";
  EXPECT_LE(std::abs(stats.ratio - 1.0), 0.005 + 1e-9);
}

TEST(Ds5MicRoute, AFlushStartsTheNextStreamFromTheTargetFill) {
  ring_t ring;
  std::vector<std::int16_t> out(48);
  const auto block = ramp(960, 0);
  ring.write(block.data(), block.size(), true, t0);
  ring.serve_packet(out.data(), out.size(), t0);
  ring.flush();
  ring.serve_packet(out.data(), out.size(), t0 + 1ms);
  EXPECT_EQ(ring.buffered_frames(), 0u);
  EXPECT_TRUE(std::all_of(out.begin(), out.end(), [](auto v) { return v == 0; }));
}

TEST(Ds5MicRoute, StereoPacketsCarryTheSameSignalOnBothChannelsAndMuteGivesSilence) {
  const std::int16_t mono[3] = { 1, -2, 0x1234 };
  std::uint8_t out[12] {};
  ds5::mic_route::to_stereo(mono, 3, false, out);
  EXPECT_EQ(out[0], 0x01);
  EXPECT_EQ(out[1], 0x00);
  EXPECT_EQ(out[2], 0x01);
  EXPECT_EQ(out[4], 0xfe);
  EXPECT_EQ(out[5], 0xff);
  EXPECT_EQ(out[8], 0x34);
  EXPECT_EQ(out[9], 0x12);
  EXPECT_EQ(out[10], 0x34);
  ds5::mic_route::to_stereo(mono, 3, true, out);
  EXPECT_TRUE(std::all_of(out, out + 12, [](auto v) { return v == 0; }));
}

TEST(Ds5MicRoute, TheHookDoesNothingWithoutAnOwnerAndForwardsToTheOwner) {
  static int calls = 0;
  static std::size_t last = 0;
  ds5::mic_route::handler().store(nullptr);
  std::int16_t sample = 0;
  ds5::mic_route::feed(&sample, 1);
  EXPECT_EQ(calls, 0);
  ds5::mic_route::handler().store([](const std::int16_t *, std::size_t frames) { ++calls; last = frames; });
  ds5::mic_route::feed(&sample, 5);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(last, 5u);
  ds5::mic_route::handler().store(nullptr);
}
