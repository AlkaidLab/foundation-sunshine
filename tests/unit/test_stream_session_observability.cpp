/**
 * @file tests/unit/test_stream_session_observability.cpp
 * @brief Tests for active streaming-session telemetry values.
 */

#include <atomic>
#include <thread>

#include <nlohmann/json.hpp>

#include "src/stream.h"
#include "src/nvhttp/sessions.h"

#include "../tests_common.h"

TEST(StreamSessionObservability, NamesEveryStopReason) {
  using stream::session::stop_reason_e;
  using stream::session::stop_reason_name;

  EXPECT_STREQ(stop_reason_name(stop_reason_e::none), "none");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::control_disconnect), "control_disconnect");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::control_timeout), "control_timeout");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::protocol_error), "protocol_error");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::video_ended), "video_ended");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::audio_ended), "audio_ended");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::client_cancel), "client_cancel");
  EXPECT_STREQ(stop_reason_name(stop_reason_e::host_terminate), "host_terminate");
}

TEST(StreamSessionObservability, PreservesFirstStopReason) {
  using stream::session::lifecycle_t;
  using stream::session::state_e;
  using stream::session::stop_reason_e;

  lifecycle_t lifecycle { state_e::RUNNING };

  EXPECT_TRUE(lifecycle.request_stop(stop_reason_e::control_disconnect));
  EXPECT_FALSE(lifecycle.request_stop(stop_reason_e::video_ended));

  const auto snapshot = lifecycle.snapshot();
  EXPECT_EQ(snapshot.state, state_e::STOPPING);
  EXPECT_EQ(snapshot.stop_reason, stop_reason_e::control_disconnect);
}

TEST(StreamSessionObservability, PublishesOneConcurrentStopReason) {
  using stream::session::lifecycle_t;
  using stream::session::state_e;
  using stream::session::stop_reason_e;

  lifecycle_t lifecycle { state_e::RUNNING };
  std::atomic<int> winners { 0 };
  std::thread control_stop([&]() {
    winners += lifecycle.request_stop(stop_reason_e::control_disconnect);
  });
  std::thread video_stop([&]() {
    winners += lifecycle.request_stop(stop_reason_e::video_ended);
  });

  control_stop.join();
  video_stop.join();

  const auto snapshot = lifecycle.snapshot();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(snapshot.state, state_e::STOPPING);
  EXPECT_NE(snapshot.stop_reason, stop_reason_e::none);
}

TEST(StreamSessionObservability, SerializesSessionBoundaries) {
  stream::session_info_t info {};
  info.stop_reason = "control_disconnect";
  info.uptime_ms = 123;
  info.control_connected = false;
  info.control_idle_ms = -1;
  info.video_idle_ms = -1;
  info.audio_idle_ms = -1;

  const auto json = nvhttp::sessions::make_session_json(info);

  EXPECT_EQ(json["stop_reason"], "control_disconnect");
  EXPECT_EQ(json["uptime_ms"], 123);
  EXPECT_EQ(json["control_connected"], false);
  EXPECT_TRUE(json["control_idle_ms"].is_null());
  EXPECT_TRUE(json["video_idle_ms"].is_null());
  EXPECT_TRUE(json["audio_idle_ms"].is_null());
}
