/**
 * @file src/confighttp/login_throttle.cpp
 * @brief Login failure backoff for Web UI password authentication.
 */

#include "login_throttle.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace confighttp::auth {
  namespace {
    std::string
    lowercase(std::string_view value) {
      std::string result { value };
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character);
      });
      return result;
    }
  }  // namespace

  login_throttle_t::login_throttle_t(throttle_policy_t policy, std::function<time_point_t()> clock):
      policy_(std::move(policy)),
      clock_(std::move(clock)) {}

  std::string
  login_throttle_t::key_for(const char *kind, const std::string_view value) {
    std::string key { kind };
    key.push_back(':');
    key.append(value);
    return key;
  }

  login_throttle_t::decision_t
  login_throttle_t::check_one_locked(const entry_t *entry, const time_point_t at) const {
    if (!entry) {
      return {};
    }
    if (at < entry->blocked_until) {
      const auto remaining = std::chrono::ceil<std::chrono::seconds>(entry->blocked_until - at);
      return { false, std::max(std::chrono::seconds { 1 }, remaining) };
    }
    return {};
  }

  login_throttle_t::decision_t
  login_throttle_t::check(const std::string_view address, const std::string_view username) {
    const auto at = clock_ ? clock_() : clock_t::now();
    const auto address_key = key_for("ip", address);
    const auto username_key = key_for("user", lowercase(username));

    std::lock_guard lock(mutex_);
    const auto address_decision = check_one_locked(
      entries_.contains(address_key) ? &entries_.at(address_key) : nullptr, at);
    const auto username_decision = check_one_locked(
      entries_.contains(username_key) ? &entries_.at(username_key) : nullptr, at);

    if (!address_decision.allowed) {
      return address_decision;
    }
    if (!username_decision.allowed) {
      return username_decision;
    }
    return {};
  }

  void
  login_throttle_t::record_failure_locked(const std::string_view key, const time_point_t at) {
    auto &entry = entries_[std::string { key }];
    entry.last_seen = at;
    if (!entry.window_started_set || at - entry.window_started >= policy_.window) {
      entry.window_started = at;
      entry.window_failures = 0;
      entry.window_started_set = true;
      // A new window also forgets the previous run of consecutive failures,
      // so one typo an hour later is not answered with the sixth step of the
      // backoff ladder [ds5-owasp-asvs4-v2-20261103] V2.2.1.
      entry.consecutive_failures = 0;
    }
    ++entry.window_failures;
    ++entry.consecutive_failures;

    if (entry.window_failures >= policy_.window_limit) {
      entry.blocked_until = std::max(entry.blocked_until, entry.window_started + policy_.window);
      return;
    }

    if (entry.consecutive_failures < policy_.free_attempts) {
      return;
    }

    const auto exponent = entry.consecutive_failures - policy_.free_attempts;
    const auto multiplier = static_cast<std::chrono::seconds::rep>(std::size_t { 1 } << std::min<std::size_t>(exponent, 20));
    const auto delay = std::min(policy_.base_delay * multiplier, policy_.maximum_delay);
    entry.blocked_until = std::max(entry.blocked_until, at + delay);
  }

  void
  login_throttle_t::record_failure(const std::string_view address, const std::string_view username) {
    const auto at = clock_ ? clock_() : clock_t::now();
    std::lock_guard lock(mutex_);
    record_failure_locked(key_for("ip", address), at);
    record_failure_locked(key_for("user", lowercase(username)), at);
    enforce_capacity_locked(at);
  }

  void
  login_throttle_t::record_success(const std::string_view address, const std::string_view username) {
    std::lock_guard lock(mutex_);
    entries_.erase(key_for("ip", address));
    entries_.erase(key_for("user", lowercase(username)));
  }

  void
  login_throttle_t::enforce_capacity_locked(const time_point_t at) {
    if (entries_.size() <= policy_.maximum_entries) {
      return;
    }

    for (auto it = entries_.begin(); it != entries_.end();) {
      if (at - it->second.last_seen >= policy_.window && at >= it->second.blocked_until) {
        it = entries_.erase(it);
      }
      else {
        ++it;
      }
    }

    while (entries_.size() > policy_.maximum_entries) {
      auto oldest = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.last_seen < oldest->second.last_seen) {
          oldest = it;
        }
      }
      entries_.erase(oldest);
    }
  }

  void
  login_throttle_t::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
  }
}  // namespace confighttp::auth
