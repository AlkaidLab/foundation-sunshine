/**
 * @file src/telemetry/service.h
 * @brief The telemetry sender: event buffer, disk queue, retry with backoff, consent, install identity and the stream trackers.
 *
 * Everything the host needs to count and send, with its dependencies injected (clock, the HTTP post, the consent answer), so it is unit tested without a
 * network or a host. telemetry.h wires one instance to the host. No call here waits for the network except pump(), which only the sender thread calls.
 *
 * Rules (contract: docs/privacy_telemetry.md):
 *   - Nothing is kept or sent for a kind whose consent is off; a consent that turns off drops what waits, in memory and on disk.
 *   - emit() never blocks on I/O and never throws; a full buffer drops the oldest events.
 *   - A batch that cannot be sent waits on disk (size and file caps); a failure backs off 30 s doubling to an hour, a Retry-After is believed and both are spread by a random share; a 4xx that describes the batch itself drops it,
 *     a 4xx a server in between may send (401, 403, 404, 405, 410, 421) keeps it.
 *   - Delivery classes (events.h delivery_e): a feedback report and an error leave as soon as the sender may send, are the last to be dropped from a full queue and survive a restart; routine records leave with the install's
 *     daily share (report_slot.h) and only for the installs in the sample, with the sample weight in the batch.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "error_report.h"
#include "events.h"
#include "hooks.h"
#include "opt_out.h"
#include "queue.h"
#include "report_slot.h"
#include "self_baseline.h"
#include "stream_stats.h"
#include "transport.h"

namespace telemetry {

  struct consent_t {
    bool usage = true;
    bool errors = true;
    bool
    any() const { return usage || errors; }
    bool
    allows(kind_e kind) const { return kind == kind_e::error ? errors : usage; }
  };

  /** The value of the two config keys: telemetry_consent (usage statistics) and telemetry_errors (error reports); both on unless the person said off. */
  consent_t
  consent_from_config(const std::string &usage_value, const std::string &errors_value, bool usage_key_present, bool errors_key_present);

  struct service_options_t {
    std::filesystem::path dir;  ///< <config>/telemetry: install_id and queue/
    std::string endpoint = "https://api.alkaidlab.com";  ///< base; batches go to <endpoint>/v1/events
    std::size_t ring_max = 2000;
    std::int64_t aggregate_interval_ms = 0;  ///< > 0: high-volume events are held and uploaded merged at most this often (and at exit); 0 sends every event as it is
    std::size_t flush_events = 200;  ///< a full buffer asks for a flush
    queue_limits_t queue;
    int max_batches_per_pump = 10;
    int max_posts_per_minute = 15;  ///< the Worker allows 20 requests a minute per install; the sender stays under it
    std::int64_t max_backoff_ms = 3600 * 1000;
    std::int64_t reconnect_window_ms = 30'000;
    /** The computer name and the account name of this machine: removed from an excerpt wherever they appear, whatever surrounds them. */
    std::vector<std::string> redact_words;
    int anomaly_logs_per_day = 6;  ///< per install; the Worker applies its own, global, limit. The first excerpt of a run is not held back by the gap below
    std::int64_t anomaly_log_min_gap_ms = 10 * 60'000;
    std::int64_t anomaly_log_span_ms = 120'000;  ///< the excerpt reaches this far back
    std::size_t anomaly_log_max_bytes = 120 * 1024;
    /** true: routine batches (everything but errors and feedback) leave once a day, from the install's moment of the day (report_slot.h); false: they leave as soon as they are written. */
    bool routine_slot = false;
    /** The share of installs that send sampled records (events.h sampled_family), in (0, 1]; 1 sends every install's. The Worker's answer (cfg.sample_rate) replaces it. */
    double sample_rate = 1.0;
    /** How long the repeats of one error are held before they leave as a count; 0: the reporter's default (ten minutes), or one day when `routine_slot` is on. */
    std::int64_t error_aggregate_ms = 0;
  };

  /** What the Worker answers with (`cfg` in the body of /v1/events), kept in <dir>/remote_config.json and read at the next start. */
  struct debug_cohort_t {
    std::int64_t until = 0;
    std::string version, gpu, client;  ///< empty: any
    int logs_per_day = 0;
  };
  struct remote_config_t {
    std::int64_t rev = 0;
    double tier1_rate = 0.1;  ///< the share of streams that send the per-window histograms
    int logs_per_day = 6;
    bool health = true;  ///< false: no stream-health events at all
    std::int64_t backoff_s = 0;  ///< wait this long before the next request
    bool paused = false;
    double sample_rate = 0.0;  ///< the share of installs that send sampled records; 0: not given (the local option holds)
    std::vector<debug_cohort_t> debug;
  };
  remote_config_t
  parse_remote_config(const nlohmann::json &cfg, std::int64_t now_ms, int default_logs_per_day = 6);

  struct service_deps_t {
    std::function<std::int64_t()> now_ms;  ///< ms since the epoch
    /** One blocking POST of `body` (JSON) to `url`; the HTTP status, 0 when nothing answered. Called on the sender thread only. */
    std::function<post_result_t(const std::string &url, const std::string &body)> post;
    std::function<consent_t()> consent;
    std::function<double()> random01;  ///< backoff jitter; default: a random generator
    /**
     * The session log that goes with a feedback report (user_report): uploads the anonymised log of the running host and returns its reference, or nothing (no log,
     * error reports off, the service does not take it). Called on the thread that ingests the page's events; null in tests that do not need it.
     */
    std::function<std::optional<std::string>(const std::string &endpoint, const std::string &install_id, const std::string &version)> session_log;
    /** The fixed word that says why the latest session_log call gave no reference (crash::session_log_state()); null in tests that do not need it. */
    std::function<std::string()> session_log_state;
    /**
     * The screenshot that goes with a feedback report: uploads the picture (PNG, JPEG or WebP bytes, already checked for type and size) and returns the reference to put into the report (prop shot_ref), or nothing:
     * the report then goes without it. Called on the thread that ingests the page's events; null in tests that do not need it.
     */
    std::function<std::optional<std::string>(const std::string &endpoint, const std::string &install_id, const std::string &version, const std::string &image)> session_shot;
    /** The fixed word that says why the latest session_shot call gave no reference (crash::session_shot_state()); null in tests that do not need it. */
    std::function<std::string()> session_shot_state;
    /** Starts / ends the shared deadline of one feedback report's uploads (crash::begin_feedback_budget / end_feedback_budget); null in tests that do not need it. */
    std::function<void()> feedback_budget_begin;
    std::function<void()> feedback_budget_end;
    /** When the computer started, ms since the epoch (0 = not known): tells a restart of the computer from a run that ended by itself. */
    std::function<std::int64_t()> boot_ms;
    /**
     * A word that is the same for every process of one boot of the computer and differs after the next boot (empty = not known). A clock that was set between two runs cannot fake a restart through it.
     * When it is set, a restart is only claimed when both runs know their word and the words differ; when it is null (tests), the start times in boot_ms are compared.
     */
    std::function<std::string()> boot_id;
    /** Runs inside flush() before each batch write to the disk queue; the tests use it to move a consent change into the middle of a flush. */
    std::function<void()> before_queue_write;
  };

  struct service_counts_t {
    std::uint64_t emitted = 0;
    std::uint64_t refused = 0;  ///< consent off or not allowed
    std::uint64_t dropped = 0;  ///< buffer full
    std::uint64_t sent_batches = 0;
    std::uint64_t sent_events = 0;
    std::uint64_t rejected_batches = 0;
    std::uint64_t failed_posts = 0;
    std::uint64_t write_failed = 0;  ///< a batch that could not be written to the disk queue (its events went back to the buffer)
    std::uint64_t sampled_out = 0;  ///< records of an install that is not in the sample (they are not written or sent)
  };

  /** Where this install stands in the sample and in the day. */
  struct report_plan_t {
    double sample_rate = 1.0;
    bool sampled_in = true;
    double weight = 1.0;  ///< carried by the sampled records of an install that is in the sample
    std::int64_t slot_offset_ms = 0;  ///< the moment of the UTC day this install reports in
    std::int64_t routine_day_done = -1;  ///< the UTC day whose routine batches were all sent (-1: none yet)
  };

  class service_t {
  public:
    service_t(service_options_t options, service_deps_t deps);

    // ---- identity ----
    /** The install id: created once in <dir>/install_id when a consent is on; empty when both are off (nothing is created then). */
    std::string
    install_id();
    const std::string &
    session_id() const { return session_id_; }
    /** {install_id, session_id, endpoint, usage, errors}; nullopt while both consents are off. */
    std::optional<nlohmann::json>
    identity();
    /** A new random install id (the person reset telemetry). */
    std::string
    rotate_install_id();
    void
    set_context(context_t context);
    context_t
    context() const;

    // ---- events ----
    /** Shape against the contract, apply consent, put in the buffer. -> true when it was kept. Thread safe, never blocks on I/O. */
    bool
    emit(kind_e kind, std::string_view name, const nlohmann::json &props, std::string_view product = "host", std::string_view version = {}, int priority = -1, std::string_view report_id = {}, std::string_view session_id = {});
    /** Events from a page (panel, web) through the host API: the same, with the page allow-list. -> the number kept. */
    std::size_t
    ingest(std::string_view product, std::string_view version, const nlohmann::json &events, ingest_report_t *report = nullptr);
    /** Writes one feedback report to the disk queue now (its own batch, priority 0). true only when the batch is on disk. */
    bool
    persist_report(const event_t &event);

    // ---- streams ----
    void
    stream_begin(const stream_dims_t &dims, std::uint32_t session_id, std::string_view client_key);
    void
    stream_end(std::uint32_t session_id, std::string_view stop_reason, std::string_view clip_offer = {}, std::string_view clip_flow = {});
    /** The host learns the encoder family only after a stream began (a probe may be running): the sender thread asks this for a stream that has none. */
    void
    set_encoder_resolver(std::function<std::string()> resolver) { encoder_resolver_ = std::move(resolver); }
    /** A stage of a stream start took `ms`: reported only when it is over the stage's bound (stage_is_slow). */
    void
    stream_stage(std::string_view stage, std::int64_t ms);
    /** A stream start failed at `stage` with `code` (fixed words and a stable error code, no text). */
    void
    stream_failed(std::string_view stage, std::string_view code);
    /** A driver operation failed (driver: vdd | gamepad | microphone | usbip | other; op: create | remove | start | stop | probe | other; code: a result code as letters and digits, or empty). */
    void
    driver_failed(std::string_view driver, std::string_view op, std::string_view code);
    /** At the start: if the previous run left the marker "running" it did not end by itself, which is reported once for that run (session_abnormal, with that run's session id). Then the marker says "running" for this run. */
    void
    begin_run_marker();
    /** At a clean stop: the marker says "exited". */
    void
    end_run_marker();
    void
    record_encoded_frame(std::uint32_t session_id) noexcept;
    void
    record_encode_ms(std::uint32_t session_id, double ms) noexcept;
    void
    record_capture_ms(std::uint32_t session_id, double ms) noexcept;
    /** Per-frame timings and counters for the stream-health windows (see stream_health.h). */
    void
    record_sample(std::uint32_t session_id, hooks::metric_e metric, double ms) noexcept;
    void
    record_count(std::uint32_t session_id, hooks::counter_e counter, std::uint64_t n) noexcept;
    /** The driver version of the streaming adapter (digits and dots), a dimension of the stream-health windows. */
    void
    set_gpu_driver(std::string driver);
    void
    record_frame_done(std::uint32_t session_id, std::int64_t steady_ns) noexcept;
    remote_config_t
    remote_config() const;
    /** The user's last actions and state changes (a bounded ring, attached to every error report). */
    void
    crumb(crumb_e kind, std::string_view id);
    /** An error of this component with its location and stack; the first of a fingerprint leaves at once, the rest is counted. Thread safe, never blocks on I/O. */
    void
    report_error(const error_input_t &error);
    bool
    streams_active() const;
    /** The sampler: read each active session's counters and the client's reports (probes) once. */
    void
    tick_streams(const std::vector<session_probe_t> &probes);

    // ---- the log ----
    /** A host log record (severity 3 warning, 4 error, 5 fatal); only counted and kept raw here, scrubbed later on the sender thread. Cheap; never throws. */
    void
    note_log(int severity, std::string_view message_template, std::string_view component = "host", std::string_view site = {}) noexcept;

    /** Every host log record (any severity) goes to a short ring; an excerpt of it leaves only after a severe stream anomaly, scrubbed. Cheap; never throws. */
    void
    note_log_line(int severity, const char *message, std::size_t length) noexcept;
    void
    note_log_line(int severity, std::string_view text) noexcept { note_log_line(severity, text.data(), text.size()); }
    /** Send the excerpts queued by severe anomalies (POST <endpoint>/v1/anomaly-log). -> the excerpts sent. Sender thread only. */
    int
    pump_logs();

    // ---- sender (the sender thread) ----
    /** Turn the buffer (and the log tallies when their hour is over) into batches on disk. `final` also closes the current log hour and gives every held count to the disk. `release_held`: the records held for the aggregate interval leave with this flush (the share of the day begins). */
    void
    flush(bool final = false, bool release_held = false);
    /** Send queued batches until one fails (then back off). -> the batches sent. */
    int
    pump();
    /** The sample, the weight and the moment of the day of this install (empty id: no install id yet, so it counts as in the sample). */
    report_plan_t
    plan();
    /** The routine batches may leave now: the share of the day has begun and today's batches were not all sent yet (always true while `routine_slot` is off). */
    bool
    routine_due();
    /** The server asked for a wait and it has not ended. */
    bool
    backing_off() const { return deps_.now_ms() < backoff_until_ms_.load(std::memory_order_relaxed); }
    /** The consent changed: drop what the off kinds had waiting, and the install id when both are off. A choice that was turned off is recorded once (opt_out.h) and sent by pump(). */
    void
    consent_changed();
    /** Compares the two choices with what they were the last time this looked: the ones turned off get their record (opt_out.h), written to disk first. Called when the config is saved and once at the start, after the context is set. */
    /** -> false when a turn-off could not be recorded yet (the context is not set, or the record could not be saved): the installation number is then kept and the state stays "on", so the next call tries again. */
    bool
    reconcile_consent();
    /** The buffer reached flush_events: the sender thread should not wait for its interval. */
    bool
    flush_wanted() const;

    // ---- bookkeeping ----
    service_counts_t
    counts() const;
    std::size_t
    buffered() const;
    std::size_t
    queued_files() const {
      std::lock_guard lock { queue_mutex_ };
      return queue_.files();
    }
    std::int64_t
    retry_not_before_ms() const { return not_before_ms_; }
    int
    consecutive_failures() const { return failures_; }
    const std::string &
    endpoint() const { return options_.endpoint; }
    std::string
    events_url() const { return options_.endpoint + "/v1/events"; }
    /** The events that wait in memory, as they would be sent (newest last), at most `max`; an install outside the sample does not see the sampled records that will not leave. */
    nlohmann::json
    preview(std::size_t max = 200);
    /** Called when the buffer reaches flush_events (the runner wakes its thread). */
    void
    set_wake(std::function<void()> wake) { wake_ = std::move(wake); }

  private:
    struct log_window_t {
      std::int64_t hour = -1;  ///< hours since the epoch
      std::uint64_t fatal = 0, error = 0, warning = 0;
    };
    using log_error_key_t = std::tuple<std::string, std::string, std::string>;  // component, source location, template
    struct log_error_t {
      int severity = 4;
      std::uint64_t count = 0;
    };

    consent_t
    consent_now() const;
    void
    collect_logs(std::int64_t now, bool close_hour);
    void
    on_window(stream_stats_t &stats, const health::window_t &window, std::int64_t now);
    void
    emit_health_chunk(stream_stats_t &stats, std::int64_t now);
    void
    apply_remote_config(const std::string &body);
    bool
    stream_detail(std::uint32_t session_id, const stream_dims_t &dims);
    /** Whether another excerpt may be sent now (the daily bound and the gap); nothing is used up until anomaly_log_commit(). Caller holds logs_mutex_. */
    bool
    anomaly_log_allowed(std::int64_t now);
    /** An excerpt was queued: use up the room (and save it). Caller holds logs_mutex_. */
    void
    anomaly_log_commit(std::int64_t now);
    /** What to do with the answer to a request that carried the batches at `paths`: sent (they are removed, the configuration it carries is applied), refused for good (removed), or to be tried again (-> false: stop). */
    bool
    handle_answer(const post_result_t &answer, std::size_t count, const std::vector<std::filesystem::path> &paths);
    /** Sends the records that are due (sender thread only); a delivered or refused one is removed, a failed one waits longer. */
    void
    send_opt_outs();
    /** The install id as it is on disk or in memory, whatever the consent now says: the record needs it for the turn-off that just happened. */
    std::string
    peek_install_id();
    void
    note_product(std::string_view product, std::string_view version);
    std::string
    log_excerpt(std::int64_t now);
    void
    quality_event(const stream_stats_t &stats, std::int64_t now);

    service_options_t options_;
    service_deps_t deps_;
    disk_queue_t queue_;
    mutable std::mutex queue_mutex_;  // the queue's directory: the sender thread's pump and a flush from another thread (shutdown) never overlap on it
    std::string session_id_;
    std::function<std::string()> encoder_resolver_;
    std::atomic<int> stage_events_ { 0 };
    std::atomic<int> driver_events_ { 0 };
    static constexpr int MAX_STAGE_EVENTS_PER_RUN = 20;
    static constexpr int MAX_DRIVER_EVENTS_PER_RUN = 20;
    void
    write_run_marker(std::string_view status, std::int64_t boot);

    mutable std::mutex mutex_;  // the buffer and the id; never held across I/O
    std::deque<event_t> ring_;
    std::deque<event_t> aggregate_ring_;  // high-volume events held until the aggregate interval is over (mutex_)
    std::int64_t last_aggregate_flush_ms_ = 0;
    // the take and the put-back of a flush and the buffer clear of a consent change hold flush_mutex_, so one change never slips between the two sections of a flush
    mutable std::mutex flush_mutex_;
    /// Per choice, how often it was found off by consent_changed(): an event a flush took out of the buffer is written or put back only while its own choice was not turned off since
    /// (turning the other choice on or off, or on again, must not lose it; turning its own off and on again must not bring it back). Read and moved under flush_mutex_.
    struct consent_marks_t {
      std::uint64_t usage = 0;
      std::uint64_t errors = 0;
      bool
      moved_since(kind_e kind, const consent_marks_t &then) const { return kind == kind_e::error ? errors != then.errors : usage != then.usage; }
    };
    consent_marks_t consent_marks_;
    context_t context_;
    std::string install_id_;
    service_counts_t counts_;
    std::mutex opt_out_mutex_;  // the pending records and the products seen; taken alone, never with the others
    std::vector<opt_out::record_t> opt_outs_;
    bool opt_outs_unsaved_ = false;  // the file does not show what opt_outs_ holds (a write failed): written again at the next pass, even when nothing is waiting
    std::map<std::string, std::string> products_seen_;  // product -> version of the clients that talked to this host
    std::function<void()> wake_;

    mutable std::mutex log_mutex_;  // the log tallies; held for a few instructions
    std::map<std::string, log_window_t> log_windows_;  // by component
    using log_error_table_t = std::map<log_error_key_t, log_error_t, std::less<>>;
    log_error_table_t log_errors_;
    static constexpr std::size_t MAX_LOG_ERRORS = 40;

    mutable std::shared_mutex streams_mutex_;
    std::map<std::uint32_t, std::shared_ptr<stream_stats_t>> streams_;
    std::map<std::string, std::int64_t> last_end_by_client_;
    std::map<std::uint32_t, std::string> client_of_;

    struct ring_line_t {
      std::int64_t ms;
      int severity;
      std::string text;
    };
    struct pending_log_t {
      std::string code;
      std::int64_t t = 0;
      std::string session;
      std::string text;
      int tries = 0;
    };
    breadcrumbs_t crumbs_;
    error_reporter_t reporter_;
    /** Writers of the breadcrumbs and the reporter take it with the consent check inside, and consent_changed() clears them under it: nothing written while error reports were off stays, nothing slips in after the clear. */
    std::mutex errors_mutex_;
    mutable std::mutex ring_mutex_;  // the log ring; held for a few instructions
    std::deque<ring_line_t> log_ring_;
    // pending_logs_, baseline_ and the anomaly_log_* state are used by the sender thread (pump_logs) and by the thread that ends a stream (on_window): logs_mutex_ guards them, never held across I/O,
    // taken before mutex_ and ring_mutex_ and never after them
    mutable std::mutex logs_mutex_;
    std::deque<pending_log_t> pending_logs_;
    std::string gpu_driver_;  // under mutex_
    remote_config_t config_;  // under mutex_
    std::unique_ptr<self_baseline_t> baseline_;
    std::int64_t anomaly_log_day_ = -1;
    int anomaly_log_count_ = 0;
    std::int64_t last_anomaly_log_ms_ = -1;
    bool anomaly_log_first_ = true;  // the first serious problem of this run always gets its excerpt (until the daily bound)

    std::int64_t not_before_ms_ = 0;
    // the share of the day (sender thread only, except started_ms_)
    std::int64_t started_ms_ = 0;
    std::atomic<std::int64_t> routine_day_done_ { -1 };  ///< the UTC day whose routine batches were all sent; kept in <dir>/routine_day (read by plan() on other threads)
    std::int64_t routine_flushed_day_ = -1;  ///< the day for which the buffer was flushed when the share began
    std::int64_t routine_not_before_ms_ = 0;  ///< after several failures the routine batches wait for the next day's share
    int routine_failures_ = 0;
    std::map<std::string, int> proxy_refusals_;  ///< per queue file: answers of a proxy or an old service (401, 403, 404, 405, 410, 421); after MAX_PROXY_REFUSALS the batch is given up so it cannot hold the head of the queue for ever (sender thread only)
    static constexpr int MAX_PROXY_REFUSALS = 8;
    /** The sample rate in force: the Worker's if it said one, else the local option. */
    double
    effective_sample_rate() const;
    void
    save_error_state();
    std::mutex error_state_mutex_;  // the text of error_days.json as it was last written
    std::string error_state_written_;
    // the server asked the clients to wait (cfg.backoff_s, or paused): priority 1 and 2 wait, priority 0 leaves once as counts
    std::atomic<std::int64_t> backoff_until_ms_ { 0 };  // written by the sender thread, read by backing_off() on any thread
    std::map<std::string, std::int64_t> p0_group_sent_at_;  // product and version of the priority 0 batches that left during a wait, and when (sender thread only)
    std::int64_t wait_period_ms_ = 0;  // how long the latest answer asked to wait: a group leaves again only after that (sender thread only)
    int failures_ = 0;
    std::deque<std::int64_t> post_times_;  // when the last requests went out (sender thread only)
    std::int64_t last_flush_ms_ = 0;
    std::int64_t last_log_flush_ms_ = 0;
  };

  /** A new random UUID v4 (text form). */
  std::string
  new_uuid_v4();

}  // namespace telemetry
