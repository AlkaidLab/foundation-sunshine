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
 *   - A batch that cannot be sent waits on disk (size and file caps); a failure backs off 30 s doubling to an hour; a 4xx other than 408/425/429 drops the batch.
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
#include <shared_mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "events.h"
#include "queue.h"
#include "stream_stats.h"
#include "transport.h"

namespace telemetry {

  struct consent_t {
    bool usage = true;
    bool errors = true;
    bool any() const { return usage || errors; }
    bool allows(kind_e kind) const { return kind == kind_e::error ? errors : usage; }
  };

  /** The value of the two config keys: telemetry_consent (usage statistics) and telemetry_errors (error reports); both on unless the person said off. */
  consent_t consent_from_config(const std::string &usage_value, const std::string &errors_value, bool usage_key_present, bool errors_key_present);

  struct service_options_t {
    std::filesystem::path dir;  ///< <config>/telemetry: install_id and queue/
    std::string endpoint = "https://api.alkaidlab.com";  ///< base; batches go to <endpoint>/v1/events
    std::size_t ring_max = 2000;
    std::size_t flush_events = 200;  ///< a full buffer asks for a flush
    queue_limits_t queue;
    int max_batches_per_pump = 10;
    int max_posts_per_minute = 15;  ///< the Worker allows 20 requests a minute per install; the sender stays under it
    std::int64_t max_backoff_ms = 3600 * 1000;
    std::int64_t reconnect_window_ms = 30'000;
  };

  struct service_deps_t {
    std::function<std::int64_t()> now_ms;  ///< ms since the epoch
    /** One blocking POST of `body` (JSON) to `url`; the HTTP status, 0 when nothing answered. Called on the sender thread only. */
    std::function<post_result_t(const std::string &url, const std::string &body)> post;
    std::function<consent_t()> consent;
    std::function<double()> random01;  ///< backoff jitter; default: a random generator
  };

  struct service_counts_t {
    std::uint64_t emitted = 0;
    std::uint64_t refused = 0;  ///< consent off or not allowed
    std::uint64_t dropped = 0;  ///< buffer full
    std::uint64_t sent_batches = 0;
    std::uint64_t sent_events = 0;
    std::uint64_t rejected_batches = 0;
    std::uint64_t failed_posts = 0;
  };

  class service_t {
  public:
    service_t(service_options_t options, service_deps_t deps);

    // ---- identity ----
    /** The install id: created once in <dir>/install_id when a consent is on; empty when both are off (nothing is created then). */
    std::string install_id();
    const std::string &session_id() const { return session_id_; }
    /** {install_id, session_id, endpoint, usage, errors}; nullopt while both consents are off. */
    std::optional<nlohmann::json> identity();
    /** A new random install id (the person reset telemetry). */
    std::string rotate_install_id();
    void set_context(context_t context);
    context_t context() const;

    // ---- events ----
    /** Shape against the contract, apply consent, put in the buffer. -> true when it was kept. Thread safe, never blocks on I/O. */
    bool emit(kind_e kind, std::string_view name, const nlohmann::json &props, std::string_view product = "host", std::string_view version = {});
    /** Events from a page (panel, web) through the host API: the same, with the page allow-list. -> the number kept. */
    std::size_t ingest(std::string_view product, std::string_view version, const nlohmann::json &events);

    // ---- streams ----
    void stream_begin(const stream_dims_t &dims, std::uint32_t session_id, std::string_view client_key);
    void stream_end(std::uint32_t session_id, std::string_view stop_reason);
    void record_encoded_frame(std::uint32_t session_id) noexcept;
    void record_encode_ms(std::uint32_t session_id, double ms) noexcept;
    void record_capture_ms(std::uint32_t session_id, double ms) noexcept;
    bool streams_active() const;
    /** The sampler: read each active session's counters and the client's reports (probes) once. */
    void tick_streams(const std::vector<session_probe_t> &probes);

    // ---- the log ----
    /** A host log record (severity 3 warning, 4 error, 5 fatal); only counted and kept raw here, scrubbed later on the sender thread. Cheap; never throws. */
    void note_log(int severity, const char *message, std::size_t length, std::string_view component = "host") noexcept;

    // ---- sender (the sender thread) ----
    /** Turn the buffer (and the log tallies when their hour is over) into batches on disk. `final` also closes the current log hour. */
    void flush(bool final = false);
    /** Send queued batches until one fails (then back off). -> the batches sent. */
    int pump();
    /** The consent changed: drop what the off kinds had waiting, and the install id when both are off. */
    void consent_changed();
    /** The buffer reached flush_events: the sender thread should not wait for its interval. */
    bool flush_wanted() const;

    // ---- bookkeeping ----
    service_counts_t counts() const;
    std::size_t buffered() const;
    std::size_t queued_files() const {
      std::lock_guard lock { queue_mutex_ };
      return queue_.files();
    }
    std::int64_t retry_not_before_ms() const { return not_before_ms_; }
    int consecutive_failures() const { return failures_; }
    const std::string &endpoint() const { return options_.endpoint; }
    std::string events_url() const { return options_.endpoint + "/v1/events"; }
    /** The events that wait in memory, as they would be sent (newest last), at most `max`. */
    nlohmann::json preview(std::size_t max = 200) const;
    /** Called when the buffer reaches flush_events (the runner wakes its thread). */
    void set_wake(std::function<void()> wake) { wake_ = std::move(wake); }

  private:
    struct log_window_t {
      std::int64_t hour = -1;  ///< hours since the epoch
      std::uint64_t fatal = 0, error = 0, warning = 0;
    };
    struct raw_error_t {
      std::string component;
      std::string raw;
      int severity = 4;
      std::uint64_t count = 0;
    };

    consent_t consent_now() const;
    void collect_logs(std::int64_t now, bool close_hour);
    void quality_event(const stream_stats_t &stats, std::int64_t now);

    service_options_t options_;
    service_deps_t deps_;
    disk_queue_t queue_;
    mutable std::mutex queue_mutex_;  // the queue's directory: the sender thread's pump and a flush from another thread (shutdown) never overlap on it
    std::string session_id_;

    mutable std::mutex mutex_;  // the buffer and the id; never held across I/O
    std::deque<event_t> ring_;
    context_t context_;
    std::string install_id_;
    service_counts_t counts_;
    std::function<void()> wake_;

    mutable std::mutex log_mutex_;  // the log tallies; held for a few instructions
    std::map<std::string, log_window_t> log_windows_;  // by component
    std::map<std::string, raw_error_t> raw_errors_;  // by component + raw prefix
    static constexpr std::size_t MAX_RAW_ERRORS = 40;

    mutable std::shared_mutex streams_mutex_;
    std::map<std::uint32_t, std::shared_ptr<stream_stats_t>> streams_;
    std::map<std::string, std::int64_t> last_end_by_client_;
    std::map<std::uint32_t, std::string> client_of_;

    std::int64_t not_before_ms_ = 0;
    int failures_ = 0;
    std::deque<std::int64_t> post_times_;  // when the last requests went out (sender thread only)
    std::int64_t last_flush_ms_ = 0;
    std::int64_t last_log_flush_ms_ = 0;
  };

  /** A new random UUID v4 (text form). */
  std::string new_uuid_v4();

}  // namespace telemetry
