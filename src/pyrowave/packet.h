/**
 * @file src/pyrowave/packet.h
 * @brief Transport-neutral PyroWave frame and packet contracts.
 */
#pragma once

#include "types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pyrowave {

  // Preserve one complete inner wire packet per RTP payload. Zero means the
  // negotiated bound cannot carry the encoder and transport headers.
  [[nodiscard]] int
  limit_rtp_packet_size(int requested_packet_size, std::uint32_t max_wire_packet_size) noexcept;

  struct bitstream_buffer_t {
    using storage_t = std::shared_ptr<const std::vector<std::uint8_t>>;

    storage_t storage {};

    [[nodiscard]] std::span<const std::uint8_t>
    view() const noexcept {
      if (!storage) {
        return {};
      }

      return { storage->data(), storage->size() };
    }

    [[nodiscard]] bool
    empty() const noexcept {
      return view().empty();
    }
  };

  struct encoded_frame_t {
    frame_id_t frame_id = 0;
    frame_kind_e kind = frame_kind_e::intra;
    frame_deadline_t deadline {};
    bitstream_buffer_t bitstream {};
  };

  struct packet_span_t {
    std::size_t offset = 0;
    std::size_t size = 0;
  };

  struct packetization_request_t {
    std::size_t packet_boundary = 0;
    std::uint32_t rtp_timestamp = 0;
    bool preserve_pyrowave_block_boundaries = false;
    bool block_aware_fec = false;
    bool mark_critical = false;
    std::vector<std::uint8_t> metadata;  // Must fit the FRAME_HEADER packet payload.
    std::uint16_t metadata_flags = 0;
  };

  struct packetization_result_t {
    failure_e failure = failure_e::none;
    bitstream_buffer_t bitstream {};
    std::vector<packet_span_t> packets;
  };

  [[nodiscard]] bool
  validate(const encoded_frame_t &frame) noexcept;

  [[nodiscard]] bool
  validate(const packetization_request_t &request) noexcept;

  [[nodiscard]] bool
  validate(const packetization_result_t &result) noexcept;

  enum class delivery_result_e : std::uint8_t {
    complete,
    partial,
    drop,
  };

  struct reassembly_policy_t {
    std::chrono::milliseconds deadline { 0 };
    bool allow_partial_frame = false;
    bool block_aware_fec = false;
  };

  [[nodiscard]] bool
  validate(const reassembly_policy_t &policy) noexcept;

}  // namespace pyrowave
