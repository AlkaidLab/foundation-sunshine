#include "src/streaming/fec.h"
#include "src/streaming/measured_bitrate.h"
#include "src/streaming/video_pacing.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST(VideoPacing, UsesMeasuredRateAndHeadroom) {
  EXPECT_EQ(streaming::video_pacing_rate_bps(0, 10'000), 8'000'000u);
  EXPECT_EQ(streaming::video_pacing_rate_bps(20'000, 10'000), 20'000'000u);
  EXPECT_EQ(streaming::video_pacing_rate_bps(5'000, 10'000), 8'000'000u);
  EXPECT_EQ(streaming::video_pacing_rate_bps(0, 0), 0u);
}

TEST(VideoPacing, OnlyLargeFramesArePaced) {
  EXPECT_FALSE(streaming::should_pace_video_frame(false, 65'535, 65'536));
  EXPECT_TRUE(streaming::should_pace_video_frame(false, 65'536, 65'536));
  EXPECT_TRUE(streaming::should_pace_video_frame(true, 65'536, 65'536));
  EXPECT_FALSE(streaming::should_pace_video_frame(true, 10, 65'536));
  EXPECT_FALSE(streaming::should_pace_video_frame(true, 100, 0));
}

TEST(VideoPacing, SlotsAccumulateAtConfiguredRate) {
  constexpr auto rate = 8'000'000u;
  constexpr auto packet_bytes = 1'000u;
  EXPECT_EQ(streaming::video_packet_slot(0, packet_bytes, rate).count(), 0);
  EXPECT_NEAR(streaming::video_packet_slot(1, packet_bytes, rate).count(), 1'000'000, 1);
  EXPECT_NEAR(streaming::video_packet_slot(8, packet_bytes, rate).count(), 8'000'000, 1);
  EXPECT_EQ(streaming::video_packet_slot(1, packet_bytes, 0).count(), 0);
}

TEST(VideoPacing, FrameRateFitsTheShareOfTheFrameInterval) {
  constexpr auto interval = 16'666'667ns;
  // 200 kB on the wire in half of 16.67 ms: 192 Mbps, far above the sustained 64 Mbps.
  const auto rate = streaming::video_pacing_frame_rate_bps(64'000'000, 200'000, interval);
  EXPECT_NEAR(static_cast<double>(rate), 192'000'000.0, 10.0);
  const auto duration = streaming::video_packet_slot(200'000 / 1'000, 1'000, rate);
  EXPECT_LE(duration.count(), interval.count() / 2 + 1'000);
  // A small frame keeps the sustained rate.
  EXPECT_NEAR(static_cast<double>(streaming::video_pacing_frame_rate_bps(64'000'000, 70'000, interval)), 67'200'000.0, 10.0);
  EXPECT_EQ(streaming::video_pacing_frame_rate_bps(64'000'000, 50'000, interval), 64'000'000u);
  // Nothing to pace with.
  EXPECT_EQ(streaming::video_pacing_frame_rate_bps(0, 200'000, interval), 0u);
  EXPECT_EQ(streaming::video_pacing_frame_rate_bps(64'000'000, 200'000, 0ns), 0u);
}

TEST(VideoPacing, FrameIntervalFollowsTheFrameRate) {
  EXPECT_EQ(streaming::video_frame_interval(60.0).count(), 16'666'666);
  EXPECT_EQ(streaming::video_frame_interval(120.0).count(), 8'333'333);
  EXPECT_NEAR(static_cast<double>(streaming::video_frame_interval(59.94).count()), 16'683'350.0, 10.0);
  EXPECT_EQ(streaming::video_frame_interval(0.0).count(), 0);
  EXPECT_EQ(streaming::video_frame_interval(-30.0).count(), 0);
  EXPECT_EQ(streaming::video_frame_interval(std::numeric_limits<double>::quiet_NaN()).count(), 0);
}

TEST(VideoPacing, WireBytesFollowThePlannedLayout) {
  video_fec::frame_t layout;
  layout.count = 2;
  layout.blocks[0].data = 47;
  layout.blocks[0].parity = 100;  // a client minimum far above the 20 % that 47 packets would plan
  layout.blocks[1].data = 10;
  layout.blocks[1].parity = 3;
  EXPECT_EQ(streaming::video_pacing_wire_bytes(layout, 1'416), (47u + 100u + 10u + 3u) * 1'416u);
  EXPECT_EQ(streaming::video_pacing_wire_bytes(video_fec::frame_t {}, 1'416), 0u);

  // Sized from that layout the frame still ends inside its share of the interval, which a 20 % estimate would not.
  constexpr auto interval = 16'666'667ns;
  video_fec::frame_t single;
  single.count = 1;
  single.blocks[0].data = 47;
  single.blocks[0].parity = 100;
  const auto wire = streaming::video_pacing_wire_bytes(single, 1'416);
  const auto rate = streaming::video_pacing_frame_rate_bps(64'000'000, wire, interval);
  EXPECT_LE(streaming::video_packet_slot(147, 1'416, rate).count(), interval.count() / 2 + 1'000);
  const auto estimate = streaming::video_pacing_frame_rate_bps(64'000'000, 47u * 1'416u * 120u / 100u, interval);
  EXPECT_GT(streaming::video_packet_slot(147, 1'416, estimate).count(), interval.count() / 2);
}

TEST(VideoPacing, AFrameBehindAnotherIsNotPaced) {
  streaming::video_pacer_t pacer { streaming::video_pacer_t::clock::time_point { 1s } };
  const auto now = streaming::video_pacer_t::clock::time_point { 2s };
  const auto behind = pacer.begin_frame(now, true, true, 64'000'000, 200'000, 16'666'667ns);
  EXPECT_FALSE(behind.paced);
  EXPECT_EQ(behind.start, now);
  const auto alone = pacer.begin_frame(now, false, true, 64'000'000, 200'000, 16'666'667ns);
  EXPECT_TRUE(alone.paced);
  const auto small = pacer.begin_frame(now, false, false, 64'000'000, 200'000, 16'666'667ns);
  EXPECT_FALSE(small.paced);
}

TEST(VideoPacing, DebtNeverReachesPastOneFrameInterval) {
  constexpr auto interval = 16'666'667ns;
  const auto t0 = streaming::video_pacer_t::clock::time_point { 5s };
  streaming::video_pacer_t pacer { t0 };
  // No interval can be honoured with these inputs, which is what the clamp is for.
  streaming::video_pacer_t::frame_t frame;
  frame.paced = true;
  frame.start = t0;
  frame.rate_bps = 1'000'000;
  frame.interval = interval;
  pacer.end_frame(frame, 1'000, 1'400, t0);
  EXPECT_EQ(pacer.next_frame_start(), t0 + interval);
}

// ---------------------------------------------------------------------------------------------------------------------
// A deterministic model of the video send thread: frames reach the thread in order, the thread sends one frame at a time,
// a batch is submitted at its due time, and the packet queue holds 32 frames before safe::queue_t::raise() clears it.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
  using sim_clock_t = std::chrono::steady_clock;
  using time_point_t = sim_clock_t::time_point;

  constexpr int fps = 60;
  constexpr int configured_kbps = 80'000;
  constexpr std::size_t blocksize = 1416;
  constexpr std::size_t batch_packets = 64 * 1024 / blocksize;
  constexpr std::size_t queue_limit = 32;
  constexpr unsigned fec_percent = 20;
  constexpr std::size_t threshold_bytes = 64 * 1024;
  constexpr auto frame_interval = std::chrono::nanoseconds { 1'000'000'000 / fps };
  constexpr double average_frame_bytes = configured_kbps * 1000.0 / 8.0 / fps;

  struct stats_t {
    std::size_t frames = 0;
    std::size_t sent = 0;
    std::size_t dropped = 0;
    std::size_t flushes = 0;
    std::size_t max_waiting = 0;
    double max_latency_ms = 0;
    double p99_latency_ms = 0;
    std::size_t late_frames = 0;  ///< frames that left more than one frame interval after they were encoded
  };

  /// What one model does with a frame: the time a batch is due, and the end-of-frame bookkeeping.
  struct model_t {
    std::string name;
    std::function<void(time_point_t now, bool waiting, std::size_t wire_bytes, std::size_t payload_bytes, int measured_kbps)> begin;
    std::function<time_point_t(std::size_t packets_sent)> due;
    std::function<void(std::size_t packets_sent, time_point_t now)> end;
  };

  // Before #1246: every frame leaves at 80 % of 1 Gbit/s in 1 ms groups, which is a line-rate burst for this bitrate.
  model_t
  model_before_1246() {
    struct state_t {
      time_point_t next_start {};
      time_point_t start {};
    };
    auto state = std::make_shared<state_t>();
    constexpr std::uint64_t rate = 800'000'000;
    return {
      "before #1246 (800 Mbit/s)",
      [state](time_point_t now, bool, std::size_t, std::size_t, int) { state->start = std::max(state->next_start, now); },
      [state](std::size_t sent) { return state->start + streaming::video_packet_slot(sent, blocksize, rate); },
      [state](std::size_t sent, time_point_t) { state->next_start = state->start + streaming::video_packet_slot(sent, blocksize, rate); },
    };
  }

  // #1246 as shipped (commit 2286b3f5): 80 % of the configured rate, frames of 64 KiB and up, the schedule carried over.
  model_t
  model_1246_as_shipped() {
    struct state_t {
      time_point_t next_start {};
      time_point_t start {};
      std::uint64_t rate = 0;
      bool pace = false;
    };
    auto state = std::make_shared<state_t>();
    return {
      "#1246 as shipped",
      [state](time_point_t now, bool, std::size_t, std::size_t payload, int measured) {
        state->rate = streaming::video_pacing_rate_bps(measured, configured_kbps);
        state->pace = streaming::should_pace_video_frame(false, payload, threshold_bytes);
        state->start = state->pace ? std::max(state->next_start, now) : now;
      },
      [state](std::size_t sent) { return state->pace ? state->start + streaming::video_packet_slot(sent, blocksize, state->rate) : state->start; },
      [state](std::size_t sent, time_point_t now) {
        state->next_start = state->pace ? state->start + streaming::video_packet_slot(sent, blocksize, state->rate) : now;
      },
    };
  }

  model_t
  model_fixed() {
    auto pacer = std::make_shared<streaming::video_pacer_t>(time_point_t {});
    auto frame = std::make_shared<streaming::video_pacer_t::frame_t>();
    return {
      "frame-interval pacer",
      [pacer, frame](time_point_t now, bool waiting, std::size_t wire, std::size_t payload, int measured) {
        *frame = pacer->begin_frame(now, waiting, streaming::should_pace_video_frame(false, payload, threshold_bytes),
          streaming::video_pacing_rate_bps(measured, configured_kbps), wire, frame_interval);
      },
      [pacer, frame](std::size_t sent) { return frame->paced ? pacer->due(*frame, sent, blocksize) : frame->start; },
      [pacer, frame](std::size_t sent, time_point_t now) { pacer->end_frame(*frame, sent, blocksize, now); },
    };
  }

  stats_t
  simulate(const std::vector<double> &frame_scale, model_t model) {
    const time_point_t t0 { 100s };
    std::vector<time_point_t> arrival;
    std::vector<std::size_t> payload;
    for (std::size_t i = 0; i < frame_scale.size(); ++i) {
      arrival.push_back(t0 + std::chrono::duration_cast<std::chrono::nanoseconds>(frame_interval * static_cast<long long>(i)));
      payload.push_back(static_cast<std::size_t>(average_frame_bytes * frame_scale[i]));
    }

    streaming::measured_bitrate_window_t window;
    const auto ms_of = [](time_point_t t) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count()); };

    stats_t stats;
    stats.frames = frame_scale.size();
    std::deque<std::size_t> waiting;
    std::size_t next = 0;
    time_point_t clock_now = t0;
    std::vector<double> latencies;

    const auto admit = [&](time_point_t upto) {
      while (next < arrival.size() && arrival[next] <= upto) {
        if (waiting.size() >= queue_limit) {
          ++stats.flushes;
          stats.dropped += waiting.size();
          waiting.clear();
        }
        waiting.push_back(next++);
      }
    };

    while (next < arrival.size() || !waiting.empty()) {
      if (waiting.empty()) clock_now = std::max(clock_now, arrival[next]);
      admit(clock_now);
      if (waiting.empty()) continue;
      stats.max_waiting = std::max(stats.max_waiting, waiting.size());
      const auto index = waiting.front();
      waiting.pop_front();

      const auto wire_bytes = payload[index] * (100 + fec_percent) / 100;
      const auto packets = (wire_bytes + blocksize - 1) / blocksize;
      model.begin(clock_now, !waiting.empty(), wire_bytes, payload[index], window.kbps(ms_of(clock_now)));

      std::size_t sent = 0;
      while (sent < packets) {
        const auto batch = std::min(batch_packets, packets - sent);
        const auto due = model.due(sent);
        if (clock_now < due) clock_now = due;
        admit(clock_now);
        window.add_bytes(batch * blocksize, ms_of(clock_now));
        sent += batch;
      }
      model.end(sent, clock_now);

      ++stats.sent;
      const auto latency_ms = std::chrono::duration<double, std::milli>(clock_now - arrival[index]).count();
      latencies.push_back(latency_ms);
      stats.max_latency_ms = std::max(stats.max_latency_ms, latency_ms);
      const auto interval_ms = std::chrono::duration<double, std::milli>(frame_interval).count();
      if (latency_ms > interval_ms) ++stats.late_frames;
    }

    std::sort(latencies.begin(), latencies.end());
    if (!latencies.empty()) stats.p99_latency_ms = latencies[std::min(latencies.size() - 1, static_cast<std::size_t>(latencies.size() * 0.99))];
    return stats;
  }

  std::vector<double>
  steady(std::size_t frames, double scale) {
    return std::vector<double>(frames, scale);
  }

  /// One second at the base size, one second rising to three times it, half a second held, one second back: turning the camera.
  std::vector<double>
  camera_turn(double base) {
    std::vector<double> trace;
    for (int i = 0; i < fps; ++i) trace.push_back(base);
    for (int i = 0; i < fps; ++i) trace.push_back(base * (1.0 + 2.0 * (i + 1) / fps));
    for (int i = 0; i < fps / 2; ++i) trace.push_back(base * 3.0);
    for (int i = 0; i < fps; ++i) trace.push_back(base);
    return trace;
  }

  void
  print_row(const char *scenario, const model_t &model, const stats_t &s) {
    std::printf("  %-26s %-26s waiting<=%2zu  latency max %7.1f ms p99 %7.1f ms  late %3zu  flushes %zu (dropped %zu)\n",
      scenario, model.name.c_str(), s.max_waiting, s.max_latency_ms, s.p99_latency_ms, s.late_frames, s.flushes, s.dropped);
  }
}  // namespace

TEST(VideoPacingSimulation, CameraTurnAt60FpsAnd80Mbps) {
  struct scenario_t {
    const char *name;
    std::vector<double> trace;
  };
  const std::vector<scenario_t> scenarios {
    { "steady, full budget", steady(240, 1.0) },
    { "steady, 60 % of budget", steady(240, 0.6) },
    { "camera turn from 1x", camera_turn(1.0) },
    { "camera turn from 0.7x", camera_turn(0.7) },
  };

  std::printf("\n  60 fps, %d kbit/s, %zu-byte packets, %u %% parity, frame interval %.2f ms\n", configured_kbps, blocksize, fec_percent,
    std::chrono::duration<double, std::milli>(frame_interval).count());
  stats_t before[4], shipped[4], fixed[4];
  for (std::size_t i = 0; i < scenarios.size(); ++i) {
    const auto old = model_before_1246();
    const auto bad = model_1246_as_shipped();
    const auto good = model_fixed();
    before[i] = simulate(scenarios[i].trace, model_before_1246());
    shipped[i] = simulate(scenarios[i].trace, model_1246_as_shipped());
    fixed[i] = simulate(scenarios[i].trace, model_fixed());
    print_row(scenarios[i].name, old, before[i]);
    print_row(scenarios[i].name, bad, shipped[i]);
    print_row(scenarios[i].name, good, fixed[i]);
  }

  const auto interval_ms = std::chrono::duration<double, std::milli>(frame_interval).count();

  // #1246 as shipped: a stream that uses its whole budget falls behind the capture clock and never catches up; the
  // standing delay grows until the packet queue is full and is cleared as a whole.
  EXPECT_GT(shipped[0].max_latency_ms, 10 * interval_ms);
  EXPECT_GT(shipped[0].flushes, 0u);
  EXPECT_GT(shipped[2].max_latency_ms, 10 * interval_ms);
  EXPECT_GT(shipped[2].flushes, 0u);
  EXPECT_GT(shipped[2].dropped, 0u);
  // A stream that stays well under budget is not hurt, which is why the problem hides on a desktop and shows in a game.
  EXPECT_LT(shipped[1].max_latency_ms, interval_ms);

  // The frame-interval pacer holds every frame inside its share of the interval and never queues or drops.
  for (const auto &s : fixed) {
    EXPECT_LE(s.max_latency_ms, interval_ms * 0.5 + 0.5);
    EXPECT_EQ(s.flushes, 0u);
    EXPECT_EQ(s.dropped, 0u);
    EXPECT_EQ(s.late_frames, 0u);
    EXPECT_LE(s.max_waiting, 1u);
  }
}
