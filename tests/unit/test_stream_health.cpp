#include "src/streaming/measured_bitrate.h"
#include "src/streaming/video_pacing.h"
#include "src/telemetry/self_baseline.h"
#include "src/telemetry/stream_health.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <numeric>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace telemetry;
using namespace telemetry::health;
using hooks::counter_e;
using hooks::metric_e;

namespace {
  dims_t
  dims60() {
    dims_t d;
    d.codec = "hevc";
    d.width = 2560;
    d.height = 1440;
    d.fps = 60;
    d.client_kind = "moonlight-macos-enhanced";
    d.client_os = "macos";
    return d;
  }

  /// A window of `frames` frames, each with the given timings, then close it 60 s after the start.
  std::optional<window_t>
  one_window(stream_health_t &h, std::int64_t &now, int frames, double host, double backlog, std::function<void()> extra = {}) {
    for (int i = 0; i < frames; ++i) {
      h.record(metric_e::host_ms, host);
      h.record(metric_e::backlog_ms, backlog);
      h.record(metric_e::encode_ms, 3);
      h.count(counter_e::frames_sent);
      h.count(counter_e::captured);
      h.count(counter_e::submitted);
    }
    if (extra) extra();
    now += WINDOW_MS;
    return h.tick(now, {});
  }
}  // namespace

TEST(StreamHealthBuckets, BoundsAreInclusiveAndTheLastBucketCatchesTheRest) {
  EXPECT_EQ(bucket_of(0), 0u);
  EXPECT_EQ(bucket_of(1), 0u);
  EXPECT_EQ(bucket_of(1.01), 1u);
  EXPECT_EQ(bucket_of(16), 5u);
  EXPECT_EQ(bucket_of(250), 13u);
  EXPECT_EQ(bucket_of(2000), 17u);
  EXPECT_EQ(bucket_of(2000.5), 18u);
  EXPECT_EQ(BUCKET_COUNT, 19u);
}

TEST(StreamHealthWindow, ClosesOnlyAfterSixtySecondsAndReportsPercentiles) {
  std::int64_t now = 1'000'000;
  stream_health_t h { now, dims60() };
  for (int i = 1; i <= 100; ++i) {
    h.record(metric_e::host_ms, i);  // 1..100 ms
    h.count(counter_e::frames_sent);
  }
  EXPECT_FALSE(h.tick(now + 59'000, {}).has_value());
  const auto w = h.tick(now + 60'000, {});
  ASSERT_TRUE(w.has_value());
  const auto &host = w->metrics[static_cast<std::size_t>(metric_e::host_ms)];
  EXPECT_EQ(host.n, 100u);
  EXPECT_NEAR(host.p50, 51, 1.5);
  EXPECT_NEAR(host.p95, 96, 1.5);
  EXPECT_DOUBLE_EQ(host.max, 100);
  EXPECT_FALSE(w->idle);
  EXPECT_NEAR(w->seconds, 60, 0.01);
}

TEST(StreamHealthWindow, ANearlyIdleWindowIsNotClassifiedAndNotCounted) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  const auto w = one_window(h, now, 3, 900, 900);
  ASSERT_TRUE(w.has_value());
  EXPECT_TRUE(w->idle);
  EXPECT_TRUE(w->anomalies.empty());
  const auto chunk = h.take_chunk(now);
  EXPECT_EQ(chunk["windows"], 0);
  EXPECT_EQ(chunk["idle_windows"], 1);
}

TEST(StreamHealthAnomaly, ASevereBacklogAndAQueueFlushAreReportedWithTheirNumbers) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  const auto w = one_window(h, now, 3000, 40, 560, [&] {
    h.count(counter_e::queue_flushes, 1);
    h.count(counter_e::queue_frames_dropped, 32);
  });
  ASSERT_TRUE(w.has_value());
  ASSERT_EQ(w->anomalies.size(), 2u);
  const auto find = [&](std::string_view c) -> const anomaly_t * {
    for (const auto &a : w->anomalies)
      if (a.code == c) return &a;
    return nullptr;
  };
  const auto *flush = find(code::queue_flush);
  ASSERT_NE(flush, nullptr);
  EXPECT_TRUE(flush->severe);
  EXPECT_EQ(flush->count, 32u);
  const auto *backlog = find(code::send_backlog);
  ASSERT_NE(backlog, nullptr);
  EXPECT_TRUE(backlog->severe);
  EXPECT_DOUBLE_EQ(backlog->value, 560);
  EXPECT_DOUBLE_EQ(backlog->threshold, 250);
}

TEST(StreamHealthAnomaly, AHealthyStreamRaisesNothing) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int minute = 0; minute < 5; ++minute) {
    const auto w = one_window(h, now, 3600, 12, 4);
    ASSERT_TRUE(w.has_value());
    EXPECT_TRUE(w->anomalies.empty()) << "minute " << minute;
  }
}

