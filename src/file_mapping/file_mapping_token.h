/**
 * @file src/file_mapping_token.h
 * @brief Short-lived one-time tokens for file mapping WebSocket sessions.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace file_mapping_token {
  class token_store_t {
  public:
    using clock_t = std::chrono::steady_clock;

    explicit token_store_t(
      std::chrono::seconds ttl = std::chrono::seconds { 60 },
      std::size_t max_tokens = 128,
      std::size_t max_tokens_per_client = 4,
      std::chrono::seconds min_issue_interval = std::chrono::seconds { 1 });

    std::string issue(std::string client_uuid, clock_t::time_point now = clock_t::now());
    std::optional<std::string> consume(const std::string &token, clock_t::time_point now = clock_t::now());
    void cleanup(clock_t::time_point now = clock_t::now());
    std::size_t size() const;

  private:
    void cleanup_unlocked(clock_t::time_point now);
    std::size_t count_client_tokens_unlocked(const std::string &client_uuid) const;

    struct token_record_t {
      std::string client_uuid;
      clock_t::time_point expires_at;
    };

    std::chrono::seconds ttl_;
    std::size_t max_tokens_;
    std::size_t max_tokens_per_client_;
    std::chrono::seconds min_issue_interval_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, token_record_t> tokens_;
    std::unordered_map<std::string, clock_t::time_point> last_issue_by_client_;
  };
}  // namespace file_mapping_token
