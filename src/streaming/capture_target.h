/** @file src/streaming/capture_target.h
 * @brief Display actually captured by one video session, read by control requests.
 */
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace streaming {
  class capture_target_t {
  public:
    void
    set(std::string display_name) {
      std::lock_guard lock { mutex_ };
      display_name_ = std::move(display_name);
    }

    std::string
    display_name() const {
      std::lock_guard lock { mutex_ };
      return display_name_;
    }

  private:
    mutable std::mutex mutex_;
    std::string display_name_;
  };

  struct capture_session_t {
    std::string client_cert_uuid;
    bool running;
    bool control_only;
    std::shared_ptr<capture_target_t> target;
  };

  struct capture_target_result_t {
    bool found = false;
    std::shared_ptr<capture_target_t> target;
  };

  // First RUNNING video session authenticated by this certificate.
  inline capture_target_result_t
  select_current_capture(const std::vector<capture_session_t> &sessions, std::string_view client_cert_uuid) {
    if (client_cert_uuid.empty()) {
      return {};
    }
    for (const auto &session : sessions) {
      if (session.running && !session.control_only && session.client_cert_uuid == client_cert_uuid) {
        return { true, session.target };
      }
    }
    return {};
  }
}  // namespace streaming