TEST(StreamHealthAnomaly, TheSameCodeIsHeldBackByTheCooldownAndCountedIntoTheNextReport) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  const auto flush = [&] { h.count(counter_e::queue_flushes, 1); h.count(counter_e::queue_frames_dropped, 32); };
  const auto first = one_window(h, now, 3000, 30, 30, flush);
  ASSERT_EQ(first->anomalies.size(), 1u);
  EXPECT_EQ(first->anomalies[0].suppressed, 0u);
  const auto second = one_window(h, now, 3000, 30, 30, flush);  // 60 s later: inside the 120 s severe cooldown
  EXPECT_TRUE(second->anomalies.empty());
  const auto third = one_window(h, now, 3000, 30, 30, flush);  // 120 s after the first
  ASSERT_EQ(third->anomalies.size(), 1u);
  EXPECT_EQ(third->anomalies[0].suppressed, 1u);
}

TEST(StreamHealthAnomaly, IdrStormEncodeOverAndTheNetworkCodes) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 3000; ++i) {
    h.record(metric_e::encode_ms, 25);  // over the 16.7 ms frame interval
    h.count(counter_e::frames_sent);
    h.count(counter_e::captured);
    h.count(counter_e::submitted);
  }
  h.count(counter_e::idr_requests, 4);
  h.count(counter_e::rfi_requests, 3);
  now += WINDOW_MS;
  probe_t probe;
  probe.loss_pct = 7;
  probe.rtt_ms = 200;
  probe.target_kbps = 80'000;
  probe.measured_kbps = 20'000;
  const auto w = h.tick(now, probe);
  std::vector<std::string> codes;
  for (const auto &a : w->anomalies) codes.push_back(a.code);
  std::sort(codes.begin(), codes.end());
  EXPECT_EQ(codes, (std::vector<std::string> { "XP_ENCODE_OVER", "XP_IDR_STORM", "XP_LOSS_BURST", "XP_RTT_HIGH" }));  // 20 000 of a configured 80 000 kbps is not an anomaly
  for (const auto &a : w->anomalies) EXPECT_EQ(is_network_code(a.code), a.code == "XP_BITRATE_SHORT" || a.code == "XP_LOSS_BURST" || a.code == "XP_RTT_HIGH");
}

TEST(StreamHealthAnomaly, AnEncoderStayingFarBelowTheConfiguredBitrateIsNotAProblem) {
  // static content under constant-bitrate rate control: the output is a fraction of the configured ceiling, every frame on time
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 3000; ++i) {
    h.record(metric_e::encode_ms, 2);
    h.count(counter_e::frames_sent);
    h.count(counter_e::captured);
    h.count(counter_e::submitted);
  }
  now += WINDOW_MS;
  probe_t probe;
  probe.target_kbps = 80'000;
  probe.measured_kbps = 800;
  const auto w = h.tick(now, probe);
  ASSERT_TRUE(w.has_value());
  for (const auto &a : w->anomalies) EXPECT_NE(a.code, "XP_BITRATE_SHORT");
  EXPECT_TRUE(w->anomalies.empty());
}

TEST(StreamHealthAnomaly, FramesCapturedButNeverSubmittedAreSkipped) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 1000; ++i) {
    h.count(counter_e::frames_sent);
    h.count(counter_e::captured);
    if (i % 5 != 0) h.count(counter_e::submitted);  // 20 % lost
  }
  now += WINDOW_MS;
  const auto w = h.tick(now, {});
  ASSERT_EQ(w->anomalies.size(), 1u);
  EXPECT_EQ(w->anomalies[0].code, "XP_FRAME_SKIP");
  EXPECT_EQ(w->anomalies[0].count, 200u);
}

