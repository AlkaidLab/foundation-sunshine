/**
 * @file src/clipboard_blob_store.h
 * @brief In-memory TTL-bounded blob storage for out-of-band clipboard payloads
 *        (large images, files) that exceed the encrypted control-stream's
 *        single-frame limit (`clipboard_bridge::kMaxPayloadBytes`).
 *
 * Design:
 *   * Producer (GUI agent or HTTP upload from a client) puts opaque bytes +
 *     a MIME hint and gets back a freshly-minted UUID.
 *   * Consumers (other clients via HTTP, or the GUI agent dispatching an
 *     inbound large object) GET by id. Reads do NOT remove by default; the
 *     TTL sweeper reclaims memory eventually so a client may retry on
 *     transient network failures.
 *   * No cross-process persistence — restarting Sunshine drops everything.
 *     Acceptable: clipboard items are short-lived by nature, and the wire
 *     KIND_REF that points here is also delivered over a live session.
 *
 * Eviction policy:
 *   * Per-blob TTL (see `kBlobTtlSeconds`).
 *   * Hard cap on total resident bytes (`kMaxStoreBytes`); when a put would
 *     exceed it, oldest blobs are evicted FIFO until it fits, even if their
 *     TTL hasn't expired.
 *
 * Threading: all public functions are thread-safe.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clipboard_bridge.h"  // for payload_t

namespace clipboard_blob_store {
  using payload_t = clipboard_bridge::payload_t;
  using blob_id = std::string;  ///< UUID v4 (canonical 36-char hex form).

  /// Per-blob hard size cap. Bigger uploads are rejected outright.
  constexpr std::size_t kMaxBlobBytes = 50ULL * 1024 * 1024;  // 50 MiB

  /// Total resident bytes cap across all blobs. FIFO eviction kicks in when
  /// a put would exceed this, regardless of TTL.
  constexpr std::size_t kMaxStoreBytes = 200ULL * 1024 * 1024;  // 200 MiB

  /// Per-blob TTL after the most recent put. Reads do NOT extend the TTL.
  constexpr int kBlobTtlSeconds = 60;

  struct put_result_t {
    blob_id id;       ///< Empty iff ok == false.
    bool ok;
    /// Reason for failure when !ok: "too_large" or "internal".
    std::string err;
  };

  /// Store bytes + MIME and return a freshly-minted id. The caller's vector
  /// is consumed. Triggers an opportunistic sweep + FIFO eviction.
  put_result_t put(payload_t bytes, std::string mime);

  struct get_result_t {
    bool found;
    payload_t bytes;
    std::string mime;
  };

  /// Look up by id. `found == false` for missing or expired entries.
  /// When `consume == true` the blob is removed on a successful read; useful
  /// for one-shot semantics where the consumer is sole.
  get_result_t get(const blob_id &id, bool consume = false);

  /// Force expiry of stale entries. Called opportunistically by put(); tests
  /// can also invoke it directly.
  void sweep_expired();

  /// Diagnostics — count and bytes of currently-resident entries.
  struct stats_t {
    std::size_t entries;
    std::size_t bytes;
  };

  stats_t stats();
}  // namespace clipboard_blob_store
