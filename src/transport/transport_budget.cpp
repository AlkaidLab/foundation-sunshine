#include "transport_budget.h"

#include <algorithm>
#include <limits>

namespace transport {
  std::optional<budget_allocation_t>
  allocate_budget(const budget_request_t &request) noexcept {
    if (request.total_kbps < 0 || request.other_kbps < 0 || request.repair_kbps < 0 ||
        request.probe_kbps < 0 || request.video_overhead_kbps < 0 || request.fec_denominator == 0) {
      return std::nullopt;
    }

    const auto reserved = static_cast<std::int64_t>(request.other_kbps) + request.repair_kbps + request.probe_kbps;
    const auto primary = std::max<std::int64_t>(0, static_cast<std::int64_t>(request.total_kbps) - reserved);
    const auto available = static_cast<std::uint64_t>(std::max<std::int64_t>(0, primary - request.video_overhead_kbps));
    const auto divisor = static_cast<std::uint64_t>(request.fec_denominator) + request.fec_numerator;
    // available <= INT_MAX and denominator <= UINT32_MAX, so this fits uint64.
    const auto encoder = available * request.fec_denominator / divisor;
    return budget_allocation_t {
      request.total_kbps,
      static_cast<int>(primary),
      static_cast<int>(encoder),
    };
  }

  std::optional<fec_block_t>
  plan_fec_block(std::size_t data_shards, unsigned percentage, unsigned minimum_parity) noexcept {
    if (data_shards == 0 || data_shards > max_unprotected_shards || percentage > 255 || minimum_parity > max_rs_shards) {
      return std::nullopt;
    }
    if (percentage == 0) {
      return fec_block_t { static_cast<std::uint16_t>(data_shards), 0, 0 };
    }
    if (data_shards > max_rs_shards) {
      return std::nullopt;
    }

    auto encoded_percentage = percentage;
    auto parity = (data_shards * encoded_percentage + 99) / 100;
    if (parity < minimum_parity) {
      // Rounding the percentage down can describe fewer parity shards than
      // were generated. Round up and recalculate the actual count instead.
      encoded_percentage = static_cast<unsigned>((100 * minimum_parity + data_shards - 1) / data_shards);
      parity = (data_shards * encoded_percentage + 99) / 100;
    }
    if (encoded_percentage > 255 || data_shards + parity > max_rs_shards) {
      return std::nullopt;
    }

    return fec_block_t {
      static_cast<std::uint16_t>(data_shards),
      static_cast<std::uint16_t>(parity),
      static_cast<std::uint8_t>(encoded_percentage),
    };
  }

  std::size_t fec_frame_t::data_shards() const noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < block_count; ++i) {
      count += blocks[i].data_shards;
    }
    return count;
  }

  std::size_t fec_frame_t::parity_shards() const noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < block_count; ++i) {
      count += blocks[i].parity_shards;
    }
    return count;
  }

  std::optional<fec_frame_t>
  plan_fec_frame(std::size_t data_shards, unsigned percentage, unsigned minimum_parity) noexcept {
    if (data_shards == 0 || data_shards > max_fec_blocks * max_unprotected_shards || percentage > 255 || minimum_parity > max_rs_shards) {
      return std::nullopt;
    }

    if (percentage != 0) {
      // At most four candidates are needed. Starting with fewer blocks avoids
      // unnecessary minimum-parity overhead for small frames.
      for (std::size_t count = 1; count <= max_fec_blocks; ++count) {
        if (data_shards < count || data_shards > count * max_rs_shards) {
          continue;
        }
        fec_frame_t frame;
        frame.block_count = count;
        bool valid = true;
        for (std::size_t i = 0; i < count; ++i) {
          const auto data = data_shards / count + (i < data_shards % count ? 1 : 0);
          const auto block = plan_fec_block(data, percentage, minimum_parity);
          if (!block) {
            valid = false;
            break;
          }
          frame.blocks[i] = *block;
        }
        if (valid) {
          return frame;
        }
      }
    }

    // Keep the historical <=255 data-shard grouping for normal unprotected
    // frames; larger unprotected frames can still use the existing 10-bit
    // index, but cannot exceed four nonempty blocks.
    fec_frame_t frame;
    frame.fec_skipped = percentage != 0;
    frame.block_count = std::min<std::size_t>(max_fec_blocks, (data_shards + max_rs_shards - 1) / max_rs_shards);
    for (std::size_t i = 0; i < frame.block_count; ++i) {
      const auto data = data_shards / frame.block_count + (i < data_shards % frame.block_count ? 1 : 0);
      frame.blocks[i] = *plan_fec_block(data, 0, 0);
    }
    return frame;
  }

  std::optional<std::uint64_t>
  ip_datagram_bytes(std::uint64_t udp_payload_bytes, bool ipv6) noexcept {
    const std::uint64_t overhead = 8 + (ipv6 ? 40 : 20);
    if (udp_payload_bytes > std::numeric_limits<std::uint64_t>::max() - overhead) {
      return std::nullopt;
    }
    return udp_payload_bytes + overhead;
  }
}  // namespace transport
