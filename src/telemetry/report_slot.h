/**
 * @file src/telemetry/report_slot.h
 * @brief The share of the day each install reports in, and which installs are in the sample. Pure code (std only).
 *
 * Routine records (usage, performance, stream summaries) leave at most once a day, from a moment of the day that follows from the install id: the hash of the id picks a minute of the UTC day, so a
 * million installs do not all knock at 00:00 UTC, and a backlog after an outage drains one install at a time. Crash reports, error reports and feedback do not wait for it.
 *
 * Which installs report routine records at all is a sample: the hash of the id (with another salt) against a rate. The same install stays in or out for as long as the rate holds, an install that is in carries
 * the weight 1 / rate so the Worker counts the ones that are out as well.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace telemetry::slot {

  inline constexpr std::int64_t DAY_MS = 86'400'000;
  /** The spread added to the slot of one day (up to this far after it), different every day. */
  inline constexpr std::int64_t DAY_JITTER_MS = 5 * 60'000;
  /** The spread of the first report after a start that found the slot already past. */
  inline constexpr std::int64_t START_JITTER_MS = 5 * 60'000;
  /** The lowest rate a sample may have (a weight above 100 would make a few installs stand for far too many). */
  inline constexpr double MIN_SAMPLE_RATE = 0.01;

  /** A 64 bit hash of a salt, a text and a number (FNV-1a, finished with a mixer: the low bits are as even as the high ones). */
  std::uint64_t hash64(std::string_view salt, std::string_view text, std::int64_t number = 0);

  /** The UTC day number (days since the epoch) of a time in milliseconds. */
  std::int64_t day_of(std::int64_t ms);

  /** The moment of the UTC day this install reports in, milliseconds after midnight, in [0, DAY_MS). */
  std::int64_t offset_ms(std::string_view install_id);

  /** A small spread in [0, span_ms) that is fixed for this install, this day and this purpose (`salt`), so it is the same wherever it is asked and needs no random source. */
  std::int64_t spread_ms(std::string_view install_id, std::string_view salt, std::int64_t day, std::int64_t span_ms);

  /** The first moment on `day` (a UTC day number) at which the routine records of this install may leave: its slot plus the day's spread. */
  std::int64_t slot_ms(std::string_view install_id, std::int64_t day);

  /**
   * The first moment at which the routine records of `day` may leave when the process started at `started_ms`: the slot of that day, or a short spread after the start when the slot has already passed
   * (a computer that was off at its slot reports soon after it is switched on, not all at once).
   */
  std::int64_t due_ms(std::string_view install_id, std::int64_t day, std::int64_t started_ms);

  /** `rate` kept inside [MIN_SAMPLE_RATE, 1]; anything that is not a number (NaN) or is zero or below gives the lowest rate. */
  double clamp_rate(double rate);

  /** Whether this install is in the sample of the given rate. A rate of 1 (or more) takes every install; the answer for one id only ever changes from out to in as the rate goes up. */
  bool sampled_in(std::string_view install_id, double rate);

  /** The weight an install that is in the sample carries: 1 / rate, for a rate in [MIN_SAMPLE_RATE, 1]. */
  double sample_weight(double rate);

}  // namespace telemetry::slot
