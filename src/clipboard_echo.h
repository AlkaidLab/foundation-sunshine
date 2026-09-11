/**
 * @file src/clipboard_echo.h
 * @brief Echo suppression for clipboard writes: a bounded ring of what this
 *        host last wrote, so the matching host-side change is not broadcast
 *        back to the clients.
 *
 * Mirrors the GUI agent's EchoState (sunshine-control-panel,
 * src-tauri/src/clipboard.rs): up to 16 (kind, payload-hash) entries, each with
 * a TTL, checked with a fresh hash of the candidate payload. The agent keeps
 * text and image rings separately; this ring carries the kind in the entry, so
 * one ring serves every kind a provider handles.
 *
 * The ring is deliberately not thread-safe: the caller owns the locking, as the
 * provider already does for its echo state.
 */
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "clipboard_bridge.h"

namespace clipboard_echo {
  /// Entries the agent keeps before dropping the oldest.
  constexpr std::size_t kCapacity = 16;

  /**
   * @brief Stable 64-bit hash of a frame payload.
   * @details FNV-1a mixed with the kind. The echo check only ever compares our
   *          own writes against our own reads, so a stable in-process hash is
   *          enough; the agent's DefaultHasher serves the same purpose.
   */
  inline std::uint64_t
  hash_payload(std::uint8_t kind, std::span<const std::uint8_t> bytes) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
    const auto mix = [&hash](std::uint8_t byte) {
      hash ^= byte;
      hash *= 1099511628211ULL;  // FNV prime
    };

    mix(kind);
    for (const auto byte : bytes) {
      mix(byte);
    }
    return hash;
  }

  class ring_t {
  public:
    /**
     * @brief Remember a payload this host just wrote.
     */
    void
    record(
      std::uint8_t kind,
      std::span<const std::uint8_t> bytes,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
      std::chrono::seconds ttl = clipboard_bridge::kEchoTtl) {
      if (size_ == kCapacity) {
        // Drop the oldest entry, exactly like the agent's pop_front().
        begin_ = (begin_ + 1) % kCapacity;
        --size_;
      }

      entries_[(begin_ + size_) % kCapacity] = entry_t { kind, hash_payload(kind, bytes), now + ttl };
      ++size_;
    }

    /**
     * @brief Whether this payload is an echo of something we wrote recently.
     * @details Expired entries are pruned first, as the agent does in is_echo().
     */
    bool
    is_echo(
      std::uint8_t kind,
      std::span<const std::uint8_t> bytes,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
      prune(now);

      const auto hash = hash_payload(kind, bytes);
      for (std::size_t i = 0; i < size_; ++i) {
        const auto &entry = entries_[(begin_ + i) % kCapacity];
        if (entry.kind == kind && entry.hash == hash) {
          return true;
        }
      }
      return false;
    }

    std::size_t
    size() const {
      return size_;
    }

    void
    clear() {
      size_ = 0;
      begin_ = 0;
    }

  private:
    struct entry_t {
      std::uint8_t kind = 0;
      std::uint64_t hash = 0;
      std::chrono::steady_clock::time_point expires {};
    };

    void
    prune(std::chrono::steady_clock::time_point now) {
      while (size_ > 0 && entries_[begin_].expires <= now) {
        begin_ = (begin_ + 1) % kCapacity;
        --size_;
      }
    }

    std::array<entry_t, kCapacity> entries_ {};
    std::size_t begin_ = 0;
    std::size_t size_ = 0;
  };
}  // namespace clipboard_echo
