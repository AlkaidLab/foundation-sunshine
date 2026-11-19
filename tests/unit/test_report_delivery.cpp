/**
 * @file tests/unit/test_report_delivery.cpp
 * @brief Crash, error and feedback records always arrive; routine records leave once a day from the install's own moment, for a sample of the installs.
 */
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include "src/telemetry/error_report.h"
#include "src/telemetry/events.h"
#include "src/telemetry/queue.h"
#include "src/telemetry/report_slot.h"
#include "src/telemetry/service.h"

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace telemetry;

namespace {
  struct temp_dir_t {
    fs::path path;
    temp_dir_t() {
      path = fs::temp_directory_path() / ("report-delivery-" + new_uuid_v4());
      fs::create_directories(path);
    }
    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  /** A UUID-shaped install id whose moment of the day lies in [from_ms, to_ms) (found by counting, so the test is the same on every run). */
  std::string
  id_with_offset(std::int64_t from_ms, std::int64_t to_ms) {
    for (int i = 0; i < 100000; ++i) {
      char text[40];
      std::snprintf(text, sizeof text, "%08x-0000-4000-8000-000000000000", i);
      const auto offset = slot::offset_ms(text);
      if (offset >= from_ms && offset < to_ms) return text;
    }
    return {};
  }

  /** An install id that is in (or out of) the sample of `rate` and whose moment of the day lies between 8:00 and 16:00. */
  std::string
  id_sampled(double rate, bool in) {
    for (int i = 0; i < 100000; ++i) {
      char text[40];
      std::snprintf(text, sizeof text, "%08x-0000-4000-8000-000000000000", i);
      const auto offset = slot::offset_ms(text);
      if (slot::sampled_in(text, rate) == in && offset >= 8 * 3'600'000 && offset < 16 * 3'600'000) return text;
    }
    return {};
  }

  constexpr std::int64_t HOUR = 3'600'000;
  constexpr std::int64_t DAY = slot::DAY_MS;

  /** A service on a fake clock whose first day starts at midnight UTC of day 20000; the install id is fixed so the moment of the day is known. */
  struct harness_t {
    temp_dir_t dir;
    std::int64_t now;
    consent_t consent;
    std::vector<json> posted;
    int next_status = 200;
    long retry_after = 0;
    double random_value = 0.0;
    std::string next_body;
    std::unique_ptr<service_t> svc;
    std::string id;

    static constexpr std::int64_t DAY0 = 20000 * slot::DAY_MS;

    explicit harness_t(const std::string &install, service_options_t options = {}, std::int64_t start = DAY0 + 1000):
        now(start), id(install) {
      options.dir = dir.path;
      options.routine_slot = true;
      std::ofstream(dir.path / "install_id") << id << '\n';
      open(options);
    }
    void
    open(service_options_t options) {
      options.dir = dir.path;
      svc.reset();
      service_deps_t deps;
      deps.now_ms = [this] { return now; };
      deps.consent = [this] { return consent; };
      deps.random01 = [this] { return random_value; };
      deps.post = [this](const std::string &, const std::string &body) {
        posted.push_back(json::parse(body));
        post_result_t answer { next_status, retry_after };
        answer.body = next_body;
        return answer;
      };
      svc = std::make_unique<service_t>(options, deps);
      context_t c;
      c.version = "5.2.0.1";
      c.channel = "stable";
      c.os = "windows";
      c.os_version = "10.0.26100";
      c.arch = "x86_64";
      c.gpu_vendor = "nvidia";
      c.locale = "zh-CN";
      svc->set_context(c);
    }
    void
    restart(service_options_t options = {}) {
      options.routine_slot = true;
      open(options);
    }
    std::set<std::string>
    posted_names() const {
      std::set<std::string> out;
      for (const auto &b : posted)
        for (const auto &e : b["events"]) out.insert(e["name"].get<std::string>());
      return out;
    }
  };

  error_input_t
  an_error(const char *code = "E_ENCODER_INIT") {
    error_input_t e;
    e.code = code;
    e.loc = "src/video.cpp:4821:encoder_init";
    e.frames = { e.loc };
    return e;
  }

