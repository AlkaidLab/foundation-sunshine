/**
 * @file src/crash/crash_log.h
 * @brief The session log that goes with a crash report or a feedback report: found, anonymised, compressed.
 *
 * The log of the run that crashed is the file sunshine.log of that run; once the host has been started again it is sunshine.log.1 (older runs shift up, five are kept).
 * It is picked by time: the file whose last write is closest to the report. The text is anonymised (log_scrub.h) before anything is compressed or leaves the computer.
 * What goes up (src/crash/crash_upload.cpp, POST /v1/crash and /v1/user-log): the gzip file, and the first and the last 64 KiB of the same anonymised text as plain
 * UTF-8, which the service spot-checks for personal data and checks against the start of the gzip file.
 */
#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crash::logfile {

  constexpr std::size_t edge_bytes = 64 * 1024;  ///< size of log_head and log_tail
  constexpr std::size_t max_gz_bytes = 2 * 1024 * 1024;  ///< what the service accepts
  constexpr std::size_t max_read_bytes = 64u * 1024 * 1024;  ///< the most of one file that is read
  constexpr std::size_t bounded_read_bytes = 3u * 1024 * 1024;  ///< what a report with a deadline reads: the scrubber keeps 2 M characters of the end, the rest is margin for a key block that straddles its cut

  struct packed_t {
    std::string gz;  ///< the anonymised log, gzip
    std::string head;  ///< the first edge_bytes of it (whole characters), plain text
    std::string tail;  ///< the last edge_bytes of it (from a line start), plain text
    std::size_t lines = 0;
    bool truncated = false;  ///< the start of the log was dropped to fit
  };

  /// gzip (RFC 1952) of `data`; empty on failure.
  std::string
  gzip(std::string_view data);

  /// Anonymises `raw`, compresses it and cuts the head and the tail. When the result is over `max_gz` the oldest lines are dropped until it fits. nullopt for an empty log.
  /// `expired` (optional) is asked while the text is anonymised and between the rounds of dropping lines: once it says yes the work stops and the answer is nullopt.
  std::optional<packed_t>
  pack(std::string_view raw, std::size_t max_gz = max_gz_bytes, const std::function<bool()> &expired = {});

  /// sunshine.log and its rotated copies sunshine.log.1 ... .5 that exist, newest name first.
  std::vector<std::filesystem::path>
  candidates(const std::filesystem::path &log_file);

  /// The candidate whose last write is closest to `when` (within `window`), or empty.
  std::filesystem::path
  pick_by_time(const std::vector<std::filesystem::path> &files, std::filesystem::file_time_type when, std::chrono::seconds window = std::chrono::minutes(10));

  /**
   * The text to attach for a report written at `when`: the run's log; when that is very short (a crash at start-up) the end of the run before it is put in front.
   * `log_file` is the configured log path. Empty when no log fits the time.
   */
  std::string
  collect(const std::filesystem::path &log_file, std::filesystem::file_time_type when);

  /// collect() for "the log of the run that is going on": the current file, no older run. Of a file over `max_bytes` only the end is read.
  std::string
  collect_current(const std::filesystem::path &log_file, std::size_t max_bytes = max_read_bytes);

}  // namespace crash::logfile
