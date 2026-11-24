/**
 * @file src/pyrowave/gpu_interop.cpp
 * @brief Validation for the platform-neutral GPU frame descriptor.
 */
#include "gpu_interop.h"

namespace pyrowave {

  namespace {

    [[nodiscard]] bool
    valid_format(const image_format_e format) noexcept {
      switch (format) {
      case image_format_e::bgra8_unorm:
      case image_format_e::rgba8_unorm:
      case image_format_e::rgba16_float:
      case image_format_e::nv12:
      case image_format_e::p010:
      case image_format_e::yuv420p:
      case image_format_e::yuv444p:
        return true;
      }

      return false;
    }

    [[nodiscard]] bool
    requires_even_dimensions(const image_format_e format) noexcept {
      return format == image_format_e::nv12 || format == image_format_e::p010 ||
             format == image_format_e::yuv420p;
    }

  }  // namespace

  bool
  validate(const gpu_frame_t &frame) noexcept {
    return valid_format(frame.format) && frame.width != 0 && frame.width <= 16384 && frame.height != 0 &&
           frame.height <= 16384 && frame.opaque_image != nullptr &&
           (!requires_even_dimensions(frame.format) ||
            (frame.width % 2 == 0 && frame.height % 2 == 0));
  }

}  // namespace pyrowave
