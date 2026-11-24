/**
 * @file src/telemetry/error_report.h
 * @brief Error reports with a location, a stack, a fingerprint and the user's last actions (breadcrumbs). Pure code (std + nlohmann).
 *
 * A report is built from fixed characters only: a source location is `src/file.cpp:line:function` (never an absolute path), a native frame is `module+0xoffset`, a breadcrumb is
 * `<seconds before>|<kind>|<id>` where the id is a lower-case word. Free text, input contents and paths cannot enter by construction. The reporter sends the first report of a fingerprint of each UTC day at once, in full,
 * and only counts the rest; the counts leave as one aggregate per fingerprint (every ten minutes by default, once a day when the host sets it so); at most 20 reports leave in an hour.
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

  /** The counts held for one fingerprint, as the event that carries them and the product (and its version, when it is not the host's) that raised the error. */
  struct error_aggregate_t {
    std::string product = "host";
    std::string version;
    nlohmann::json props;
  };

  class error_reporter_t {
  public:
    static constexpr std::int64_t AGGREGATE_MS = 10 * 60'000;
    static constexpr int MAX_PER_HOUR = 20;
    /** Kinds of error remembered at once: the most that can be admitted in a day (20 an hour), so a restart on the same day still knows every one that was sent in full. */
    static constexpr std::size_t MAX_GROUPS = 600;

    /** How long the repeats of a fingerprint are held before they leave as one aggregate (default AGGREGATE_MS; the host sets one day, so a repeating error costs one record a day). */
    void
    set_aggregate_ms(std::int64_t ms);
    /**
     * -> the props of an `error_report` event to send now, or nullopt (counted for the next aggregate). The first report of a fingerprint on each UTC day is sent in full (stack and actions); every later one that
     * day is only counted.
     */
    std::optional<nlohmann::json>
    report(const error_input_t &error, const std::vector<std::string> &crumbs, std::int64_t now_ms);
    /**
     * The same for an error_report a page (panel, web) sent, already shaped (it carries its own fingerprint): the first of the day for that product and fingerprint is returned as it is, the others are counted into
     * the aggregate with the product and the version they came from. -> nullopt when counted.
     */
    std::optional<nlohmann::json>
    admit(std::string_view product, std::string_view version, const nlohmann::json &props, std::int64_t now_ms);
    /** The counts held back, one event per fingerprint, when the aggregate time has passed since it was last sent. */
    std::vector<nlohmann::json>
    flush(std::int64_t now_ms);
    /** The same, with the product and version of every aggregate (one per version that raised the error). `all`: whatever the time since the last send and whatever the hourly bound says (the host is stopping: every count goes to the disk now, none is lost to the bound). */
    std::vector<error_aggregate_t>
    flush_groups(std::int64_t now_ms, bool all = false);
    /** Forget everything held back and the count of what was sent: error reports were turned off, so nothing counted or kept while they were on may leave when they are turned on again. */
    void
    clear();
    /** What a restart must remember so one fingerprint is not sent in full twice on one day: {day, keys} of the groups whose full report was sent today. */
    nlohmann::json
    state(std::int64_t now_ms) const;
    /** Takes the state of an earlier run back; a state of another day is ignored. */
    void
    restore(const nlohmann::json &state, std::int64_t now_ms);

  private:
    struct group_t {
      error_input_t error;
      std::vector<std::string> crumbs;
      std::int64_t last_sent_ms = 0;
      std::int64_t full_day = -1;  ///< the UTC day of the last report sent in full
      /// repeats counted since the last aggregate, by the version of the product that raised them (empty: the host's own)
      std::map<std::string, std::uint64_t> held;
      /// repeats counted on an earlier UTC day: they leave at the next flush, whatever the aggregate time says, and never mix with the counts of today
      std::map<std::string, std::uint64_t> carried;
      std::string product = "host";
      nlohmann::json page;  ///< the first error_report of a page's fingerprint (null for an error of the host)
    };
    static void
    carry_over(group_t &g, std::int64_t day);
    nlohmann::json
    props_of(const group_t &g, const std::string &fingerprint, std::uint64_t count, bool detail) const;
    mutable std::mutex mutex_;
    std::map<std::string, group_t> groups_;
    std::deque<std::int64_t> sent_;
    std::int64_t aggregate_ms_ = AGGREGATE_MS;
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