TEST(StreamHealthChunk, HistogramsAddUpToTheWindowsAndTheChunkResets) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 4; ++i) one_window(h, now, 3000, 10 + 30 * i, 5);
  const auto chunk = h.take_chunk(now);
  EXPECT_EQ(chunk["windows"], 4);
  EXPECT_EQ(chunk["window_s"], 240);
  EXPECT_EQ(chunk["frames"], 12000);
  const auto &host_p95 = chunk["h"]["host_p95"];
  ASSERT_EQ(host_p95.size(), BUCKET_COUNT);
  int host_total = 0;
  for (const auto &n : host_p95) host_total += n.get<int>();
  EXPECT_EQ(host_total, 4);
  EXPECT_EQ(chunk["codec"], "hevc");
  EXPECT_EQ(chunk["fps"], 60);
  EXPECT_EQ(chunk["client_kind"], "moonlight-macos-enhanced");
  EXPECT_TRUE(h.chunk_empty());
  EXPECT_EQ(h.take_chunk(now)["windows"], 0);
}

TEST(StreamHealthChunk, EveryKeyOfTheContractIsThere) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  one_window(h, now, 3000, 10, 5);
  const auto chunk = h.take_chunk(now);
  const std::vector<std::string> keys { "host_p50", "host_p95", "host_max", "capture_p95", "convert_p95", "encode_p95", "encode_max", "send_p95", "send_max", "backlog_p95", "backlog_max", "jitter_p50", "jitter_p95" };
  ASSERT_EQ(hist_keys().size(), keys.size());
  for (const auto &k : keys) EXPECT_TRUE(chunk["h"].contains(k)) << k;
}

