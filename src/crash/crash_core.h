/**
 * @file src/crash/crash_core.h
 * @brief Crash report meta: frames, signature, JSON, consent and the pending folder (telemetry v1 /v1/crash).
 *
 * Portable and free of the host's own headers so the Windows handler, the POSIX signal handler, the uploader and the unit tests share one
 * definition. Everything the handlers call (sha256, signature, render_meta, format_iso_utc) works on caller-supplied fixed buffers and
 * allocates nothing.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crash {
  constexpr std::size_t max_frames = 64;
  constexpr std::size_t signature_frames = 8;
  constexpr std::size_t max_pending = 20;
  constexpr std::uint64_t max_dump_bytes = 20ull * 1024 * 1024;
  constexpr std::size_t module_name_size = 64;
  constexpr std::size_t meta_buffer_size = 16 * 1024;

  struct frame_t {
    char module[module_name_size] {};  ///< file name of the module, no directory (a person's user name can be in a path)
    std::uint64_t offset = 0;  ///< address minus the module's base; frames after the first are return addresses
  };

  struct meta_t {
    const char *product = "host";  ///< host | panel | web | sparxie-launch
    const char *version = "";
    const char *os_version = "";
    const char *install_id = "";  ///< empty: left out, the uploader adds it
    const char *kind = "crash";  ///< crash | hang | abort | panic | unhandled-dotnet
    const char *fault_code = "";  ///< "0xC0000005", "SIGSEGV", "terminate"; the JSON key is exception_code (a macro in MinGW excpt.h, so not the member name)
    std::int64_t time = 0;  ///< unix seconds, rendered as ISO-8601 UTC
    std::uint64_t uptime_s = 0;
    const frame_t *frames = nullptr;
    std::size_t frame_count = 0;
  };

  /// SHA-256 of `size` bytes (FIPS 180-4).
  void
  sha256(const void *data, std::size_t size, std::uint8_t out[32]);

  /// Runtime and system modules whose frames sit on top of every abort or throw and say nothing about the caller.
  bool
  is_runtime_module(std::string_view module);

  /// Index of the first frame that is not a runtime/system frame (0 when every frame is one).
  std::size_t
  first_caller_frame(const frame_t *frames, std::size_t count);

  /**
   * Signature: SHA-256 over the first 8 frames from the first caller frame on, each normalised as "<module lower case>+0x<offset lower hex>"
   * joined by '\n'. `out` gets 64 hex digits and a NUL.
   */
  void
  signature_hex(const frame_t *frames, std::size_t count, char out[65]);

  /// ISO-8601 UTC, "2026-11-12T09:30:00Z" (20 chars + NUL).
  void
  format_iso_utc(std::int64_t unix_seconds, char out[24]);

  /// The meta JSON the contract names, into `out`; returns its length, 0 when it does not fit.
  std::size_t
  render_meta(const meta_t &meta, char *out, std::size_t capacity);

  /**
   * Error reports (and so crash reports) may leave the computer unless the person turned them off. Same rule as foundation-ui/telemetry:
   * `telemetry_errors` decides when present; without it an old `telemetry_consent = off` still means off; else on.
   */
  bool
  errors_enabled_from_conf(std::string_view conf_text);

  /// The value of `key` in a sunshine.conf text (last one wins), empty when absent.
  std::string
  conf_value(std::string_view conf_text, std::string_view key);

  /// File stem of one report: "<unix seconds>-<product>-<kind>-<signature first 8>".
  std::string
  report_stem(std::int64_t unix_seconds, std::string_view product, std::string_view kind, std::string_view signature);

  struct pending_report_t {
    std::filesystem::path meta;
    std::filesystem::path dump;  ///< empty when there is none
    std::filesystem::file_time_type time;
    std::uintmax_t bytes = 0;
  };

  /// Reports in a pending folder, oldest first. A stray .dmp without a .json is removed.
  std::vector<pending_report_t>
  list_pending(const std::filesystem::path &dir);

  /// Keep the newest `keep` reports, remove the rest (and everything older than `max_age_days`); returns how many went.
  std::size_t
  trim_pending(const std::filesystem::path &dir, std::size_t keep = max_pending, int max_age_days = 30);

  /// Removes every pending report.
  void
  clear_pending(const std::filesystem::path &dir);

  /// The folder of a config directory.
  std::filesystem::path
  pending_dir(const std::filesystem::path &config_dir);

  /**
   * Privacy pass over the bytes of a minidump before it leaves the computer. The dump type already keeps heap and string data out
   * (see crash_win.cpp); what is left can still hold a path (module list, a stack word that points at a string). Blanked in place with 'x',
   * same length so the file stays valid, in ASCII/UTF-8 and in UTF-16LE:
   *  - the account name behind a "\\Users\\", "/Users/" or "/home/" marker (up to the next separator),
   *  - every whole-token occurrence (not inside a longer word) of `names` (user name, computer name), ignoring ASCII case; names under 3 bytes are ignored.
   * @return how many places were blanked.
   */
  std::size_t
  redact_private_names(std::string &data, const std::vector<std::string> &names);

  /// A random UUID v4 in text form.
  std::string
  new_uuid_v4();

  /// `<config>/telemetry/install_id`: the one-line UUID that identifies this installation (telemetry v1 identity), created when absent.
  std::string
  read_or_create_install_id(const std::filesystem::path &config_dir, bool create);
}  // namespace crash
