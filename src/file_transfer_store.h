/**
 * @file src/file_transfer_store.h
 * @brief Short-lived file-transfer offers for host-to-client sends.
 *
 * The clipboard bridge carries only a small FILE_OFFER control message. The
 * actual file bytes stay on disk and are streamed through the HTTPS endpoint
 * when a paired client fetches the offer id.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace file_transfer_store {
  using offer_id = std::string;

  constexpr int kOfferTtlSeconds = 10 * 60;
  constexpr std::uint64_t kMaxSingleFileBytes = 4ULL * 1024 * 1024 * 1024;

  struct offer_t {
    offer_id id;
    std::filesystem::path path;
    std::string display_name;
    std::uint64_t size;
    std::chrono::steady_clock::time_point expires_at;
  };

  struct create_result_t {
    bool ok;
    std::string err;
    offer_t offer;
  };

  /// Create a single-file offer from an existing host path. Directories and
  /// non-regular files are rejected in the initial MVP.
  create_result_t create_single_file_offer(const std::filesystem::path &path);

  struct get_result_t {
    bool found;
    offer_t offer;
  };

  /// Look up a live offer by id. Reads do not consume the offer so clients can
  /// retry interrupted downloads until TTL expiry.
  get_result_t get(const offer_id &id);

  void sweep_expired();

  struct stats_t {
    std::size_t entries;
  };

  stats_t stats();
}  // namespace file_transfer_store
