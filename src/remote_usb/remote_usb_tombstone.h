/**
 * @file src/remote_usb/remote_usb_tombstone.h
 * @brief Small TTL- and capacity-bounded set for asynchronous tombstones.
 *
 * The set is intentionally not internally synchronized.  The owner can keep
 * its existing lock and use the same value-type API as std::unordered_set.
 * Expiry is swept opportunistically by contains(), insert(), and size().
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <unordered_map>
#include <utility>

namespace remote_usb {

  /** Keep enough history for bursts while bounding memory below a few MiB. */
  inline constexpr std::size_t tombstone_default_max_entries = 4096;
  /** Longer than the host attach/detach command deadlines and callback tail. */
  inline constexpr auto tombstone_default_ttl = std::chrono::minutes(5);

  /**
   * A bounded set whose entries expire after a fixed interval.
   *
   * `max_entries` is a hard resident-entry limit.  When it is reached, the
   * oldest live entry is evicted before a new one is inserted.  Re-inserting an
   * existing key refreshes its TTL without increasing the resident count.
   * Callers should serialize access when using the set from multiple threads.
   */
  template <typename Key,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
  class bounded_tombstone_set final {
  public:
    using clock_t = std::chrono::steady_clock;

    struct policy {
      std::size_t max_entries { tombstone_default_max_entries };
      clock_t::duration ttl { tombstone_default_ttl };
    };

    explicit bounded_tombstone_set(policy policy = {}):
        policy_ { std::move(policy) } {
    }

    bounded_tombstone_set(const bounded_tombstone_set &) = delete;
    bounded_tombstone_set &
    operator=(const bounded_tombstone_set &) = delete;
    bounded_tombstone_set(bounded_tombstone_set &&) = delete;
    bounded_tombstone_set &
    operator=(bounded_tombstone_set &&) = delete;

    /** Insert or refresh a key. Returns false when the policy disables storage. */
    bool
    insert(const Key &key, clock_t::time_point now = clock_t::now()) {
      if (policy_.max_entries == 0 || policy_.ttl <= clock_t::duration::zero()) {
        return false;
      }

      prune(now);
      const auto expires_at = now + policy_.ttl;
      if (auto existing = entries_.find(key); existing != entries_.end()) {
        /* A refresh becomes the newest entry.  splice() does not allocate, so
         * the old record remains intact if anything above this point fails. */
        order_.splice(order_.end(), order_, existing->second.order);
        existing->second.expires_at = expires_at;
        return true;
      }

      while (entries_.size() >= policy_.max_entries) {
        evict_oldest();
      }

      order_.push_back(key);
      const auto order = std::prev(order_.end());
      try {
        auto [entry, inserted] = entries_.emplace(key, record { expires_at, order });
        if (!inserted) {
          /* A custom hash/equality implementation can report an equivalent key
           * after the lookup above.  Treat it exactly like a refresh and discard
           * the temporary list node. */
          order_.erase(order);
          order_.splice(order_.end(), order_, entry->second.order);
          entry->second.expires_at = expires_at;
          return true;
        }
      }
      catch (...) {
        /* emplace may fail after the list node has been allocated. */
        order_.erase(order);
        throw;
      }
      return true;
    }

    /** Return whether a non-expired key is present. */
    bool
    contains(const Key &key, clock_t::time_point now = clock_t::now()) {
      prune(now);
      return entries_.find(key) != entries_.end();
    }

    /** Remove one key and its ordering node. */
    std::size_t
    erase(const Key &key) {
      const auto entry = entries_.find(key);
      if (entry == entries_.end()) {
        return 0;
      }
      order_.erase(entry->second.order);
      entries_.erase(entry);
      return 1;
    }

    /** Remove all keys and their ordering nodes. */
    void
    clear() noexcept {
      entries_.clear();
      order_.clear();
    }

    /** Return the current resident count after sweeping expired keys. */
    std::size_t
    size(clock_t::time_point now = clock_t::now()) {
      prune(now);
      return entries_.size();
    }

  private:
    using order_t = std::list<Key>;

    struct record {
      clock_t::time_point expires_at;
      typename order_t::iterator order;
    };

    void
    prune(clock_t::time_point now) {
      while (!order_.empty()) {
        const auto entry = entries_.find(order_.front());
        if (entry == entries_.end()) {
          /* Defensive recovery if a user-provided hash/equality violates the
           * map/list invariant. */
          order_.pop_front();
          continue;
        }
        if (entry->second.expires_at > now) {
          /* Refreshes move to the back and TTL is fixed, so expiry order is
           * also list order for the steady_clock timestamps used in normal
           * operation. */
          break;
        }
        order_.pop_front();
        entries_.erase(entry);
      }
    }

    void
    evict_oldest() {
      if (order_.empty()) {
        /* This is only reachable if a user-provided container invariant was
         * violated.  Keep the hard bound intact rather than spinning forever. */
        if (!entries_.empty()) {
          entries_.erase(entries_.begin());
        }
        return;
      }
      const auto entry = entries_.find(order_.front());
      if (entry == entries_.end()) {
        order_.pop_front();
        return;
      }
      order_.pop_front();
      entries_.erase(entry);
    }

    policy policy_;
    std::unordered_map<Key, record, Hash, KeyEqual> entries_;
    order_t order_;
  };

}  // namespace remote_usb
