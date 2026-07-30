/**
 * @file src/platform/windows/display_cursor.cpp
 * @brief See display_cursor.h.
 */
#include "display_cursor.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "src/cursor_channel.h"
#include "src/logging.h"

namespace platf::dxgi {
  using namespace std::literals;

  util::buffer_t<std::uint8_t>
  make_cursor_xor_image(const util::buffer_t<std::uint8_t> &img_data,
                        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t inverted = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        return {};
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        util::buffer_t<std::uint8_t> cursor_img = img_data;
        std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
          auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
          if (alpha == 0x00) {
            pixel = transparent;
          }
          else if (alpha != 0xFF) {
            BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
          }
        });
        return cursor_img;
      }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;
    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};
    const auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_data = (std::uint32_t *) std::begin(cursor_img);
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        const auto bit = 1 << c;
        const auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);
        *pixel_data++ = color_type == 3 ? inverted : transparent;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  util::buffer_t<std::uint8_t>
  make_cursor_alpha_image(const util::buffer_t<std::uint8_t> &img_data,
                          DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info) {
    constexpr std::uint32_t black = 0xFF000000;
    constexpr std::uint32_t white = 0xFFFFFFFF;
    constexpr std::uint32_t transparent = 0;

    switch (shape_info.Type) {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        util::buffer_t<std::uint8_t> cursor_img = img_data;
        std::for_each((std::uint32_t *) std::begin(cursor_img), (std::uint32_t *) std::end(cursor_img), [](auto &pixel) {
          auto alpha = (std::uint8_t) ((pixel >> 24) & 0xFF);
          if (alpha == 0xFF) {
            pixel = transparent;
          }
          else if (alpha == 0x00) {
            pixel |= 0xFF000000;
          }
          else {
            BOOST_LOG(warning) << "Illegal alpha value in masked color cursor: " << alpha;
          }
        });
        return cursor_img;
      }
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
        return img_data;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        break;
      default:
        BOOST_LOG(error) << "Invalid cursor shape type: " << shape_info.Type;
        return {};
    }

    shape_info.Height /= 2;
    util::buffer_t<std::uint8_t> cursor_img {shape_info.Width * shape_info.Height * 4};
    const auto bytes = shape_info.Pitch * shape_info.Height;
    auto pixel_data = (std::uint32_t *) std::begin(cursor_img);
    auto and_mask = std::begin(img_data);
    auto xor_mask = std::begin(img_data) + bytes;

    for (auto x = 0; x < bytes; ++x) {
      for (auto c = 7; c >= 0 && ((std::uint8_t *) pixel_data) != std::end(cursor_img); --c) {
        const auto bit = 1 << c;
        const auto color_type = ((*and_mask & bit) ? 1 : 0) + ((*xor_mask & bit) ? 2 : 0);
        switch (color_type) {
          case 0:
            *pixel_data = black;
            break;
          case 2:
            *pixel_data = white;
            break;
          default:
            *pixel_data = transparent;
            break;
        }
        ++pixel_data;
      }
      ++and_mask;
      ++xor_mask;
    }

    return cursor_img;
  }

  bool
  set_cursor_texture(device_t::pointer device,
                     gpu_cursor_t &cursor,
                     util::buffer_t<std::uint8_t> &&cursor_img,
                     DXGI_OUTDUPL_POINTER_SHAPE_INFO &shape_info) {
    if (cursor_img.size() == 0) {
      cursor.input_res.reset();
      cursor.set_texture(0, 0, nullptr);
      return true;
    }

    D3D11_SUBRESOURCE_DATA data {
      std::begin(cursor_img),
      4 * shape_info.Width,
      0
    };

    D3D11_TEXTURE2D_DESC texture_desc {};
    texture_desc.Width = shape_info.Width;
    texture_desc.Height = static_cast<UINT>(cursor_img.size() / data.SysMemPitch);
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    texture2d_t texture;
    auto status = device->CreateTexture2D(&texture_desc, &data, &texture);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create mouse texture [0x"sv
                       << util::hex(status).to_string_view() << ']';
      return false;
    }

    cursor.input_res.reset();
    status = device->CreateShaderResourceView(texture.get(), nullptr, &cursor.input_res);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create cursor shader resource view [0x"sv
                       << util::hex(status).to_string_view() << ']';
      return false;
    }

    cursor.set_texture(texture_desc.Width, texture_desc.Height, std::move(texture));
    return true;
  }

  bool
  local_cursor_mode_active() {
    return cursor_channel::local_mode_active();
  }

  bool
  normalize_cursor_shape(const vdd_capture_t::cursor_snapshot &cursor,
                         bool include_xor,
                         normalized_cursor_shape_t &normalized) {
    normalized = {};
    if (cursor.shape_buffer.empty() || cursor.width == 0 ||
        cursor.height == 0 || cursor.pitch == 0) {
      return false;
    }

    switch (cursor.shape_type) {
      case 0:
        if ((cursor.height & 1u) != 0) {
          return false;
        }
        normalized.info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        break;
      case 1:
        normalized.info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
        break;
      case 2:
        normalized.info.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
        break;
      default:
        return false;
    }

    normalized.info.Width = cursor.width;
    normalized.info.Height = cursor.height;
    normalized.info.Pitch = cursor.pitch;
    normalized.info.HotSpot.x = cursor.xhot;
    normalized.info.HotSpot.y = cursor.yhot;

    const bool color_shape = cursor.shape_type != 0;
    if (color_shape &&
        cursor.width > std::numeric_limits<UINT32>::max() / 4u) {
      return false;
    }
    const UINT32 packed_pitch = color_shape ? cursor.width * 4u : cursor.pitch;
    if (color_shape && cursor.pitch < packed_pitch) {
      return false;
    }

    const std::size_t source_size =
      static_cast<std::size_t>(cursor.pitch) * cursor.height;
    const std::size_t packed_size =
      static_cast<std::size_t>(packed_pitch) * cursor.height;
    if (cursor.shape_buffer.size() < source_size) {
      return false;
    }

    util::buffer_t<std::uint8_t> img_data(packed_size);
    if (color_shape && cursor.pitch != packed_pitch) {
      for (UINT32 row = 0; row < cursor.height; ++row) {
        std::memcpy(
          std::begin(img_data) + static_cast<std::size_t>(row) * packed_pitch,
          cursor.shape_buffer.data() + static_cast<std::size_t>(row) * cursor.pitch,
          packed_pitch
        );
      }
      normalized.info.Pitch = packed_pitch;
    }
    else {
      std::memcpy(std::begin(img_data), cursor.shape_buffer.data(), packed_size);
    }

    normalized.alpha = make_cursor_alpha_image(img_data, normalized.info);
    const UINT32 output_height =
      normalized.info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ?
        normalized.info.Height / 2u : normalized.info.Height;
    if (normalized.alpha.size() !=
        static_cast<std::size_t>(normalized.info.Width) * output_height * 4u) {
      return false;
    }

    if (include_xor) {
      normalized.xor_mask = make_cursor_xor_image(img_data, normalized.info);
    }
    return true;
  }

  bool
  publish_local_cursor(const vdd_capture_t::cursor_snapshot &cursor) {
    cursor_channel::publish_visibility(cursor.visible, cursor.shape_id);
    if (!cursor.shape_updated) {
      return false;
    }

    const bool empty_hidden_shape = !cursor.visible &&
                                    cursor.shape_buffer.empty() &&
                                    cursor.width == 0 &&
                                    cursor.height == 0;
    if (empty_hidden_shape) {
      return true;
    }
    if (cursor.shape_buffer.empty() || cursor.width == 0 || cursor.height == 0) {
      return false;
    }

    normalized_cursor_shape_t normalized;
    if (!normalize_cursor_shape(cursor, false, normalized)) {
      return false;
    }
    const UINT32 output_height =
      normalized.info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ?
        normalized.info.Height / 2u : normalized.info.Height;
    const bool valid_hotspot = normalized.info.HotSpot.x >= 0 &&
                               normalized.info.HotSpot.y >= 0 &&
                               normalized.info.HotSpot.x < static_cast<LONG>(normalized.info.Width) &&
                               normalized.info.HotSpot.y < static_cast<LONG>(output_height);
    if (!valid_hotspot ||
        normalized.info.Width > std::numeric_limits<std::uint16_t>::max() ||
        output_height > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }

    std::vector<std::uint8_t> bgra(
      std::begin(normalized.alpha),
      std::end(normalized.alpha)
    );
    cursor_channel::publish_shape(
      cursor.visible,
      cursor.shape_id,
      static_cast<std::uint16_t>(normalized.info.Width),
      static_cast<std::uint16_t>(output_height),
      static_cast<std::int16_t>(normalized.info.HotSpot.x),
      static_cast<std::int16_t>(normalized.info.HotSpot.y),
      std::move(bgra)
    );
    return true;
  }
}  // namespace platf::dxgi