  json
  feedback_events() {
    return json::array({ json { { "kind", "error" }, { "name", "user_report" }, { "props", json { { "category", "bug" }, { "component", "panel" }, { "message", "it broke" } } } } });
  }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// the moment of the day

TEST(ReportSlot, DaysAreUtcDaysAlsoBeforeTheEpoch) {
  EXPECT_EQ(slot::day_of(0), 0);
  EXPECT_EQ(slot::day_of(DAY - 1), 0);
  EXPECT_EQ(slot::day_of(DAY), 1);
  EXPECT_EQ(slot::day_of(-1), -1);
  EXPECT_EQ(slot::day_of(-DAY), -1);
  EXPECT_EQ(slot::day_of(-DAY - 1), -2);
}

TEST(ReportSlot, EveryInstallGetsOneFixedMomentOfTheDayAndTheMomentsAreEvenlySpread) {
  constexpr int INSTALLS = 24000;
  std::array<int, 24> hours {};
  std::array<int, 144> ten_minutes {};
  for (int i = 0; i < INSTALLS; ++i) {
    const auto id = "install-" + std::to_string(i);
    const auto offset = slot::offset_ms(id);
    ASSERT_GE(offset, 0);
    ASSERT_LT(offset, DAY);
    EXPECT_EQ(offset, slot::offset_ms(id)) << "the same id always gives the same moment";
    ++hours[static_cast<std::size_t>(offset / HOUR)];
    ++ten_minutes[static_cast<std::size_t>(offset / (10 * 60'000))];
  }
  // 1000 per hour on average (a spread of about 31); no hour is more than a fifth off
  for (const auto n : hours) {
    EXPECT_GT(n, 800);
    EXPECT_LT(n, 1200);
  }
  // 167 per ten minutes (a spread of about 13); a flat share, not a few popular minutes
  for (const auto n : ten_minutes) {
    EXPECT_GT(n, 110);
    EXPECT_LT(n, 230);
  }
}

TEST(ReportSlot, NearEqualIdsDoNotGetNearEqualMoments) {
  // ids that differ in one character (what a counter or a copied folder gives) must not queue up behind each other
  std::set<std::int64_t> hours;
  for (int i = 0; i < 40; ++i) hours.insert(slot::offset_ms("00000000-0000-4000-8000-0000000000" + std::to_string(10 + i)) / HOUR);
  EXPECT_GT(hours.size(), 15u);
}

TEST(ReportSlot, TheShareOfADayIsItsMomentPlusASmallSpreadThatChangesFromDayToDay) {
  const std::string id = "11111111-2222-4333-8444-555555555555";
  std::set<std::int64_t> spreads;
  for (std::int64_t day = 20000; day < 20030; ++day) {
    const auto at = slot::slot_ms(id, day);
    EXPECT_GE(at, day * DAY + slot::offset_ms(id));
    EXPECT_LT(at, day * DAY + slot::offset_ms(id) + slot::DAY_JITTER_MS);
    EXPECT_EQ(at, slot::slot_ms(id, day)) << "no random source: asked twice, the same answer";
    spreads.insert(at - day * DAY - slot::offset_ms(id));
  }
  EXPECT_GT(spreads.size(), 20u);
}

TEST(ReportSlot, ARunThatStartedAfterTheShareReportsSoonAfterItsStartNotAtOnce) {
  const auto id = id_with_offset(2 * HOUR, 3 * HOUR);
  ASSERT_FALSE(id.empty());
  const std::int64_t day = 20000;
  // started before the share: the share
  EXPECT_EQ(slot::due_ms(id, day, day * DAY + 1000), slot::slot_ms(id, day));
  // started after it: a spread after the start, different for different installs
  const auto started = day * DAY + 20 * HOUR;
  const auto due = slot::due_ms(id, day, started);
  EXPECT_GE(due, started);
  EXPECT_LT(due, started + slot::START_JITTER_MS);
  std::set<std::int64_t> spread;
  for (int i = 0; i < 30; ++i) spread.insert(slot::due_ms("id-" + std::to_string(i), day, started) - started);
  EXPECT_GT(spread.size(), 20u);
}

// ---------------------------------------------------------------------------------------------------------------------
// the sample

TEST(ReportSample, TheShareOfInstallsThatAreInIsTheRate) {
  for (const double rate : { 0.1, 0.25, 0.5 }) {
    int in = 0;
    constexpr int INSTALLS = 20000;
    for (int i = 0; i < INSTALLS; ++i)
      if (slot::sampled_in("install-" + std::to_string(i), rate)) ++in;
    EXPECT_NEAR(static_cast<double>(in) / INSTALLS, rate, rate * 0.1) << rate;
  }
}

TEST(ReportSample, AnInstallStaysInOrOutAndOnlyEverJoinsAsTheRateGoesUp) {
  for (int i = 0; i < 2000; ++i) {
    const auto id = "install-" + std::to_string(i);
    EXPECT_EQ(slot::sampled_in(id, 0.1), slot::sampled_in(id, 0.1));
    if (slot::sampled_in(id, 0.1)) EXPECT_TRUE(slot::sampled_in(id, 0.5));
    if (slot::sampled_in(id, 0.5)) EXPECT_TRUE(slot::sampled_in(id, 1.0));
  }
}

TEST(ReportSample, ARateOfOneTakesEveryInstallAndTheWeightIsTheInverse) {
  for (int i = 0; i < 200; ++i) EXPECT_TRUE(slot::sampled_in("install-" + std::to_string(i), 1.0));
  EXPECT_DOUBLE_EQ(slot::sample_weight(1.0), 1.0);
  EXPECT_DOUBLE_EQ(slot::sample_weight(0.1), 10.0);
  EXPECT_DOUBLE_EQ(slot::sample_weight(0.25), 4.0);
  // a rate that makes no sense is held inside the bounds: never a weight above 100, never a division by zero
  EXPECT_DOUBLE_EQ(slot::sample_weight(0.0), 100.0);
  EXPECT_DOUBLE_EQ(slot::sample_weight(-3.0), 100.0);
  EXPECT_DOUBLE_EQ(slot::sample_weight(7.0), 1.0);
  EXPECT_DOUBLE_EQ(slot::clamp_rate(std::nan("")), slot::MIN_SAMPLE_RATE);
}

TEST(ReportSample, TheSampleWeightRestoresTheCount) {
  // the sampled installs, each counted with its weight, add up to about all of them
  constexpr int INSTALLS = 20000;
  double counted = 0;
  for (int i = 0; i < INSTALLS; ++i)
    if (slot::sampled_in("install-" + std::to_string(i), 0.1)) counted += slot::sample_weight(0.1);
  EXPECT_NEAR(counted, INSTALLS, INSTALLS * 0.1);
}

TEST(ReportSample, TheWorkersRateIsKeptInsideTheBoundsAndAbsentMeansTheLocalOne) {
  EXPECT_DOUBLE_EQ(parse_remote_config(json { { "sample_rate", 0.5 } }, 0).sample_rate, 0.5);
  EXPECT_DOUBLE_EQ(parse_remote_config(json { { "sample_rate", 5 } }, 0).sample_rate, 1.0);
  EXPECT_DOUBLE_EQ(parse_remote_config(json { { "sample_rate", 0.0001 } }, 0).sample_rate, slot::MIN_SAMPLE_RATE);
  EXPECT_DOUBLE_EQ(parse_remote_config(json { { "sample_rate", -1 } }, 0).sample_rate, 0.0);
  EXPECT_DOUBLE_EQ(parse_remote_config(json { { "sample_rate", "high" } }, 0).sample_rate, 0.0);
  EXPECT_DOUBLE_EQ(parse_remote_config(json::object(), 0).sample_rate, 0.0);
}

TEST(ReportSample, FailuresAndReportsAreNeverInTheSampledFamily) {
  const auto event = [](kind_e kind, const char *name, json props = json::object()) {
    event_t e;
    e.kind = kind;
    e.name = name;
    e.props = std::move(props);
    return e;
  };
  EXPECT_FALSE(sampled_family(event(kind_e::error, "user_report")));
  EXPECT_FALSE(sampled_family(event(kind_e::error, "error_report")));
  EXPECT_FALSE(sampled_family(event(kind_e::error, "error", json { { "severity", "fatal" } })));
  EXPECT_FALSE(sampled_family(event(kind_e::error, "driver_event")));
  EXPECT_FALSE(sampled_family(event(kind_e::experience, "stream_failed")));
  EXPECT_FALSE(sampled_family(event(kind_e::usage, "session_abnormal")));
  EXPECT_TRUE(sampled_family(event(kind_e::usage, "app_start")));
  EXPECT_TRUE(sampled_family(event(kind_e::usage, "stream_start")));
  EXPECT_TRUE(sampled_family(event(kind_e::experience, "panel_page")));
  EXPECT_TRUE(sampled_family(event(kind_e::experience, "stream_health")));
  EXPECT_TRUE(sampled_family(event(kind_e::experience, "stream_regress")));
}

TEST(ReportSample, DeliveryClassesFollowTheKindAndThePriority) {
  const auto event = [](kind_e kind, const char *name, json props = json::object(), int priority = -1) {
    event_t e;
    e.kind = kind;
    e.name = name;
    e.props = std::move(props);
    e.priority = priority;
    return e;
  };
  EXPECT_EQ(delivery_of(event(kind_e::error, "user_report")), delivery_e::feedback);
  EXPECT_EQ(delivery_of(event(kind_e::error, "error_report")), delivery_e::error);
  EXPECT_EQ(delivery_of(event(kind_e::error, "error", json { { "severity", "fatal" } })), delivery_e::error);
  EXPECT_EQ(delivery_of(event(kind_e::error, "driver_event")), delivery_e::error);
  EXPECT_EQ(delivery_of(event(kind_e::error, "session_abnormal")), delivery_e::error);
  // the count of a known error and a small error are routine: they leave with the share of the day
  EXPECT_EQ(delivery_of(event(kind_e::error, "error_report", json::object(), 1)), delivery_e::routine);
  EXPECT_EQ(delivery_of(event(kind_e::error, "error", json { { "severity", "error" } })), delivery_e::routine);
  EXPECT_EQ(delivery_of(event(kind_e::usage, "app_start")), delivery_e::routine);
  EXPECT_EQ(delivery_of(event(kind_e::experience, "stream_health")), delivery_e::routine);
  EXPECT_EQ(delivery_of(event(kind_e::experience, "stream_failed")), delivery_e::routine);
}

// ---------------------------------------------------------------------------------------------------------------------
// the disk queue

namespace {
  json
  batch_of(int n) {
    return json { { "install_id", "x" }, { "events", json::array() }, { "n", n } };
  }
}  // namespace

TEST(DeliveryQueue, FeedbackLeavesFirstThenErrorsThenTheRestAndTheLanesSeparateThem) {
  temp_dir_t dir;
  disk_queue_t q(dir.path);
  ASSERT_TRUE(q.push(batch_of(1), 2));
  ASSERT_TRUE(q.push(batch_of(2), 0));
  ASSERT_TRUE(q.push(batch_of(3), 0, delivery_e::error));
  ASSERT_TRUE(q.push(batch_of(4), 0, delivery_e::feedback));
  ASSERT_TRUE(q.push(batch_of(5), 0, delivery_e::error));
  EXPECT_EQ(q.front()->batch["n"], 4);
  EXPECT_EQ(q.front(2, lane_e::urgent)->batch["n"], 4);
  EXPECT_EQ(q.front(2, lane_e::routine)->batch["n"], 2);  // priority 0 before 2
  q.remove(q.front()->path);
  EXPECT_EQ(q.front()->batch["n"], 3);  // the oldest error
  q.remove(q.front(2, lane_e::urgent)->path);
  EXPECT_EQ(q.front(2, lane_e::urgent)->batch["n"], 5);
  q.remove(q.front(2, lane_e::urgent)->path);
  EXPECT_FALSE(q.front(2, lane_e::urgent));
  EXPECT_EQ(q.front()->batch["n"], 2);
  EXPECT_EQ(q.files_of(delivery_e::routine), 2u);
}

TEST(DeliveryQueue, AFullQueueGivesUpRoutineBatchesBeforeErrorsAndErrorsBeforeFeedback) {
  temp_dir_t dir;
  queue_limits_t limits;
  limits.max_files = 6;
  disk_queue_t q(dir.path, limits);
  for (int i = 0; i < 2; ++i) ASSERT_TRUE(q.push(batch_of(i), 0, delivery_e::feedback));
  for (int i = 10; i < 12; ++i) ASSERT_TRUE(q.push(batch_of(i), 0, delivery_e::error));
  for (int i = 20; i < 24; ++i) ASSERT_TRUE(q.push(batch_of(i), 2));
  for (int i = 30; i < 33; ++i) ASSERT_TRUE(q.push(batch_of(i), 0));  // routine priority 0
  EXPECT_EQ(q.files(), 6u);
  EXPECT_EQ(q.files_of(delivery_e::feedback), 2u);
  EXPECT_EQ(q.files_of(delivery_e::error), 2u);
  EXPECT_EQ(q.files_of(2), 0u);  // priority 2 went first, though it was older than the priority 0 batches that stayed
  // more errors than the room: the oldest errors go before any feedback does
  for (int i = 40; i < 46; ++i) ASSERT_TRUE(q.push(batch_of(i), 0, delivery_e::error));
  EXPECT_EQ(q.files(), 6u);
  EXPECT_EQ(q.files_of(delivery_e::feedback), 2u);
  EXPECT_EQ(q.files_of(delivery_e::error), 4u);
  EXPECT_EQ(q.front(2, lane_e::urgent)->batch["n"], 0);  // feedback first
}

TEST(DeliveryQueue, FeedbackIsTheLastToGoWhenOnlyFeedbackIsLeftOverTheCaps) {
  temp_dir_t dir;
  queue_limits_t limits;
  limits.max_files = 3;
  limits.max_feedback_files = 10;
  disk_queue_t q(dir.path, limits);
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(q.push(batch_of(i), 0, delivery_e::feedback));
  EXPECT_EQ(q.files(), 3u);
  EXPECT_EQ(q.front()->batch["n"], 3);  // the oldest of them went
}

TEST(DeliveryQueue, EachOfTheThreeHasABoundOfItsOwnSoOneFloodCannotPushOutTheOthers) {
  temp_dir_t dir;
  queue_limits_t limits;
  limits.max_files = 100;
  limits.max_p0_files = 3;
  limits.max_error_files = 4;
  limits.max_feedback_files = 2;
  disk_queue_t q(dir.path, limits);
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.push(batch_of(i), 0, delivery_e::feedback));
  for (int i = 10; i < 20; ++i) ASSERT_TRUE(q.push(batch_of(i), 0, delivery_e::error));
  for (int i = 30; i < 40; ++i) ASSERT_TRUE(q.push(batch_of(i), 0));
  EXPECT_EQ(q.files_of(delivery_e::feedback), 2u);
  EXPECT_EQ(q.files_of(delivery_e::error), 4u);
  EXPECT_EQ(q.files_of(delivery_e::routine), 3u);
  // the newest of each stayed
  EXPECT_EQ(q.front(2, lane_e::urgent)->batch["n"], 3);
  const auto routine = q.entries(0, 100);
  ASSERT_EQ(routine.size(), 9u);
  EXPECT_EQ(routine.back().batch["n"], 39);
}

TEST(DeliveryQueue, TheByteCapAlsoKeepsFeedbackAndErrorsLast) {
  temp_dir_t dir;
  queue_limits_t limits;
  limits.max_bytes = 4000;
  disk_queue_t q(dir.path, limits);
  const auto big = [](int n) {
    auto b = batch_of(n);
    b["pad"] = std::string(1000, 'x');
    return b;
  };
  ASSERT_TRUE(q.push(big(1), 0, delivery_e::feedback));
  ASSERT_TRUE(q.push(big(2), 0, delivery_e::error));
  for (int i = 3; i < 8; ++i) ASSERT_TRUE(q.push(big(i), 2));
  EXPECT_LE(q.bytes(), 4000u);
  EXPECT_EQ(q.files_of(delivery_e::feedback), 1u);
  EXPECT_EQ(q.files_of(delivery_e::error), 1u);
}

TEST(DeliveryQueue, TheClassSurvivesARestartInTheFileName) {
  temp_dir_t dir;
  {
    disk_queue_t q(dir.path);
    ASSERT_TRUE(q.push(batch_of(1), 0, delivery_e::feedback));
    ASSERT_TRUE(q.push(batch_of(2), 0, delivery_e::error));
    ASSERT_TRUE(q.push(batch_of(3), 1));
  }
  disk_queue_t again(dir.path);
  EXPECT_EQ(again.files_of(delivery_e::feedback), 1u);
  EXPECT_EQ(again.files_of(delivery_e::error), 1u);
  EXPECT_EQ(again.files_of(delivery_e::routine), 1u);
  EXPECT_EQ(again.front()->batch["n"], 1);
  // a file that an older version wrote (no class letter) is routine
  EXPECT_EQ(disk_queue_t::delivery_of_name("0001-000001-p0.json"), delivery_e::routine);
  EXPECT_EQ(disk_queue_t::delivery_of_name("0001-000001-p2.json"), delivery_e::routine);
  EXPECT_EQ(disk_queue_t::delivery_of_name("0001-000001-p0f.json"), delivery_e::feedback);
  EXPECT_EQ(disk_queue_t::delivery_of_name("0001-000001-p0e.json"), delivery_e::error);
}

// ---------------------------------------------------------------------------------------------------------------------
// retry and backoff

TEST(DeliveryBackoff, DoublesFromTheBaseToTheCapAndTheRandomShareIsAtMostAFifth) {
  EXPECT_EQ(backoff_seconds(0, 0.0), 0);
  EXPECT_EQ(backoff_seconds(1, 0.0), 30);
  EXPECT_EQ(backoff_seconds(2, 0.0), 60);
  EXPECT_EQ(backoff_seconds(20, 0.0), 3600);
  // the numbers of the crash uploader: 15 minutes doubling to four hours
  EXPECT_EQ(backoff_seconds(1, 0.0, 900, 14400), 900);
  EXPECT_EQ(backoff_seconds(2, 0.0, 900, 14400), 1800);
  EXPECT_EQ(backoff_seconds(5, 0.0, 900, 14400), 14400);
  EXPECT_EQ(backoff_seconds(9, 0.0, 900, 14400), 14400);
  EXPECT_EQ(backoff_seconds(1, 0.5), 33);
  std::set<std::int64_t> waits;
  for (int i = 0; i < 100; ++i) {
    const auto wait = backoff_seconds(3, i / 100.0);
    EXPECT_GE(wait, 120);
    EXPECT_LE(wait, 144);
    waits.insert(wait);
  }
  EXPECT_GT(waits.size(), 10u) << "installs that failed together come back at different moments";
}

TEST(DeliveryBackoff, ARetryAfterIsBelievedUpToTheCapAndSpreadByTheRandomShare) {
  EXPECT_EQ(retry_after_ms(0, 0.5), 0);
  EXPECT_EQ(retry_after_ms(-5, 0.5), 0);
  EXPECT_EQ(retry_after_ms(120, 0.0), 120'000);
  EXPECT_EQ(retry_after_ms(120, 0.5), 132'000);
  EXPECT_EQ(retry_after_ms(120, 0.999), 143'976);
  EXPECT_EQ(retry_after_ms(999999, 0.0, 3600), 3'600'000);
  std::set<std::int64_t> waits;
  for (int i = 0; i < 100; ++i) waits.insert(retry_after_ms(300, i / 100.0));
  EXPECT_GT(waits.size(), 50u);
}

TEST(DeliveryBackoff, OnlyAnAnswerAboutTheBatchItselfDropsIt) {
  for (const int status : { 0, 408, 425, 429, 500, 502, 503, 504, 401, 403, 404, 405, 410, 421 }) EXPECT_TRUE(status_retryable(status)) << status;
  for (const int status : { 400, 413, 415, 422, 301, 302 }) EXPECT_FALSE(status_retryable(status)) << status;
}

// ---------------------------------------------------------------------------------------------------------------------
// the error reporter: one full report of a kind of error a day, the rest counted

TEST(ErrorDaily, TheFirstOfTheDayIsFullTheRestAreOnlyCountedAndANewDayIsFullAgain) {
  error_reporter_t reporter;
  const auto e = an_error();
  const std::vector<std::string> crumbs { "30|act|tile.start" };
  const auto first = reporter.report(e, crumbs, 1000);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->contains("frames"));
  for (int i = 0; i < 20; ++i) EXPECT_FALSE(reporter.report(e, crumbs, 2000 + i * HOUR / 2).has_value()) << i;  // 10 hours of repeats on the same day
  const auto next_day = reporter.report(e, crumbs, DAY + 5);
  ASSERT_TRUE(next_day.has_value());
  EXPECT_TRUE(next_day->contains("frames"));
  EXPECT_EQ((*next_day)["fingerprint"], (*first)["fingerprint"]);
  EXPECT_EQ((*next_day)["count"], 1);
}

TEST(ErrorDaily, TheRepeatsLeaveAsOneCountPerKindOfErrorWhenTheAggregateTimeIsADay) {
  error_reporter_t reporter;
  reporter.set_aggregate_ms(DAY);
  const auto a = an_error("E_A");
  const auto b = an_error("E_B");
  ASSERT_TRUE(reporter.report(a, {}, 1000));
  ASSERT_TRUE(reporter.report(b, {}, 1000));
  for (int i = 0; i < 9; ++i) EXPECT_FALSE(reporter.report(a, {}, 2000 + i));
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(reporter.report(b, {}, 2000 + i));
  EXPECT_TRUE(reporter.flush(1000 + 23 * HOUR).empty()) << "the day is not over";
  const auto counts = reporter.flush(DAY + 10);
  ASSERT_EQ(counts.size(), 2u);
  std::set<std::uint64_t> numbers;
  for (const auto &c : counts) {
    numbers.insert(c["count"].get<std::uint64_t>());
    EXPECT_FALSE(c.contains("frames")) << "a count carries the group and the number, not the stack again";
    EXPECT_FALSE(c.contains("crumbs"));
  }
  EXPECT_EQ(numbers, (std::set<std::uint64_t> { 9, 3 }));
  EXPECT_TRUE(reporter.flush(2 * DAY).empty());
}

TEST(ErrorDaily, ADayThatEndsWithTheHostStoppingGivesTheCountsToTheDiskAtOnce) {
  error_reporter_t reporter;
  reporter.set_aggregate_ms(DAY);
  ASSERT_TRUE(reporter.report(an_error(), {}, 1000));
  for (int i = 0; i < 4; ++i) EXPECT_FALSE(reporter.report(an_error(), {}, 2000));
  EXPECT_TRUE(reporter.flush_groups(5000).empty());
  const auto counts = reporter.flush_groups(5000, true);
  ASSERT_EQ(counts.size(), 1u);
  EXPECT_EQ(counts[0].props["count"], 4);
  EXPECT_EQ(counts[0].product, "host");
}

TEST(ErrorDaily, AtMostTwentyKindsOfErrorLeaveInAnHourStillHolds) {
  error_reporter_t reporter;
  int sent = 0;
  for (int i = 0; i < 40; ++i)
    if (reporter.report(an_error(("E_KIND_" + std::to_string(i)).c_str()), {}, 1000 + i)) ++sent;
  EXPECT_EQ(sent, error_reporter_t::MAX_PER_HOUR);
}

TEST(ErrorDaily, ARestartRemembersWhatWasSentInFullToday) {
  error_reporter_t first_run;
  ASSERT_TRUE(first_run.report(an_error(), {}, 1000));
  const auto state = first_run.state(2000);
  ASSERT_EQ(state["keys"].size(), 1u);
  error_reporter_t second_run;
  second_run.restore(state, 3000);
  EXPECT_FALSE(second_run.report(an_error(), {}, 4000).has_value()) << "the same day, the same kind of error: counted, not sent in full again";
  // the state of another day is not believed
  error_reporter_t next_day;
  next_day.restore(state, DAY + 3000);
  EXPECT_TRUE(next_day.report(an_error(), {}, DAY + 4000).has_value());
  // nonsense is ignored
  error_reporter_t broken;
  broken.restore(json { { "day", "x" } }, 3000);
  broken.restore(json::array(), 3000);
  EXPECT_TRUE(broken.report(an_error(), {}, 4000).has_value());
}

TEST(ErrorDaily, ThePageErrorsOfOneFingerprintAreMergedUnderTheProductAndVersionThatSentThem) {
  error_reporter_t reporter;
  reporter.set_aggregate_ms(DAY);
  const json props { { "component", "panel" }, { "lang", "js" }, { "fingerprint", "abc123" }, { "code", "E_PAGE" }, { "severity", "error" }, { "count", 1 }, { "frames", json::array({ "render@app.js:1:2" }) } };
  const auto first = reporter.admit("panel", "1.2.3", props, 1000);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->contains("frames"));
  EXPECT_FALSE(reporter.admit("panel", "1.2.3", props, 2000).has_value());
  EXPECT_FALSE(reporter.admit("panel", "1.2.3", props, 3000).has_value());
  // the same fingerprint from the web page is its own group
  EXPECT_TRUE(reporter.admit("web", "9", props, 3000).has_value());
  const auto counts = reporter.flush_groups(DAY + 1);
  ASSERT_EQ(counts.size(), 1u);
  EXPECT_EQ(counts[0].product, "panel");
  EXPECT_EQ(counts[0].version, "1.2.3");
  EXPECT_EQ(counts[0].props["count"], 2);
  EXPECT_EQ(counts[0].props["fingerprint"], "abc123");
  EXPECT_FALSE(counts[0].props.contains("frames"));
  // a report without a fingerprint cannot be grouped: it passes as it is
  EXPECT_TRUE(reporter.admit("panel", "1", json { { "code", "E_X" } }, 5000).has_value());
}

// ---------------------------------------------------------------------------------------------------------------------
// the service: the three that must arrive, and the routine share of the day

TEST(ReportDelivery, ErrorsAndFeedbackLeaveAtOnceAndRoutineRecordsWaitForTheShareOfTheDay) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  ASSERT_FALSE(id.empty());
  harness_t h(id);
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->report_error(an_error());
  ASSERT_EQ(h.svc->ingest("panel", "1.0", feedback_events()), 1u);
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 2);
  EXPECT_EQ(h.posted_names(), (std::set<std::string> { "error_report", "user_report" }));
  EXPECT_EQ(h.posted.front()["events"][0]["name"], "user_report") << "feedback first";
  // hours later, still before the share: the usage record is still waiting
  h.now = harness_t::DAY0 + 7 * HOUR;
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.svc->queued_files(), 1u);
  // the share begins
  h.now = slot::slot_ms(id, 20000) + 1000;
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted_names().count("app_start"), 1u);
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.svc->plan().routine_day_done, 20000);
}

