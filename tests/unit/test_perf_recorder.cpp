#include "src/perf_recorder.h"
#include "src/file_handler.h"
#include "src/thread_safe.h"

#include <gtest/gtest.h>
#include <limits>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
  nlohmann::json session_snapshot(std::uint32_t id) {
    const auto snapshot = perf::current_snapshot_json();
    for (const auto &session : snapshot["sessions"]) {
      if (session["session_id"] == id) return session;
    }
    return {};
  }
}

TEST(PyrowaveProbe, EndExportsValidJsonOnceAndOmitsClientName) {
  constexpr std::uint32_t id = 71005;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() / ("sunshine-probe-unit-" + std::to_string(stamp));
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  perf::begin_session({ .session_id = id, .client_name = "private-client", .probe_enabled = true, .probe_directory = file_handler::path_to_utf8(directory) });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_wait_ms = 25.0, .send_queue_frames = 2.0 });
  perf::end_session(id);
  perf::end_session(id);
  std::vector<std::filesystem::path> files;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) files.push_back(entry.path());
  ASSERT_EQ(files.size(), 1);
  std::ifstream input(files.front());
  const auto report = nlohmann::json::parse(input);
  EXPECT_EQ(report["role"], "sunshine");
  EXPECT_FALSE(report.contains("client_name"));
  EXPECT_EQ(report["pyrowave_probe"]["send_queue_depth"]["last_frames"], 2.0);
  input.close();
  EXPECT_TRUE(std::filesystem::remove(files.front()));
  EXPECT_TRUE(std::filesystem::remove(directory));
}

TEST(PyrowaveProbe, DisabledSessionDoesNotCollectProbeData) {
  constexpr std::uint32_t id = 71001;
  perf::begin_session({ .session_id = id });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_frames = 5.0 });
  EXPECT_FALSE(session_snapshot(id).contains("pyrowave_probe"));
  perf::end_session(id);
}

TEST(PyrowaveProbe, ZeroDepthIsObservedAndUnavailableTimingStaysNull) {
  constexpr std::uint32_t id = 71002;
  perf::begin_session({ .session_id = id, .probe_enabled = true });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_frames = 0.0 });
  const auto probe = session_snapshot(id)["pyrowave_probe"];
  EXPECT_EQ(probe["send_queue_depth"]["total_samples"], 1);
  EXPECT_EQ(probe["send_queue_depth"]["last_frames"], 0.0);
  EXPECT_TRUE(probe["gpu_completion_wait"]["avg_ms"].is_null());
  perf::end_session(id);
}

TEST(PyrowaveProbe, CapturesGrowingQueueAndIgnoresInvalidSamples) {
  constexpr std::uint32_t id = 71003;
  perf::begin_session({ .session_id = id, .probe_enabled = true });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_wait_ms = 10.0, .send_queue_frames = 1.0 });
  perf::record_pyrowave_probe(id, { .frame_id = 2, .send_queue_wait_ms = 50.0, .send_queue_frames = 5.0 });
  perf::record_pyrowave_probe(id, { .frame_id = 3, .send_queue_wait_ms = std::numeric_limits<double>::quiet_NaN(), .send_queue_frames = -1.0 });
  const auto probe = session_snapshot(id)["pyrowave_probe"];
  EXPECT_EQ(probe["send_queue_wait"]["total_samples"], 2);
  EXPECT_EQ(probe["send_queue_wait"]["lifetime_max_ms"], 50.0);
  EXPECT_EQ(probe["send_queue_depth"]["series_frames"], nlohmann::json::array({ 1.0, 5.0 }));
  perf::end_session(id);
  perf::record_pyrowave_probe(id, { .frame_id = 4, .send_queue_frames = 100.0 });
  EXPECT_EQ(session_snapshot(id)["pyrowave_probe"]["send_queue_depth"]["total_samples"], 2);
}

TEST(PyrowaveProbe, CaptureMailboxAndBroadcastQueueKeepSeparateScopes) {
  constexpr std::uint32_t id = 71006;
  perf::begin_session({ .session_id = id, .probe_enabled = true });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .capture_pending_frames = 1.0, .capture_mailbox_overwrites = 4 });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_frames = 1.0, .shared_queue_discards = 7 });
  const auto probe = session_snapshot(id)["pyrowave_probe"];
  EXPECT_EQ(probe["capture_pending_depth"]["last_frames"], 1.0);
  EXPECT_EQ(probe["send_queue_depth"]["last_frames"], 1.0);
  EXPECT_EQ(probe["capture_mailbox_overwrites_total"], 4);
  EXPECT_EQ(probe["shared_queue_discards_total"], 7);
  perf::end_session(id);
}

