/**
 * @file src/file_mapping_token.cpp
 * @brief Short-lived one-time tokens for file mapping WebSocket sessions.
 */
#include "file_mapping_token.h"

#include <array>
#include <random>
#include <sstream>

namespace file_mapping_token {
  namespace {
    std::string
    random_token() {
      std::array<unsigned char, 32> bytes {};
      std::random_device random;
      for (auto &byte : bytes) {
        byte = static_cast<unsigned char>(random());
      }

      std::ostringstream out;
      out << std::hex;
      for (const auto byte : bytes) {
        out.width(2);
        out.fill('0');
        out << static_cast<unsigned int>(byte);
      }
      return out.str();
    }
  }  // namespace

  token_store_t::token_store_t(
    std::chrono::seconds ttl,
    std::size_t max_tokens,
    std::size_t max_tokens_per_client,
    std::chrono::seconds min_issue_interval):
      ttl_(ttl),
      max_tokens_(max_tokens),
      max_tokens_per_client_(max_tokens_per_client),
      min_issue_interval_(min_issue_interval) {
  }

  std::string
  token_store_t::issue(std::string client_uuid, clock_t::time_point now) {
    if (client_uuid.empty()) {
      return {};
    }

    std::scoped_lock lock { mutex_ };
    cleanup_unlocked(now);

    if (max_tokens_ != 0 && tokens_.size() >= max_tokens_) {
      return {};
    }
    if (max_tokens_per_client_ != 0 && count_client_tokens_unlocked(client_uuid) >= max_tokens_per_client_) {
      return {};
    }
    if (auto last = last_issue_by_client_.find(client_uuid); last != last_issue_by_client_.end() && now < last->second + min_issue_interval_) {
      return {};
    }

    auto token = random_token();
    while (tokens_.contains(token)) {
      token = random_token();
    }

    last_issue_by_client_[client_uuid] = now;
    tokens_.emplace(token, token_record_t { std::move(client_uuid), now + ttl_ });
    return token;
  }

  std::optional<std::string>
  token_store_t::consume(const std::string &token, clock_t::time_point now) {
    std::scoped_lock lock { mutex_ };
    cleanup_unlocked(now);

    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
      return std::nullopt;
    }
    if (it->second.expires_at < now) {
      tokens_.erase(it);
      return std::nullopt;
    }

    auto client_uuid = std::move(it->second.client_uuid);
    tokens_.erase(it);
    return client_uuid;
  }

  void
  token_store_t::cleanup(clock_t::time_point now) {
    std::scoped_lock lock { mutex_ };
    cleanup_unlocked(now);
  }

  void
  token_store_t::cleanup_unlocked(clock_t::time_point now) {
    for (auto it = tokens_.begin(); it != tokens_.end();) {
      if (it->second.expires_at < now) {
        it = tokens_.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::size_t
  token_store_t::count_client_tokens_unlocked(const std::string &client_uuid) const {
    std::size_t count = 0;
    for (const auto &[_, token] : tokens_) {
      if (token.client_uuid == client_uuid) {
        ++count;
      }
    }
    return count;
  }

  std::size_t
  token_store_t::size() const {
    std::scoped_lock lock { mutex_ };
    return tokens_.size();
  }
}  // namespace file_mapping_token
