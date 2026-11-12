/**
 * @file src/platform/linux/edid.h
 * @brief Minimal EDID reader for the display product name.
 *
 * Windows gets a monitor's friendly name from the display API, which resolves
 * it from the monitor's EDID. Linux has the same bytes in the kernel's sysfs
 * blob (`/sys/class/drm/card*-<connector>/edid`), so this parses just enough of
 * the base block to recover the Display Product Name descriptor (tag 0xFC) and
 * hands the caller a name that matches what Windows shows.
 *
 * Everything here is a pure byte transformation so it can be unit-tested
 * against generated EDIDs; a panel whose EDID carries no name descriptor (some
 * laptop panels do not) yields nullopt and the caller keeps its fallback.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace edid {
  /// Base block size; extension blocks are not needed for the name.
  constexpr std::size_t base_block_bytes = 128;

  /// The four 18-byte descriptor slots at the end of the base block.
  constexpr std::size_t descriptor_offset = 54;
  constexpr std::size_t descriptor_size = 18;

  /// Display Product Name descriptor tag.
  constexpr std::uint8_t tag_display_product_name = 0xFC;

  /**
   * @brief Whether the blob looks like an EDID base block.
   */
  inline bool
  is_valid(std::span<const std::uint8_t> blob) {
    static constexpr std::uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (blob.size() < base_block_bytes) {
      return false;
    }
    return std::equal(std::begin(header), std::end(header), blob.begin());
  }

  /**
   * @brief The display product name, as the EDID advertises it.
   * @returns nullopt when the blob is not an EDID or carries no name
   *          descriptor; the name is trimmed of its terminator and padding
   *          (the descriptor is 13 bytes, NUL/0x0A terminated, space padded).
   */
  inline std::optional<std::string>
  monitor_name(std::span<const std::uint8_t> blob) {
    if (!is_valid(blob)) {
      return std::nullopt;
    }

    for (std::size_t slot = 0; slot < 4; ++slot) {
      const auto offset = descriptor_offset + slot * descriptor_size;
      const auto *descriptor = blob.data() + offset;

      // A display descriptor has 00 00 00 <tag> 00 in its first five bytes.
      if (descriptor[0] != 0x00 || descriptor[1] != 0x00 || descriptor[2] != 0x00 ||
          descriptor[3] != tag_display_product_name || descriptor[4] != 0x00) {
        continue;
      }

      std::string name;
      for (std::size_t i = 5; i < descriptor_size; ++i) {
        const auto byte = descriptor[i];
        if (byte == 0x00 || byte == 0x0A) {
          break;  // terminator
        }
        name.push_back(static_cast<char>(byte));
      }

      // The payload is space padded; trailing whitespace is not part of the name.
      while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) {
        name.pop_back();
      }

      if (!name.empty()) {
        return name;
      }
    }

    return std::nullopt;
  }
}  // namespace edid
