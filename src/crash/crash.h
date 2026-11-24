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

#include "crash_log.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <chrono>
#include <optional>
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
  /// Monotonic milliseconds that stand still while the system sleeps or hibernates. The watchdog counts a thread's busy time on this clock: on the one
  /// that does not stop (std::chrono::steady_clock on Windows) a start that was running when the machine went to sleep is a "hang" the moment it wakes.
  std::uint64_t
  awake_ms();

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

  /// Stops and joins the watchdog thread that `install` starts. A process that never starts the uploader (the service wrapper) calls this before it returns from main:
  /// the thread is a static std::thread, and one destroyed while joinable ends the process in std::terminate. Safe to call twice; `stop()` includes it.
  void
  stop_watchdog();

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

  /// Installed by the host: returns the JSON of the machine (CPU, GPUs, memory, displays, language, install source) for the "env" of a crash report. Read at upload time.
  void
  set_environment_provider(std::function<std::string()> provider);

  /**
   * The anonymised session log of the run that is going on, uploaded to POST <endpoint>/v1/user-log for a feedback report. Returns the reference to put into the
   * report (prop log_ref), or nothing when there is no log or the service does not take it (404, a limit): the report then goes without a log.
   * The error-report switch does not decide: the log is part of a feedback report, and pressing 发送 is the consent for it.
   */
  std::optional<std::string>
  upload_session_log(const std::string &endpoint, const std::string &install_id, const std::string &version);

  /// Why the latest upload_session_log() gave no reference (a fixed word: uploaded | errors_off (no longer produced: the log of a feedback report is not held back by the switch) | no_log | pack_failed | refused | unreachable | bad_answer), so the report can say it.
  const char *
  session_log_state();

  /**
   * The screenshot of a feedback report, uploaded to POST <endpoint>/v1/user-screenshot (multipart: "meta" + "image"). `image` is PNG, JPEG or WebP bytes the caller has already checked. Returns the reference to put into the
   * report (prop shot_ref), or nothing (a limit, a refusal, no network): the report then goes without its screenshot. The server checks the picture again and keeps it without its metadata.
   */
  std::optional<std::string>
  upload_session_shot(const std::string &endpoint, const std::string &install_id, const std::string &version, const std::string &image);

  /**
   * One deadline for the uploads of one feedback report on this thread (the screenshot, then the log with its collection and packing): each upload gets at most 20 s of what is left and none starts with
   * less than 2 s left, so the host answers the page within its wait. The log's collection and packing get at most 10 s of what is left and leave 5 s for its upload: with less than that left the log
   * is not collected at all, and a log that is not packed in time is left out (unreachable). end_feedback_budget() lifts it; without a budget an upload keeps the fixed 20 s and the log's steps are not timed.
   */
  void
  begin_feedback_budget(std::chrono::seconds total);
  void
  end_feedback_budget();
  /// The timeout the next feedback upload would get (0: it would not start). For the tests.
  long
  feedback_upload_timeout_for_tests();

  /// Why the latest upload_session_shot() gave no reference (a fixed word: uploaded | none | too_large | rejected | unreachable | refused | bad_answer | unavailable).
  const char *
  session_shot_state();

  /// The two slow steps in front of the log's upload, replaceable so that the tests can make them slow. `expired` says when the report's deadline for them has passed; a step that
  /// notices it stops early (the packing answers nullopt).
  struct log_stages_t {
    std::function<std::string(const std::filesystem::path &log_file, std::size_t max_bytes)> collect;
    std::function<std::optional<logfile::packed_t>(std::string_view text, const std::function<bool()> &expired)> pack;
  };

  /// upload_session_log() for a given config directory, without the consent check (the caller has made it): the tests and tools. `stages` replaces the collecting and the packing (null: the real ones).
  std::optional<std::string>
  upload_session_log_from(const std::filesystem::path &config_dir, const std::string &endpoint, const std::string &install_id, const std::string &version, const log_stages_t *stages = nullptr);

  /// Seconds the uploader waits before the next pass after a 429/503 (Retry-After) or a failure; 0 after a success. For the tests.
  long
  upload_wait_hint_s();

  /// The random share that spreads the waits and the start delay of the uploader (a number in [0, 1)); an empty function restores the random generator. For the tests.
  void
  set_upload_jitter_source(std::function<double()> source);

  // --- writing, used by the handlers and by other tools that share the folder -------------------------------------------------------------
  /// Writes a report from outside a crash (tests, tools): meta text and an optional dump file already on disk.
  bool
  write_report(const std::filesystem::path &config_dir, const std::string &stem, const std::string &meta_json, const std::filesystem::path &dump = {});
}  // namespace crash
