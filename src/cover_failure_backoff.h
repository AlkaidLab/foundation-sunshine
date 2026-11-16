/**
 * @file src/cover_failure_backoff.h
 * @brief Remembers which cover URLs failed, so an application list that is read again and again does not download the same failing cover every time.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

namespace http {

  /**
   * @brief A failed URL is not tried again for a while; every further failure doubles that time up to a limit, and a success forgets the URL.
   *
   * The application list is validated on every refresh and every client request (process.cpp: validate_app_image_path downloads a cover that has no file yet),
   * so a cover that cannot be fetched (the CDN answers with something that is not an image, a game without a header image) was fetched again each time:
   * one install reported the same download error 366 times in a day. Std only.
   */
  class cover_failure_backoff_t {
  public:
    using clock = std::chrono::steady_clock;

    explicit cover_failure_backoff_t(std::chrono::seconds first = std::chrono::minutes { 10 }, std::chrono::seconds limit = std::chrono::hours { 6 }, std::size_t capacity = 512):
        first_ { first }, limit_ { limit }, capacity_ { capacity } {}

    /** @return true when the URL failed recently enough that it should not be tried now. */
    bool
    blocked(const std::string &url, clock::time_point now = clock::now()) {
      std::lock_guard lock { mutex_ };
      const auto it = failures_.find(url);
      return it != failures_.end() && now < it->second.retry_at;
    }

    void
    note_failure(const std::string &url, clock::time_point now = clock::now()) {
      std::lock_guard lock { mutex_ };
      if (failures_.size() >= capacity_ && failures_.find(url) == failures_.end()) {
        // make room with the waits that have run out; when every one is still running, the new URL is not remembered (it is tried again at its next turn)
        // and the ones that are waiting keep waiting
        for (auto it = failures_.begin(); it != failures_.end();) {
          it = now >= it->second.retry_at ? failures_.erase(it) : std::next(it);
        }
        if (failures_.size() >= capacity_) return;
      }
      auto &entry = failures_[url];
      const std::chrono::seconds wait = std::min(limit_, std::chrono::seconds { first_.count() * (1LL << std::min(entry.count, 20)) });
      entry.count = std::min(entry.count + 1, 20);
      entry.retry_at = now + wait;
    }

    void
    note_success(const std::string &url) {
      std::lock_guard lock { mutex_ };
      failures_.erase(url);
    }

  private:
    struct entry_t {
      int count = 0;
      clock::time_point retry_at {};
    };

    std::chrono::seconds first_;
    std::chrono::seconds limit_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::unordered_map<std::string, entry_t> failures_;
  };

}  // namespace http
