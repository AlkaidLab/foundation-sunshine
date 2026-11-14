/**
 * @file src/confighttp/login_throttle.h
 * @brief Login failure backoff for Web UI password authentication.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace confighttp::auth {
  using clock_t = std::chrono::system_clock;
  using time_point_t = clock_t::time_point;

  /**
   * Anti-automation policy.
   *
   * Five free failures are followed by an exponential delay; the one-hour
   * window keeps a single account below the 100 attempts per hour ceiling in
   * OWASP ASVS 4.0.3 V2.2.1 [ds5-owasp-asvs4-v2-20261103].
   */
  struct throttle_policy_t {
    std::size_t free_attempts = 5;
    std::chrono::seconds base_delay { 30 };
    std::chrono::seconds maximum_delay { 15 * 60 };
    std::chrono::seconds window { 60 * 60 };
    std::size_t window_limit = 100;
    std::size_t maximum_entries = 4096;
  };

  class login_throttle_t {
  public:
    struct decision_t {
      bool allowed = true;
      std::chrono::seconds retry_after { 0 };
    };

    explicit login_throttle_t(throttle_policy_t policy = {}, std::function<time_point_t()> clock = {});

    decision_t
    check(std::string_view address, std::string_view username);

    void
    record_failure(std::string_view address, std::string_view username);

    void
    record_success(std::string_view address, std::string_view username);

    void
    clear();

  private:
    struct entry_t {
      std::size_t consecutive_failures = 0;
      std::size_t window_failures = 0;
      bool window_started_set = false;
      time_point_t window_started;
      time_point_t blocked_until;
      time_point_t last_seen;
    };

    static std::string
    key_for(const char *kind, std::string_view value);

    decision_t
    check_one_locked(const entry_t *entry, time_point_t at) const;

    void
    record_failure_locked(std::string_view key, time_point_t at);

    void
    enforce_capacity_locked(time_point_t at);

    throttle_policy_t policy_;
    std::function<time_point_t()> clock_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, entry_t> entries_;
  };
}  // namespace confighttp::auth
