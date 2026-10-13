#include "src/streaming/perf_recorder.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
  nlohmann::json rates_for(std::uint32_t id, std::chrono::steady_clock::time_point now) {
    const auto snapshot = perf::current_snapshot_json(now);
    for (const auto &session : snapshot["sessions"]) {
      if (session["session_id"] == id) return session["frame_rates"];
    }
    return {};
  }

  void begin(std::uint32_t id) {
    perf::begin_session({ id, "RTSS test", 1920, 1080, 120, 20000, "nvenc", "vdd", false });
  }
}

TEST(PerfFrameRates, SeparatesSourceCaptureSubmissionOutputAndRepeats) {
  constexpr auto id = 91001u;
  begin(id);
  const auto start = std::chrono::steady_clock::now();
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->kind = perf::capture_source_kind_e::vdd_published;
  source->observe_sequence(10000);
  for (int i = 0; i < 110; ++i) perf::record_capture_frame(id, source, start);
  source->observe_sequence(10120);
  for (int i = 0; i < 120; ++i) perf::record_encode_submission(id, i >= 110);
  for (int i = 0; i < 115; ++i) perf::record_encoded_frame(id);

  EXPECT_FALSE(rates_for(id, start + 500ms)["ready"]);
  auto rates = rates_for(id, start + 1s);
  EXPECT_EQ(rates["source_kind"], "vdd_published");
  EXPECT_EQ(rates["window_ms"], 1000.0);
  EXPECT_EQ(rates["source"]["fps"], 120.0);
  EXPECT_EQ(rates["capture"]["fps"], 110.0);
  EXPECT_EQ(rates["submitted"]["fps"], 120.0);
  EXPECT_EQ(rates["encoded"]["fps"], 115.0);
  EXPECT_EQ(rates["repeated"]["fps"], 10.0);

  // A polling monitor must show a stall even when no new frame arrives.
  rates = rates_for(id, start + 2s);
  EXPECT_EQ(rates["source"]["fps"], 0.0);
  EXPECT_EQ(rates["capture"]["fps"], 0.0);
  EXPECT_EQ(rates["encoded"]["fps"], 0.0);
  perf::end_session(id);
  perf::record_encoded_frame(id);
  EXPECT_EQ(rates_for(id, start + 3s)["encoded"]["frames_total"], 115);
}

TEST(PerfFrameRates, ReinitializingTheSourceStartsANewCommonWindow) {
  constexpr auto id = 91002u;
  begin(id);
  const auto start = std::chrono::steady_clock::now();
  auto first = std::make_shared<perf::capture_source_counter_t>();
  first->kind = perf::capture_source_kind_e::wgc_arrivals;
  perf::record_capture_frame(id, first, start);
  first->record(120);
  EXPECT_EQ(rates_for(id, start + 1s)["source"]["fps"], 120.0);

  auto second = std::make_shared<perf::capture_source_counter_t>();
  second->kind = perf::capture_source_kind_e::dxgi_updates;
  second->record(100000);  // Must not spike the measured rate on display switch.
  perf::record_capture_frame(id, second, start + 1s);
  EXPECT_FALSE(rates_for(id, start + 1500ms)["ready"]);
  second->record(120);
  const auto rates = rates_for(id, start + 2s);
  EXPECT_EQ(rates["source_kind"], "dxgi_updates");
  EXPECT_EQ(rates["source"]["fps"], 120.0);
  EXPECT_EQ(rates["capture"]["fps"], 1.0);
  perf::end_session(id);
}

TEST(PerfFrameRates, SharedCaptureSourceDoesNotMergeSessionRates) {
  constexpr auto first_id = 91003u, second_id = 91004u;
  begin(first_id);
  begin(second_id);
  const auto start = std::chrono::steady_clock::now();
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->kind = perf::capture_source_kind_e::wgc_arrivals;
  for (int i = 0; i < 110; ++i) perf::record_capture_frame(first_id, source, start);
  for (int i = 0; i < 60; ++i) perf::record_capture_frame(second_id, source, start);
  source->record(120);
  EXPECT_EQ(rates_for(first_id, start + 1s)["source"]["fps"], 120.0);
  EXPECT_EQ(rates_for(second_id, start + 1s)["source"]["fps"], 120.0);
  EXPECT_EQ(rates_for(first_id, start + 1s)["capture"]["fps"], 110.0);
  EXPECT_EQ(rates_for(second_id, start + 1s)["capture"]["fps"], 60.0);
  perf::end_session(first_id);
  perf::end_session(second_id);
}

TEST(PerfFrameRates, CompletedSessionFreezesSourceTotalWhileSharedCaptureContinues) {
  constexpr auto first_id = 91006u, second_id = 91007u;
  begin(first_id);
  begin(second_id);
  const auto start = std::chrono::steady_clock::now();
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->kind = perf::capture_source_kind_e::wgc_arrivals;
  perf::record_capture_frame(first_id, source, start);
  perf::record_capture_frame(second_id, source, start);
  source->record(120);
  // Ending before any snapshot must still save the final source count.
  perf::end_session(first_id);
  source->record(120);
  perf::end_session(first_id);  // Repeated cleanup must not refresh the saved count.
  auto finished = rates_for(first_id, start + 2s);
  EXPECT_EQ(finished["source"]["frames_total"], 120);
  EXPECT_EQ(finished["source"]["fps"], 0.0);
  EXPECT_EQ(finished["capture"]["frames_total"], 1);
  EXPECT_EQ(rates_for(second_id, start + 2s)["source"]["frames_total"], 240);
  source->record(120);
  EXPECT_EQ(rates_for(first_id, start + 3s)["source"]["frames_total"], 120);
  EXPECT_EQ(rates_for(second_id, start + 3s)["source"]["frames_total"], 360);
  perf::end_session(second_id);
}

TEST(PerfFrameRates, UnsupportedSourceIsNullInsteadOfTheTargetRate) {
  constexpr auto id = 91005u;
  begin(id);
  const auto start = std::chrono::steady_clock::now();
  perf::record_capture_frame(id, {}, start);
  const auto rates = rates_for(id, start + 1s);
  EXPECT_EQ(rates["source_kind"], "unavailable");
  EXPECT_TRUE(rates["source"]["fps"].is_null());
  EXPECT_TRUE(rates["capture"]["fps"].is_number());
  perf::end_session(id);
}

TEST(PerfFrameRates, ProducerSequenceSkipsAndRestartsAreHandled) {
  perf::capture_source_counter_t source;
  source.observe_sequence(10000);
  EXPECT_EQ(source.frames, 0u);
  source.observe_sequence(10003);
  source.observe_sequence(2);
  source.observe_sequence(4);
  EXPECT_EQ(source.frames, 5u);
}

TEST(PerfFrameRates, SourceEventsCanArriveConcurrently) {
  perf::capture_source_counter_t source;
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < 10000; ++j) source.record();
    });
  }
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(source.frames, 40000u);
}

TEST(PerfFrameRates, LongPollingIntervalsUseElapsedTimeAndCounterResetWarmsUp) {
  perf::frame_rate_window_t window;
  const auto start = std::chrono::steady_clock::now();
  window.reset(start, {});
  const perf::frame_rate_window_t::counts_t counts { 300, 275, 300, 300, 25 };
  const auto rates = window.sample(start + 2500ms, counts);
  ASSERT_TRUE(rates);
  EXPECT_DOUBLE_EQ((*rates)[0], 120.0);
  EXPECT_DOUBLE_EQ((*rates)[1], 110.0);
  EXPECT_DOUBLE_EQ((*rates)[4], 10.0);
  EXPECT_FALSE(window.sample(start + 3s, {}));
}
