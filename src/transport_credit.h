#pragma once

#include <cstdint>

namespace transport::detail {
  constexpr std::int64_t credit_scale = 1000000;

  // Callers validate their own bounds, clamp rate changes and own the clock.
  // Credit is in byte-microseconds and never exceeds the current burst ceiling.
  constexpr std::int64_t
  refill_credit(std::int64_t credit, std::uint64_t rate_bytes_per_second,
    std::uint64_t burst_bytes, std::uint64_t elapsed_us) noexcept {
    if (!rate_bytes_per_second) return credit;
    const auto ceiling = static_cast<std::int64_t>(burst_bytes * credit_scale);
    const auto room = static_cast<std::uint64_t>(ceiling - credit);
    const auto fill_us = room / rate_bytes_per_second + (room % rate_bytes_per_second != 0);
    // Compare before multiplying: long idle time saturates without overflow.
    if (elapsed_us >= fill_us) return ceiling;
    return credit + static_cast<std::int64_t>(elapsed_us * rate_bytes_per_second);
  }
}  // namespace transport::detail
