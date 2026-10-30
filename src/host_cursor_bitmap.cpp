/**
 * @file src/host_cursor_bitmap.cpp
 * @brief See host_cursor_bitmap.h.
 */
#include "host_cursor_bitmap.h"

#include "host_cursor_protocol.h"

#include <algorithm>
#include <bit>

namespace host_cursor_bitmap {
  namespace {
    // Windows allows larger cursors than the wire carries; those are box-filtered
    // by an integer factor. Anything beyond this is not a real cursor.
    constexpr std::uint32_t max_source_dimension = 4096;

    struct canvas_t {
      std::uint32_t width;
      std::uint32_t height;
      std::vector<std::uint8_t> bgra;
      std::vector<std::uint8_t> invert;

      canvas_t(std::uint32_t w, std::uint32_t h):
          width { w },
          height { h },
          bgra(std::size_t(w) * h * 4),
          invert(std::size_t(w) * h) {
      }

      void
      set(std::uint32_t x, std::uint32_t y, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a) {
        auto *p = &bgra[(std::size_t(y) * width + x) * 4];
        p[0] = b;
        p[1] = g;
        p[2] = r;
        p[3] = a;
      }

      void
      set_invert(std::uint32_t x, std::uint32_t y) {
        invert[std::size_t(y) * width + x] = 1;
      }
    };

    bool
    valid_source(std::uint32_t width, std::uint32_t height) {
      return width && height && width <= max_source_dimension && height <= max_source_dimension;
    }

    bool
    mask_bit(std::span<const std::uint8_t> mask, std::uint32_t pitch, std::uint32_t x, std::uint32_t y) {
      return (mask[std::size_t(y) * pitch + x / 8] >> (7 - x % 8)) & 1;
    }

    const std::uint8_t *
    pixel(std::span<const std::uint8_t> bgra, std::uint32_t pitch, std::uint32_t x, std::uint32_t y) {
      return &bgra[std::size_t(y) * pitch + std::size_t(x) * 4];
    }

