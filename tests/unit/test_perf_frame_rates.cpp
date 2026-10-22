#include "src/streaming/perf_recorder.h"

#include <gtest/gtest.h>
#include <future>
#include <stdexcept>
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

class PerfFrameRates : public ::testing::Test {
protected:
  void SetUp() override { perf::set_frame_rate_subscription("unit-test", true); }
  void TearDown() override { perf::set_frame_rate_subscription("unit-test", false); }
};

TEST_F(PerfFrameRates, SeparatesSourceCaptureSubmissionOutputAndRepeats) {
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

TEST_F(PerfFrameRates, ReinitializingTheSourceStartsANewCommonWindow) {
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

TEST_F(PerfFrameRates, SharedCaptureSourceDoesNotMergeSessionRates) {
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

TEST_F(PerfFrameRates, CompletedSessionFreezesSourceTotalWhileSharedCaptureContinues) {
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

TEST_F(PerfFrameRates, UnsupportedSourceIsNullInsteadOfTheTargetRate) {
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

TEST_F(PerfFrameRates, ProducerSequenceSkipsAndRestartsAreHandled) {
  perf::capture_source_counter_t source;
  source.observe_sequence(10000);
  EXPECT_EQ(source.frames, 0u);
  source.observe_sequence(10003);
  source.observe_sequence(2);
  source.observe_sequence(4);
  EXPECT_EQ(source.frames, 5u);
}

TEST_F(PerfFrameRates, SourceEventsCanArriveConcurrently) {
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

TEST_F(PerfFrameRates, LongPollingIntervalsUseElapsedTimeAndCounterResetWarmsUp) {
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

TEST(PerfFrameRateCollection, DisabledByDefaultAndPassiveSnapshotsDoNotEnableIt) {
  EXPECT_EQ(perf::frame_rate_collection_epoch(), 0u);
  constexpr auto id = 92001u;
  begin(id);
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->record(100);
  source->observe_sequence(10000);
  source->observe_sequence(10120);
  perf::record_capture_frame(id, source);
  perf::record_encode_submission(id, true);
  perf::record_encoded_frame(id);
  const auto rates = rates_for(id, std::chrono::steady_clock::now() + 2s);
  EXPECT_FALSE(rates["enabled"]);
  EXPECT_FALSE(rates["ready"]);
  EXPECT_TRUE(rates["capture"]["fps"].is_null());
  EXPECT_EQ(rates["capture"]["frames_total"], 0);
  EXPECT_EQ(rates["submitted"]["frames_total"], 0);
  EXPECT_EQ(rates["encoded"]["frames_total"], 0);
  EXPECT_EQ(source->frames, 0u);
  EXPECT_EQ(perf::frame_rate_collection_epoch(), 0u);
  perf::end_session(id);
}

TEST(PerfFrameRateCollection, ConsumersRenewAndReleaseIndependently) {
  perf::set_frame_rate_subscription("osd", true);
  const auto first_epoch = perf::frame_rate_collection_epoch();
  EXPECT_NE(first_epoch, 0u);
  auto state = perf::set_frame_rate_subscription("dashboard", true);
  EXPECT_EQ(state["subscribers"], 2);
  perf::set_frame_rate_subscription("osd", true);
  EXPECT_EQ(perf::frame_rate_collection_epoch(), first_epoch);
  state = perf::set_frame_rate_subscription("osd", false);
  EXPECT_TRUE(state["enabled"]);
  EXPECT_EQ(state["subscribers"], 1);
  perf::set_frame_rate_subscription("missing", false);
  EXPECT_EQ(perf::frame_rate_collection_epoch(), first_epoch);
  state = perf::set_frame_rate_subscription("dashboard", false);
  EXPECT_FALSE(state["enabled"]);
  EXPECT_EQ(state["subscribers"], 0);
}

TEST(PerfFrameRateCollection, ReenableResetsWindowAndExcludesVddPublicationsWhileDisabled) {
  constexpr auto id = 92002u;
  begin(id);
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->kind = perf::capture_source_kind_e::vdd_published;
  perf::set_frame_rate_subscription("restart-test", true);
  auto start = std::chrono::steady_clock::now();
  source->observe_sequence(10000);
  perf::record_capture_frame(id, source, start);
  source->observe_sequence(10120);
  EXPECT_EQ(rates_for(id, start + 1s)["source"]["fps"], 120.0);
  perf::set_frame_rate_subscription("restart-test", false);
  source->observe_sequence(20000);
  source->record(1000);
  perf::record_capture_frame(id, source);
  perf::record_encode_submission(id, true);
  perf::record_encoded_frame(id);
  EXPECT_EQ(source->frames, 120u);
  auto disabled = rates_for(id, start + 2s);
  EXPECT_FALSE(disabled["enabled"]);
  EXPECT_EQ(disabled["capture"]["frames_total"], 1);
  EXPECT_EQ(disabled["submitted"]["frames_total"], 0);
  EXPECT_EQ(disabled["encoded"]["frames_total"], 0);

  perf::set_frame_rate_subscription("restart-test", true);
  start = std::chrono::steady_clock::now();
  EXPECT_FALSE(rates_for(id, start)["ready"]);
  source->observe_sequence(20100);  // Re-prime: never count the disabled gap.
  source->observe_sequence(20220);
  auto resumed = rates_for(id, start + 1s);
  EXPECT_NEAR(resumed["source"]["fps"].get<double>(), 120.0, 0.02);
  EXPECT_EQ(resumed["source"]["frames_total"], 120);
  EXPECT_EQ(resumed["capture"]["frames_total"], 0);
  perf::set_frame_rate_subscription("restart-test", false);
  perf::end_session(id);
}

TEST(PerfFrameRateCollection, AbandonedConsumerExpiresWithoutFrameOrSnapshotCallbacks) {
  perf::set_frame_rate_subscription("expiry-test", true, 1s);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (perf::frame_rate_collection_epoch() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(perf::frame_rate_collection_epoch(), 0u);
  perf::set_frame_rate_subscription("expiry-test", false);
}

TEST(PerfFrameRateCollection, RejectsInvalidSubscriptionsWithoutEnablingCollection) {
  EXPECT_THROW(perf::set_frame_rate_subscription("", true), std::invalid_argument);
  EXPECT_THROW(perf::set_frame_rate_subscription("space not allowed", true), std::invalid_argument);
  EXPECT_THROW(perf::set_frame_rate_subscription(std::string(129, 'x'), true), std::invalid_argument);
  EXPECT_THROW(perf::set_frame_rate_subscription("test", true, 0ms), std::invalid_argument);
  EXPECT_THROW(perf::set_frame_rate_subscription("test", true, 31s), std::invalid_argument);
  EXPECT_EQ(perf::frame_rate_collection_epoch(), 0u);
}

TEST(PerfFrameRateCollection, ExpiringOneConsumerDoesNotStopAnother) {
  perf::set_frame_rate_subscription("short-lived", true, 1s);
  perf::set_frame_rate_subscription("still-open", true);
  const auto generation = perf::frame_rate_collection_epoch();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  nlohmann::json state;
  do {
    std::this_thread::sleep_for(10ms);
    state = perf::current_snapshot_json()["frame_rate_collection"];
  } while (state["subscribers"] == 2 && std::chrono::steady_clock::now() < deadline);
  EXPECT_TRUE(state["enabled"]);
  EXPECT_EQ(state["subscribers"], 1);
  EXPECT_EQ(perf::frame_rate_collection_epoch(), generation);
  perf::set_frame_rate_subscription("short-lived", false);
  perf::set_frame_rate_subscription("still-open", false);
}

TEST(PerfFrameRateCollection, ConcurrentFrameHooksStopAfterLastRelease) {
  constexpr auto id = 92003u;
  begin(id);
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->kind = perf::capture_source_kind_e::wgc_arrivals;
  perf::set_frame_rate_subscription("race-test", true);
  std::atomic<bool> stop { false };
  std::atomic<unsigned> attempts { 0 };
  std::thread producer([&]() {
    while (!stop.load()) {
      source->record();
      perf::record_capture_frame(id, source);
      perf::record_encode_submission(id, true);
      perf::record_encoded_frame(id);
      ++attempts;
    }
  });
  const auto wait_for_attempts = [&](unsigned target) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (attempts < target && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_GE(attempts.load(), target);
  };
  wait_for_attempts(100);
  EXPECT_GT(rates_for(id, std::chrono::steady_clock::now())["encoded"]["frames_total"], 0);
  for (int i = 0; i < 12; ++i) {
    perf::set_frame_rate_subscription("race-test", false);
    perf::set_frame_rate_subscription("race-test", true);
  }
  perf::set_frame_rate_subscription("race-test", false);
  wait_for_attempts(attempts.load() + 100);
  const auto frozen = rates_for(id, std::chrono::steady_clock::now());
  const auto source_total = source->frames.load();
  wait_for_attempts(attempts.load() + 100);
  const auto later = rates_for(id, std::chrono::steady_clock::now());
  EXPECT_FALSE(later["enabled"]);
  for (const auto *stage : { "source", "capture", "submitted", "encoded", "repeated" }) {
    EXPECT_EQ(later[stage]["frames_total"], frozen[stage]["frames_total"]);
  }
  EXPECT_EQ(source->frames, source_total);
  stop = true;
  producer.join();
  perf::end_session(id);
}

TEST(PerfFrameRateCollection, DisableDrainsAcceptedSourceWritesBeforeReenable) {
  constexpr auto id = 92004u;
  begin(id);
  perf::set_frame_rate_subscription("drain-test", true);
  auto source = std::make_shared<perf::capture_source_counter_t>();
  source->kind = perf::capture_source_kind_e::wgc_arrivals;
  perf::record_capture_frame(id, source);
  std::promise<bool> admitted;
  auto admitted_result = admitted.get_future();
  std::promise<void> allow_write;
  auto write_signal = allow_write.get_future();
  std::thread delayed_source([&]() {
    const perf::detail::source_update_guard_t update { perf::frame_rate_collection_epoch() };
    admitted.set_value(update.epoch() != 0);
    write_signal.wait();
    // Model a source hook preempted between its epoch check and atomic update.
    if (update.epoch()) source->frames.fetch_add(120, std::memory_order_relaxed);
  });
  EXPECT_TRUE(admitted_result.get());
  auto disable = std::async(std::launch::async, []() {
    return perf::set_frame_rate_subscription("drain-test", false);
  });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (perf::frame_rate_collection_epoch() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  EXPECT_EQ(perf::frame_rate_collection_epoch(), 0u);
  EXPECT_EQ(disable.wait_for(20ms), std::future_status::timeout);
  auto enable = std::async(std::launch::async, []() {
    return perf::set_frame_rate_subscription("drain-test", true);
  });
  allow_write.set_value();
  delayed_source.join();
  EXPECT_FALSE(disable.get()["enabled"]);
  EXPECT_TRUE(enable.get()["enabled"]);
  EXPECT_EQ(source->frames, 120u);
  auto resumed = rates_for(id, std::chrono::steady_clock::now());
  EXPECT_EQ(resumed["source"]["frames_total"], 0);
  source->record();
  resumed = rates_for(id, std::chrono::steady_clock::now());
  EXPECT_EQ(resumed["source"]["frames_total"], 1);
  perf::set_frame_rate_subscription("drain-test", false);
  perf::end_session(id);
}

TEST(PerfFrameRateCollection, EnrollmentWithAnObsoleteEpochIsRejected) {
  perf::set_frame_rate_subscription("old-epoch-test", true);
  const auto old_epoch = perf::frame_rate_collection_epoch();
  perf::set_frame_rate_subscription("old-epoch-test", false);
  perf::set_frame_rate_subscription("old-epoch-test", true);
  EXPECT_NE(perf::frame_rate_collection_epoch(), old_epoch);
  {
    const perf::detail::source_update_guard_t stale { old_epoch };
    EXPECT_EQ(stale.epoch(), 0u);
  }
  EXPECT_EQ(perf::detail::source_updates_in_flight.load(), 0u);
  perf::set_frame_rate_subscription("old-epoch-test", false);
}

TEST(PerfFrameRateCollection, LateSourceMetadataCannotPrimeOrIncrementTheNewWindow) {
  perf::set_frame_rate_subscription("metadata-test", true);
  perf::capture_source_counter_t source;
  const auto old_epoch = perf::frame_rate_collection_epoch();
  source.observe_sequence(10000, old_epoch);
  perf::set_frame_rate_subscription("metadata-test", false);
  perf::set_frame_rate_subscription("metadata-test", true);
  // The APIs can finish acquiring old data after a quick lease transition.
  source.record(120, old_epoch);
  source.observe_sequence(10120, old_epoch);
  EXPECT_EQ(source.frames, 0u);
  source.observe_sequence(20000);  // Prime using metadata from the current epoch.
  source.observe_sequence(20120);
  EXPECT_EQ(source.frames, 120u);
  perf::set_frame_rate_subscription("metadata-test", false);
}
