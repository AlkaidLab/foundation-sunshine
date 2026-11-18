/**
 * @file src/ds5/feature_reports.h
 * @brief Feature report 0x05 (calibration) of the virtual DualSense.
 *
 * A game that applies the pad's own calibration (SDL's PS5 driver and Sony's libraries do) divides
 * by the plus/minus ranges of this report. An all-zero report makes every sensitivity NaN or
 * infinite, and the gyroscope and accelerometer read as garbage or nothing, although the raw
 * input report carries correct counts. The values below describe a pad whose motion counts are the
 * ones the host writes (16.4 counts per degree per second, 8192 counts per g, no bias):
 *
 *   gyro bias 0, plus/minus +-8856 (540 deg/s), speed plus/minus 540 deg/s, so the calibrated
 *   resolution is 2 * 540 * 1024 / 17712 = 62.44 counts per count, which turns raw counts into
 *   1/1024 degree per second, the unit of the Linux driver and of SDL;
 *   accelerometer plus/minus +-8192 (1 g), bias 0, sensitivity 1.
 *
 * Layout (hid-playstation dualsense_get_calibration_data and SDL_hidapi_ps5.c): report id, then
 * little-endian int16 at bytes 1..34, two reserved bytes, and the CRC32 of the report with the
 * 0x53 seed prefix in bytes 37..40.
 */
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace ds5::feature {
  /* Authority: ds5-linux-hid-playstation-20261107, ds5-sdl-hidapi-ps5-20261107,
   * and ds5-dualsensectl-main-20261107. Those public readers define 0x05, 0x09,
   * and 0x20 as GET-only information reports; none defines 0x80 or 0x81. */
  constexpr bool
  is_public_get_only_feature_report(std::uint8_t id) {
    return id == 0x05 || id == 0x09 || id == 0x20;
  }

  constexpr std::size_t calibration_bytes = 41;
  constexpr float gyro_counts_per_dps = 16.4f;
  constexpr float accel_counts_per_g = 8192.0f;

  namespace detail {
    inline std::uint32_t
    crc32_seeded(std::uint8_t seed, const std::uint8_t *data, std::size_t length) {
      std::uint32_t crc = 0xffffffffu;
      const auto feed = [&crc](std::uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
      };
      feed(seed);
      for (std::size_t index = 0; index < length; ++index) feed(data[index]);
      return ~crc;
    }

    inline void
    put16(std::uint8_t *at, std::int16_t value) {
      const auto bits = static_cast<std::uint16_t>(value);
      at[0] = static_cast<std::uint8_t>(bits & 0xffu);
      at[1] = static_cast<std::uint8_t>(bits >> 8);
    }

    inline std::int16_t
    get16(const std::uint8_t *at) {
      return static_cast<std::int16_t>(static_cast<std::uint16_t>(at[0] | (at[1] << 8)));
    }
  }  // namespace detail

  /** @brief The 41-byte reply of GET_REPORT(feature, 0x05). */
  inline std::array<std::uint8_t, calibration_bytes>
  calibration_report() {
    std::array<std::uint8_t, calibration_bytes> report {};
    report[0] = 0x05;
    const auto gyro_range = static_cast<std::int16_t>(std::lround(540.0f * gyro_counts_per_dps));  // 8856
    const auto accel_range = static_cast<std::int16_t>(accel_counts_per_g);
    auto *at = report.data() + 1;
    for (int axis = 0; axis < 3; ++axis) detail::put16(at + axis * 2, 0);  // gyro bias pitch, yaw, roll
    at += 6;
    for (int axis = 0; axis < 3; ++axis) {
      detail::put16(at + axis * 4, gyro_range);  // plus
      detail::put16(at + axis * 4 + 2, static_cast<std::int16_t>(-gyro_range));  // minus
    }
    at += 12;
    detail::put16(at, 540);  // gyro speed plus
    detail::put16(at + 2, 540);  // gyro speed minus
    at += 4;
    for (int axis = 0; axis < 3; ++axis) {
      detail::put16(at + axis * 4, accel_range);  // plus
      detail::put16(at + axis * 4 + 2, static_cast<std::int16_t>(-accel_range));  // minus
    }
    const auto crc = detail::crc32_seeded(0x53, report.data(), calibration_bytes - 4);
    for (std::size_t index = 0; index < 4; ++index) {
      report[calibration_bytes - 4 + index] = static_cast<std::uint8_t>(crc >> (8 * index));
    }
    return report;
  }

  constexpr std::size_t firmware_bytes = 64;

  /**
   * @brief The 64-byte reply of GET_REPORT(feature, 0x20), the firmware information of a pad.
   *
   * Layout (hid-playstation dualsense_get_firmware_info and the other public readers): report id, build date
   * text from byte 1 ("Jul  4 2025"), build time text from byte 12, hardware version as a little-endian u32 at
   * byte 24, firmware version at byte 28 and update version as a u16 at byte 44 (the number SDL calls the
   * firmware version). The bytes are those a real DualSense answered over Bluetooth on 2026-11-01 (hardware
   * 0x00000711, firmware 0x0110002a, update version 0x0630). The last four bytes are the CRC32 (seed 0xA3) the pad appended to a Bluetooth report; the bytes before them
   * are unchanged, so the CRC stays valid and a reader that checks it is satisfied, and one that does not never looks.
   */
  inline std::array<std::uint8_t, firmware_bytes>
  firmware_report() {
    static constexpr std::array<std::uint8_t, firmware_bytes> real = {
      0x20,
      0x4a,
      0x75,
      0x6c,
      0x20,
      0x20,
      0x34,
      0x20,
      0x32,
      0x30,
      0x32,
      0x35,
      0x31,
      0x30,
      0x3a,
      0x31,
      0x30,
      0x3a,
      0x33,
      0x32,
      0x02,
      0x00,
      0x04,
      0x00,
      0x11,
      0x07,
      0x00,
      0x00,
      0x2a,
      0x00,
      0x10,
      0x01,
      0x00,
      0xa8,
      0x01,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x30,
      0x06,
      0x00,
      0x00,
      0x3c,
      0x00,
      0x01,
      0x00,
      0x0a,
      0x00,
      0x02,
      0x00,
      0x06,
      0x00,
      0x00,
      0x00,
      0xd2,
      0x7e,
      0x7a,
      0x7f,
    };
    return real;
  }

  /** @brief The CRC32 a Bluetooth feature report carries in its last four bytes (seed 0xA3 over everything before it). */
  inline std::uint32_t
  feature_crc(const std::uint8_t *report, std::size_t length_without_crc) {
    return detail::crc32_seeded(0xa3, report, length_without_crc);
  }

  /** @brief True when the trailing four bytes of @p report are the correct feature CRC. */
  inline bool
  feature_crc_valid(const std::uint8_t *report, std::size_t length) {
    if (length < 5) return false;
    std::uint32_t stored = 0;
    for (std::size_t i = 0; i < 4; ++i) stored |= static_cast<std::uint32_t>(report[length - 4 + i]) << (8 * i);
    return feature_crc(report, length - 4) == stored;
  }

  /**
   * @brief Feature reports that start, carry or finish a firmware update or a manufacturing command on a real pad
   *        (0xa0, 0xe0, 0xf0 to 0xf2, 0xf4, 0xf5). The virtual pad has no firmware: a SET of one of them is refused with a
   *        stall and nothing is forwarded anywhere; a GET answers zeros of the declared size.
   */
  constexpr bool
  is_firmware_update_report(std::uint8_t id) {
    return id == 0xa0 || id == 0xe0 || id == 0xf0 || id == 0xf1 || id == 0xf2 || id == 0xf4 || id == 0xf5;
  }

  enum class feature_set_action : std::uint8_t { ignore,
    refuse };

  /** SET_REPORT(feature) never reaches a client path without a public writable definition. */
  constexpr feature_set_action
  feature_set_action_for(std::uint8_t id) {
    return is_firmware_update_report(id) ? feature_set_action::refuse : feature_set_action::ignore;
  }

  struct calibration_check_t {
    bool crc_ok { false };
    bool usable { false };  ///< every sensitivity finite and within SDL's sanity window
    float gyro_sensitivity[3] {};
    float accel_sensitivity[3] {};
    int gyro_bias[3] {};
    int accel_bias[3] {};
  };

  /**
   * @brief Decode a 0x05 report the way SDL's PS5 driver does and apply its validity test
   *        (bias within 1024, gyro sensitivity within 50% of 64, accelerometer within 50% of 1).
   */
  inline calibration_check_t
  check_calibration(const std::uint8_t *report, std::size_t length) {
    calibration_check_t check;
    if (report == nullptr || length < 35 || report[0] != 0x05) return check;
    if (length >= calibration_bytes) {
      const auto crc = detail::crc32_seeded(0x53, report, calibration_bytes - 4);
      std::uint32_t stored = 0;
      for (std::size_t index = 0; index < 4; ++index) stored |= static_cast<std::uint32_t>(report[calibration_bytes - 4 + index]) << (8 * index);
      check.crc_ok = crc == stored;
    }
    const auto v = [&](std::size_t byte) { return static_cast<int>(detail::get16(report + byte)); };
    const float numerator = static_cast<float>(v(19) + v(21)) * 1024.0f;
    for (int axis = 0; axis < 3; ++axis) {
      check.gyro_bias[axis] = v(1 + axis * 2);
      const int plus = v(7 + axis * 4);
      const int minus = v(9 + axis * 4);
      check.gyro_sensitivity[axis] = numerator / static_cast<float>(plus - minus);
      const int accel_plus = v(23 + axis * 4);
      const int accel_minus = v(25 + axis * 4);
      const int range = accel_plus - accel_minus;
      check.accel_bias[axis] = accel_plus - range / 2;
      check.accel_sensitivity[axis] = 2.0f * accel_counts_per_g / static_cast<float>(range);
    }
    bool usable = true;
    for (int axis = 0; axis < 3; ++axis) {
      usable = usable && std::isfinite(check.gyro_sensitivity[axis]) && std::isfinite(check.accel_sensitivity[axis]);
      usable = usable && std::abs(check.gyro_bias[axis]) <= 1024 && std::abs(check.accel_bias[axis]) <= 1024;
      usable = usable && std::fabs(1.0f - check.gyro_sensitivity[axis] / 64.0f) <= 0.5f;
      usable = usable && std::fabs(1.0f - check.accel_sensitivity[axis] / 1.0f) <= 0.5f;
    }
    check.usable = usable;
    return check;
  }
}  // namespace ds5::feature
