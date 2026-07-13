/**
 * @file tests/unit/test_stream_session_observability.cpp
 * @brief Tests for active streaming-session telemetry values.
 */

#include "src/stream.h"

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
  EXPECT_STREQ(stop_reason_name(stop_reason_e::host_terminate), "host_terminate");
}
