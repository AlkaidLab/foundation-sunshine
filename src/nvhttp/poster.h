/**
 * @file src/nvhttp/poster.h
 * @brief Asynchronous application-poster representations and HTTP validators.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nvhttp::apps::poster {

  inline constexpr std::size_t preview_limit_bytes = 200 * 1024;
  inline constexpr int preview_longest_side = 640;
  /** Maximum number of files kept in the poster cache. */
  inline constexpr std::size_t cache_file_limit = 64;
  /** Maximum time a poster request waits for a preview. */
  inline constexpr std::chrono::milliseconds preview_wait_limit { 2000 };

  struct representation_t {
    std::filesystem::path path;
    std::string content_type;
    std::string etag;
    std::uintmax_t length = 0;
  };

  struct byte_range_t {
    std::uintmax_t first = 0;
    std::uintmax_t last = 0;
  };

  /** Result of one cache cleanup pass. */
  struct cleanup_result_t {
    bool available = false;  ///< the directory exists and can hold files
    std::size_t removed_temporary = 0;  ///< temporary files of writes that never finished
    std::size_t evicted = 0;  ///< cache files removed to keep cache_file_limit
  };

  /** Enqueue decoding/encoding without delaying the asset response. */
  void
  schedule(const std::filesystem::path &source);

  /** Wait until scheduled work finishes or `timeout` expires. */
  bool
  wait_idle(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /** Remove stale temporary files and old cache files. Best effort and never throws. */
  cleanup_result_t
  cleanup_cache_dir(const std::filesystem::path &directory) noexcept;

  /** Return the full representation, falling back to the untouched source. */
  std::optional<representation_t>
  full(const std::filesystem::path &source);

  /** Return a ready preview without waiting for the background pass. */
  std::optional<representation_t>
  preview(const std::filesystem::path &source);

  /** Wait for a scheduled source to produce a preview, up to `limit`. */
  std::optional<representation_t>
  preview_when_ready(const std::filesystem::path &source, std::chrono::milliseconds limit = preview_wait_limit);

  /** Parse the single byte-range form supported by the asset endpoint. */
  std::optional<byte_range_t>
  parse_range(std::string_view value, std::uintmax_t length) noexcept;

  bool
  if_none_match(std::string_view value, std::string_view etag) noexcept;

  /**
   * Whether Range may be applied. fs-rfc9110-ranges-20261112 section 13.1.5:
   * a strong If-Range validator must match, otherwise the range is ignored.
   */
  bool
  if_range_matches(std::string_view value, std::string_view etag) noexcept;

}  // namespace nvhttp::apps::poster
