#pragma once

#include <mutex>
#include <string>

namespace spatial_audio {
  class MatLiveStatus {
  public:
    struct Snapshot {
      unsigned leases = 0, running = 0;
      bool observed = false;
      std::string last_error;
      const char *
      state() const {
        if (running) return "streaming";
        if (leases) return "ready";
        if (!last_error.empty()) return "error";
        return observed ? "idle" : "unobserved";
      }
    };
    Snapshot
    snapshot() const {
      std::lock_guard lock(mutex_);
      return value_;
    }
    void
    acquired() {
      std::lock_guard lock(mutex_);
      ++value_.leases;
      value_.observed = true;
      value_.last_error.clear();
    }
    void
    started() {
      std::lock_guard lock(mutex_);
      ++value_.running;
    }
    void
    released(bool running) {
      std::lock_guard lock(mutex_);
      --value_.leases;
      if (running) --value_.running;
    }
    void
    failed(const std::string &error) {
      std::lock_guard lock(mutex_);
      value_.observed = true;
      value_.last_error = error;
    }

  private:
    mutable std::mutex mutex_;
    Snapshot value_;
  };

  inline MatLiveStatus mat_live_status;
}  // namespace spatial_audio
