/**
 * @file src/pyrowave/gpu_interop.h
 * @brief Opaque GPU resource boundary for PyroWave.
 *
 * Platform implementations own the native device, image and synchronization
 * handles. The common layer only sees a description and a lifetime contract.
 */
#pragma once

#include "types.h"

#include <array>
#include <cstdint>

namespace pyrowave {

  enum class image_format_e : std::uint8_t {
    bgra8_unorm,
    rgba8_unorm,
    rgba16_float,
    nv12,
    p010,
    yuv420p,
    yuv444p,
  };

  enum class sync_handle_kind_e : std::uint8_t {
    none,
    win32_opaque,
    win32_kmt,
    d3d11_timeline_fence,
  };

  struct device_identity_t {
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    bool has_device_uuid = false;
    bool has_driver_uuid = false;
    bool has_device_luid = false;
    std::array<std::uint8_t, 16> device_uuid {};
    std::array<std::uint8_t, 16> driver_uuid {};
    std::array<std::uint8_t, 8> device_luid {};
  };

  struct gpu_frame_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    image_format_e format = image_format_e::bgra8_unorm;
    void *opaque_image = nullptr;
    device_identity_t device {};
  };

  [[nodiscard]] bool
  validate(const gpu_frame_t &frame) noexcept;

  struct interop_result_t {
    availability_e availability = availability_e::unavailable;
    failure_e failure = failure_e::none;
    sync_handle_kind_e sync_kind = sync_handle_kind_e::none;
  };

  class interop_t {
  public:
    virtual ~interop_t() = default;

    [[nodiscard]] virtual interop_result_t
    import_frame(const gpu_frame_t &frame) noexcept = 0;

    [[nodiscard]] virtual interop_result_t
    convert_to_encoder_input(const gpu_frame_t &frame, chroma_e chroma) noexcept = 0;

    virtual void
    release_frame() noexcept = 0;
  };

}  // namespace pyrowave