// ---------------------------------------------------------------------------------------------------------------------
// The motivating case, replayed: the send thread of v2026.1114.173055 (#1246 as shipped) against the one before it.
// The model is the one of the pacer fix's simulation (60 fps, 80 Mbit/s, 1416-byte packets, 32-frame packet queue that
// is cleared as a whole when full). Each sent frame feeds the stream-health windows the way stream.cpp does.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
  using sim_clock_t = std::chrono::steady_clock;
  using tp_t = sim_clock_t::time_point;
  using namespace std::chrono_literals;

  constexpr int fps = 60;
  constexpr int configured_kbps = 80'000;
  constexpr std::size_t blocksize = 1416;
  constexpr std::size_t batch_packets = 64 * 1024 / blocksize;
  constexpr std::size_t queue_limit = 32;
  constexpr unsigned fec_percent = 20;
  constexpr std::size_t threshold_bytes = 64 * 1024;
  constexpr auto frame_interval = std::chrono::nanoseconds { 1'000'000'000 / fps };
  constexpr double average_frame_bytes = configured_kbps * 1000.0 / 8.0 / fps;

  struct frame_result_t {
    double completed_ms = 0;  ///< since the start of the stream
    double backlog_ms = 0;  ///< arrival (packet ready) to the last batch
    double queue_wait_ms = 0;  ///< arrival to the start of sending
    double send_ms = 0;
    std::size_t flush_dropped = 0;  ///< frames dropped by a queue clear that happened while this frame was handled
  };

  enum class model_e { before_1246,
    as_shipped };

  std::vector<frame_result_t>
  simulate(const std::vector<double> &frame_scale, model_e which) {
    const tp_t t0 { 100s };
    std::vector<tp_t> arrival;
    std::vector<std::size_t> payload;
    for (std::size_t i = 0; i < frame_scale.size(); ++i) {
      arrival.push_back(t0 + std::chrono::duration_cast<std::chrono::nanoseconds>(frame_interval * static_cast<long long>(i)));
      payload.push_back(static_cast<std::size_t>(average_frame_bytes * frame_scale[i]));
    }
    streaming::measured_bitrate_window_t window;
    const auto ms_of = [](tp_t t) { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count()); };

    std::vector<frame_result_t> out;
    std::deque<std::size_t> waiting;
    std::size_t next = 0;
    tp_t clock_now = t0;
    std::size_t pending_dropped = 0;
    tp_t next_start {}, start {};

    const auto admit = [&](tp_t upto) {
      while (next < arrival.size() && arrival[next] <= upto) {
        if (waiting.size() >= queue_limit) {
          pending_dropped += waiting.size();
          waiting.clear();
        }
        waiting.push_back(next++);
      }
    };
    const auto since = [&](tp_t t) { return std::chrono::duration<double, std::milli>(t - t0).count(); };

    while (next < arrival.size() || !waiting.empty()) {
      if (waiting.empty()) clock_now = std::max(clock_now, arrival[next]);
      admit(clock_now);
      if (waiting.empty()) continue;
      const auto index = waiting.front();
      waiting.pop_front();
      const auto begin_handling = clock_now;
      const auto wire_bytes = payload[index] * (100 + fec_percent) / 100;
      const auto packets = (wire_bytes + blocksize - 1) / blocksize;
      const auto measured = static_cast<int>(window.kbps(ms_of(clock_now)));
      const std::uint64_t rate = which == model_e::before_1246 ? 800'000'000ULL : streaming::video_pacing_rate_bps(measured, configured_kbps);
      const bool pace = which == model_e::before_1246 || streaming::should_pace_video_frame(false, payload[index], threshold_bytes);
      start = pace ? std::max(next_start, clock_now) : clock_now;
      std::size_t sent = 0;
      while (sent < packets) {
        const auto batch = std::min(batch_packets, packets - sent);
        const auto due = pace ? start + streaming::video_packet_slot(sent, blocksize, rate) : start;
        if (clock_now < due) clock_now = due;
        admit(clock_now);
        window.add_bytes(batch * blocksize, ms_of(clock_now));
        sent += batch;
      }
      next_start = pace ? start + streaming::video_packet_slot(sent, blocksize, rate) : clock_now;
      frame_result_t r;
      r.completed_ms = since(clock_now);
      r.backlog_ms = std::chrono::duration<double, std::milli>(clock_now - arrival[index]).count();
      r.queue_wait_ms = std::chrono::duration<double, std::milli>(begin_handling - arrival[index]).count();
      r.send_ms = std::chrono::duration<double, std::milli>(clock_now - begin_handling).count();
      r.flush_dropped = pending_dropped;
      pending_dropped = 0;
      out.push_back(r);
    }
    return out;
  }

  /// Four minutes of a game: mostly steady at 0.9 of the budget with a camera turn (3x for a second and a half) every 45 s.
  std::vector<double>
  four_minutes() {
    std::vector<double> trace;
    for (int second = 0; second < 240; ++second) {
      const bool turn = second % 45 >= 40 && second % 45 < 42;
      for (int f = 0; f < fps; ++f) trace.push_back(turn ? 2.6 : 0.9);
    }
    return trace;
  }

  struct replay_t {
    std::vector<nlohmann::json> chunks;
    std::vector<anomaly_t> anomalies;
    std::vector<window_t> windows;
  };

  replay_t
  replay(model_e which) {
    const auto frames = simulate(four_minutes(), which);
    stream_health_t h { 0, dims60() };
    replay_t out;
    std::int64_t next_tick = 2000;
    const auto tick = [&](std::int64_t now) {
      probe_t probe;
      probe.target_kbps = configured_kbps;
      probe.measured_kbps = configured_kbps;
      if (const auto w = h.tick(now, probe)) {
        out.windows.push_back(*w);
        for (const auto &a : w->anomalies) out.anomalies.push_back(a);
      }
    };
    for (const auto &f : frames) {
      const auto now = static_cast<std::int64_t>(f.completed_ms);
      while (next_tick <= now) {
        tick(next_tick);
        next_tick += 2000;
      }
      h.record(metric_e::host_ms, f.queue_wait_ms + 4);
      h.record(metric_e::backlog_ms, f.backlog_ms);
      h.record(metric_e::send_ms, f.send_ms);
      h.record(metric_e::encode_ms, 5);
      h.frame_done(static_cast<std::int64_t>(f.completed_ms * 1e6));
      h.count(counter_e::frames_sent);
      h.count(counter_e::captured);
      h.count(counter_e::submitted);
      if (f.flush_dropped > 0) {
        h.count(counter_e::queue_flushes);
        h.count(counter_e::queue_frames_dropped, f.flush_dropped);
      }
    }
    tick(next_tick + 60'000);
    out.chunks.push_back(h.take_chunk(1));
    return out;
  }
}  // namespace

