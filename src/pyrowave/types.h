/**
 * @file src/pyrowave/types.h
 * @brief Platform-independent types for the experimental PyroWave path.
 *
 * This header is an API boundary only. It does not enable PyroWave, register
 * a codec, or expose Vulkan/Windows handles to the common Sunshine layer.
 */
#pragma once

#include <chrono>
#include <cstdint>

namespace pyrowave {

  using session_id_t = std::uint64_t;
  using frame_id_t = std::uint64_t;

  struct api_version_t {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    [[nodiscard]] constexpr bool
    matches_exactly(const api_version_t &other) const noexcept {
      return major == other.major && minor == other.minor && patch == other.patch;
    }
  };

  enum class availability_e : std::uint8_t {
    disabled,
    unavailable,
    available,
    active,
    failed,
  };

  enum class failure_e : std::uint8_t {
    none,
    feature_disabled,
    api_mismatch,
    vulkan_unsupported,
    device_mismatch,
    image_import_failed,
    format_conversion_failed,
    synchronization_unsupported,
    encoder_creation_failed,
    packetization_failed,
    client_unsupported,
    configuration_invalid,
    invalid_state,
    session_stopped,
  };

  enum class chroma_e : std::uint8_t {
    yuv420,
    yuv444,
  };

  enum class transfer_e : std::uint8_t {
    bt709,
    pq,
    hlg,
  };

  enum class frame_kind_e : std::uint8_t {
    intra,
  };

  struct stream_config_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t framerate_num = 0;
    std::uint32_t framerate_den = 1;
    std::uint32_t packet_size = 0;
    chroma_e chroma = chroma_e::yuv420;
    transfer_e transfer = transfer_e::bt709;
  };

  [[nodiscard]] bool
  validate(const stream_config_t &config) noexcept;

  struct frame_deadline_t {
    std::chrono::steady_clock::time_point captured_at {};
    std::chrono::milliseconds budget { 0 };
  };

  [[nodiscard]] bool
  validate(const frame_deadline_t &deadline) noexcept;

}  // namespace pyrowave
