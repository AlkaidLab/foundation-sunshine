/**
 * @file src/settings_reset.h
 * @brief Restoring the host settings to their defaults, and taking that back: the file work behind /api/config/reset.
 *
 * A reset first copies the current settings file next to itself (`sunshine.conf.reset-<UTC stamp>`), then writes a settings file that holds only the keys that are
 * kept, and optionally puts the bundled application list in place of apps.json (that file is copied first too). The five newest copies stay; each older one goes with
 * the application list copy of the same stamp. Paired devices are not part of any of these files, so they are never touched. A restore puts the newest copy back
 * (after copying what is there now, so that the restore can be taken back as well). Std only: the reader of the settings format is passed in, so the suite that
 * tests this runs without the rest of the host.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace settings_reset {

  /// How many copies stay (the settings file; an application list copy goes with the one of its stamp).
  inline constexpr std::size_t kept_copies = 5;

  /**
   * The keys that a reset leaves as they are, and a restore too:
   * - what the tray controls and the widget token (the Web UI does not save them either);
   * - the per-device settings (`clients`: they belong to the paired devices that stay);
   * - the language and the service port (the person has to be able to read the page and reach it after the restart);
   * - the two privacy choices (a reset never turns on what the person turned off);
   * - where the data lives (state, credentials, certificate, key, application list, log): another path would leave the kept data behind.
   */
  const std::vector<std::string_view> &
  kept_keys();

  /// Reads the text of a settings file into key/value pairs (the host's own reader).
  using parse_t = std::function<std::map<std::string, std::string>(std::string_view)>;
  /// Milliseconds since the Unix epoch.
  using now_t = std::function<std::int64_t()>;

  struct paths_t {
    /// sunshine.conf
    std::filesystem::path config;
    /// apps.json
    std::filesystem::path apps;
    /// the application list that a new install starts with
    std::filesystem::path bundled_apps;
  };

  enum class error_e {
    none,
    /// another reset or restore is running, or one has finished and the host is about to restart
    busy,
    /// there is no copy to restore
    no_backup,
    /// the settings (or a copy) could not be read
    read_failed,
    /// a copy could not be written: nothing was changed
    backup_failed,
    /// the new settings could not be written: the earlier ones are back
    write_failed,
    /// the bundled application list is not there: nothing was changed
    defaults_missing,
  };

  /// The word of an error in the API answer.
  std::string_view
  error_code(error_e error);

  struct backup_t {
    /// `20261119-153012` (UTC), with `-2`, `-3`... when two copies are made within a second
    std::string stamp;
    /// when the copy was made, from its stamp
    std::int64_t created_ms = 0;
    /// the application list was copied with it
    bool has_apps = false;
  };

  struct result_t {
    error_e error = error_e::none;
    /// the copy of what was there before this call
    std::string backup;
    /// restore: the stamp of the copy that was put back
    std::string restored;
    /// the application list was replaced (reset) or put back (restore)
    bool apps_changed = false;
    explicit
    operator bool() const {
      return error == error_e::none;
    }
  };

  /// The copies that exist, newest first.
  std::vector<backup_t>
  list_backups(const paths_t &paths);

  /// Copy, then write the kept keys only; with `clear_apps` the bundled application list replaces apps.json (copied first).
  result_t
  reset_to_defaults(const paths_t &paths, bool clear_apps, const parse_t &parse, const now_t &now);

  /// Put the newest copy back (the settings that are there now are copied first). The kept keys keep their present values.
  result_t
  restore_latest(const paths_t &paths, const parse_t &parse, const now_t &now);

  /// `20261119-153012` from milliseconds since the epoch (UTC), and back; 0 for text that is not a stamp.
  std::string
  format_stamp(std::int64_t unix_ms);
  std::int64_t
  parse_stamp(std::string_view stamp);

  /**
   * One reset or restore at a time, and none after one has worked: the host restarts, and a second click that arrives meanwhile must not make a second copy or
   * undo the first. `leave(true)` closes the gate for good (until the process ends).
   */
  class gate_t {
  public:
    bool
    try_enter() {
      bool expected = false;
      if (!busy_.compare_exchange_strong(expected, true)) return false;
      if (closed_.load()) {
        busy_.store(false);
        return false;
      }
      return true;
    }
    void
    leave(bool worked) {
      if (worked) closed_.store(true);
      busy_.store(false);
    }

  private:
    std::atomic<bool> busy_ { false };
    std::atomic<bool> closed_ { false };
  };

}  // namespace settings_reset
