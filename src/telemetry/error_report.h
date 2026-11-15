/**
 * @file src/telemetry/error_report.h
 * @brief Error reports with a location, a stack, a fingerprint and the user's last actions (breadcrumbs). Pure code (std + nlohmann).
 *
 * A report is built from fixed characters only: a source location is `src/file.cpp:line:function` (never an absolute path), a native frame is `module+0xoffset`, a breadcrumb is
 * `<seconds before>|<kind>|<id>` where the id is a lower-case word. Free text, input contents and paths cannot enter by construction. The reporter sends the first report of a fingerprint at once and
 * counts the rest into one aggregate every ten minutes; at most 20 reports leave in an hour.
 */
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace telemetry {

  inline constexpr std::size_t MAX_BREADCRUMBS = 50;
  inline constexpr std::size_t MAX_FRAMES = 32;
  /** The tail of a component's own log in an error report: lines, and characters per line (anonymised by the sender, scrubbed again by the host). */
  inline constexpr std::size_t MAX_LOG_LINES = 40;
  inline constexpr std::size_t MAX_LOG_LINE_CHARS = 200;

  enum class crumb_e { nav,
    act,
    set,
    stream,
    dev,
    state,
    net };
  std::string_view
  crumb_name(crumb_e kind);

  /** A bounded ring of the user's last actions and state changes. Thread safe; add() never blocks for long. */
  class breadcrumbs_t {
  public:
    explicit breadcrumbs_t(std::size_t capacity = MAX_BREADCRUMBS);
    /** `id` is reduced to [a-z0-9_.:+-], at most 48 characters; an id that is empty afterwards is not recorded. */
    void
    add(crumb_e kind, std::string_view id, std::int64_t now_ms);
    /** Oldest first, "<seconds before now>|<kind>|<id>". */
    std::vector<std::string>
    snapshot(std::int64_t now_ms) const;
    void
    clear();

  private:
    struct item_t {
      std::int64_t ms;
      crumb_e kind;
      std::string id;
    };
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<item_t> items_;
  };

  /** A word for a breadcrumb or a code: lower case, characters outside [a-z0-9_.:+-] become '_', at most `max` characters. */
  std::string
  crumb_word(std::string_view text, std::size_t max = 48);

  /** `src/stream.cpp:3912:videoBroadcastThread`. The path is cut at the first of src/, src_assets/, tools/, tests/ (so no build directory leaks); empty when the file is not under one. */
  std::string
  location_of(const std::source_location &where);
  /** "void __cdecl stream::session::foo(int)" and "stream::foo()" -> "stream::foo". */
  std::string
  function_word(std::string_view signature);
  /** A frame that may be sent, or empty: `module+0xoffset`, `src/…/file:line[:function]` (a root of this source tree, no absolute path, no drive, no `..`), or a page frame `function@file:line:col`; at most 120 characters. */
  std::string
  clean_frame(std::string_view frame);
  /** FNV-1a 64 over component, code and the first three frames (functions and files, not offsets), as 16 hex digits. Same error, same fingerprint, on every machine and build. */
  std::string
  fingerprint_of(std::string_view component, std::string_view code, const std::vector<std::string> &frames);

  struct error_input_t {
    std::string component = "host";
    std::string lang = "cpp";
    std::string code;  ///< an error code word (E_ENCODER_INIT, a status name), letters, digits and _.:+-
    std::string severity = "error";
    std::string loc;  ///< location_of()
    std::vector<std::string> frames;  ///< clean_frame()
    std::string build;  ///< the build id (a commit or a hash), tokens only
    std::string page;  ///< the page or route, for the web
  };

  class error_reporter_t {
  public:
    static constexpr std::int64_t AGGREGATE_MS = 10 * 60'000;
    static constexpr int MAX_PER_HOUR = 20;

    /** -> the props of an `error_report` event to send now, or nullopt (counted for the next aggregate). */
    std::optional<nlohmann::json>
    report(const error_input_t &error, const std::vector<std::string> &crumbs, std::int64_t now_ms);
    /** The counts held back, one event per fingerprint, when ten minutes have passed since it was last sent. */
    std::vector<nlohmann::json>
    flush(std::int64_t now_ms);
    /** Forget everything held back and the count of what was sent: error reports were turned off, so nothing counted or kept while they were on may leave when they are turned on again. */
    void
    clear();

  private:
    struct group_t {
      error_input_t error;
      std::vector<std::string> crumbs;
      std::int64_t last_sent_ms = 0;
      std::uint64_t held = 0;
    };
    nlohmann::json
    props_of(const group_t &g, const std::string &fingerprint, std::uint64_t count, bool detail) const;
    std::mutex mutex_;
    std::map<std::string, group_t> groups_;
    std::deque<std::int64_t> sent_;
  };

  /** Stack frames of the calling thread as `module+0xoffset` (the module's file name only); empty where the platform has no way. */
  std::vector<std::string>
  capture_frames(std::size_t skip = 1);

  /**
   * One install-result.json of the installer shell (tools/installer_shell.cpp) -> the error to report, or nullopt: a success, a restart request, a cancel, or a file that does not name a known step and
   * a code word of the fixed shapes (`payload_invalid`, `engine_exit_<n>`, `webview2_exit_<n>`). Nothing from the file is copied but those two words.
   */
  std::optional<error_input_t>
  installer_failure_report(std::string_view result_json);

  /**
   * The failures of earlier installs: every `<root>/<run>/install-result.json` that has not been taken yet, as errors to report. Each file taken is renamed to install-result.reported.json, so one
   * failed run is reported once; a file that cannot be read or renamed is skipped. At most `limit` files per call.
   */
  std::vector<error_input_t>
  take_installer_failures(const std::filesystem::path &root, std::size_t limit = 5);

}  // namespace telemetry