TEST(ReportDelivery, WhatTheBufferHoldsWhenTheShareBeginsLeavesWithIt) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.now = slot::slot_ms(id, 20000) + 1000;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));  // never flushed: it is still in memory
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted_names(), std::set<std::string> { "app_start" });
}

TEST(ReportDelivery, RoutineRecordsLeaveOnceADayAndTheNextShareIsTheNextDays) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.now = slot::slot_ms(id, 20000) + 1000;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  EXPECT_EQ(h.svc->pump(), 1);
  // later the same day: another record waits, nothing goes out
  h.now += 3 * HOUR;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_exit", json { { "uptime_s", 100 } }));
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.posted.size(), 1u);
  // the next day, before its share: still waiting; after it: sent
  h.now = harness_t::DAY0 + DAY + HOUR;
  EXPECT_EQ(h.svc->pump(), 0);
  h.now = slot::slot_ms(id, 20001) + 1000;
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted.size(), 2u);
  EXPECT_EQ(h.posted.back()["events"][0]["name"], "app_exit");
}

TEST(ReportDelivery, ARestartAfterTheDaysShareDoesNotSendTheDayAgainButTheNextDayIsSent) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.now = slot::slot_ms(id, 20000) + 1000;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  ASSERT_EQ(h.svc->pump(), 1);
  h.now += 2 * HOUR;
  h.restart();
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.now += 10 * 60'000;
  EXPECT_EQ(h.svc->pump(), 0) << "today's share was sent before the restart";
  h.now = slot::slot_ms(id, 20001) + 1000;
  EXPECT_EQ(h.svc->pump(), 1);
}

