/**
 * @file src/crash/log_scrub.h
 * @brief Anonymisation of a whole session log before it is attached to a crash report or a feedback report.
 *
 * Removed: user and computer names, the user segment of paths, IPv4/IPv6 and MAC addresses (every distinct value becomes a stable placeholder inside one log: <ip#1>,
 * <mac#1>), e-mail addresses, serial numbers and device instance ids, tokens, keys, cookies, pairing PINs, Wi-Fi names, game account names. Kept: timestamps, levels,
 * components, codes, numbers, versions, GPU and driver strings and every config value that is not a secret.
 *
 * The rules, their order and the placeholders are the ones of the shared test vectors (tests/data/log_scrub_vectors.json, the same file the report service and the
 * Panel run); crash_unit_tests runs every vector. The text is UTF-8; invalid bytes become U+FFFD so the result is valid UTF-8 and survives a round trip.
 */
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace crash::logscrub {

  struct result_t {
    std::string text;
    std::size_t lines = 0;
    bool truncated = false;  ///< the beginning was cut: only the tail of a very long log is kept
    bool cancelled = false;  ///< `expired` said yes before the last line was done: `text` holds only the lines before that and must not be used
  };

  /// Scrubs `raw`. `max_chars` bounds the kept tail (whole lines), `max_line` cuts a single line. `expired` (optional) is asked every few lines: once it says yes the work stops and result_t::cancelled is set.
  result_t
  scrub(std::string_view raw, std::size_t max_chars = 2'000'000, std::size_t max_line = 600, const std::function<bool()> &expired = {});

  /// Replaces invalid UTF-8 with U+FFFD and drops control characters (the same set as the reference implementation).
  std::string
  sanitize_utf8(std::string_view raw);

}  // namespace crash::logscrub