TEST(PyrowaveProbe, TimelineAndSampleWindowsStayBounded) {
  constexpr std::uint32_t id = 71004;
  perf::begin_session({ .session_id = id, .probe_enabled = true });
  const auto start = std::chrono::steady_clock::now();
  for (int index = 0; index < 605; ++index) {
    perf::record_pyrowave_probe(id, { .frame_id = index + 1, .send_queue_frames = static_cast<double>(index) }, start + std::chrono::seconds(index));
  }
  const auto probe = session_snapshot(id)["pyrowave_probe"];
  EXPECT_EQ(probe["timeline"].size(), 600);
  EXPECT_EQ(probe["omitted_seconds"], 5);
  EXPECT_EQ(probe["send_queue_depth"]["samples"], 240);
  EXPECT_EQ(probe["send_queue_depth"]["total_samples"], 605);
  perf::end_session(id);
}

TEST(PyrowaveProbe, ConcurrentStageTimestampsShareChronologicalBuckets) {
  constexpr std::uint32_t id = 71007;
  perf::begin_session({ .session_id = id, .probe_enabled = true });
  const auto start = std::chrono::steady_clock::now();
  perf::record_pyrowave_probe(id, { .frame_id = 2, .capture_pending_frames = 1.0 }, start + std::chrono::seconds(2));
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_frames = 2.0 }, start + std::chrono::seconds(1));
  perf::record_pyrowave_probe(id, { .frame_id = 2, .send_queue_frames = 3.0 }, start + std::chrono::seconds(2));
  const auto timeline = session_snapshot(id)["pyrowave_probe"]["timeline"];
  ASSERT_EQ(timeline.size(), 2);
  EXPECT_LT(timeline[0]["second"].get<int>(), timeline[1]["second"].get<int>());
  EXPECT_EQ(timeline[1]["capture_pending_peak_frames"], 1.0);
  EXPECT_EQ(timeline[1]["send_queue_peak_frames"], 3.0);
  perf::end_session(id);
}

TEST(PyrowaveProbe, SnapshotOwnsSamplesAndKeepsCompletedSessionData) {
  constexpr std::uint32_t id = 71008;
  perf::begin_session({ .session_id = id, .bitrate_kbps = 10000, .probe_enabled = true });
  perf::record_pyrowave_probe(id, { .frame_id = 1, .send_queue_frames = 1.0 });
  perf::record_pipeline_sample(id, { .encode_ms = 2.0, .pyrowave = true });
  const auto first = session_snapshot(id);
  perf::update_session_bitrate(id, 20000);
  perf::record_pyrowave_probe(id, { .frame_id = 2, .send_queue_frames = 3.0 });
  perf::end_session(id);
  const auto completed = session_snapshot(id);
  EXPECT_TRUE(first["active"]);
  EXPECT_EQ(first["bitrate_kbps"], 10000);
  EXPECT_EQ(first["pyrowave_probe"]["send_queue_depth"]["series_frames"], nlohmann::json::array({ 1.0 }));
  EXPECT_FALSE(completed["active"]);
  EXPECT_EQ(completed["bitrate_kbps"], 20000);
  EXPECT_EQ(completed["pyrowave_probe"]["metadata_scope"], "latest_session_configuration_not_per_frame");
  EXPECT_EQ(completed["pyrowave_probe"]["send_queue_depth"]["series_frames"], nlohmann::json::array({ 1.0, 3.0 }));
  EXPECT_EQ(completed["pipeline"]["encode"]["last_ms"], 2.0);
  EXPECT_EQ(completed["pyrowave"]["frames"], 1);
}

TEST(PyrowaveProbe, QueueSnapshotCountsOverflowDiscardWithoutChangingPolicy) {
  safe::queue_t<int> queue(2);
  queue.raise(1);
  queue.raise(2);
  EXPECT_EQ(queue.snapshot().pending, 2);
  queue.raise(3);
  const auto state = queue.snapshot();
  EXPECT_EQ(state.pending, 1);
  EXPECT_EQ(state.capacity, 2);
  EXPECT_EQ(state.discarded, 2);
  const auto value = queue.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 3);
  queue.stop();
  queue.raise(4);
  EXPECT_EQ(queue.snapshot().pending, 0);
}

TEST(PyrowaveProbe, CaptureMailboxKeepsLatestFrameAndCountsReplacement) {
  safe::event_t<int> mailbox;
  mailbox.raise(1);
  mailbox.raise(2);
  EXPECT_EQ(mailbox.snapshot().pending, 1);
  EXPECT_EQ(mailbox.snapshot().overwritten, 1);
  const auto value = mailbox.pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 2);
  EXPECT_EQ(mailbox.snapshot().pending, 0);
  mailbox.raise(3);
  EXPECT_EQ(mailbox.snapshot().overwritten, 1);
  mailbox.stop();
}