TEST(ReportDelivery, AnInstallThatWasOffAtItsShareReportsSoonAfterTheStartNotAtOnce) {
  const auto id = id_with_offset(2 * HOUR, 3 * HOUR);
  const auto start = harness_t::DAY0 + 20 * HOUR;
  harness_t h(id, {}, start);
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  const auto due = slot::due_ms(id, 20000, start);
  EXPECT_GE(due, start);
  EXPECT_LT(due, start + slot::START_JITTER_MS);
  if (due > start) {
    h.now = due - 1;
    EXPECT_EQ(h.svc->pump(), 0);
  }
  h.now = due;
  EXPECT_EQ(h.svc->pump(), 1);
}

TEST(ReportDelivery, AShareThatFailsThreeTimesWaitsForTheNextDay) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.now = slot::slot_ms(id, 20000) + 1000;
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();
  h.next_status = 503;
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(h.svc->pump(), 0);
    h.now += 2 * HOUR;
  }
  EXPECT_EQ(h.posted.size(), 3u);
  // the server is well again, the same day: the rest of the share waits for the next day
  h.next_status = 200;
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.posted.size(), 3u);
  EXPECT_EQ(h.svc->queued_files(), 1u);
  h.now = slot::due_ms(id, 20001, harness_t::DAY0 + 1000) + 1000;
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.svc->queued_files(), 0u);
}