    // Inverting pixels become white; any transparent pixel touching one becomes
    // black. The outline is computed from the original transparency only, so it
    // never grows into a second ring.
    void
    resolve_inverts(canvas_t &canvas) {
      const auto w = canvas.width, h = canvas.height;
      const auto transparent = [&](std::uint32_t x, std::uint32_t y) {
        return !canvas.invert[std::size_t(y) * w + x] && canvas.bgra[(std::size_t(y) * w + x) * 4 + 3] == 0;
      };
      std::vector<std::uint8_t> outline(std::size_t(w) * h);
      for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
          if (!canvas.invert[std::size_t(y) * w + x]) continue;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const auto nx = std::int64_t(x) + dx, ny = std::int64_t(y) + dy;
              if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
              if (transparent(std::uint32_t(nx), std::uint32_t(ny))) outline[std::size_t(ny) * w + nx] = 1;
            }
          }
        }
      }
      for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
          const auto i = std::size_t(y) * w + x;
          if (canvas.invert[i]) {
            canvas.set(x, y, 0xFF, 0xFF, 0xFF, 0xFF);
          }
          else if (outline[i]) {
            canvas.set(x, y, 0, 0, 0, 0xFF);
          }
        }
      }
    }

    // Box filter in premultiplied space so transparent pixels do not darken edges.
    image_t
    finish(canvas_t &&canvas, std::uint32_t hotspot_x, std::uint32_t hotspot_y) {
      resolve_inverts(canvas);

      const auto largest = std::max(canvas.width, canvas.height);
      const std::uint32_t factor = (largest + host_cursor_protocol::max_dimension - 1) / host_cursor_protocol::max_dimension;
      image_t image;
      image.width = std::uint16_t((canvas.width + factor - 1) / factor);
      image.height = std::uint16_t((canvas.height + factor - 1) / factor);
      image.hotspot_x = std::uint16_t(std::min(hotspot_x / factor, image.width - 1u));
      image.hotspot_y = std::uint16_t(std::min(hotspot_y / factor, image.height - 1u));
      image.pixel_scale = factor;
      if (factor == 1) {
        image.bgra = std::move(canvas.bgra);
        return image;
      }

      image.bgra.resize(std::size_t(image.width) * image.height * 4);
      for (std::uint32_t oy = 0; oy < image.height; ++oy) {
        for (std::uint32_t ox = 0; ox < image.width; ++ox) {
          std::uint64_t sum[4] {};
          std::uint32_t count = 0;
          for (std::uint32_t y = oy * factor; y < std::min((oy + 1) * factor, canvas.height); ++y) {
            for (std::uint32_t x = ox * factor; x < std::min((ox + 1) * factor, canvas.width); ++x) {
              const auto *p = &canvas.bgra[(std::size_t(y) * canvas.width + x) * 4];
              for (int c = 0; c < 3; ++c) sum[c] += std::uint32_t(p[c]) * p[3];
              sum[3] += p[3];
              ++count;
            }
          }
          auto *out = &image.bgra[(std::size_t(oy) * image.width + ox) * 4];
          const auto alpha = sum[3] / count;
          for (int c = 0; c < 3; ++c) out[c] = sum[3] ? std::uint8_t(sum[c] / sum[3]) : 0;
          out[3] = std::uint8_t(alpha);
        }
      }
      return image;
    }
  }  // namespace

  std::optional<image_t>
  from_monochrome(std::uint32_t width,
                  std::uint32_t height,
                  std::uint32_t pitch,
                  std::span<const std::uint8_t> masks,
                  std::uint32_t hotspot_x,
                  std::uint32_t hotspot_y) {
    if (!valid_source(width, height) || pitch < (width + 7) / 8 ||
        masks.size() < std::size_t(pitch) * height * 2) {
      return std::nullopt;
    }
    canvas_t canvas { width, height };
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const bool and_bit = mask_bit(masks, pitch, x, y);
        const bool xor_bit = mask_bit(masks, pitch, x, y + height);
        if (and_bit && xor_bit) {
          canvas.set_invert(x, y);
        }
        else if (!and_bit) {
          const std::uint8_t v = xor_bit ? 0xFF : 0;
          canvas.set(x, y, v, v, v, 0xFF);
        }
      }
    }
    return finish(std::move(canvas), hotspot_x, hotspot_y);
  }

  std::optional<image_t>
  from_color(std::uint32_t width,
             std::uint32_t height,
             std::uint32_t pitch,
             std::span<const std::uint8_t> bgra,
             std::uint32_t hotspot_x,
             std::uint32_t hotspot_y) {
    if (!valid_source(width, height) || pitch < width * 4 ||
        bgra.size() < std::size_t(pitch) * (height - 1) + std::size_t(width) * 4) {
      return std::nullopt;
    }
    canvas_t canvas { width, height };
    for (std::uint32_t y = 0; y < height; ++y) {
      std::copy_n(pixel(bgra, pitch, 0, y), std::size_t(width) * 4, &canvas.bgra[std::size_t(y) * width * 4]);
    }
    return finish(std::move(canvas), hotspot_x, hotspot_y);
  }

  std::optional<image_t>
  from_color_and_mask(std::uint32_t width,
                      std::uint32_t height,
                      std::uint32_t color_pitch,
                      std::span<const std::uint8_t> bgra,
                      std::uint32_t mask_pitch,
                      std::span<const std::uint8_t> and_mask,
                      std::uint32_t hotspot_x,
                      std::uint32_t hotspot_y) {
    if (!valid_source(width, height) || color_pitch < width * 4 ||
        bgra.size() < std::size_t(color_pitch) * (height - 1) + std::size_t(width) * 4 ||
        mask_pitch < (width + 7) / 8 || and_mask.size() < std::size_t(mask_pitch) * height) {
      return std::nullopt;
    }
    canvas_t canvas { width, height };
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const auto *p = pixel(bgra, color_pitch, x, y);
        if (!mask_bit(and_mask, mask_pitch, x, y)) {
          canvas.set(x, y, p[0], p[1], p[2], 0xFF);
        }
        else if (p[0] || p[1] || p[2]) {
          canvas.set_invert(x, y);
        }
      }
    }
    return finish(std::move(canvas), hotspot_x, hotspot_y);
  }

  double
  gdi_display_scale(std::uint32_t width, std::uint32_t height, std::uint32_t read_standard, std::uint32_t display_standard) {
    if (!read_standard || !display_standard || width != read_standard || height != read_standard) {
      return 1;
    }
    return double(display_standard) / read_standard;
  }

  std::uint64_t
  content_hash(const image_t &image) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&](std::uint8_t byte) {
      hash ^= byte;
      hash *= 0x100000001b3ULL;
    };
    for (const std::uint64_t v : { std::uint64_t(image.width), std::uint64_t(image.height), std::uint64_t(image.hotspot_x), std::uint64_t(image.hotspot_y), std::bit_cast<std::uint64_t>(image.pixel_scale) }) {
      for (int i = 0; i < 8; ++i) mix(std::uint8_t(v >> (i * 8)));
    }
    for (const auto byte : image.bgra) mix(byte);
    return hash;
  }
}  // namespace host_cursor_bitmap
