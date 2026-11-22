/**
 * @file src/telemetry/report_slot.cpp
 * @brief The share of the day each install reports in, and which installs are in the sample.
 */
#include "report_slot.h"

#include <algorithm>
#include <cmath>

namespace telemetry::slot {

  std::uint64_t
  hash64(std::string_view salt, std::string_view text, std::int64_t number) {
    std::uint64_t h = 14695981039346656037ULL;
    const auto mix = [&](unsigned char c) {
      h ^= c;
      h *= 1099511628211ULL;
    };
    for (const unsigned char c : salt) mix(c);
    mix(0xFF);
    for (const unsigned char c : text) mix(c);
    mix(0xFF);
    for (int i = 0; i < 8; ++i) mix(static_cast<unsigned char>((static_cast<std::uint64_t>(number) >> (8 * i)) & 0xFF));
    // splitmix64 finisher: FNV alone leaves the low bits of near-equal texts correlated
    h += 0x9E3779B97F4A7C15ULL;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBULL;
    return h ^ (h >> 31);
  }

  std::int64_t
  day_of(std::int64_t ms) {
    // floor, so a time before the epoch (a clock that was never set) is still a stable day number
    return ms >= 0 ? ms / DAY_MS : -((-ms + DAY_MS - 1) / DAY_MS);
  }

  std::int64_t
  offset_ms(std::string_view install_id) {
    return static_cast<std::int64_t>(hash64("slot", install_id) % static_cast<std::uint64_t>(DAY_MS));
  }

  std::int64_t
  spread_ms(std::string_view install_id, std::string_view salt, std::int64_t day, std::int64_t span_ms) {
    if (span_ms <= 0) return 0;
    return static_cast<std::int64_t>(hash64(salt, install_id, day) % static_cast<std::uint64_t>(span_ms));
  }

  std::int64_t
  slot_ms(std::string_view install_id, std::int64_t day) {
    return day * DAY_MS + offset_ms(install_id) + spread_ms(install_id, "slot-day", day, DAY_JITTER_MS);
  }

  std::int64_t
  due_ms(std::string_view install_id, std::int64_t day, std::int64_t started_ms) {
    const auto slot = slot_ms(install_id, day);
    // a process that started after the slot (or in the middle of the day) waits a short, per install spread from its start, so installs switched on together do not report together
    const auto after_start = started_ms + spread_ms(install_id, "slot-start", day, START_JITTER_MS);
    return std::max(slot, after_start);
  }

  double
  clamp_rate(double rate) {
    if (!(rate > 0.0)) return MIN_SAMPLE_RATE;  // zero, negative or NaN
    return std::clamp(rate, MIN_SAMPLE_RATE, 1.0);
  }

  bool
  sampled_in(std::string_view install_id, double rate) {
    rate = clamp_rate(rate);
    if (rate >= 1.0) return true;
    // 53 bits of the hash as a number in [0, 1): exactly representable, so the comparison does not depend on rounding
    const auto bits = hash64("sample", install_id) >> 11;
    return static_cast<double>(bits) / 9007199254740992.0 < rate;
  }

  double
  sample_weight(double rate) {
    return 1.0 / clamp_rate(rate);
  }

}  // namespace telemetry::slot