TEST(ReportDelivery, ErrorsAndFeedbackSurviveAFailingServerAndARestartAndArriveInOrder) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.next_status = 503;
  h.retry_after = 120;
  h.svc->report_error(an_error("E_A"));
  ASSERT_EQ(h.svc->ingest("panel", "1.0", feedback_events()), 1u);
  h.svc->report_error(an_error("E_B"));
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_EQ(h.posted.size(), 1u) << "one failure stops the pass";
  EXPECT_EQ(h.svc->queued_files(), 3u) << "nothing was lost: the feedback, the two errors and the count of the feedback (routine)";
  // the host is closed and started again two hours later; the server is back
  h.now += 2 * HOUR;
  h.restart();
  h.next_status = 200;
  h.posted.clear();
  int sent = 0;
  for (int i = 0; i < 5 && h.svc->queued_files() > 1; ++i) sent += h.svc->pump();
  EXPECT_EQ(sent, 2);
  EXPECT_EQ(h.svc->queued_files(), 1u) << "the routine count waits for the share of the day";
  ASSERT_EQ(h.posted.size(), 2u);
  EXPECT_EQ(h.posted[0]["events"][0]["name"], "user_report") << "feedback first, then the errors";
  EXPECT_EQ(h.posted[1]["events"].size(), 2u);
  EXPECT_EQ(h.posted[1]["events"][0]["name"], "error_report");
}

TEST(ReportDelivery, ABatchThatCannotBeSentKeepsAnErrorOnDiskWhenTheServerAnswersWithAProxysRefusal) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.svc->report_error(an_error());
  h.svc->flush();
  for (const int status : { 403, 404, 401, 410 }) {
    h.next_status = status;
    EXPECT_EQ(h.svc->pump(), 0) << status;
    EXPECT_EQ(h.svc->queued_files(), 1u) << status;
    h.now += 3 * HOUR;
  }
  // a refusal about the batch itself drops it
  h.next_status = 400;
  h.svc->pump();
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.svc->counts().rejected_batches, 1u);
}

TEST(ReportDelivery, ABatchThatAProxyKeepsRefusingIsGivenUpSoItDoesNotStopTheOnesBehindIt) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.svc->report_error(an_error("E_STUCK"));
  h.svc->flush();
  h.next_status = 403;
  for (int i = 0; i < 7; ++i) {
    h.svc->pump();
    h.now += 2 * HOUR;
    EXPECT_EQ(h.svc->queued_files(), 1u) << "refusal " << i + 1 << ": still kept";
  }
  h.svc->pump();  // the eighth answer of the same kind
  EXPECT_EQ(h.svc->queued_files(), 0u);
  EXPECT_EQ(h.svc->counts().rejected_batches, 1u);
  // what comes after is sent
  h.now += 2 * HOUR;
  h.next_status = 200;
  h.svc->report_error(an_error("E_NEXT"));
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 1);
}

