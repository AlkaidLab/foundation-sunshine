/**
 * @file src/clipboard_blob_store.cpp
 * @brief See clipboard_blob_store.h.
 */
#include "clipboard_blob_store.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <openssl/rand.h>

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

    /// Generate a UUID-v4-shaped 36-char hex id using OpenSSL's CSPRNG.
    ///
    /// The id functions as a bearer capability: any HTTPS-authenticated client
    /// that learns it (via a KIND_REF wire frame) can fetch the blob bytes
    /// without further per-blob authorisation. To keep that property safe we
    /// must use a cryptographically-strong random source, not std::mt19937 —
    /// 122 bits of CSPRNG entropy + the 60 s TTL make guessing infeasible.
    std::string
    make_id() {
      std::uint8_t raw[16];
      if (RAND_bytes(raw, sizeof(raw)) != 1) {
        // RAND_bytes only fails when the OpenSSL RNG is uninitialised, which
        // would make the whole TLS stack unusable too — surface as a hard
        // error rather than silently degrading entropy.
        throw std::runtime_error("clipboard_blob_store: RAND_bytes failed");
      }

      // Force version=4 nibble and RFC 4122 variant bits.
      raw[6] = static_cast<std::uint8_t>((raw[6] & 0x0F) | 0x40);
      raw[8] = static_cast<std::uint8_t>((raw[8] & 0x3F) | 0x80);

      static constexpr char kHex[] = "0123456789abcdef";
      std::string s(36, '-');
      static constexpr int kSlots[] = { 0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34 };
      for (int i = 0; i < 16; ++i) {
        const std::size_t pos = static_cast<std::size_t>(kSlots[i]);
        s[pos] = kHex[raw[i] >> 4];
        s[pos + 1] = kHex[raw[i] & 0xF];
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
