/**
 * @file src/crash/crash.h
 * @brief Crash, abort and hang capture for the host; the upload of what it wrote (telemetry v1, POST /v1/crash).
 *
 * A fatal exception, std::terminate, abort, a pure virtual call or an invalid CRT parameter writes a minidump (Windows) and a meta JSON into
 * <config>/crashes/pending/; a watchdog does the same, as kind "hang", for a thread that stays busy too long. Nothing leaves the computer
 * here: the uploader sends the pending reports on the next start and every half hour while error reports are on (config key
 * telemetry_errors, default on), deletes a report after a 2xx, keeps 20 at most and never waits on anything the host needs to start.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace crash {
  struct options_t {
    std::string product = "host";
    std::string version;  ///< the full version string
    std::string os_version;
    std::filesystem::path config_dir;  ///< reports go to <config_dir>/crashes/pending
    std::filesystem::path config_file;  ///< sunshine.conf: the error-report consent and the endpoint override are read from it
  };

  /// Installs the handlers and the watchdog thread. Call once, as early as possible. Safe to call again with the same options.
  void
  install(const options_t &options);

  /// Re-reads the consent (and the endpoint) from the config file; the watchdog does it every 30 s as well, so a change in the UI is seen.
  void
  refresh_config();

  /// Points the handlers at another config file (the command line can name one); implies refresh_config().
  void
  set_config_file(const std::filesystem::path &config_file);

  /// Whether error reports may be written and sent right now.
  bool
  enabled();

  // --- hang watchdog -------------------------------------------------------------------------------------------------------------------
  /// Registers the calling thread under `name`; the slot is "hung" when it stays busy for `timeout_ms`. Returns the slot, -1 when full.
  int
  watch_register(const char *name, std::uint32_t timeout_ms);
  /// Gives the slot back (and the thread handle held for it); call when the watched thread ends.
  void
  watch_release(int slot);
  void
  watch_busy(int slot);
  void
  watch_idle(int slot);

  /// Busy for the life of the scope; a thread blocked on purpose (waiting for work) is idle and never reported.
  struct busy_scope_t {
    explicit busy_scope_t(int slot): slot(slot) { watch_busy(slot); }
    ~busy_scope_t() { watch_idle(slot); }
    busy_scope_t(const busy_scope_t &) = delete;
    busy_scope_t &
    operator=(const busy_scope_t &) = delete;
    int slot;
  };

  /// A thread under watch for its whole life: registers here, releases at the end of the scope.
  struct watched_thread_t {
    watched_thread_t(const char *name, std::uint32_t timeout_ms): slot(watch_register(name, timeout_ms)) {}
    ~watched_thread_t() { watch_release(slot); }
    watched_thread_t(const watched_thread_t &) = delete;
    watched_thread_t &
    operator=(const watched_thread_t &) = delete;
    int slot;
  };

  // --- upload --------------------------------------------------------------------------------------------------------------------------
  /// Starts the background uploader: a first pass after a short delay, then every 30 minutes. Never blocks the caller.
  void
  start_uploader();
  /// Stops the uploader (bounded wait).
  void
  stop();

  /**
   * One upload pass over the pending folder, synchronous: used by the uploader thread and the tests. Returns how many reports were
   * delivered (or rejected for good); `endpoint` is the base URL such as https://api.alkaidlab.com.
   */
  int
  upload_pending_once(const std::filesystem::path &config_dir, const std::string &endpoint, const std::string &install_id);

  /// The base URL: env SUNSHINE_TELEMETRY_ENDPOINT, else config key telemetry_endpoint, else https://api.alkaidlab.com.
  std::string
  endpoint_from(const std::string &conf_text);

  /// SUNSHINE_TEST_CRASH=null-deref|abort|hang|throw|terminate makes this process fail on purpose; =upload-only runs one upload pass and exits
  /// (never set in a real run).
  void
  run_test_switch();

  // --- writing, used by the handlers and by other tools that share the folder -------------------------------------------------------------
  /// Writes a report from outside a crash (tests, tools): meta text and an optional dump file already on disk.
  bool
  write_report(const std::filesystem::path &config_dir, const std::string &stem, const std::string &meta_json, const std::filesystem::path &dump = {});
}  // namespace crash
