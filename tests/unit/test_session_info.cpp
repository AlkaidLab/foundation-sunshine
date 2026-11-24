/**
 * @file tests/unit/test_session_info.cpp
 * @brief Codec naming and the loss/codec keys of the session JSON.
 */
#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include "src/session_info.h"

TEST(SessionInfoCodec, NamesEveryVideoFormat) {
  EXPECT_EQ(stream::codec_name(0), "h264");
  EXPECT_EQ(stream::codec_name(1), "hevc");
  EXPECT_EQ(stream::codec_name(2), "av1");
  EXPECT_EQ(stream::codec_name(0x00010000), "pyrowave");
}

TEST(SessionInfoCodec, UnknownFormatHasNoName) {
  EXPECT_EQ(stream::codec_name(-1), "");
  EXPECT_EQ(stream::codec_name(3), "");
  EXPECT_EQ(stream::codec_name(0x00010001), "");
}

TEST(SessionInfoJson, UnreportedLossIsNull) {
  stream::session_info_t info {};
  info.video_format = 1;
  info.codec = stream::codec_name(info.video_format);

  nlohmann::json json;
  stream::add_session_quality_json(json, info);

  EXPECT_EQ(json["video_format"], 1);
  EXPECT_EQ(json["codec"], "hevc");
  ASSERT_TRUE(json.contains("packet_loss"));
  ASSERT_TRUE(json.contains("dropped_frames"));
  EXPECT_TRUE(json["packet_loss"].is_null());
  EXPECT_TRUE(json["dropped_frames"].is_null());
}

TEST(SessionInfoJson, UnknownFormatKeepsTheKeys) {
  stream::session_info_t info {};

  nlohmann::json json;
  stream::add_session_quality_json(json, info);

  EXPECT_EQ(json["video_format"], -1);
  EXPECT_EQ(json["codec"], "");
  EXPECT_TRUE(json["packet_loss"].is_null());
  EXPECT_TRUE(json["dropped_frames"].is_null());
}

TEST(SessionInfoJson, ReportedLossIsNumeric) {
  stream::session_info_t info {};
  info.video_format = 2;
  info.codec = stream::codec_name(info.video_format);
  info.packet_loss = 1.5;
  info.dropped_frames = 7;

  nlohmann::json json;
  stream::add_session_quality_json(json, info);

  EXPECT_EQ(json["codec"], "av1");
  ASSERT_TRUE(json["packet_loss"].is_number());
  EXPECT_DOUBLE_EQ(json["packet_loss"].get<double>(), 1.5);
  ASSERT_TRUE(json["dropped_frames"].is_number_integer());
  EXPECT_EQ(json["dropped_frames"], 7);
}

TEST(SessionInfoJson, ZeroLossIsReportedNotNull) {
  stream::session_info_t info {};
  info.packet_loss = 0.0;
  info.dropped_frames = 0;

  nlohmann::json json;
  stream::add_session_quality_json(json, info);

  EXPECT_FALSE(json["packet_loss"].is_null());
  EXPECT_DOUBLE_EQ(json["packet_loss"].get<double>(), 0.0);
  EXPECT_FALSE(json["dropped_frames"].is_null());
  EXPECT_EQ(json["dropped_frames"], 0);
}

TEST(SessionInfoJson, GamepadKeysAreAlwaysPresent) {
  stream::session_info_t info {};

  nlohmann::json json;
  stream::add_session_gamepad_json(json, info);

  ASSERT_TRUE(json.contains("gamepad_type"));
  ASSERT_TRUE(json.contains("gamepad_dualsense_identity"));
  EXPECT_EQ(json["gamepad_type"], "");
  EXPECT_FALSE(json["gamepad_dualsense_identity"].get<bool>());
}

TEST(SessionInfoJson, DualShock4RunIsReportedWithTypeOnly) {
  stream::session_info_t info {};
  info.gamepad_type = "ds4";

  nlohmann::json json;
  stream::add_session_gamepad_json(json, info);

  EXPECT_EQ(json["gamepad_type"], "ds4");
  EXPECT_FALSE(json["gamepad_dualsense_identity"].get<bool>());
}

TEST(SessionInfoJson, IdentifiedDualSenseKeepsItsIdentityFlag) {
  stream::session_info_t info {};
  info.gamepad_type = "ds4";
  info.gamepad_dualsense_identity = true;

  nlohmann::json json;
  stream::add_session_gamepad_json(json, info);

  EXPECT_EQ(json["gamepad_type"], "ds4");
  EXPECT_TRUE(json["gamepad_dualsense_identity"].get<bool>());
}