TEST(StreamHealthReplay, TheShippedPacerIsFlaggedAndTheOneBeforeItIsNot) {
  const auto before = replay(model_e::before_1246);
  const auto shipped = replay(model_e::as_shipped);

  std::set<std::string> before_codes, shipped_codes;
  double shipped_backlog_max = 0;
  std::uint64_t dropped = 0;
  for (const auto &a : before.anomalies) before_codes.insert(a.code);
  for (const auto &a : shipped.anomalies) {
    shipped_codes.insert(a.code);
    if (a.code == code::send_backlog) shipped_backlog_max = std::max(shipped_backlog_max, a.value);
    if (a.code == code::queue_flush) dropped += a.count;
  }
  std::printf("  before #1246: windows %zu, codes: %zu\n  #1246 as shipped: windows %zu, flushes %llu frames, backlog max %.0f ms, codes:", before.windows.size(), before_codes.size(), shipped.windows.size(), static_cast<unsigned long long>(dropped), shipped_backlog_max);
  for (const auto &c : shipped_codes) std::printf(" %s", c.c_str());
  std::printf("\n");

  // Tier 0 counts the windows over the bound of each statistic. What the model shows: the backlog, the queue flushes and the host latency move, the cadence jitter of the send thread does not
  // (a backlogged thread sends one frame after another at a steady pace, so the frame-to-frame jitter of the host stays low; the client's own render jitter is not visible here).
  const auto jitter = [](const replay_t &r) {
    double p95 = 0, max = 0;
    for (const auto &w : r.windows) {
      p95 = std::max(p95, w.metrics[static_cast<std::size_t>(metric_e::jitter_ms)].p95);
      max = std::max(max, w.metrics[static_cast<std::size_t>(metric_e::jitter_ms)].max);
    }
    return std::pair { p95, max };
  };
  std::printf("  worst window jitter (p95 / max): before %.2f / %.2f ms, as shipped %.2f / %.2f ms\n", jitter(before).first, jitter(before).second, jitter(shipped).first, jitter(shipped).second);
  std::printf("  bad windows (before): %s\n  bad windows (as shipped): %s\n", before.chunks[0]["bad"].dump().c_str(), shipped.chunks[0]["bad"].dump().c_str());
  EXPECT_TRUE(before.chunks[0]["bad"].empty());
  EXPECT_GT(shipped.chunks[0]["bad"].value("backlog_max", 0), 0);
  EXPECT_GT(shipped.chunks[0]["bad"].value("host_p95", 0) + shipped.chunks[0]["bad"].value("host_max", 0), 0);
  EXPECT_TRUE(before_codes.empty());
  EXPECT_TRUE(shipped_codes.count("XP_QUEUE_FLUSH"));
  EXPECT_TRUE(shipped_codes.count("XP_SEND_BACKLOG"));
  EXPECT_GT(shipped_backlog_max, 250);
  EXPECT_GE(dropped, 32u);

  // the files the Worker's regression test replays
  if (const char *dir = std::getenv("STREAM_HEALTH_REPLAY_DIR")) {
    for (const auto &[name, r] : { std::pair { "before-1246", &before }, std::pair { "shipped-1246", &shipped } }) {
      nlohmann::json doc;
      doc["model"] = name;
      doc["chunks"] = r->chunks;
      doc["anomalies"] = nlohmann::json::array();
      for (const auto &a : r->anomalies)
        doc["anomalies"].push_back({ { "code", a.code }, { "severe", a.severe }, { "value", a.value }, { "threshold", a.threshold }, { "count", a.count }, { "suppressed", a.suppressed } });
      std::ofstream(std::string { dir } + "/" + name + ".json") << doc.dump(1);
    }
  }
}

