/** @file src/streaming/capture_target.h
 * @brief Actual capture target shared by one video session and control requests.
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace streaming {
  class capture_target_t {
  public:
    class locked_t {
    public:
      explicit locked_t(const capture_target_t &target):
          lock_(target.mutex_), target_(target) {
      }

      const std::string &
      display_name() const {
        return target_.display_name_;
      }

      const std::string &
      device_id() const {
        return target_.device_id_;
      }

      bool
      stopped() const {
        return target_.stopped_;
      }

    private:
      std::unique_lock<std::mutex> lock_;
      const capture_target_t &target_;
    };

    locked_t
    lock() const {
      return locked_t { *this };
    }

    // Only publish a display actually opened by the capture backend. Holding
    // lock() through a control operation serializes it with capture switching.
    void
    publish(std::string display_name, std::string device_id) {
      std::lock_guard lock { mutex_ };
      if (!stopped_) {
        display_name_ = std::move(display_name);
        device_id_ = std::move(device_id);
      }
    }

    // Called before releasing/reinitializing the old display, never afterwards.
    void
    invalidate() {
      std::lock_guard lock { mutex_ };
      display_name_.clear();
      device_id_.clear();
    }

    // Permanent: a late capture callback cannot revive a stopped session.
    void
    stop() {
      std::lock_guard lock { mutex_ };
      stopped_ = true;
      display_name_.clear();
      device_id_.clear();
    }

  private:
    mutable std::mutex mutex_;
    std::string display_name_;
    std::string device_id_;
    bool stopped_ = false;
  };

  enum class capture_target_error_e {
    none,
    no_active_stream,
    ambiguous_stream,
  };

  struct capture_session_t {
    std::string client_cert_uuid;
    bool running;
    bool control_only;
    std::shared_ptr<capture_target_t> target;
  };

  struct capture_target_result_t {
    capture_target_error_e error = capture_target_error_e::no_active_stream;
    std::shared_ptr<capture_target_t> target;
  };

  // The expected identity comes from GET options. Require a stable device ID
  // so a reused GDI name cannot silently redirect a pending adjustment.
  inline bool
  matches_capture_identity(std::string_view expected_device_id, std::string_view expected_display_name,
    std::string_view actual_device_id, std::string_view actual_display_name) {
    return !expected_device_id.empty() && !actual_device_id.empty() &&
           expected_device_id == actual_device_id &&
           (expected_display_name.empty() || expected_display_name == actual_display_name);
  }

  inline capture_target_result_t
  select_current_capture(const std::vector<capture_session_t> &sessions, std::string_view client_cert_uuid) {
    capture_target_result_t result;
    if (client_cert_uuid.empty()) {
      return result;
    }
    for (const auto &session : sessions) {
      if (!session.running || session.control_only || session.client_cert_uuid != client_cert_uuid) {
        continue;
      }
      if (result.error == capture_target_error_e::none) {
        return { capture_target_error_e::ambiguous_stream, {} };
      }
      result = { capture_target_error_e::none, session.target };
    }
    return result;
  }
}  // namespace streaming
