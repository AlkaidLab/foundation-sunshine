// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <bit>
#include <cstdint>

namespace haptics::wire {
  inline void
  write_u16(std::uint8_t *p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
  }

  inline void
  write_u32(std::uint8_t *p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
  }

  inline void
  write_u64(std::uint8_t *p, std::uint64_t value) {
    write_u32(p, static_cast<std::uint32_t>(value));
    write_u32(p + 4, static_cast<std::uint32_t>(value >> 32));
  }

  inline void
  write_float(std::uint8_t *p, float value) {
    write_u32(p, std::bit_cast<std::uint32_t>(value));
  }
}  // namespace haptics::wire
