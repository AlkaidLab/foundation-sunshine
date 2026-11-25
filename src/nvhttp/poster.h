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
  /** Cache files kept in the poster cache directory; the oldest are removed beyond it. */
  inline constexpr std::size_t cache_file_limit = 64;
  /** The longest a poster request waits for a preview that the background pass is still producing. */
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

  /** What one pass over the cache directory found and did. */
  struct cleanup_result_t {
    bool available = false;  ///< the directory exists and can hold files
    std::size_t removed_temporary = 0;  ///< temporary files of writes that never finished
    std::size_t evicted = 0;  ///< cache files removed to keep cache_file_limit
  };

  /** Enqueue decoding/encoding without delaying the asset response. */
  void
  schedule(const std::filesystem::path &source);

  /**
   * Block until the background pass has finished every scheduled source and holds no file open.
   * The worker is detached, so a caller that deletes a source right after scheduling it (a test) waits here first.
   * @return false when the timeout passed with work still pending.
   */
  bool
  wait_idle(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * Remove the temporary files that unfinished writes left in `directory` and the oldest cache files beyond
   * cache_file_limit. Best effort: whatever fails stays as it is, and the result tells how far it got. Never throws.
   */
  cleanup_result_t
  cleanup_cache_dir(const std::filesystem::path &directory) noexcept;

  /** Return the full representation, falling back to the untouched source. */
  std::optional<representation_t>
  full(const std::filesystem::path &source);

  /** Return a ready preview, or nullopt while the background pass is pending. Does not wait. */
  std::optional<representation_t>
  preview(const std::filesystem::path &source);

  /**
   * The preview of a source of more than preview_limit_bytes, for a caller that has just scheduled it: waits while
   * the background pass still has this version, and no longer than `limit`. A smaller source is its own preview.
   * @return nullopt at once when nothing will produce the preview (the pass was not asked for this version, gave up
   * on it, or cannot write the cache), and when the limit passed without one.
   */
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
