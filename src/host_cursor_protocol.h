/**
 * @file src/host_cursor_protocol.h
 * @brief Wire format of cursor protocol v2 (0x5509 version 2).
 *
 * See docs/client-cursor-v2-design.md. All multibyte fields are little-endian.
 * The client copy lives in moonlight-common-c's src/CursorStream.c.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace host_cursor_protocol {
  // Same control message type as protocol v1; byte 0 of every payload is the version.
  constexpr std::uint16_t packet_type = 0x5509;

  // Reliable ENet channel for host to client v2 messages, so a large shape never
  // holds up the reliable traffic on channel 0.
  constexpr std::uint8_t channel = 0x09;

  constexpr std::uint8_t version = 2;
  constexpr std::uint8_t kind_state = 1;
  constexpr std::uint8_t kind_shape = 2;
  constexpr std::uint8_t format_bgra8 = 1;  // B8G8R8A8, straight alpha, top-down, packed
  constexpr std::uint16_t max_dimension = 256;
  constexpr std::size_t max_chunk_bytes = 16384;
  constexpr std::size_t shape_header_size = 28;
  constexpr std::size_t state_payload_size = 44;
  constexpr std::size_t request_payload_size = 8;
  constexpr std::size_t control_header_size = 4;  // control_header_v2: type, payloadLength

  namespace state_flags {
    constexpr std::uint8_t visible = 0x01;
    constexpr std::uint8_t fallback = 0x02;
    constexpr std::uint8_t has_position = 0x04;
    constexpr std::uint8_t has_clip = 0x08;
    constexpr std::uint8_t composited = 0x10;
    constexpr std::uint8_t resync = 0x20;
    constexpr std::uint8_t all = 0x3F;
  }  // namespace state_flags

  namespace request_flags {
    constexpr std::uint8_t subscribe = 0x01;
    constexpr std::uint8_t client_draws = 0x02;
    constexpr std::uint8_t all = 0x03;
  }  // namespace request_flags

  struct state_t {
    std::uint8_t flags = 0;
    std::uint32_t seq = 0;
    std::uint32_t shape_id = 0;
    std::uint16_t ref_width = 0;
    std::uint16_t ref_height = 0;
    std::uint32_t scale_q16 = 0x10000;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t clip_left = 0;
    std::int32_t clip_top = 0;
    std::int32_t clip_right = 0;
    std::int32_t clip_bottom = 0;

    bool
    operator==(const state_t &) const = default;
  };

  struct request_t {
    std::uint8_t flags = 0;
    std::uint32_t resend_shape_id = 0;

    bool
    operator==(const request_t &) const = default;
  };

  namespace detail {
    inline void
    put16(std::uint8_t *p, std::uint16_t v) {
      p[0] = std::uint8_t(v);
      p[1] = std::uint8_t(v >> 8);
    }

    inline void
    put32(std::uint8_t *p, std::uint32_t v) {
      put16(p, std::uint16_t(v));
      put16(p + 2, std::uint16_t(v >> 16));
    }

    inline std::uint32_t
    get32(const std::uint8_t *p) {
      return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
    }

    inline std::vector<std::uint8_t>
    with_header(std::size_t payload_size) {
      std::vector<std::uint8_t> out(control_header_size + payload_size);
      put16(out.data(), packet_type);
      put16(out.data() + 2, std::uint16_t(payload_size));
      return out;
    }
  }  // namespace detail

  /**
   * @brief Encode one v2 state.
   * @return The control_header_v2 plaintext.
   */
  inline std::vector<std::uint8_t>
  encode_state(const state_t &state) {
    auto out = detail::with_header(state_payload_size);
    auto *p = out.data() + control_header_size;
    p[0] = version;
    p[1] = kind_state;
    p[2] = state.flags & state_flags::all;
    detail::put32(p + 4, state.seq);
    detail::put32(p + 8, state.shape_id);
    detail::put16(p + 12, state.ref_width);
    detail::put16(p + 14, state.ref_height);
    detail::put32(p + 16, state.scale_q16);
    detail::put32(p + 20, std::uint32_t(state.x));
    detail::put32(p + 24, std::uint32_t(state.y));
    detail::put32(p + 28, std::uint32_t(state.clip_left));
    detail::put32(p + 32, std::uint32_t(state.clip_top));
    detail::put32(p + 36, std::uint32_t(state.clip_right));
    detail::put32(p + 40, std::uint32_t(state.clip_bottom));
    return out;
  }

  /**
   * @brief Split one straight-alpha BGRA shape into v2 shape chunks of at most max_chunk_bytes pixels.
   * @return The control_header_v2 plaintexts, or nothing for a shape the client would reject.
   */
  inline std::vector<std::vector<std::uint8_t>>
  encode_shape(std::uint32_t shape_id,
               std::uint16_t width,
               std::uint16_t height,
               std::uint16_t hotspot_x,
               std::uint16_t hotspot_y,
               std::span<const std::uint8_t> bgra) {
    std::vector<std::vector<std::uint8_t>> chunks;
    const std::size_t total = std::size_t(width) * height * 4;
    if (!shape_id || !width || !height || width > max_dimension || height > max_dimension ||
        hotspot_x >= width || hotspot_y >= height || bgra.size() != total) {
      return chunks;
    }
    for (std::size_t offset = 0; offset < total; offset += max_chunk_bytes) {
      const auto length = std::min(max_chunk_bytes, total - offset);
      auto out = detail::with_header(shape_header_size + length);
      auto *p = out.data() + control_header_size;
      p[0] = version;
      p[1] = kind_shape;
      p[2] = format_bgra8;
      detail::put32(p + 4, shape_id);
      detail::put16(p + 8, width);
      detail::put16(p + 10, height);
      detail::put16(p + 12, hotspot_x);
      detail::put16(p + 14, hotspot_y);
      detail::put32(p + 16, std::uint32_t(total));
      detail::put32(p + 20, std::uint32_t(offset));
      detail::put16(p + 24, std::uint16_t(length));
      std::copy_n(bgra.begin() + offset, length, p + shape_header_size);
      chunks.push_back(std::move(out));
    }
    return chunks;
  }

  /**
   * @brief Parse a v2 request payload (after the control header).
   * @return The request, or nothing unless it is exactly a version 2 request with known
   *         flags and zero reserved bytes.
   */
  inline std::optional<request_t>
  parse_request(std::span<const std::uint8_t> payload) {
    if (payload.size() != request_payload_size || payload[0] != version ||
        (payload[1] & ~request_flags::all) || payload[2] || payload[3]) {
      return std::nullopt;
    }
    return request_t { payload[1], detail::get32(payload.data() + 4) };
  }

  /**
   * @brief Reference encoding of the client's v2 request, for tests.
   */
  inline std::array<std::uint8_t, request_payload_size>
  encode_request(const request_t &request) {
    std::array<std::uint8_t, request_payload_size> out {};
    out[0] = version;
    out[1] = request.flags & request_flags::all;
    detail::put32(out.data() + 4, request.resend_shape_id);
    return out;
  }
}  // namespace host_cursor_protocol