TEST(ReportDelivery, ARetryAfterIsSpreadByTheRandomShareAndTheBackoffToo) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.svc->report_error(an_error());
  h.svc->flush();
  h.next_status = 429;
  h.retry_after = 300;
  h.random_value = 0.5;
  const auto before = h.now;
  h.svc->pump();
  EXPECT_EQ(h.svc->retry_not_before_ms(), before + 330'000);  // 300 s and a tenth
  h.now = h.svc->retry_not_before_ms();
  h.retry_after = 0;
  h.random_value = 0.0;
  const auto second = h.now;
  h.svc->pump();
  EXPECT_EQ(h.svc->retry_not_before_ms(), second + 60'000);  // the second failure: 30 s doubled
}

TEST(ReportDelivery, ARepeatingErrorCostsOneFullReportAndOneCountADayAlsoAcrossARestart) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  for (int i = 0; i < 50; ++i) h.svc->report_error(an_error());
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.posted[0]["events"].size(), 1u);
  EXPECT_EQ(h.posted[0]["events"][0]["props"]["count"], 1);
  EXPECT_TRUE(h.posted[0]["events"][0]["props"].contains("frames"));
  // the host stops: the counts go to the disk; started again the same day, the same error is counted, not sent in full
  h.svc->flush(true);
  h.restart();
  for (int i = 0; i < 10; ++i) h.svc->report_error(an_error());
  h.svc->flush();
  h.now = slot::slot_ms(id, 20000) + 1000;
  EXPECT_GE(h.svc->pump(), 1);
  std::uint64_t counted = 0;
  int full = 0;
  for (std::size_t i = 1; i < h.posted.size(); ++i)
    for (const auto &e : h.posted[i]["events"]) {
      if (e["name"] != "error_report") continue;
      if (e["props"].contains("frames")) ++full;
      counted += e["props"]["count"].get<std::uint64_t>();
    }
  EXPECT_EQ(full, 0) << "the first of the day was sent before the restart";
  EXPECT_EQ(counted, 49u) << "the 49 repeats of the first run reached the disk when the host stopped; the 10 of the second run wait for the next flush";
}

TEST(ReportDelivery, APageErrorOfTheSameFingerprintIsMergedToo) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  const json events = json::array({ json { { "kind", "error" }, { "name", "error_report" }, { "props", json { { "component", "panel" }, { "lang", "js" }, { "fingerprint", "abc123" }, { "code", "E_PAGE" }, { "severity", "error" } } } } });
  for (int i = 0; i < 20; ++i) EXPECT_EQ(h.svc->ingest("panel", "1.0", events), 1u) << "the page is told every one was accepted";
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.posted[0]["events"].size(), 1u) << "one full report";
  EXPECT_EQ(h.posted[0]["product"], "panel");
}

