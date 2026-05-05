/**
 * @file src/security/ip_counter.h
 * @brief Thread-safe per-IP counter map with a hard size cap.
 *
 * Provides the common storage layer for IP-keyed safety/observability
 * primitives (rate limiters, log throttlers, ...). Built on top of
 * `sync_util::sync_t` so locking follows the project's idiomatic pattern.
 *
 * When the map reaches @c kDefaultMaxEntries and a brand-new IP arrives,
 * the entire map is cleared and the cycle restarts. This is intentionally
 * cheap and predictable -- scanners or NAT churn cannot grow memory without
 * bound, and the counter is not a security control on its own.
 */
#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>

#include "sync.h"

namespace security {

  /**
   * @brief Bounded, mutex-guarded ip->entry map intended as a base class for
   * concrete per-IP counters.
   * @tparam EntryT Per-IP entry type. Must be default-constructible.
   */
  template <typename EntryT>
  class ip_counter_t {
  public:
    using map_t = std::unordered_map<std::string, EntryT>;
    static constexpr std::size_t kDefaultMaxEntries = 256;

    explicit ip_counter_t(std::size_t max_entries = kDefaultMaxEntries):
        max_entries_ { max_entries } {}

    ip_counter_t(const ip_counter_t &) = delete;
    ip_counter_t &
    operator=(const ip_counter_t &) = delete;

    /**
     * @brief Forget the entry for @p key, if any.
     */
    void
    forget(const std::string &key) {
      auto lg = entries_.lock();
      entries_->erase(key);
    }

    /**
     * @brief Current number of tracked IPs (mostly for tests/diagnostics).
     */
    std::size_t
    size() {
      auto lg = entries_.lock();
      return entries_->size();
    }

  protected:
    /**
     * @brief Run @p fn on the entry for @p key while holding the lock.
     * If the map is full and @p key is new, the map is cleared first so the
     * insertion succeeds without unbounded growth.
     * @return The value returned by @p fn.
     */
    template <typename Fn>
    auto
    with_entry(const std::string &key, Fn &&fn) -> decltype(fn(std::declval<EntryT &>())) {
      auto lg = entries_.lock();
      if (entries_->size() >= max_entries_ && entries_->find(key) == entries_->end()) {
        entries_->clear();
      }
      return std::forward<Fn>(fn)((*entries_)[key]);
    }

    /**
     * @brief Erase entries for which @p pred returns true.
     */
    template <typename Pred>
    void
    erase_if_locked(Pred &&pred) {
      auto lg = entries_.lock();
      for (auto it = entries_->begin(); it != entries_->end();) {
        if (pred(it->second)) {
          it = entries_->erase(it);
        }
        else {
          ++it;
        }
      }
    }

  private:
    sync_util::sync_t<map_t> entries_;
    std::size_t max_entries_;
  };

}  // namespace security
