/**
 * @file src/rtsp_limits.h
 * @brief How large one RTSP message may be, and the log line for one that is not allowed.
 *
 * Kept free of any other header so that the checks can be tested without the RTSP server.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rtsp_stream {

  /**
   * The most bytes of one RTSP message (an encrypted header with its payload, or a plaintext request) that the host holds for a connection.
   *
   * The limit was a fixed 2 KiB buffer from the start of this RTSP server (2021), sized for a plain SDP; the encrypted messages added in 2024 (Moonlight core
   * version 1 and later) went through the same buffer unchanged. A client that asks for many options in the ANNOUNCE (automatic FEC and bitrate, extra capability
   * lines) goes past it. A message is read before the client is known, so the limit must stay small: 16 KiB holds a long SDP many times over and is nothing next to
   * the memory of a connection.
   */
  inline constexpr std::size_t RTSP_MAX_MESSAGE_SIZE = 16 * 1024;

  /**
   * @brief Whether a message that starts `used` bytes into a buffer of `capacity` bytes fits in it.
   * @param header_size Bytes of the fixed header that come first.
   * @param payload_length The payload length the peer declared. It is the peer's number: it is compared without adding, so a huge value cannot wrap around.
   */
  constexpr bool
  rtsp_message_fits(std::size_t capacity, std::size_t used, std::size_t header_size, std::uint64_t payload_length) {
    if (used > capacity || header_size > capacity - used) return false;
    return payload_length <= capacity - used - header_size;
  }

  /// The log line for a message that does not fit: who sent it, how long it is (`exact`: the length it declared; otherwise the bytes read so far) and what the limit is.
  inline std::string
  describe_oversized_rtsp_message(std::string_view where, std::string_view peer, std::uint64_t message_size, std::size_t limit, bool exact = true) {
    std::string text { "RTSP: " };
    text.append(where);
    text += ": message from peer ";
    text += peer.empty() ? std::string_view { "(unknown)" } : peer;
    text += exact ? " is " : " is at least ";
    text += std::to_string(message_size);
    text += " bytes, the limit is ";
    text += std::to_string(limit);
    text += " bytes; closing the connection";
    return text;
  }
}  // namespace rtsp_stream
