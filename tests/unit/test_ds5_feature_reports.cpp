/**
 * @file tests/unit/test_ds5_feature_reports.cpp
 * @brief The calibration feature report a game reads before it trusts the pad's motion data.
 */
#include "src/ds5/feature_reports.h"

#include <gtest/gtest.h>

#include <string>

#include <array>
#include <cmath>

namespace {
  /* What the host sent before: the report id and zeros. */
  std::array<std::uint8_t, ds5::feature::calibration_bytes>
  earlier_report() {
    std::array<std::uint8_t, ds5::feature::calibration_bytes> report {};
    report[0] = 0x05;
    return report;
  }

  TEST(Ds5FeatureCalibration, TheEarlierAllZeroReportMakesTheMotionUnusableForAGame) {
    const auto check = ds5::feature::check_calibration(earlier_report().data(), ds5::feature::calibration_bytes);
    EXPECT_FALSE(check.usable);
    EXPECT_FALSE(std::isfinite(check.gyro_sensitivity[0]));  // 0 / 0
    EXPECT_FALSE(std::isfinite(check.accel_sensitivity[0]));  // x / 0
  }

  TEST(Ds5FeatureCalibration, TheReportDescribesTheCountsTheHostWrites) {
    const auto report = ds5::feature::calibration_report();
    const auto check = ds5::feature::check_calibration(report.data(), report.size());
    EXPECT_TRUE(check.crc_ok);
    EXPECT_TRUE(check.usable);
    for (int axis = 0; axis < 3; ++axis) {
      EXPECT_EQ(check.gyro_bias[axis], 0);
      EXPECT_EQ(check.accel_bias[axis], 0);
      // Calibrated gyro counts are 1/1024 degree per second: 16.4 counts per dps * sensitivity = 1024.
      EXPECT_NEAR(ds5::feature::gyro_counts_per_dps * check.gyro_sensitivity[axis], 1024.0f, 1.0f);
      EXPECT_NEAR(check.accel_sensitivity[axis], 1.0f, 1e-6f);
    }
  }

  TEST(Ds5FeatureCalibration, ARotationOf90DegreesPerSecondReadsBackAs90) {
    const auto report = ds5::feature::calibration_report();
    const auto check = ds5::feature::check_calibration(report.data(), report.size());
    const auto raw = static_cast<int>(std::lround(90.0f * ds5::feature::gyro_counts_per_dps));
    const float degrees_per_second = static_cast<float>(raw - check.gyro_bias[0]) * check.gyro_sensitivity[0] / 1024.0f;
    EXPECT_NEAR(degrees_per_second, 90.0f, 0.2f);
  }

  TEST(Ds5FeatureCalibration, TheCrcCoversTheSeedAndTheReport) {
    auto report = ds5::feature::calibration_report();
    report[5] ^= 0x01;
    EXPECT_FALSE(ds5::feature::check_calibration(report.data(), report.size()).crc_ok);
    // The seed is the first byte of the CRC input: CRC32 of "123456789" is 0xcbf43926.
    const std::uint8_t rest[] = { '2', '3', '4', '5', '6', '7', '8', '9' };
    EXPECT_EQ(ds5::feature::detail::crc32_seeded('1', rest, sizeof rest), 0xcbf43926u);
  }
}  // namespace

TEST(Ds5FirmwareReport, CarriesTheVersionsOfARealPad) {
  const auto report = ds5::feature::firmware_report();
  EXPECT_EQ(report[0], 0x20);
  EXPECT_EQ(std::string(report.begin() + 1, report.begin() + 12), "Jul  4 2025");
  EXPECT_EQ(std::string(report.begin() + 12, report.begin() + 20), "10:10:32");
  const auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(report[at]) | (report[at + 1] << 8) | (report[at + 2] << 16) | (static_cast<std::uint32_t>(report[at + 3]) << 24); };
  EXPECT_EQ(u32(24), 0x00000711u);
  EXPECT_EQ(u32(28), 0x0110002au);
  EXPECT_EQ(report[44] | (report[45] << 8), 0x0630);
  EXPECT_TRUE(ds5::feature::feature_crc_valid(report.data(), report.size()));
  EXPECT_EQ(u32(60), 0x7f7a7ed2u);
}

TEST(Ds5FirmwareReport, TheCrcSeedIsTheFeatureSeed) {
  // The real pad's 0x09 answer, whose last four bytes are the CRC of the first sixteen.
  const std::array<std::uint8_t, 20> real = { 0x09, 0x51, 0xa9, 0xb7, 0x9a, 0x3a, 0x14, 0x08, 0x25, 0x00, 0xce, 0x7f, 0x67, 0x89, 0x4d, 0xf8, 0x61, 0xfe, 0x80, 0x58 };
  EXPECT_TRUE(ds5::feature::feature_crc_valid(real.data(), real.size()));
}

TEST(Ds5FirmwareUpdateClass, IsRefusedAndTheOthersAreNot) {
  for (std::uint8_t id : { 0xa0, 0xe0, 0xf0, 0xf1, 0xf2, 0xf4, 0xf5 }) EXPECT_TRUE(ds5::feature::is_firmware_update_report(id)) << int(id);
  for (std::uint8_t id : { 0x05, 0x08, 0x09, 0x20, 0x21, 0x22, 0x80, 0x81, 0x85 }) EXPECT_FALSE(ds5::feature::is_firmware_update_report(id)) << int(id);
}

TEST(Ds5FeaturePolicy, UnknownSetReportsAreIgnoredAndNeverForwarded) {
  EXPECT_EQ(ds5::feature::feature_set_action_for(0x80), ds5::feature::feature_set_action::ignore);
  EXPECT_EQ(ds5::feature::feature_set_action_for(0x81), ds5::feature::feature_set_action::ignore);
  EXPECT_TRUE(ds5::feature::is_public_get_only_feature_report(0x05));
  EXPECT_TRUE(ds5::feature::is_public_get_only_feature_report(0x09));
  EXPECT_TRUE(ds5::feature::is_public_get_only_feature_report(0x20));
  EXPECT_FALSE(ds5::feature::is_public_get_only_feature_report(0x80));
  EXPECT_FALSE(ds5::feature::is_public_get_only_feature_report(0x81));
  for (const auto id : { 0xa0, 0xe0, 0xf0, 0xf1, 0xf2, 0xf4, 0xf5 }) {
    EXPECT_EQ(ds5::feature::feature_set_action_for(id), ds5::feature::feature_set_action::refuse) << int(id);
  }
}
