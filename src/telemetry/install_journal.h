/**
 * @file src/telemetry/install_journal.h
 * @brief What the installer leaves for the host to report: one line per step of an install, upgrade, repair or uninstall.
 *
 * The installer (cmake/packaging/sunshine.iss.in) appends JSON lines to <config>/telemetry/install_journal.jsonl:
 *   {"v":1,"kind":"start","run":"<id>","phase":"upgrade","from":"v2026.1113.173055"}
 *   {"v":1,"kind":"component","run":"<id>","component":"usbip","step":"install","exit":102,"ms":4200}
 *   {"v":1,"kind":"finish","run":"<id>","exit":3010,"ms":90000}
 * The host reads it at its next start, turns it into `install_result` (one per component) and `install_summary` (one per run) events and deletes the file.
 * A run that started and never finished (the installer was killed, the computer lost power) is reported as `incomplete` once the journal has been quiet for ten minutes.
 * Pure code (std + nlohmann::json): the text is untrusted, every field is checked against a word list, a pattern or a range.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "events.h"

namespace telemetry::install_journal {

  inline constexpr std::size_t MAX_BYTES = 64 * 1024;
  inline constexpr std::size_t MAX_LINES = 400;
  inline constexpr std::size_t MAX_RUNS = 8;
  /** A run without a finish line is "incomplete" when the journal has not been written for this long. */
  inline constexpr std::int64_t QUIET_MS = 10LL * 60 * 1000;

  struct item_t {
    kind_e kind = kind_e::error;
    std::string name;  ///< install_result | install_summary
    nlohmann::json props = nlohmann::json::object();
  };

  struct result_t {
    std::vector<item_t> items;
    /** false: a run is still going (no finish line, the journal is fresh); leave the file for the next start */
    bool consumed = true;
  };

  /** The class of a failed step from its exit code: permission, disk, cancelled or other. */
  std::string error_class_of(long long exit_code);
  /** "3010" for the exit code 3010, "0xC0000005" for a negative one (a code must start with a letter or a digit). */
  std::string exit_code_text(long long exit_code);
  /** The numeric part of a version ("v2026.1113.173055.name" -> "2026.1113.173055"), empty when there is none. */
  std::string version_token(std::string_view text);

  /** `text` is the content of the journal, `age_ms` the time since it was last written. */
  result_t events_from_journal(std::string_view text, std::int64_t age_ms);

}  // namespace telemetry::install_journal