TEST(ReportDelivery, ErrorsOffMeansNoErrorIsCountedOrKept) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.consent = { true, false };
  for (int i = 0; i < 5; ++i) h.svc->report_error(an_error());
  const json events = json::array({ json { { "kind", "error" }, { "name", "error_report" }, { "props", json { { "component", "panel" }, { "fingerprint", "abc123" }, { "code", "E_PAGE" } } } } });
  EXPECT_EQ(h.svc->ingest("panel", "1.0", events), 0u);
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_TRUE(h.posted.empty());
  EXPECT_EQ(h.svc->queued_files(), 0u);
  // feedback is the person's own act: it goes whatever the switch says
  EXPECT_EQ(h.svc->ingest("panel", "1.0", feedback_events()), 1u);
  EXPECT_EQ(h.svc->pump(), 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// the sample in the service

TEST(ReportSampling, AnInstallOutsideTheSampleSendsNoRoutineRecordButEveryErrorAndFeedback) {
  const auto id = id_sampled(0.1, false);
  ASSERT_FALSE(id.empty());
  service_options_t options;
  options.sample_rate = 0.1;
  harness_t h(id, options);
  for (int i = 0; i < 5; ++i) h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
  h.svc->emit(kind_e::usage, "stream_start", json { { "codec", "hevc" } });
  h.svc->report_error(an_error());
  ASSERT_EQ(h.svc->ingest("panel", "1.0", feedback_events()), 1u);
  h.svc->flush();
  h.now = slot::slot_ms(id, 20000) + 1000;
  h.svc->pump();
  h.now += HOUR;
  h.svc->pump();
  // the count of the feedback (feedback_submit) is no sampled record; the six usage records of an install outside the sample were not even written
  EXPECT_EQ(h.posted_names(), (std::set<std::string> { "error_report", "user_report", "feedback_submit" }));
  EXPECT_GE(h.svc->counts().sampled_out, 6u);
  EXPECT_FALSE(h.svc->plan().sampled_in);
}

TEST(ReportSampling, AnInstallInTheSampleCarriesTheWeightOnItsRoutineBatchesOnly) {
  const auto id = id_sampled(0.1, true);
  ASSERT_FALSE(id.empty());
  service_options_t options;
  options.sample_rate = 0.1;
  harness_t h(id, options);
  h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
  h.svc->report_error(an_error());
  h.svc->flush();
  h.now = slot::slot_ms(id, 20000) + 1000;
  h.svc->pump();
  ASSERT_EQ(h.posted.size(), 2u);
  int usage = 0, errors = 0;
  for (const auto &batch : h.posted) {
    const auto name = batch["events"][0]["name"].get<std::string>();
    if (name == "app_start") {
      ++usage;
      ASSERT_TRUE(batch.contains("sample_weight"));
      EXPECT_EQ(batch["sample_weight"], 10);
    }
    else {
      ++errors;
      EXPECT_EQ(name, "error_report");
      EXPECT_FALSE(batch.contains("sample_weight")) << "an error counts for itself: " << name;
    }
  }
  EXPECT_EQ(usage, 1);
  EXPECT_EQ(errors, 1);
  const auto plan = h.svc->plan();
  EXPECT_TRUE(plan.sampled_in);
  EXPECT_DOUBLE_EQ(plan.weight, 10.0);
  EXPECT_DOUBLE_EQ(plan.sample_rate, 0.1);
}

TEST(ReportSampling, ARateOfOneSendsEveryInstallWithNoWeight) {
  const auto id = id_sampled(0.1, false);
  harness_t h(id);  // the default rate is 1
  h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
  h.svc->flush();
  h.now = slot::slot_ms(id, 20000) + 1000;
  h.svc->pump();
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_FALSE(h.posted[0].contains("sample_weight"));
}

TEST(ReportSampling, TheRateTheWorkerGivesReplacesTheLocalOneAndIsKept) {
  const auto id = id_sampled(0.1, false);
  service_options_t options;
  options.sample_rate = 0.1;
  harness_t h(id, options);
  h.svc->report_error(an_error());
  h.svc->flush();
  h.next_body = json { { "ok", true }, { "cfg", json { { "sample_rate", 1.0 } } } }.dump();
  ASSERT_EQ(h.svc->pump(), 1);  // the answer to the error carries the configuration
  EXPECT_DOUBLE_EQ(h.svc->plan().sample_rate, 1.0);
  EXPECT_TRUE(h.svc->plan().sampled_in);
  h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
  h.svc->flush();
  h.now = slot::slot_ms(id, 20000) + 1000;
  h.next_body.clear();
  h.svc->pump();
  EXPECT_EQ(h.posted_names().count("app_start"), 1u);
  // read again at the next start
  h.restart(options);
  EXPECT_DOUBLE_EQ(h.svc->plan().sample_rate, 1.0);
}

TEST(ReportSampling, TheEnvelopeCarriesTheWeightOnlyAboveOne) {
  context_t c;
  c.install_id = "x";
  EXPECT_FALSE(envelope_fields(c, "host", "1.0").contains("sample_weight"));
  c.sample_weight = 1.0;
  EXPECT_FALSE(envelope_fields(c, "host", "1.0").contains("sample_weight"));
  c.sample_weight = 10.0;
  EXPECT_EQ(envelope_fields(c, "host", "1.0")["sample_weight"], 10);
  c.sample_weight = 100.0 / 3.0;
  EXPECT_DOUBLE_EQ(envelope_fields(c, "host", "1.0")["sample_weight"].get<double>(), 33.33);
  c.sample_weight = 5000.0;
  EXPECT_EQ(envelope_fields(c, "host", "1.0")["sample_weight"], 100) << "never above the weight of the lowest rate";
}

TEST(ReportSampling, WhenBothSwitchesAreOffNothingIsSampledOrSentAndNoIdIsMade) {
  service_options_t options;
  options.sample_rate = 0.1;
  harness_t h("x", options);
  std::error_code ec;
  fs::remove(h.dir.path / "install_id", ec);  // a fresh install: no id exists yet
  h.restart(options);
  h.consent = { false, false };
  h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
  h.svc->report_error(an_error());
  h.svc->flush();
  h.now += DAY;
  EXPECT_EQ(h.svc->pump(), 0);
  EXPECT_TRUE(h.posted.empty());
  EXPECT_FALSE(fs::exists(h.dir.path / "install_id")) << "no identity is made for records that are not allowed";
}

// ---------------------------------------------------------------------------------------------------------------------
// the old way still works when the share of the day is off

TEST(ReportDelivery, WithoutTheShareEverythingLeavesAsSoonAsItIsWritten) {
  temp_dir_t dir;
  std::int64_t now = harness_t::DAY0 + 1000;
  std::vector<json> posted;
  service_deps_t deps;
  deps.now_ms = [&] { return now; };
  deps.consent = [] { return consent_t {}; };
  deps.random01 = [] { return 0.0; };
  deps.post = [&](const std::string &, const std::string &body) {
    posted.push_back(json::parse(body));
    return post_result_t { 200 };
  };
  service_options_t options;
  options.dir = dir.path;
  service_t s(options, deps);
  s.emit(kind_e::usage, "app_start", json { { "cold", true } });
  s.report_error(an_error());
  s.flush();
  EXPECT_EQ(s.pump(), 2);
  EXPECT_EQ(posted.size(), 2u);
}

// ---------------------------------------------------------------------------------------------------------------------
// what the first review of the change found: counts across midnight, versions, the hourly bound at exit, the order of the lanes

TEST(ErrorDaily, CountsOfAnEarlierDayLeaveAtTheNextFlushAndNeverBecomeCountsOfTheNewDay) {
  error_reporter_t reporter;
  reporter.set_aggregate_ms(DAY);
  ASSERT_TRUE(reporter.report(an_error(), {}, DAY - 2 * HOUR));
  for (int i = 0; i < 5; ++i) EXPECT_FALSE(reporter.report(an_error(), {}, DAY - HOUR));  // 23:00 UTC
  // the first of the new day comes before any flush has run after midnight
  ASSERT_TRUE(reporter.report(an_error(), {}, DAY + 60'000));
  for (int i = 0; i < 2; ++i) EXPECT_FALSE(reporter.report(an_error(), {}, DAY + 120'000));
  const auto early = reporter.flush_groups(DAY + 180'000);
  ASSERT_EQ(early.size(), 1u) << "the five of yesterday leave now, the two of today wait for their own aggregate time";
  EXPECT_EQ(early[0].props["count"], 5);
  const auto later = reporter.flush_groups(2 * DAY + 180'000);
  ASSERT_EQ(later.size(), 1u);
  EXPECT_EQ(later[0].props["count"], 2);
}

TEST(ErrorDaily, ThePageErrorsAreCountedPerVersionAndAShapedCountIsAdded) {
  error_reporter_t reporter;
  reporter.set_aggregate_ms(DAY);
  const auto props = [](int count) { return json { { "component", "panel" }, { "fingerprint", "abc123" }, { "code", "E_PAGE" }, { "severity", "error" }, { "count", count } }; };
  ASSERT_TRUE(reporter.admit("panel", "1.2.3", props(1), 1000));
  EXPECT_FALSE(reporter.admit("panel", "1.2.3", props(7), 2000));  // a page that counted seven of them itself
  EXPECT_FALSE(reporter.admit("panel", "1.2.4", props(1), 3000));  // the page was updated meanwhile
  const auto counts = reporter.flush_groups(DAY + 1);
  ASSERT_EQ(counts.size(), 2u);
  std::map<std::string, std::int64_t> by_version;
  for (const auto &c : counts) by_version[c.version] = c.props["count"].get<std::int64_t>();
  EXPECT_EQ(by_version["1.2.3"], 7);
  EXPECT_EQ(by_version["1.2.4"], 1);
}

TEST(ErrorDaily, WhenTheHostStopsTheCountsLeaveEvenWhenTheHourlyBoundIsUsedUp) {
  error_reporter_t reporter;
  reporter.set_aggregate_ms(DAY);
  for (int i = 0; i < error_reporter_t::MAX_PER_HOUR + 5; ++i) reporter.report(an_error(("E_KIND_" + std::to_string(i)).c_str()), {}, 1000 + i);
  EXPECT_FALSE(reporter.report(an_error("E_KIND_0"), {}, 5000).has_value());  // a repeat of a kind that went out
  EXPECT_FALSE(reporter.report(an_error("E_KIND_30"), {}, 5000).has_value());  // a kind that found the hour used up: counted
  EXPECT_TRUE(reporter.flush_groups(6000).empty());
  const auto all = reporter.flush_groups(6000, true);
  EXPECT_GE(all.size(), 2u) << "nothing is lost to the bound when the host stops";
}

TEST(ErrorDaily, AtMostOneFullReportAKindAndEveryKindOfTheDayIsRememberedAcrossARestart) {
  error_reporter_t first_run;
  // the hourly bound admits 20 kinds an hour; over eleven hours that is more than 200 kinds in one day
  int sent = 0;
  for (int hour = 0; hour < 11; ++hour)
    for (int k = 0; k < 20; ++k)
      if (first_run.report(an_error(("E_" + std::to_string(hour) + "_" + std::to_string(k)).c_str()), {}, hour * HOUR + k)) ++sent;
  EXPECT_EQ(sent, 220);
  const auto state = first_run.state(11 * HOUR);
  EXPECT_EQ(state["keys"].size(), 220u);
  error_reporter_t second_run;
  second_run.restore(state, 11 * HOUR + 1);
  int again = 0;
  for (int hour = 0; hour < 11; ++hour)
    for (int k = 0; k < 20; ++k)
      if (second_run.report(an_error(("E_" + std::to_string(hour) + "_" + std::to_string(k)).c_str()), {}, 12 * HOUR + hour * 1000 + k)) ++again;
  EXPECT_EQ(again, 0) << "every kind that was sent in full today is still known after the restart";
}

TEST(ReportDelivery, TakingTheErrorChoiceBackAlsoForgetsWhatWasSentInFullOnTheDisk) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.svc->report_error(an_error());
  h.svc->flush();
  ASSERT_TRUE(fs::exists(h.dir.path / "error_days.json"));
  h.consent = { true, false };
  h.svc->consent_changed();
  EXPECT_FALSE(fs::exists(h.dir.path / "error_days.json"));
  // switched on again and the host restarted the same day: the error is sent in full again
  h.consent = { true, true };
  h.restart();
  h.svc->report_error(an_error());
  h.svc->flush();
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_TRUE(h.posted.back()["events"][0]["props"].contains("frames"));
}

TEST(ReportDelivery, TheDedupeMarkIsOnlyWrittenWhenTheReportReachedTheDisk) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  harness_t h(id);
  h.svc->report_error(an_error());
  // the queue directory cannot be written: the report stays in the buffer and the day's mark is not saved
  fs::remove_all(h.dir.path / "queue");
  { std::ofstream(h.dir.path / "queue") << "not a directory"; }
  h.svc->flush();
  EXPECT_EQ(h.svc->buffered(), 1u);
  EXPECT_FALSE(fs::exists(h.dir.path / "error_days.json"));
}

TEST(ReportDelivery, RoutineBatchesWaitWhileMoreErrorsThanOnePassCarriesAreStillWaiting) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  service_options_t options;
  options.max_batches_per_pump = 1;
  harness_t h(id, options);
  h.now = slot::slot_ms(id, 20000) + 1000;
  h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
  h.svc->flush();
  for (int i = 0; i < 2; ++i) {
    h.svc->report_error(an_error(("E_" + std::to_string(i)).c_str()));
    h.svc->flush();
  }
  EXPECT_EQ(h.svc->pump(), 1);
  ASSERT_EQ(h.posted.size(), 1u);
  EXPECT_EQ(h.posted[0]["events"][0]["name"], "error_report") << "one error batch left and one is still waiting: the usage batch does not go first";
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted[1]["events"][0]["name"], "error_report");
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted[2]["events"][0]["name"], "app_start");
}

TEST(ReportDelivery, WhatWaitsForTheAggregateIntervalLeavesWithTheShareOfTheDay) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  service_options_t options;
  options.aggregate_interval_ms = 6 * HOUR;
  harness_t h(id, options);
  ASSERT_TRUE(h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } }));
  h.svc->flush();  // into the aggregate buffer: six hours have not passed
  EXPECT_EQ(h.svc->queued_files(), 0u);
  h.now = slot::slot_ms(id, 20000) + 1000;
  EXPECT_EQ(h.svc->pump(), 1);
  EXPECT_EQ(h.posted_names().count("app_start"), 1u) << "the day is not settled before the held records were written and sent";
  EXPECT_EQ(h.svc->plan().routine_day_done, 20000);
}

