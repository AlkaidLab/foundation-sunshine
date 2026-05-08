/**
 * @file src/clipboard_blob_store.cpp
 * @brief See clipboard_blob_store.h.
 */
#include "clipboard_blob_store.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace clipboard_blob_store {
  namespace {
    using clock_t = std::chrono::steady_clock;

    struct entry_t {
      payload_t bytes;
      std::string mime;
      clock_t::time_point expires_at;
    };

    std::mutex g_mu;
    // Map keyed by blob_id; insertion order tracked separately for FIFO eviction.
    std::unordered_map<blob_id, entry_t> g_entries;
    std::deque<blob_id> g_fifo;
    std::size_t g_total_bytes = 0;

    /// Generate a UUID-v4-shaped 36-char hex id without pulling in boost::uuid
    /// here. Cryptographic strength is not required — these ids are scoped to
    /// a live HTTPS-authenticated session and expire in 60 s.
    std::string
    make_id() {
      thread_local std::mt19937_64 rng { std::random_device {}() };
      std::uniform_int_distribution<std::uint64_t> dist;

      std::uint64_t hi = dist(rng);
      std::uint64_t lo = dist(rng);

      // Force version=4 nibble and variant bits per RFC 4122.
      hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
      lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

      static constexpr char kHex[] = "0123456789abcdef";
      std::string s(36, '-');
      auto write_byte = [&](std::size_t pos, std::uint8_t b) {
        s[pos] = kHex[b >> 4];
        s[pos + 1] = kHex[b & 0xF];
      };

      // 8-4-4-4-12 layout. Bytes 0..7 = hi, bytes 8..15 = lo.
      auto byte_at = [&](int i) -> std::uint8_t {
        std::uint64_t v = (i < 8) ? hi : lo;
        int shift = (7 - (i & 7)) * 8;
        return static_cast<std::uint8_t>((v >> shift) & 0xFF);
      };

      static constexpr int kSlots[] = { 0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34 };
      for (int i = 0; i < 16; ++i) {
        write_byte(static_cast<std::size_t>(kSlots[i]), byte_at(i));
      }
      return s;
    }

    /// Caller must hold g_mu. Drops everything past TTL.
    void
    sweep_locked(clock_t::time_point now) {
      // FIFO is roughly in insertion order, but TTLs are uniform so it's
      // also roughly expiry order. Walk from front and stop at first live one.
      while (!g_fifo.empty()) {
        auto it = g_entries.find(g_fifo.front());
        if (it == g_entries.end()) {
          g_fifo.pop_front();
          continue;
        }
        if (it->second.expires_at > now) {
          break;
        }
        g_total_bytes -= it->second.bytes.size();
        g_entries.erase(it);
        g_fifo.pop_front();
      }
    }

    /// Caller must hold g_mu. Force-evict oldest entries until total bytes
    /// + `incoming` fits under `kMaxStoreBytes`.
    void
    evict_for_locked(std::size_t incoming) {
      while (!g_fifo.empty() && g_total_bytes + incoming > kMaxStoreBytes) {
        auto it = g_entries.find(g_fifo.front());
        g_fifo.pop_front();
        if (it != g_entries.end()) {
          g_total_bytes -= it->second.bytes.size();
          g_entries.erase(it);
        }
      }
    }
  }  // namespace

  put_result_t
  put(payload_t bytes, std::string mime) {
    if (bytes.size() > kMaxBlobBytes) {
      return { {}, false, "too_large" };
    }
    if (bytes.size() > kMaxStoreBytes) {
      // Even after a full FIFO purge it wouldn't fit.
      return { {}, false, "too_large" };
    }

    auto now = clock_t::now();
    const std::size_t incoming = bytes.size();

    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);
    evict_for_locked(incoming);

    blob_id id = make_id();
    // Defensive: ensure no collision (vanishingly unlikely).
    while (g_entries.find(id) != g_entries.end()) {
      id = make_id();
    }

    entry_t e;
    e.bytes = std::move(bytes);
    e.mime = std::move(mime);
    e.expires_at = now + std::chrono::seconds(kBlobTtlSeconds);

    g_total_bytes += incoming;
    g_fifo.push_back(id);
    g_entries.emplace(id, std::move(e));

    return { id, true, {} };
  }

  get_result_t
  get(const blob_id &id, bool consume) {
    auto now = clock_t::now();

    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);

    auto it = g_entries.find(id);
    if (it == g_entries.end()) {
      return { false, {}, {} };
    }

    if (consume) {
      get_result_t r { true, std::move(it->second.bytes), std::move(it->second.mime) };
      g_total_bytes -= r.bytes.size();
      g_entries.erase(it);
      // Lazy: leave the dead id in g_fifo; sweep_locked drops it on next pass.
      return r;
    }

    // Copy out without mutating storage so retries work.
    return { true, it->second.bytes, it->second.mime };
  }

  void
  sweep_expired() {
    auto now = clock_t::now();
    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);
  }

  stats_t
  stats() {
    std::lock_guard<std::mutex> lk(g_mu);
    return { g_entries.size(), g_total_bytes };
  }
}  // namespace clipboard_blob_store
