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

namespace telemetry {

  struct queue_limits_t {
    std::size_t max_files = 100;
    std::size_t max_bytes = 1024 * 1024;  ///< total size of the queue directory
    std::size_t max_file_bytes = 300 * 1024;  ///< a larger file is not read back (it would break the 256 KB request limit)
  };

  class disk_queue_t {
  public:
    struct entry_t {
      std::filesystem::path path;
      nlohmann::json batch;
    };

    explicit disk_queue_t(std::filesystem::path dir, queue_limits_t limits = {});

    /** Write a batch. Afterwards the caps are enforced by deleting the oldest files. -> false when it could not be written. */
    bool push(const nlohmann::json &batch);
    /** The oldest readable batch. A file that cannot be read or is not a batch is deleted and the next one is tried. */
    std::optional<entry_t> front();
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
    std::vector<std::filesystem::path> list() const;

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

  /** Seconds to wait after `failures` consecutive failures: 30 s doubling to one hour, spread by `jitter` in [0, 1) up to 20 %. */
  std::int64_t backoff_seconds(int failures, double jitter);

  /** Whether an HTTP status is worth retrying (no answer, 408, 425, 429, 5xx) or means the batch is refused for good (other 4xx: dropped). */
  bool status_retryable(int status);

}  // namespace telemetry
