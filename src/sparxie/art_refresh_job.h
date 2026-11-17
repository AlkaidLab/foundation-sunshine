#pragma once

#include "src/sparxie/model.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sparxie {
  // One process-owned artwork job. Requests only take the state lock; HTTP and
  // image work run on its owned thread, which is joined before host shutdown.
  class ArtRefreshJob {
  public:
    using Work = std::function<json(const std::atomic_bool &)>;

    ~ArtRefreshJob() {
      stop();
    }

    json
    request(Work work) {
      std::lock_guard lock(mutex_);
      if (stopping_ || state_["state"] == "checking") {
        return state_;
      }
      if (worker_.joinable()) {
        worker_.join();
      }
      ++generation_;
      state_ = { { "ok", true }, { "state", "checking" }, { "jobId", generation_ }, { "retryAfterMs", kStatusRetryMs }, { "updated", 0 } };
      try {
        worker_ = std::thread([this, work = std::move(work)]() {
          json result;
          try {
            result = work(cancelled_);
            result["state"] = "ready";
          }
          catch (...) {
            result = { { "ok", false }, { "state", "failed" }, { "error_code", "art_refresh_failed" }, { "updated", 0 } };
          }
          std::lock_guard lock(mutex_);
          if (stopping_) {
            result["state"] = "stopped";
          }
          result["jobId"] = generation_;
          state_ = std::move(result);
          completed_.push_back(state_);
          if (completed_.size() > kRetainedResults) {
            completed_.pop_front();
          }
        });
      }
      catch (...) {
        state_["state"] = "failed";
        state_["ok"] = false;
        state_["error_code"] = "art_refresh_start_failed";
      }
      return state_;
    }

    json
    status(std::uint64_t job_id = 0) const {
      std::lock_guard lock(mutex_);
      if (job_id != 0 && state_["jobId"] != job_id) {
        for (const auto &result : completed_) {
          if (result["jobId"] == job_id) {
            return result;
          }
        }
        return { { "ok", false }, { "state", "expired" }, { "jobId", job_id }, { "updated", 0 } };
      }
      return state_;
    }

    void
    stop() {
      std::thread worker;
      {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        cancelled_.store(true);
        worker = std::move(worker_);
      }
      if (worker.joinable()) {
        worker.join();
      }
      std::lock_guard lock(mutex_);
      state_["state"] = "stopped";
    }

  private:
    static constexpr int kStatusRetryMs = 2000;
    static constexpr std::size_t kRetainedResults = 16;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic_bool cancelled_ { false };
    bool stopping_ = false;
    std::uint64_t generation_ = 0;
    std::deque<json> completed_;
    json state_ = { { "ok", true }, { "state", "idle" }, { "jobId", 0 }, { "updated", 0 } };
  };
}  // namespace sparxie
