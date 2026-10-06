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
      // ceil(D*F/100) >= M iff D*F > 100*(M-1). Choose the smallest
      // representable percentage; ceil(100*M/D) can add an unnecessary shard.
      encoded_percentage = static_cast<unsigned>((100 * (minimum_parity - 1)) / data_shards + 1);
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

  std::optional<budget_allocation_t>
  allocate_budget(const budget_request_t &request, const video_packetization_t &wire) noexcept {
    auto allocation = allocate_budget(request);
    if (!allocation || !wire.codec_payload_bytes || wire.codec_payload_bytes >= wire.ip_packet_bytes ||
        wire.ip_packet_bytes > 65535 || wire.frame_header_bytes > 65535 ||
        !wire.frame_rate_num || !wire.frame_rate_den || wire.minimum_parity > max_rs_shards) return {};
    const auto scaled_fec = static_cast<std::uint64_t>(request.fec_numerator) * 100;
    const auto percentage = scaled_fec / request.fec_denominator + (scaled_fec % request.fec_denominator != 0);
    if (percentage > 255) return {};
    const auto rate_denominator = static_cast<std::uint64_t>(wire.frame_rate_den) * 1000;
    int encoder_ceiling = 0;
    std::uint64_t largest_ip_rate = 0;
    // The existing 4-block/10-bit frame index bounds this search and every
    // multiplication below (at most4092*65535*8*UINT32_MAX < UINT64_MAX).
    for (std::size_t data = 1; data <= max_fec_blocks * max_unprotected_shards; ++data) {
      const auto payload = static_cast<std::uint64_t>(data) * wire.codec_payload_bytes;
      if (payload <= wire.frame_header_bytes) continue;
      const auto codec_rate = (payload - wire.frame_header_bytes) * 8 * wire.frame_rate_num / rate_denominator;
      const auto layout = plan_fec_frame(data, static_cast<unsigned>(percentage), wire.minimum_parity);
      if (!layout) return {};
      const auto ip_rate = static_cast<std::uint64_t>(layout->data_shards() + layout->parity_shards()) *
                           wire.ip_packet_bytes * 8 * wire.frame_rate_num;
      const auto ip_kbps = ip_rate / rate_denominator + (ip_rate % rate_denominator != 0);
      // Minimum-parity percentage rounding can make a smaller block cost more
      // than the next larger one. Do not hide that cost behind a larger plan.
      largest_ip_rate = std::max(largest_ip_rate, ip_kbps);
      if (!layout->fec_skipped && largest_ip_rate <= static_cast<std::uint64_t>(allocation->primary_video_kbps))
        encoder_ceiling = static_cast<int>(std::min(codec_rate, static_cast<std::uint64_t>(allocation->encoder_kbps)));
      if (codec_rate >= static_cast<std::uint64_t>(allocation->encoder_kbps)) break;
    }
    allocation->encoder_kbps = encoder_ceiling;
    return allocation;
  }
}  // namespace transport