TEST(ReportDelivery, ThreeFailuresMustFollowEachOtherToPutTheShareOffToTheNextDay) {
  const auto id = id_with_offset(8 * HOUR, 16 * HOUR);
  service_options_t options;
  options.max_batches_per_pump = 1;
  harness_t h(id, options);
  h.now = slot::slot_ms(id, 20000) + 1000;
  for (int i = 0; i < 3; ++i) {
    h.svc->emit(kind_e::usage, "app_start", json { { "cold", true } });
    h.svc->flush();
  }
  ASSERT_EQ(h.svc->queued_files(), 3u);
  // 503, 200, 503, 200, 503: no three in a row
  const int answers[] = { 503, 200, 503, 200, 503 };
  for (const int status : answers) {
    h.next_status = status;
    h.svc->pump();
    h.now += 2 * HOUR;
  }
  h.next_status = 200;
  EXPECT_EQ(h.svc->pump(), 1) << "the last batch still leaves the same day";
  EXPECT_EQ(h.svc->queued_files(), 0u);
}

TEST(DeliveryQueue, AFeedbackReportOrAnErrorThatTheCapsTookOutAtOnceWasNotKept) {
  temp_dir_t dir;
  queue_limits_t limits;
  limits.max_bytes = 200;
  disk_queue_t q(dir.path, limits);
  auto big = batch_of(1);
  big["pad"] = std::string(1000, 'x');
  EXPECT_FALSE(q.push(big, 0, delivery_e::feedback)) << "the caller keeps it and tries again";
  EXPECT_FALSE(q.push(big, 0, delivery_e::error));
  EXPECT_EQ(q.files(), 0u);
  EXPECT_TRUE(q.push(big, 2)) << "a routine batch is simply given up, as before";
}

TEST(DeliveryQueue, BatchesAnEarlierVersionWroteWithAFeedbackReportOrAnErrorAreGivenTheirClassAtOpen) {
  temp_dir_t dir;
  const auto write = [&](const char *name, const json &events) {
    std::ofstream(dir.path / name) << json { { "install_id", "x" }, { "events", events } }.dump();
  };
  write("0000000000001-000001-p0.json", json::array({ json { { "kind", "error" }, { "name", "user_report" } } }));
  write("0000000000002-000002-p0.json", json::array({ json { { "kind", "error" }, { "name", "error_report" } } }));
  write("0000000000003-000003-p0.json", json::array({ json { { "kind", "experience" }, { "name", "stream_health" } } }));
  write("0000000000004-000004-p2.json", json::array({ json { { "kind", "usage" }, { "name", "app_start" } } }));
  disk_queue_t q(dir.path);
  EXPECT_EQ(q.files_of(delivery_e::feedback), 1u);
  EXPECT_EQ(q.files_of(delivery_e::error), 1u);
  EXPECT_EQ(q.files_of(delivery_e::routine), 2u);
  EXPECT_EQ(q.front(2, lane_e::urgent)->batch["events"][0]["name"], "user_report");
}

TEST(ReportDelivery, WhileTheServerAsksForAWaitBatchesOfAnotherWeightAreNotMergedIntoOne) {
  const auto id = id_sampled(0.1, true);
  service_options_t options;
  options.sample_rate = 0.1;
  harness_t h(id, options);
  // a stream summary (weighted) and a failed stream start (not sampled) of one run: both priority 0, both routine
  h.svc->emit(kind_e::experience, "stream_failed", json { { "stage", "encoder_init" }, { "code", "E_X" } });
  h.svc->flush();
  h.svc->emit(kind_e::experience, "stream_health", json::object());
  h.svc->flush();
  h.next_body = json { { "cfg", json { { "backoff_s", 600 } } } }.dump();
  h.svc->report_error(an_error());
  h.svc->flush();
  ASSERT_EQ(h.svc->pump(), 1);  // the answer asks for a wait
  h.next_body.clear();
  h.posted.clear();
  h.now += 61'000;
  h.svc->pump();
  for (const auto &batch : h.posted)
    for (const auto &e : batch["events"])
      if (e["name"] == "stream_failed") EXPECT_FALSE(batch.contains("sample_weight")) << "a failure counts for itself";
}
