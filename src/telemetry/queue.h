/**
 * @file src/telemetry/queue.h
 * @brief The on-disk queue of telemetry batches: size caps, oldest dropped first, and the retry backoff.
 *
 * Pure code (std + nlohmann::json + std::filesystem). A batch is one envelope of POST /v1/events (events.h). The queue lives in <config>/telemetry/queue;
 * a file is written to a temporary name and renamed, so a crash never leaves a half batch that would be sent.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "events.h"

namespace telemetry {

  struct queue_limits_t {
    std::size_t max_files = 100;
    std::size_t max_bytes = 1024 * 1024;  ///< total size of the queue directory
    std::size_t max_file_bytes = 300 * 1024;  ///< a larger file is not read back (it would break the 256 KB request limit)
    std::size_t max_p0_files = 40;  ///< routine batches of priority 0 kept at most; the others make room before any of them is dropped
    std::size_t max_error_files = 60;  ///< batches of errors (delivery_e::error) kept at most
    std::size_t max_feedback_files = 30;  ///< feedback reports (delivery_e::feedback) kept at most
  };

  /** Which batches a read looks at: all of them, only the ones that must arrive (errors and feedback), or only the routine ones. */
  enum class lane_e {
    any,
    urgent,
    routine,
  };

  class disk_queue_t {
  public:
    struct entry_t {
      std::filesystem::path path;
      nlohmann::json batch;
    };

    explicit disk_queue_t(std::filesystem::path dir, queue_limits_t limits = {});

    /**
     * Write a batch of events of one priority (event_priority: 0, 1 or 2) and one delivery class. Afterwards the caps are enforced: priority 2 goes first, then 1, then routine priority 0, then errors, feedback last, oldest first
     * within each; errors and feedback also have a bound of their own, routine priority 0 is only cut past `max_p0_files`. -> false when it could not be written.
     */
    bool
    push(const nlohmann::json &batch, int priority = 2, delivery_e delivery = delivery_e::routine);
    /**
     * The next readable batch at or above `max_priority` in the given lane: feedback first, then errors, then priority 0 to 2, oldest first within one. A file that cannot be read or is not a batch is deleted and the next one is tried.
     */
    std::optional<entry_t>
    front(int max_priority = 2, lane_e lane = lane_e::any);
    /** Up to `limit` readable batches of priority 0 up to `max_priority`, in the order of front(). */
    std::vector<entry_t>
    entries(int max_priority, std::size_t limit);
    std::size_t
    files_of(int priority) const;
    /** How many files hold batches of this delivery class. */
    std::size_t
    files_of(delivery_e delivery) const;
    /** The delivery class a file name says (older files and files of any other name are routine). */
    static delivery_e
    delivery_of_name(const std::filesystem::path &path);
    void remove(const std::filesystem::path &path);
    /** Replace the content of a kept file (the events of a kind whose consent is off were taken out). */
    bool rewrite(const std::filesystem::path &path, const nlohmann::json &batch);
    void clear();
    /** Take the events a consent no longer allows out of every kept batch; a batch with none left is deleted. */
    void refilter(bool usage, bool errors);

    std::size_t files() const;
    std::size_t bytes() const;
    /** Delete the oldest files until both caps hold. -> how many were deleted. */
    std::size_t enforce_limits();
    /** The batches dropped by the caps so far in this process. */
    std::uint64_t dropped() const { return dropped_; }

    const std::filesystem::path &directory() const { return dir_; }

  private:
    /** All files: sorted by priority, then by name (time). */
    std::vector<std::filesystem::path> list() const;
    static int
    priority_of_name(const std::filesystem::path &path);

    std::filesystem::path dir_;
    queue_limits_t limits_;
    std::uint64_t counter_ = 0;
    std::uint64_t dropped_ = 0;
  };

  /**
   * Take out of an envelope the events that its consent no longer allows (usage and experience need `usage`, error needs `errors`).
   * -> how many events are left (0: the batch has nothing to send).
   */
  std::size_t filter_by_consent(nlohmann::json &envelope, bool usage, bool errors);

  /** Seconds to wait after `failures` consecutive failures: `base_s` doubling to `cap_s` (30 s to one hour by default), spread by `jitter` in [0, 1) up to 20 %. */
  std::int64_t backoff_seconds(int failures, double jitter, int base_s = 30, int cap_s = 3600);

  /** `asked_s` (the Retry-After of an answer, up to `cap_s`) in milliseconds, spread by `jitter` in [0, 1) up to 20 %, so clients that were told the same number do not come back in the same second. 0 when nothing was asked. */
  std::int64_t retry_after_ms(long asked_s, double jitter, std::int64_t cap_s = 3600);

  /**
   * What to do with a batch after the answer. Retryable: no answer (0), 408, 425, 429 and 5xx; the batch stays on disk. Also kept: a 4xx that a server in between may have sent for a reason that is not about
   * this batch (401, 403, 404, 405, 410, 421: a proxy, a firewall, a service that does not have the route yet), so a report is not lost to a wrong answer. Refused for good: 400, 413, 415, 422 and the other
   * 4xx that describe the body itself (it is dropped), and 3xx (a redirect is not an answer to a POST).
   */
  bool status_retryable(int status);

}  // namespace telemetry
