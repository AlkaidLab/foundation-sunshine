/**
 * @file tests/unit/test_abr_reported_loss.cpp
 * @brief Read-only access to the latest client-reported loss kept by the ABR module.
 */
#include <gtest/gtest.h>

#include "src/streaming/abr.h"

namespace {
  abr::config_t
  enabled_config() {
    abr::config_t cfg;
    cfg.enabled = true;
    cfg.min_bitrate_kbps = 2000;
    cfg.max_bitrate_kbps = 50000;
    return cfg;
  }
}  // namespace

TEST(AbrReportedLoss, UnknownClientHasNoValue) {
  EXPECT_FALSE(abr::latest_reported_loss("abr-loss-unknown-client").has_value());
}

TEST(AbrReportedLoss, NoValueBeforeTheFirstReport) {
  const std::string client = "abr-loss-no-report";
  abr::enable(client, enabled_config(), 20000, "Test Game");

  EXPECT_FALSE(abr::latest_reported_loss(client).has_value());

  abr::cleanup(client);
}

TEST(AbrReportedLoss, ReturnsTheLatestReport) {
  const std::string client = "abr-loss-latest";
  abr::enable(client, enabled_config(), 20000, "Test Game");

  abr::process_feedback(client, { .packet_loss = 3.5, .rtt_ms = 10.0, .decode_fps = 60.0, .dropped_frames = 4, .current_bitrate_kbps = 20000 });
  auto first = abr::latest_reported_loss(client);
  ASSERT_TRUE(first.has_value());
  EXPECT_DOUBLE_EQ(first->packet_loss, 3.5);
  EXPECT_EQ(first->dropped_frames, 4);

  abr::process_feedback(client, { .packet_loss = 0.0, .rtt_ms = 10.0, .decode_fps = 60.0, .dropped_frames = 0, .current_bitrate_kbps = 20000 });
  auto second = abr::latest_reported_loss(client);
  ASSERT_TRUE(second.has_value());
  EXPECT_DOUBLE_EQ(second->packet_loss, 0.0);
  EXPECT_EQ(second->dropped_frames, 0);

  abr::cleanup(client);
}

TEST(AbrReportedLoss, ClearedWhenAbrIsEnabledAgainAndOnCleanup) {
  const std::string client = "abr-loss-reset";
  abr::enable(client, enabled_config(), 20000, "Test Game");
  abr::process_feedback(client, { .packet_loss = 1.0, .rtt_ms = 10.0, .decode_fps = 60.0, .dropped_frames = 1, .current_bitrate_kbps = 20000 });
  ASSERT_TRUE(abr::latest_reported_loss(client).has_value());

  abr::enable(client, enabled_config(), 20000, "Test Game");
  EXPECT_FALSE(abr::latest_reported_loss(client).has_value());

  abr::process_feedback(client, { .packet_loss = 1.0, .rtt_ms = 10.0, .decode_fps = 60.0, .dropped_frames = 1, .current_bitrate_kbps = 20000 });
  abr::cleanup(client);
  EXPECT_FALSE(abr::latest_reported_loss(client).has_value());
}

TEST(AbrReportedLoss, OutOfRangeReportIsClamped) {
  const std::string client = "abr-loss-clamp";
  abr::enable(client, enabled_config(), 20000, "Test Game");
  abr::process_feedback(client, { .packet_loss = 250.0, .rtt_ms = 10.0, .decode_fps = 60.0, .dropped_frames = -3, .current_bitrate_kbps = 20000 });

  auto value = abr::latest_reported_loss(client);
  ASSERT_TRUE(value.has_value());
  EXPECT_DOUBLE_EQ(value->packet_loss, 100.0);
  EXPECT_EQ(value->dropped_frames, 0);

  abr::cleanup(client);
}
