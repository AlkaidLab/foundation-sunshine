/**
 * @file src/nvhttp/poster.h
 * @brief Asynchronous application-poster representations and HTTP validators.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nvhttp::apps::poster {

  inline constexpr std::size_t preview_limit_bytes = 200 * 1024;
  inline constexpr int preview_longest_side = 640;

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

  /** Enqueue decoding/encoding without delaying the asset response. */
  void
  schedule(const std::filesystem::path &source);

  /** Return the full representation, falling back to the untouched source. */
  std::optional<representation_t>
  full(const std::filesystem::path &source);

  /** Return a ready preview, or nullopt while the background pass is pending. */
  std::optional<representation_t>
  preview(const std::filesystem::path &source);

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