TEST(StreamHealthChunk, Tier0CarriesTheBadWindowCountsAndTier1AddsTheHistograms) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 4; ++i) one_window(h, now, 3000, i < 3 ? 12 : 90, 5);  // one window over the 60 ms bound of the host p95
  const auto tier0 = h.take_chunk(now, false);
  EXPECT_FALSE(tier0.contains("h"));
  EXPECT_EQ(tier0["bad"]["host_p95"], 1);
  EXPECT_FALSE(tier0["bad"].contains("backlog_p95"));
  EXPECT_EQ(tier0["windows"], 4);
  for (int i = 0; i < 2; ++i) one_window(h, now, 3000, 12, 5);
  const auto tier1 = h.take_chunk(now, true);
  EXPECT_TRUE(tier1.contains("h"));
  EXPECT_TRUE(tier1["bad"].empty());
}

TEST(StreamHealthJitter, SteadyFramesHaveNoneAndAnUnevenCadenceShowsAtOnce) {
  stream_health_t h { 0, dims60() };
  const std::int64_t interval = 16'666'667;
  std::int64_t t = 1'000'000'000;
  for (int i = 0; i < 3600; ++i) {
    t += interval;
    h.count(counter_e::frames_sent);
    h.frame_done(t);
  }
  auto w = h.tick(WINDOW_MS, {});
  ASSERT_TRUE(w.has_value());
  EXPECT_LT(w->metrics[static_cast<std::size_t>(metric_e::jitter_ms)].p95, 0.2);
  // 3 ms of unevenness on every other frame
  for (int i = 0; i < 3600; ++i) {
    t += interval + (i % 2 ? 3'000'000 : -3'000'000);
    h.count(counter_e::frames_sent);
    h.frame_done(t);
  }
  w = h.tick(2 * WINDOW_MS, {});
  const auto &j = w->metrics[static_cast<std::size_t>(metric_e::jitter_ms)];
  EXPECT_GT(j.p95, 4.0);
  // a long pause (a static picture) is not jitter
  stream_health_t idle { 0, dims60() };
  idle.frame_done(1'000'000'000);
  idle.frame_done(3'000'000'000);
  idle.frame_done(3'016'000'000);
  idle.frame_done(3'033'000'000);
  for (int i = 0; i < 20; ++i) idle.count(counter_e::frames_sent);
  const auto iw = idle.tick(WINDOW_MS, {});
  EXPECT_LT(iw->metrics[static_cast<std::size_t>(metric_e::jitter_ms)].max, 1.0);
}

namespace {
  window_t
  window_with(double jitter_p95, double backlog_p95 = 3) {
    window_t w;
    w.idle = false;
    w.metrics[static_cast<std::size_t>(metric_e::jitter_ms)] = { jitter_p95 / 2, jitter_p95, jitter_p95, 3000 };
    w.metrics[static_cast<std::size_t>(metric_e::backlog_ms)] = { backlog_p95 / 2, backlog_p95, backlog_p95, 3000 };
    w.metrics[static_cast<std::size_t>(metric_e::host_ms)] = { 8, 12, 14, 3000 };
    w.metrics[static_cast<std::size_t>(metric_e::send_ms)] = { 2, 4, 5, 3000 };
    w.metrics[static_cast<std::size_t>(metric_e::encode_ms)] = { 3, 5, 6, 3000 };
    return w;
  }
}  // namespace

TEST(SelfBaseline, AnInstallWhoseJitterWentFromAFractionToSeveralMillisecondsReportsItOnceForTheVersion) {
  const auto dir = std::filesystem::temp_directory_path() / ("sb-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const auto file = dir / "stream_baseline.json";
  {
    self_baseline_t before { file, "v1" };
    for (int i = 0; i < 40; ++i) EXPECT_TRUE(before.observe(window_with(0.3)).empty());
    before.save();
  }
  self_baseline_t after { file, "v2" };  // the next start, a new version
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(after.observe(window_with(2.6)).empty());  // not yet: five windows in a row
  const auto events = after.observe(window_with(2.6));
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].metric, "jitter_p95");
  EXPECT_NEAR(events[0].baseline, 0.3, 0.05);
  EXPECT_NEAR(events[0].current, 2.6, 0.01);
  EXPECT_TRUE(events[0].version_changed);
  for (int i = 0; i < 10; ++i) EXPECT_TRUE(after.observe(window_with(2.6)).empty());  // once per version
  std::filesystem::remove_all(dir);
}

TEST(SelfBaseline, ASingleBadWindowAndAnInstallStillLearningSayNothing) {
  self_baseline_t learning { {}, "v1" };
  for (int i = 0; i < 20; ++i) EXPECT_TRUE(learning.observe(window_with(i < 10 ? 0.3 : 5)).empty());  // fewer than 30 windows: this is the baseline
  self_baseline_t steady { {}, "v1" };
  for (int i = 0; i < 40; ++i) steady.observe(window_with(0.3));
  EXPECT_TRUE(steady.observe(window_with(5)).empty());
  EXPECT_TRUE(steady.observe(window_with(0.3)).empty());  // the run is broken by a good window
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(steady.observe(window_with(5)).empty());
  // a tiny absolute change is not a regression even when the ratio is large
  self_baseline_t tiny { {}, "v1" };
  for (int i = 0; i < 40; ++i) tiny.observe(window_with(0.1));
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(tiny.observe(window_with(0.6)).empty());
}

TEST(StreamHealthWindow, AValueBeyondTheLastBinIsReportedAsItIsNotAsTheEdgeOfTheBin) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 100; ++i) {
    h.record(metric_e::host_ms, 2500);
    h.count(counter_e::frames_sent);
  }
  now += WINDOW_MS;
  const auto w = h.tick(now, {});
  ASSERT_TRUE(w.has_value());
  const auto &host = w->metrics[static_cast<std::size_t>(metric_e::host_ms)];
  EXPECT_DOUBLE_EQ(host.p50, 2500);
  EXPECT_DOUBLE_EQ(host.p95, 2500);
  const auto chunk = h.take_chunk(now, true);
  EXPECT_EQ(chunk["h"]["host_p95"][BUCKET_COUNT - 1], 1);  // the overflow bucket, not the one below
}

TEST(StreamHealthWindow, TheSenderThreadAndTheThreadEndingAStreamCanBothUseTheWindows) {
  stream_health_t h { 0, dims60() };
  std::atomic<bool> stop { false };
  std::thread sender([&] {
    std::int64_t now = 0;
    while (!stop) {
      now += WINDOW_MS;
      (void) h.tick(now, {});
      if (h.chunk_due(now)) (void) h.take_chunk(now, true);
    }
  });
  std::thread feeder([&] {
    for (int i = 0; i < 20000; ++i) {
      h.record(metric_e::host_ms, i % 50);
      h.count(counter_e::frames_sent);
    }
  });
  std::int64_t now = 1;
  for (int i = 0; i < 200; ++i) {
    now += WINDOW_MS;
    (void) h.finish(now, {});
    (void) h.take_chunk(now, true);
  }
  feeder.join();
  stop = true;
  sender.join();
  SUCCEED();  // the point is that it runs clean under a thread sanitizer and without a crash
}

TEST(StreamHealthWindow, ManySamplesAtOneSecondAndOneBeyondKeepTheirDistribution) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 99; ++i) {
    h.record(metric_e::host_ms, 1000);
    h.count(counter_e::frames_sent);
  }
  h.record(metric_e::host_ms, 2500);
  h.count(counter_e::frames_sent);
  now += WINDOW_MS;
  const auto w = h.tick(now, {});
  ASSERT_TRUE(w.has_value());
  const auto &host = w->metrics[static_cast<std::size_t>(metric_e::host_ms)];
  EXPECT_DOUBLE_EQ(host.p95, 1000);  // not the largest value: 99 of 100 samples were 1000 ms, and 1000 is inside the bucket (500, 1000]
  EXPECT_DOUBLE_EQ(host.max, 2500);
  const auto chunk = h.take_chunk(now, true);
  EXPECT_EQ(chunk["h"]["host_p95"][16], 1);  // the bucket whose inclusive upper bound is 1000 ms
}

