/**
 * @file src/platform/windows/virtual_device_host/protocol.h
 * @brief Stable SDS5 v1 virtual-device-host wire contract.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace platf::virtual_device_host::protocol {
  inline constexpr std::uint32_t MAGIC = 0x35534453;  // "SDS5" little-endian
  inline constexpr std::uint16_t VERSION = 1;
  inline constexpr std::size_t HEADER_SIZE = 16;
  inline constexpr std::uint32_t MAX_PAYLOAD = 1024 * 1024;

  inline constexpr std::uint32_t CAP_HID = 1u << 0;
  inline constexpr std::uint32_t CAP_OUTPUT = 1u << 1;
  inline constexpr std::uint32_t CAP_AUDIO_FOUR_CHANNEL = 1u << 2;
  inline constexpr std::uint32_t CAP_AUTHORED_HAPTICS_PCM = 1u << 3;
  inline constexpr std::uint32_t CAP_TOUCHPAD = 1u << 4;
  inline constexpr std::uint32_t CAP_MOTION = 1u << 5;
  inline constexpr std::uint32_t CAP_BATTERY = 1u << 6;
  inline constexpr std::uint32_t CAP_ADAPTIVE_TRIGGERS = 1u << 7;
  inline constexpr std::uint32_t CAP_GENSHIN_COMPATIBILITY_IDENTITY = 1u << 8;
  inline constexpr std::uint32_t CAP_AUDIO_POLICY_VIOLATION = 1u << 9;
  inline constexpr std::uint32_t CAP_VIRTUAL_MICROPHONE = 1u << 10;
  inline constexpr std::uint32_t CAP_PERSISTENT_DEVICE_HOST = 1u << 11;
  inline constexpr std::uint32_t CAP_MICROPHONE_STATUS = 1u << 12;

  inline constexpr std::uint8_t ATTACH_FLAG_GENSHIN_COMPATIBILITY = 1u << 0;
  inline constexpr std::uint32_t MICROPHONE_SAMPLE_RATE_HZ = 48'000;
  inline constexpr std::uint8_t MICROPHONE_CHANNELS = 1;
  inline constexpr std::uint8_t MICROPHONE_BITS_PER_SAMPLE = 16;
  inline constexpr std::uint16_t MAX_MIC_PCM_FRAMES = 960;
  inline constexpr std::size_t MIC_CREATE_PAYLOAD_SIZE = 8;
  inline constexpr std::size_t MIC_CREATE_REPLY_PAYLOAD_SIZE = 16;
  inline constexpr std::size_t MIC_OPERATION_REPLY_PAYLOAD_SIZE = 8;
  inline constexpr std::size_t MIC_PCM_HEADER_SIZE = 20;
  inline constexpr std::size_t MIC_STATUS_PAYLOAD_SIZE = 28;

  enum class message_e: std::uint16_t {
    hello = 1,
    hello_reply = 2,
    attach = 3,
    attach_reply = 4,
    detach = 5,
    detach_reply = 6,
    input = 7,
    touch = 8,
    motion = 9,
    battery = 10,
    shutdown = 11,
    mic_create = 12,
    mic_create_reply = 13,
    mic_pcm = 14,
    mic_flush = 15,
    mic_flush_reply = 16,
    mic_destroy = 17,
    mic_destroy_reply = 18,
    status = 100,
    rumble = 101,
    adaptive_triggers = 102,
    led = 103,
    haptics_pcm = 104,
    audio_policy_violation = 105,
    host_status = 106,
    mic_status = 107,
    error = 255,
  };

  enum mic_pcm_flag_e: std::uint16_t {
    mic_stream_start = 1u << 0,
    mic_stream_end = 1u << 1,
    mic_discontinuity = 1u << 2,
    mic_silence = 1u << 3,
  };

  enum class mic_result_e: std::int32_t {
    success = 0,
    invalid_format = -1001,
    transport_unavailable = -1002,
    device_creation_failed = -1003,
    device_not_created = -1004,
  };

  constexpr void write_u16(std::uint8_t *destination, std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8);
  }

  constexpr void write_u32(std::uint8_t *destination, std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8);
    destination[2] = static_cast<std::uint8_t>(value >> 16);
    destination[3] = static_cast<std::uint8_t>(value >> 24);
  }

  constexpr std::array<std::uint8_t, HEADER_SIZE> encode_header(
    message_e type, std::uint32_t payload_size, std::uint32_t request_id) {
    std::array<std::uint8_t, HEADER_SIZE> header {};
    write_u32(header.data(), MAGIC);
    write_u16(header.data() + 4, VERSION);
    write_u16(header.data() + 6, static_cast<std::uint16_t>(type));
    write_u32(header.data() + 8, payload_size);
    write_u32(header.data() + 12, request_id);
    return header;
  }

  static_assert(encode_header(message_e::mic_create, 8, 0x11223344) ==
                std::array<std::uint8_t, HEADER_SIZE> {
                  0x53, 0x44, 0x53, 0x35, 0x01, 0x00, 0x0c, 0x00,
                  0x08, 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
                });
}  // namespace platf::virtual_device_host::protocol
