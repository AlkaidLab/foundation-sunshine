/**
 * @file src/streaming/dynamic_param_protocol.h
 * @brief Sunshine dynamic-parameter control payloads shared by host paths.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace streaming::dynamic_param_protocol {
  constexpr std::uint16_t control_type = 0x5506;
  constexpr std::int32_t bitrate_parameter = 2;
  // Client opt-in bit in x-ml-general.featureFlags (ML_FF_BITRATE_CALLBACK). The
  // host advertises the same bit in x-ss-general.featureFlags (LI_FF_BITRATE_CALLBACK)
  // before sending the host-to-client form of 0x5506. 0x4000 is the next free
  // fork bit of docs/protocol-extension-registry.md (0x8000 is endpoint refresh);
  // 0x400 is not used here, it is reserved for another meaning upstream.
  constexpr std::uint32_t bitrate_callback_feature = 0x4000;

  inline std::array<std::uint8_t, 8>
  encode_bitrate_update(std::int32_t bitrate_kbps) {
    const auto encode = [](std::uint32_t value) {
      return std::array<std::uint8_t, 4> {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 24),
      };
    };
    const auto type = encode(static_cast<std::uint32_t>(bitrate_parameter));
    const auto value = encode(static_cast<std::uint32_t>(bitrate_kbps));
    return { type[0], type[1], type[2], type[3], value[0], value[1], value[2], value[3] };
  }

  inline bool
  decode_bitrate_update(std::string_view payload, std::int32_t &bitrate_kbps) {
    if (payload.size() != 8) return false;
    const auto decode = [&](std::size_t offset) {
      return static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[offset])) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[offset + 1])) << 8) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[offset + 2])) << 16) |
             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[offset + 3])) << 24);
    };
    if (decode(0) != static_cast<std::uint32_t>(bitrate_parameter)) return false;
    bitrate_kbps = static_cast<std::int32_t>(decode(4));
    return bitrate_kbps > 0 && bitrate_kbps <= 800000;
  }
}  // namespace streaming::dynamic_param_protocol