TEST(StreamHealthWindow, TheUpperEdgeOfABucketIsInsideItAndOneOverTheBudgetIsNotMistakenForTheBudget) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 99; ++i) {
    h.record(metric_e::encode_ms, 16);
    h.count(counter_e::frames_sent);
  }
  h.record(metric_e::encode_ms, 17);
  h.count(counter_e::frames_sent);
  now += WINDOW_MS;
  const auto w = h.tick(now, {});
  ASSERT_TRUE(w.has_value());
  EXPECT_DOUBLE_EQ(w->metrics[static_cast<std::size_t>(metric_e::encode_ms)].p95, 16);  // not 17
  for (const auto &a : w->anomalies) EXPECT_NE(a.code, "XP_ENCODE_OVER") << "99 frames of 16 ms and one of 17 ms are inside the 16.7 ms budget at 60 fps";
  // 99 of 17 ms are not
  stream_health_t slow { now, dims60() };
  for (int i = 0; i < 100; ++i) {
    slow.record(metric_e::encode_ms, 17);
    slow.count(counter_e::frames_sent);
  }
  now += WINDOW_MS;
  const auto s = slow.tick(now, {});
  ASSERT_TRUE(s.has_value());
  bool over = false;
  for (const auto &a : s->anomalies) over = over || a.code == "XP_ENCODE_OVER";
  EXPECT_TRUE(over);
}

