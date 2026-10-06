/**
 * @file src/transport/transport_budget.h
 * @brief Wire-budget allocation and the existing Moonlight RS shard limits.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace transport {
  constexpr std::size_t max_fec_blocks = 4;
  constexpr std::uint16_t max_rs_shards = 255;
  constexpr std::uint16_t max_unprotected_shards = 1023;

  struct budget_request_t {
    int total_kbps = 0;
    int other_kbps = 0;
    int repair_kbps = 0;
    int probe_kbps = 0;
    int video_overhead_kbps = 0;
    std::uint32_t fec_numerator = 0;
    std::uint32_t fec_denominator = 100;
    bool operator==(const budget_request_t &) const = default;
  };

  struct budget_allocation_t {
    int wire_budget_kbps = 0;
    int primary_video_kbps = 0;
    int encoder_kbps = 0;
  };

  // Invalid inputs have no allocation; exhausted budgets have a zero encoder
  // target. The caller decides whether to suspend video or lower its format.
  std::optional<budget_allocation_t>
  allocate_budget(const budget_request_t &request) noexcept;

  // Immutable negotiated wire layout. Codec bytes exclude the short frame
  // header; every shard includes its padded payload and all outer headers.
  struct video_packetization_t {
    std::uint32_t codec_payload_bytes = 0;
    std::uint32_t ip_packet_bytes = 0;
    std::uint32_t frame_header_bytes = 0;
    std::uint32_t frame_rate_num = 0;
    std::uint32_t frame_rate_den = 1;
    unsigned minimum_parity = 0;
  };

  // Preserve the requested reserve/ratio ceiling, then bound it by the actual
  // padded RS layout at the negotiated mean frame rate. This does not predict
  // or guarantee the size/deadline of an individual encoded access unit.
  std::optional<budget_allocation_t>
  allocate_budget(const budget_request_t &request, const video_packetization_t &packetization) noexcept;

  struct fec_block_t {
    std::uint16_t data_shards = 0;
    std::uint16_t parity_shards = 0;
    std::uint8_t encoded_percentage = 0;

    std::uint16_t total_shards() const noexcept {
      return data_shards + parity_shards;
    }
  };

  // encoded_percentage must round-trip through the client's ceil(D * F/100).
  // A minimum parity count can require more parity than the requested ratio.
  std::optional<fec_block_t>
  plan_fec_block(std::size_t data_shards, unsigned percentage, unsigned minimum_parity) noexcept;

  struct fec_frame_t {
    std::array<fec_block_t, max_fec_blocks> blocks {};
    std::size_t block_count = 0;
    bool fec_skipped = false;

    std::size_t data_shards() const noexcept;
    std::size_t parity_shards() const noexcept;
  };

  // Plans balanced, nonempty blocks, or an explicitly unprotected fallback.
  // Frames beyond the 10-bit per-block index are rejected, never truncated.
  std::optional<fec_frame_t>
  plan_fec_frame(std::size_t data_shards, unsigned percentage, unsigned minimum_parity) noexcept;

  // IP-layer accounting for a successfully submitted UDP datagram. Ethernet
  // overhead is intentionally excluded on both the sender and receiver.
  std::optional<std::uint64_t>
  ip_datagram_bytes(std::uint64_t udp_payload_bytes, bool ipv6) noexcept;
}  // namespace transport