TEST(StreamHealthWindow, ManySamplesBeyondTheLastBinKeepTheirOwnDistribution) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  for (int i = 0; i < 99; ++i) {
    h.record(metric_e::host_ms, 4000);
    h.count(counter_e::frames_sent);
  }
  h.record(metric_e::host_ms, 20000);
  h.count(counter_e::frames_sent);
  now += WINDOW_MS;
  const auto w = h.tick(now, {});
  ASSERT_TRUE(w.has_value());
  const auto &host = w->metrics[static_cast<std::size_t>(metric_e::host_ms)];
  EXPECT_DOUBLE_EQ(host.p50, 4000);
  EXPECT_DOUBLE_EQ(host.p95, 4000);
  EXPECT_DOUBLE_EQ(host.max, 20000);
}

TEST(StreamHealthAnomaly, ASevereReportIsNotHeldBackByAnEarlierWarningOfTheSameCode) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  const auto capture = [&](double ms) { return [&h, ms] { h.record(metric_e::capture_ms, ms); }; };
  const auto warning = one_window(h, now, 3000, 10, 5, capture(400));  // over 250 ms, under 1000 ms
  ASSERT_EQ(warning->anomalies.size(), 1u);
  EXPECT_FALSE(warning->anomalies[0].severe);
  const auto severe = one_window(h, now, 3000, 10, 5, capture(1500));  // 60 s later: inside the 300 s warning cooldown
  ASSERT_EQ(severe->anomalies.size(), 1u);
  EXPECT_TRUE(severe->anomalies[0].severe);
  EXPECT_EQ(severe->anomalies[0].suppressed, 0u);
  // the severe cooldown still holds severe ones back, and a warning right after a severe one stays held back by the warning cooldown
  EXPECT_TRUE(one_window(h, now, 3000, 10, 5, capture(1500))->anomalies.empty());
  EXPECT_TRUE(one_window(h, now, 3000, 10, 5, capture(400))->anomalies.empty());
}

TEST(StreamHealthChunk, WindowsWithoutATargetDoNotPullTheTargetDown) {
  std::int64_t now = 0;
  stream_health_t h { now, dims60() };
  one_window(h, now, 3000, 10, 5);  // the probe had no target yet
  for (int i = 0; i < 3000; ++i) {
    h.record(metric_e::host_ms, 10);
    h.count(counter_e::frames_sent);
    h.count(counter_e::captured);
    h.count(counter_e::submitted);
  }
  now += WINDOW_MS;
  probe_t probe;
  probe.target_kbps = 80'000;
  h.tick(now, probe);
  const auto chunk = h.take_chunk(now);
  EXPECT_EQ(chunk["windows"], 2);
  EXPECT_EQ(chunk["target_kbps"], 80'000);
  EXPECT_EQ(chunk["bitrate_band"], bitrate_band(80'000));
}
