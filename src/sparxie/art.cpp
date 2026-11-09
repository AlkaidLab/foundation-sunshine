#include "src/sparxie/art.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "src/stb_image_write.h"
// the host's own decoder of images is the OS one (WIC); this private one reads back what the host wrote (a JPEG hero, a PNG cover: to tell a blank one) and
// the pictures of the person's own that get the Sparxie badge (PNG or JPEG)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "src/stb_image.h"
#include "src/sparxie/badge_mark.inc"
namespace sparxie {
  namespace {
    json
    array_at(const json &payload, const char *wrapper, const char *key) {
      if (payload.contains("data") && payload["data"].is_object() && payload["data"].contains(key)) return payload["data"][key];
      if (payload.contains(key)) return payload[key];
      if (payload.contains(wrapper) && payload[wrapper].is_object() && payload[wrapper].contains(key)) return payload[wrapper][key];
      return json::array();
    }
    std::string
    url_of(const json &node) {
      if (node.is_object() && node.contains("url") && node["url"].is_string()) return node["url"].get<std::string>();
      return {};
    }
    std::uint8_t
    sample(const Rgba &image, int x, int y, int channel) {
      x = std::clamp(x, 0, image.w - 1);
      y = std::clamp(y, 0, image.h - 1);
      return image.px[(y * image.w + x) * 4 + channel];
    }

    struct Brand {
      std::uint8_t top[3];  // the lighter tone the card starts from
      std::uint8_t base[3];  // the title's brand colour
      const char *word;  // what the card draws: the title's Latin name
    };

    // One brand pair per title, in the tones of the title's own key art.
    const Brand &
    brand_of(std::string_view game) {
      static const std::vector<std::pair<const char *, Brand>> table = {
        { "genshin", { { 72, 84, 104 }, { 36, 42, 56 }, "GENSHIN IMPACT" } },
        { "starrail", { { 66, 72, 126 }, { 30, 32, 66 }, "HONKAI STAR RAIL" } },
        { "zzz", { { 70, 70, 78 }, { 24, 24, 28 }, "ZENLESS ZONE ZERO" } },
        { "hi3", { { 118, 110, 172 }, { 58, 52, 108 }, "HONKAI IMPACT 3RD" } },
        { "wuwa", { { 32, 70, 82 }, { 10, 22, 28 }, "WUTHERING WAVES" } },
        { "pgr", { { 128, 38, 42 }, { 58, 14, 18 }, "PUNISHING GRAY RAVEN" } },
        { "arknights", { { 56, 56, 62 }, { 14, 14, 17 }, "ARKNIGHTS" } },
        { "endfield", { { 96, 82, 46 }, { 26, 22, 13 }, "ARKNIGHTS ENDFIELD" } },
        { "nte", { { 84, 56, 128 }, { 30, 18, 54 }, "NTE" } },
      };
      for (const auto &row : table)
        if (game == row.first) return row.second;
      static const Brand plain { { 64, 66, 72 }, { 26, 27, 32 }, "" };
      return plain;
    }

    // The card's own 5x7 block letters. A glyph is seven characters, each
    // 'A' plus that row's five pixel bits, in the order of kFontOrder.
    const char kFontOrder[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :.-'!";

    std::string_view
    glyph_of(char c) {
      static const char *const glyphs[] = {
        +"ORR`RRR",  // A
        "_R_RRR_",  // B
        "ORQQQRO",  // C
        "_RRRRR_",  // D
        "`Q_QQQ`",  // E
        "`Q_QQQQ",  // F
        "ORQXRRP",  // G
        "RR`RRRR",  // H
        "`EEEEE`",  // I
        "HCCCCSM",  // J
        "RS]SRRR",  // K
        "QQQQQQ`",  // L
        "R\\VVRRR",  // M
        "RZVTRRR",  // N
        "ORRRRRO",  // O
        "_RR_QQQ",  // P
        "ORRRVSN",  // Q
        "_RR_SRR",  // R
        "PQQOBB_",  // S
        "`EEEEEE",  // T
        "RRRRRRO",  // U
        "RRRRRKE",  // V
        "RRRVV\\R",  // W
        "RKEEKRR",  // X
        "RKEEEEE",  // Y
        "`BCEIQ`",  // Z
        "ORTVZRO",  // 0
        "EMEEEEO",  // 1
        "ORBGIQ`",  // 2
        "_BBOBB_",  // 3
        "CGKS`CC",  // 4
        "`Q_BBRO",  // 5
        "OQ_RRRO",  // 6
        "`BCEIII",  // 7
        "ORRORRO",  // 8
        "ORRRPBO",  // 9
        "AAAAAAA",  //
        "AEEAEEA",  // :
        "AAAAAEE",  // .
        "AAA`AAA",  // -
        "EEIAAAA",  // '
        "EEEEEAE",  // !
      };
      const char *at = std::strchr(kFontOrder, c);
      static const char blank[] = "AAAAAAA";
      return at && *at != ' ' ? std::string_view(glyphs[at - kFontOrder]) : std::string_view(blank);
    }

    void
    draw_text(std::string_view text, int x0, int y0, int scale, const std::uint8_t colour[3], Rgba &into) {
      for (std::size_t i = 0; i < text.size(); ++i) {
        const std::string_view rows = glyph_of(text[i]);
        for (int y = 0; y < 7; ++y) {
          const int row = rows[static_cast<std::size_t>(y)] - 'A';
          for (int x = 0; x < 5; ++x)
            if (row >> (4 - x) & 1) {
              for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx) {
                  const int px = x0 + static_cast<int>(i) * 6 * scale + x * scale + dx;
                  const int py = y0 + y * scale + dy;
                  if (px < 0 || py < 0 || px >= into.w || py >= into.h) continue;
                  for (int c = 0; c < 3; ++c) into.px[(static_cast<std::size_t>(py * into.w + px)) * 4 + c] = colour[c];
                }
            }
        }
      }
    }

    void
    draw_word(const char *word, int centred_y, Rgba &into, int max_scale = 5) {
      if (!word || !*word) return;
      const std::string_view text(word);
      const auto space = text.find(' ');
      const std::uint8_t white[3] = { 244, 244, 246 };
      auto width = [](std::size_t count, int scale) { return static_cast<int>((count * 6 - 1) * scale); };
      // A long name breaks at its space and draws two lines instead of
      // shrinking past the size the card is read at.
      if (text.size() > 14 && space != std::string_view::npos) {
        const std::string_view first = text.substr(0, space);
        const std::string_view rest = text.substr(space + 1);
        const int scale = std::min(max_scale, static_cast<int>((into.w * 92 / 100) / std::max<std::size_t>(1, 6 * std::max(first.size(), rest.size()) - 1)));
        draw_text(first, (into.w - width(first.size(), scale)) / 2, centred_y - scale * 11 / 2, scale, white, into);
        draw_text(rest, (into.w - width(rest.size(), scale)) / 2, centred_y + scale * 11 / 2, scale, white, into);
        return;
      }
      const int scale = std::min(max_scale, static_cast<int>((into.w * 92 / 100) / std::max<std::size_t>(1, 6 * text.size() - 1)));
      draw_text(text, (into.w - width(text.size(), scale)) / 2, centred_y - scale * 7 / 2, scale, white, into);
    }
  }  // namespace
  std::string
  art_version_of(std::string url) {
    const auto query = url.find('?');
    if (query != std::string::npos) url.resize(query);
    const auto slash = url.find_last_of('/');
    std::string name = slash == std::string::npos ? url : url.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
  }
  int
  cover_window_x(const Rgba &background) {
    if (background.w <= 0 || background.h <= 0 || background.px.size() < static_cast<std::size_t>(background.w * background.h * 4)) return 0;
    const int window = std::max(1, background.h * 2 / 3);
    if (window >= background.w) return 0;
    std::vector<double> column(static_cast<std::size_t>(background.w), 0);
    for (int x = 1; x < background.w; ++x) {
      double energy = 0;
      for (int y = 0; y < background.h; ++y) {
        const int left = (y * background.w + x - 1) * 4;
        const int here = (y * background.w + x) * 4;
        const int lum_left = background.px[left] + background.px[left + 1] + background.px[left + 2];
        const int lum_here = background.px[here] + background.px[here + 1] + background.px[here + 2];
        energy += std::abs(lum_here - lum_left);
      }
      column[static_cast<std::size_t>(x)] = energy;
    }
    int best = 0;
    double best_energy = -1;
    for (int x = 0; x + window <= background.w; ++x) {
      double sum = 0;
      for (int i = 0; i < window; ++i) sum += column[static_cast<std::size_t>(x + i)];
      if (sum > best_energy) {
        best_energy = sum;
        best = x;
      }
    }
    return best;
  }
  Rgba
  fit_cover(const Rgba &finished) {
    Rgba cover;
    if (finished.w <= 0 || finished.h <= 0 || finished.px.size() < static_cast<std::size_t>(finished.w * finished.h * 4)) return cover;
    if (finished.w == 600 && finished.h == 900) return finished;
    cover.w = 600;
    cover.h = 900;
    cover.px.resize(static_cast<std::size_t>(cover.w * cover.h * 4));
    for (int y = 0; y < cover.h; ++y)
      for (int x = 0; x < cover.w; ++x) {
        const std::size_t at = static_cast<std::size_t>((y * cover.w + x) * 4);
        for (int c = 0; c < 4; ++c) cover.px[at + c] = sample(finished, x * finished.w / cover.w, y * finished.h / cover.h, c);
      }
    return cover;
  }
  Rgba
  crop_banner(const Rgba &background) {
    Rgba banner;
    if (background.w <= 0 || background.h <= 0 || background.px.size() < static_cast<std::size_t>(background.w * background.h * 4)) return banner;
    const int y0 = std::min(background.h - 1, background.h * 16 / 100);
    int height = std::max(1, background.w / 6);
    if (y0 + height > background.h) height = background.h - y0;
    banner.w = background.w;
    banner.h = height;
    banner.px.resize(static_cast<std::size_t>(banner.w * banner.h * 4));
    for (int y = 0; y < height; ++y) {
      const std::uint8_t *src = background.px.data() + static_cast<std::size_t>((y0 + y) * background.w * 4);
      std::copy(src, src + background.w * 4, banner.px.begin() + static_cast<std::ptrdiff_t>(y * background.w * 4));
    }
    return banner;
  }
  namespace {
    // Where the hero puts the logo: centred horizontally, centred at this fraction of the height (inside the safe area), at most this large, and not
    // magnified beyond what the small launcher logos survive.
    constexpr double kLogoCenterY = 0.42;
    constexpr double kLogoMaxW = 0.36;
    constexpr double kLogoMaxH = 0.34;
    constexpr double kLogoMaxScale = 4.5;
    // The portrait cover's logo: the visible mark at most 66 percent of the width and 17 percent of the height, centred, its middle at 76 percent of the height (the
    // lower third, above the clients' bottom title area).
    constexpr double kCoverLogoMaxW = 0.66;
    constexpr double kCoverLogoMaxH = 0.17;
    constexpr double kCoverLogoCenterY = 0.76;
    constexpr int kHeroWidth = 1920;

    bool
    valid_image(const Rgba &image) {
      return image.w > 0 && image.h > 0 && image.px.size() >= static_cast<std::size_t>(image.w) * static_cast<std::size_t>(image.h) * 4;
    }

    int
    luma(const Rgba &image, int x, int y) {
      const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.w) + static_cast<std::size_t>(x)) * 4;
      return (image.px[at] * 3 + image.px[at + 1] * 6 + image.px[at + 2]) / 10;
    }

    // The offset of the window of `window` samples with the most detail.
    int
    best_window(const std::vector<double> &energy, int window) {
      const int count = static_cast<int>(energy.size());
      if (window >= count) return 0;
      double sum = 0;
      for (int i = 0; i < window; ++i) sum += energy[static_cast<std::size_t>(i)];
      double best = sum;
      int at = 0;
      for (int x = 1; x + window <= count; ++x) {
        sum += energy[static_cast<std::size_t>(x + window - 1)] - energy[static_cast<std::size_t>(x - 1)];
        if (sum > best) {
          best = sum;
          at = x;
        }
      }
      return at;
    }

    struct Window {
      int x = 0;
      int y = 0;
      int w = 0;
      int h = 0;
    };

    // The largest 16:9 window of the background. A 16:9 background is taken whole; a wider one slides along x, a taller one along y, to where the detail is.
    Window
    hero_window(const Rgba &background) {
      Window win { 0, 0, background.w, background.h };
      const long long wide = static_cast<long long>(background.w) * 9;
      const long long tall = static_cast<long long>(background.h) * 16;
      if (wide > tall) {
        win.w = std::max(1, background.h * 16 / 9);
        std::vector<double> column(static_cast<std::size_t>(background.w), 0);
        for (int x = 1; x < background.w; ++x)
          for (int y = 0; y < background.h; ++y) column[static_cast<std::size_t>(x)] += std::abs(luma(background, x, y) - luma(background, x - 1, y));
        win.x = best_window(column, win.w);
      }
      else if (wide < tall) {
        win.h = std::max(1, background.w * 9 / 16);
        std::vector<double> row(static_cast<std::size_t>(background.h), 0);
        for (int y = 1; y < background.h; ++y)
          for (int x = 0; x < background.w; ++x) row[static_cast<std::size_t>(y)] += std::abs(luma(background, x, y) - luma(background, x, y - 1));
        win.y = best_window(row, win.h);
      }
      return win;
    }

    // Area average of the window into ow x oh. The background is opaque art: its alpha channel is ignored (a decoder may report 0 for an image that has none, and
    // weighting by it turned a whole Star Rail poster black).
    Rgba
    scale_window(const Rgba &src, const Window &win, int ow, int oh) {
      Rgba out;
      out.w = ow;
      out.h = oh;
      out.px.resize(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh) * 4);
      for (int y = 0; y < oh; ++y) {
        const int y0 = win.y + y * win.h / oh;
        const int y1 = std::min(win.y + win.h, std::max(y0 + 1, win.y + ((y + 1) * win.h + oh - 1) / oh));
        for (int x = 0; x < ow; ++x) {
          const int x0 = win.x + x * win.w / ow;
          const int x1 = std::min(win.x + win.w, std::max(x0 + 1, win.x + ((x + 1) * win.w + ow - 1) / ow));
          long long r = 0, g = 0, b = 0;
          const int count = (x1 - x0) * (y1 - y0);
          for (int sy = y0; sy < y1; ++sy)
            for (int sx = x0; sx < x1; ++sx) {
              const std::size_t at = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(src.w) + static_cast<std::size_t>(sx)) * 4;
              r += src.px[at];
              g += src.px[at + 1];
              b += src.px[at + 2];
            }
          const std::size_t to = (static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)) * 4;
          out.px[to] = static_cast<std::uint8_t>(r / count);
          out.px[to + 1] = static_cast<std::uint8_t>(g / count);
          out.px[to + 2] = static_cast<std::uint8_t>(b / count);
          out.px[to + 3] = 255;
        }
      }
      return out;
    }

    struct Crop {
      int x = 0;
      int y = 0;
      int w = 0;
      int h = 0;
    };

    // The bounding box of what the logo shows (alpha above a threshold); w == 0 when it shows nothing.
    Crop
    visible_crop(const Rgba &logo) {
      if (!valid_image(logo)) return {};
      int x0 = logo.w, y0 = logo.h, x1 = -1, y1 = -1;
      for (int y = 0; y < logo.h; ++y)
        for (int x = 0; x < logo.w; ++x) {
          if (sample(logo, x, y, 3) <= 8) continue;
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x);
          y1 = std::max(y1, y);
        }
      if (x1 < 0) return {};
      return { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
    }

    // one pass of a box blur of radius r along rows and then columns of a w x h plane (running sums, edges clamped)
    void
    box_blur(std::vector<float> &plane, int w, int h, int r) {
      std::vector<float> line(static_cast<std::size_t>(std::max(w, h)));
      const auto pass = [&](int length, int count, auto at) {
        for (int k = 0; k < count; ++k) {
          for (int i = 0; i < length; ++i) line[static_cast<std::size_t>(i)] = plane[at(k, i)];
          float sum = 0;
          for (int i = -r; i <= r; ++i) sum += line[static_cast<std::size_t>(std::clamp(i, 0, length - 1))];
          for (int i = 0; i < length; ++i) {
            plane[at(k, i)] = sum / static_cast<float>(2 * r + 1);
            sum += line[static_cast<std::size_t>(std::clamp(i + r + 1, 0, length - 1))] - line[static_cast<std::size_t>(std::clamp(i - r, 0, length - 1))];
          }
        }
      };
      pass(w, h, [&](int row, int i) { return static_cast<std::size_t>(row) * static_cast<std::size_t>(w) + static_cast<std::size_t>(i); });
      pass(h, w, [&](int column, int i) { return static_cast<std::size_t>(i) * static_cast<std::size_t>(w) + static_cast<std::size_t>(column); });
    }

    // Lanczos-3: the sharpest of the usual enlargement kernels that does not show a staircase on the thin strokes of a logo
    double
    lanczos3(double t) {
      t = std::abs(t);
      if (t < 1e-9) return 1;
      if (t >= 3) return 0;
      constexpr double pi = 3.14159265358979323846;
      return std::sin(pi * t) / (pi * t) * std::sin(pi * t / 3) / (pi * t / 3);
    }

    // The logo's crop scaled to tw x th as premultiplied floats (r, g, b, a in 0..1): Lanczos-3 when enlarging, an area average when shrinking. Outside the
    // crop the logo is transparent, so its edge stays soft instead of smearing.
    std::vector<float>
    scale_logo(const Rgba &logo, const Crop &crop, int tw, int th) {
      std::vector<float> out(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0.f);
      const auto premult = [&](int x, int y, float *rgba) {
        if (x < 0 || y < 0 || x >= crop.w || y >= crop.h) return;
        const float a = sample(logo, crop.x + x, crop.y + y, 3) / 255.f;
        rgba[0] += sample(logo, crop.x + x, crop.y + y, 0) / 255.f * a;
        rgba[1] += sample(logo, crop.x + x, crop.y + y, 1) / 255.f * a;
        rgba[2] += sample(logo, crop.x + x, crop.y + y, 2) / 255.f * a;
        rgba[3] += a;
      };
      const double sx = static_cast<double>(tw) / crop.w;
      const double sy = static_cast<double>(th) / crop.h;
      for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
          float *dst = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(tw) + static_cast<std::size_t>(x)) * 4];
          if (sx < 1 || sy < 1) {
            const int x0 = x * crop.w / tw;
            const int x1 = std::max(x0 + 1, ((x + 1) * crop.w + tw - 1) / tw);
            const int y0 = y * crop.h / th;
            const int y1 = std::max(y0 + 1, ((y + 1) * crop.h + th - 1) / th);
            for (int yy = y0; yy < y1; ++yy)
              for (int xx = x0; xx < x1; ++xx) premult(xx, yy, dst);
            const float n = static_cast<float>((x1 - x0) * (y1 - y0));
            for (int c = 0; c < 4; ++c) dst[c] /= n;
            continue;
          }
          const double fx = (x + 0.5) / sx - 0.5;
          const double fy = (y + 0.5) / sy - 0.5;
          const int ix = static_cast<int>(std::floor(fx));
          const int iy = static_cast<int>(std::floor(fy));
          double weights = 0;
          float sum[4] = { 0, 0, 0, 0 };
          for (int j = -2; j <= 3; ++j)
            for (int i = -2; i <= 3; ++i) {
              const double w = lanczos3(fx - (ix + i)) * lanczos3(fy - (iy + j));
              float px[4] = { 0, 0, 0, 0 };
              premult(ix + i, iy + j, px);
              for (int c = 0; c < 4; ++c) sum[c] += static_cast<float>(px[c] * w);
              weights += w;
            }
          for (int c = 0; c < 4; ++c) dst[c] = std::clamp(static_cast<float>(sum[c] / weights), 0.f, 1.f);
          for (int c = 0; c < 3; ++c) dst[c] = std::min(dst[c], dst[3]);
        }
      return out;
    }
  }  // namespace

  bool
  image_is_blank(const Rgba &image) {
    if (!valid_image(image)) return true;
    const int step = std::max(1, std::min(image.w, image.h) / 96);
    double sum = 0, square = 0;
    long long count = 0;
    for (int y = 0; y < image.h; y += step)
      for (int x = 0; x < image.w; x += step) {
        const double v = luma(image, x, y);
        sum += v;
        square += v * v;
        ++count;
      }
    const double mean = sum / static_cast<double>(count);
    const double deviation = std::sqrt(std::max(0.0, square / static_cast<double>(count) - mean * mean));
    return mean < 8 || deviation < 3;
  }

  bool
  jpeg_is_blank(std::string_view bytes) {
    int w = 0, h = 0, comp = 0;
    unsigned char *pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &comp, 4);
    if (!pixels) return true;
    Rgba image;
    image.w = w;
    image.h = h;
    image.px.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(pixels);
    return image_is_blank(image);
  }

  bool
  logo_has_plate(const Rgba &logo) {
    const Crop crop = visible_crop(logo);
    if (crop.w <= 0) return false;
    long long opaque = 0;
    double luma_sum = 0;
    for (int y = 0; y < crop.h; ++y)
      for (int x = 0; x < crop.w; ++x) {
        if (sample(logo, crop.x + x, crop.y + y, 3) <= 200) continue;
        ++opaque;
        luma_sum += (sample(logo, crop.x + x, crop.y + y, 0) * 3 + sample(logo, crop.x + x, crop.y + y, 1) * 6 + sample(logo, crop.x + x, crop.y + y, 2)) / 10.0;
      }
    if (opaque == 0) return false;
    return static_cast<double>(opaque) / (static_cast<double>(crop.w) * crop.h) > 0.85 && luma_sum / static_cast<double>(opaque) < 70;
  }

  Rgba
  logo_without_plate(const Rgba &logo) {
    if (!logo_has_plate(logo)) return logo;
    Rgba out = logo;
    for (int i = 0; i < logo.w * logo.h; ++i) {
      std::uint8_t *px = &out.px[static_cast<std::size_t>(i) * 4];
      const int m = std::max({ px[0], px[1], px[2] });
      if (m == 0) {
        px[3] = 0;
        continue;
      }
      px[3] = static_cast<std::uint8_t>(px[3] * m / 255);
      for (int c = 0; c < 3; ++c) px[c] = static_cast<std::uint8_t>(std::min(255, px[c] * 255 / m));
    }
    return out;
  }

  int
  logo_visible_area(const Rgba &logo) {
    const Crop crop = visible_crop(logo_without_plate(logo));
    return crop.w * crop.h;
  }

  std::vector<std::uint8_t>
  encode_jpeg(const Rgba &image, int quality) {
    if (!valid_image(image)) return {};
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image.w) * static_cast<std::size_t>(image.h) * 3);
    for (std::size_t i = 0; i < static_cast<std::size_t>(image.w) * static_cast<std::size_t>(image.h); ++i)
      for (int c = 0; c < 3; ++c) rgb[i * 3 + static_cast<std::size_t>(c)] = image.px[i * 4 + static_cast<std::size_t>(c)];
    std::vector<std::uint8_t> out;
    const auto sink = [](void *context, void *data, int size) {
      auto *bytes = static_cast<std::vector<std::uint8_t> *>(context);
      const auto *begin = static_cast<const std::uint8_t *>(data);
      bytes->insert(bytes->end(), begin, begin + size);
    };
    if (!stbi_write_jpg_to_func(sink, &out, image.w, image.h, 3, rgb.data(), std::clamp(quality, 1, 100))) return {};
    return out;
  }

  HeroBox
  hero_logo_box(int hero_w, int hero_h, const Rgba &raw_logo) {
    const Rgba logo = logo_without_plate(raw_logo);
    const Crop crop = visible_crop(logo);
    if (crop.w <= 0 || hero_w <= 0 || hero_h <= 0) return {};
    const double scale = std::min({ kLogoMaxW * hero_w / crop.w, kLogoMaxH * hero_h / crop.h, kLogoMaxScale });
    HeroBox box;
    box.w = std::max(1, static_cast<int>(std::lround(crop.w * scale)));
    box.h = std::max(1, static_cast<int>(std::lround(crop.h * scale)));
    box.x = (hero_w - box.w) / 2;
    box.y = static_cast<int>(std::lround(kLogoCenterY * hero_h - box.h / 2.0));
    return box;
  }

  HeroBox
  cover_logo_box(int cover_w, int cover_h, const Rgba &raw_logo) {
    const Rgba logo = logo_without_plate(raw_logo);
    const Crop crop = visible_crop(logo);
    if (crop.w <= 0 || cover_w <= 0 || cover_h <= 0) return {};
    const double scale = std::min({ kCoverLogoMaxW * cover_w / crop.w, kCoverLogoMaxH * cover_h / crop.h, kLogoMaxScale });
    HeroBox box;
    box.w = std::max(1, static_cast<int>(std::lround(crop.w * scale)));
    box.h = std::max(1, static_cast<int>(std::lround(crop.h * scale)));
    box.x = (cover_w - box.w) / 2;
    box.y = static_cast<int>(std::lround(kCoverLogoCenterY * cover_h - box.h / 2.0));
    return box;
  }

  namespace {
    // The one way every poster carries its logo (the landscape hero and the portrait cover alike): a backdrop too close in tone to the logo is dimmed (lifted for a
    // dark logo) softly and only behind the logo, a soft shadow in the logo's own shape sets it apart, and the logo itself is laid on with Lanczos-3 scaling.
    void
    lay_logo(Rgba &canvas, const Rgba &logo, const Crop &crop, const HeroBox &box) {
    // How the logo's tone compares with the backdrop it lands on; a logo on its own opaque plate reads by itself.
    double logo_luma = 0, logo_weight = 0;
    long long opaque = 0;
    for (int y = 0; y < crop.h; ++y)
      for (int x = 0; x < crop.w; ++x) {
        const double a = sample(logo, crop.x + x, crop.y + y, 3) / 255.0;
        if (a > 0.78) ++opaque;
        logo_luma += a * ((sample(logo, crop.x + x, crop.y + y, 0) * 3 + sample(logo, crop.x + x, crop.y + y, 1) * 6 + sample(logo, crop.x + x, crop.y + y, 2)) / 10.0);
        logo_weight += a;
      }
    logo_luma = logo_weight > 0 ? logo_luma / logo_weight : 128;
    const bool plate = static_cast<double>(opaque) / (static_cast<double>(crop.w) * crop.h) > 0.85;
    double back_luma = 0;
    {
      long long total = 0;
      for (int y = box.y; y < box.y + box.h; y += 2)
        for (int x = box.x; x < box.x + box.w; x += 2) {
          back_luma += luma(canvas, std::clamp(x, 0, canvas.w - 1), std::clamp(y, 0, canvas.h - 1));
          ++total;
        }
      back_luma = total > 0 ? back_luma / static_cast<double>(total) : 128;
    }
    const bool light_logo = logo_luma >= 128;
    const double contrast = light_logo ? logo_luma - back_luma : back_luma - logo_luma;
    const double strength = plate ? 0 : std::clamp((110 - contrast) / 150.0, 0.0, 0.55);
    if (strength > 0) {
      const double cx = box.x + box.w / 2.0;
      const double cy = box.y + box.h / 2.0;
      const double rx = box.w * 0.5 * 1.6;
      const double ry = box.h * 0.5 * 1.9;
      const int x0 = std::max(0, static_cast<int>(cx - rx)), x1 = std::min(canvas.w - 1, static_cast<int>(cx + rx));
      const int y0 = std::max(0, static_cast<int>(cy - ry)), y1 = std::min(canvas.h - 1, static_cast<int>(cy + ry));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const double dx = (x - cx) / rx, dy = (y - cy) / ry;
          const double d2 = dx * dx + dy * dy;
          if (d2 >= 1) continue;
          const double amount = strength * (1 - d2) * (1 - d2);
          const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.w) + static_cast<std::size_t>(x)) * 4;
          for (int c = 0; c < 3; ++c) {
            const double v = canvas.px[at + c];
            canvas.px[at + c] = static_cast<std::uint8_t>(std::lround(light_logo ? v * (1 - amount) : v + (255 - v) * amount));
          }
        }
    }

    const std::vector<float> scaled = scale_logo(logo, crop, box.w, box.h);
    // a soft shadow in the logo's own shape (dark behind a light logo, light behind a dark one), the way a game's header art sets its logo apart; a plate needs none
    if (!plate) {
      const int radius = std::max(3, box.h / 28);
      const int pad = radius * 3;
      const int pw = box.w + 2 * pad, ph = box.h + 2 * pad;
      std::vector<float> shadow(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph), 0.f);
      for (int y = 0; y < box.h; ++y)
        for (int x = 0; x < box.w; ++x) shadow[static_cast<std::size_t>(y + pad) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x + pad)] = scaled[(static_cast<std::size_t>(y) * static_cast<std::size_t>(box.w) + static_cast<std::size_t>(x)) * 4 + 3];
      box_blur(shadow, pw, ph, radius);
      box_blur(shadow, pw, ph, radius);
      const int drop = radius / 2;
      for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
          const int dst_x = box.x - pad + x, dst_y = box.y - pad + y + drop;
          if (dst_x < 0 || dst_y < 0 || dst_x >= canvas.w || dst_y >= canvas.h) continue;
          const float amount = std::min(1.f, shadow[static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x)] * 1.6f) * 0.6f;
          const std::size_t at = (static_cast<std::size_t>(dst_y) * static_cast<std::size_t>(canvas.w) + static_cast<std::size_t>(dst_x)) * 4;
          for (int c = 0; c < 3; ++c) {
            const float v = canvas.px[at + c];
            canvas.px[at + c] = static_cast<std::uint8_t>(std::lround(light_logo ? v * (1 - amount) : v + (255 - v) * amount));
          }
        }
    }
    for (int y = 0; y < box.h; ++y)
      for (int x = 0; x < box.w; ++x) {
        const int dst_x = box.x + x, dst_y = box.y + y;
        if (dst_x < 0 || dst_y < 0 || dst_x >= canvas.w || dst_y >= canvas.h) continue;
        const float *src = &scaled[(static_cast<std::size_t>(y) * static_cast<std::size_t>(box.w) + static_cast<std::size_t>(x)) * 4];
        const std::size_t at = (static_cast<std::size_t>(dst_y) * static_cast<std::size_t>(canvas.w) + static_cast<std::size_t>(dst_x)) * 4;
        for (int c = 0; c < 3; ++c) canvas.px[at + c] = static_cast<std::uint8_t>(std::lround(std::clamp(src[c] * 255.f + canvas.px[at + c] * (1.f - src[3]), 0.f, 255.f)));
      }
    }
  }  // namespace

  Rgba
  compose_hero(const Rgba &background, const Rgba &raw_logo) {
    if (!valid_image(background)) return {};
    const Rgba logo = logo_without_plate(raw_logo);
    const Window win = hero_window(background);
    const int out_w = std::min(kHeroWidth, win.w);
    const int out_h = std::max(1, out_w * 9 / 16);
    Rgba hero = scale_window(background, win, out_w, out_h);
    const Crop crop = visible_crop(logo);
    const HeroBox box = hero_logo_box(out_w, out_h, logo);
    if (crop.w <= 0 || box.w <= 0) return hero;

    lay_logo(hero, logo, crop, box);
    return hero;
  }

  Rgba
  compose_cover(const Rgba &background, const Rgba &logo) {
    Rgba cover;
    cover.w = 600;
    cover.h = 900;
    cover.px.assign(static_cast<std::size_t>(cover.w * cover.h * 4), 0);
    if (background.w <= 0 || background.h <= 0) return cover;
    const int window = std::min(background.w, std::max(1, background.h * 2 / 3));
    const int origin = cover_window_x(background);
    for (int y = 0; y < cover.h; ++y) {
      for (int x = 0; x < cover.w; ++x) {
        const int src_x = origin + x * window / cover.w;
        const int src_y = y * background.h / cover.h;
        const std::size_t at = static_cast<std::size_t>((y * cover.w + x) * 4);
        // the art is opaque: the alpha a decoder reports for it (a lossless WebP whose alpha bit is unset can come back as 0) must not make the cover transparent
        for (int c = 0; c < 3; ++c) cover.px[at + c] = sample(background, src_x, src_y, c);
        cover.px[at + 3] = 255;
        const int scrim_start = cover.h - cover.h * 22 / 100;
        if (y >= scrim_start) {
          const int alpha = (y - scrim_start) * 180 / std::max(1, cover.h - scrim_start);
          for (int c = 0; c < 3; ++c) cover.px[at + c] = static_cast<std::uint8_t>(cover.px[at + c] * (255 - alpha) / 255);
        }
      }
    }
    const Rgba plain_logo = logo_without_plate(logo);
    const Crop crop = visible_crop(plain_logo);
    const HeroBox box = cover_logo_box(cover.w, cover.h, plain_logo);
    if (crop.w > 0 && box.w > 0) lay_logo(cover, plain_logo, crop, box);
    return cover;
  }

  Rgba
  decode_image(const std::uint8_t *data, std::size_t size) {
    Rgba out;
    if (!data || size == 0 || size > static_cast<std::size_t>(INT32_MAX)) return out;
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &comp) || w <= 0 || h <= 0 || static_cast<long long>(w) * h > 40'000'000LL) return out;
    unsigned char *pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!pixels) return out;
    out.w = w;
    out.h = h;
    out.px.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(pixels);
    return out;
  }

  Rgba
  badge_mark() {
    return decode_image(kBadgeMarkPng, sizeof(kBadgeMarkPng));
  }

  BadgeBox
  badge_box(int w, int h) {
    BadgeBox box;
    if (w <= 0 || h <= 0) return box;
    box.size = std::max(8, static_cast<int>(std::lround(std::min(0.26 * w, 0.25 * h))));
    const int margin = static_cast<int>(std::lround(box.size * 0.14));
    box.x = w - margin - box.size;
    box.y = margin;
    return box;
  }

  Rgba
  with_badge(const Rgba &image, const Rgba &mark) {
    if (!valid_image(image) || !valid_image(mark)) return image;
    const Crop crop = visible_crop(mark);
    if (crop.w <= 0) return image;
    Rgba out = image;
    const BadgeBox box = badge_box(image.w, image.h);
    const int s = box.size;
    // the plate: a rounded square of smoked glass (the backdrop blurred and darkened, so the mark reads on any key art), a thin light rim and a soft shadow below it
    const double radius = s * 0.30;
    const double rim = std::max(1.5, s / 70.0);
    const int pad = std::max(4, s / 8);
    const int pw = s + 2 * pad;
    const auto distance = [&](double px, double py) {
      const double half = s / 2.0;
      const double qx = std::abs(px - (pad + half)) - (half - radius);
      const double qy = std::abs(py - (pad + half)) - (half - radius);
      return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
    };
    const auto coverage = [](double d) { return std::clamp(0.5 - d, 0.0, 1.0); };
    {
      std::vector<float> shadow(static_cast<std::size_t>(pw) * static_cast<std::size_t>(pw), 0.f);
      for (int y = 0; y < pw; ++y)
        for (int x = 0; x < pw; ++x) shadow[static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x)] = static_cast<float>(coverage(distance(x + 0.5, y + 0.5)));
      const int blur = std::max(2, s / 14);
      box_blur(shadow, pw, pw, blur);
      box_blur(shadow, pw, pw, blur);
      const int drop = std::max(1, s / 36);
      for (int y = 0; y < pw; ++y)
        for (int x = 0; x < pw; ++x) {
          const int dx = box.x - pad + x, dy = box.y - pad + y + drop;
          if (dx < 0 || dy < 0 || dx >= out.w || dy >= out.h) continue;
          const float amount = std::min(1.f, shadow[static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x)] * 1.3f) * 0.45f;
          const std::size_t at = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(out.w) + static_cast<std::size_t>(dx)) * 4;
          for (int c = 0; c < 3; ++c) out.px[at + c] = static_cast<std::uint8_t>(std::lround(out.px[at + c] * (1.f - amount)));
        }
    }
    // the backdrop under the plate, blurred (after the shadow, so the plate's own shade is part of it)
    std::vector<float> blurred[3];
    for (int c = 0; c < 3; ++c) {
      blurred[c].assign(static_cast<std::size_t>(s) * static_cast<std::size_t>(s), 0.f);
      for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) blurred[c][static_cast<std::size_t>(y) * static_cast<std::size_t>(s) + static_cast<std::size_t>(x)] = sample(out, box.x + x, box.y + y, c);
      box_blur(blurred[c], s, s, std::max(2, s / 9));
      box_blur(blurred[c], s, s, std::max(2, s / 9));
    }
    const double fit = 0.80 * s / std::max(crop.w, crop.h);
    const int mw = std::max(1, static_cast<int>(std::lround(crop.w * fit)));
    const int mh = std::max(1, static_cast<int>(std::lround(crop.h * fit)));
    const std::vector<float> scaled_mark = scale_logo(mark, crop, mw, mh);
    const int mx = box.x + (s - mw) / 2;
    const int my = box.y + (s - mh) / 2 + s / 40;
    for (int y = 0; y < s; ++y)
      for (int x = 0; x < s; ++x) {
        const int dx = box.x + x, dy = box.y + y;
        if (dx < 0 || dy < 0 || dx >= out.w || dy >= out.h) continue;
        const double d = distance(x + pad + 0.5, y + pad + 0.5);
        const double cov = coverage(d);
        if (cov <= 0) continue;
        const std::size_t at = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(out.w) + static_cast<std::size_t>(dx)) * 4;
        // smoked glass: the blurred backdrop at 38 percent over a near-black tint, a little lighter toward the top, and a thin light rim
        const double lift = 1.0 - static_cast<double>(y) / s;
        const double rim_cov = std::clamp(0.5 - (std::abs(d + rim / 2) - rim / 2), 0.0, 1.0);
        for (int c = 0; c < 3; ++c) {
          const double glass = blurred[c][static_cast<std::size_t>(y) * static_cast<std::size_t>(s) + static_cast<std::size_t>(x)] * 0.38 + 16 + 14 * lift;
          double v = out.px[at + c] * (1 - cov) + glass * cov;
          v = v * (1 - rim_cov * 0.30) + 255 * rim_cov * 0.30;
          out.px[at + c] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
        }
        out.px[at + 3] = static_cast<std::uint8_t>(std::max<int>(out.px[at + 3], static_cast<int>(std::lround(cov * 255))));
      }
    for (int y = 0; y < mh; ++y)
      for (int x = 0; x < mw; ++x) {
        const int dx = mx + x, dy = my + y;
        if (dx < 0 || dy < 0 || dx >= out.w || dy >= out.h) continue;
        const float *src = &scaled_mark[(static_cast<std::size_t>(y) * static_cast<std::size_t>(mw) + static_cast<std::size_t>(x)) * 4];
        const std::size_t at = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(out.w) + static_cast<std::size_t>(dx)) * 4;
        for (int c = 0; c < 3; ++c) out.px[at + c] = static_cast<std::uint8_t>(std::lround(std::clamp(src[c] * 255.f + out.px[at + c] * (1.f - src[3]), 0.f, 255.f)));
      }
    return out;
  }


  Rgba
  placeholder_background(std::string_view game) {
    // 600x900 with a whole-width window is exactly what compose_cover()
    // maps one to one, so the card people see is the card drawn here.
    const Brand &brand = brand_of(game);
    Rgba card;
    card.w = 600;
    card.h = 900;
    card.px.assign(static_cast<std::size_t>(card.w * card.h * 4), 255);
    for (int y = 0; y < card.h; ++y) {
      // the vertical blend carries the wordmark; the darker base dominates
      // from a third of the way down
      const int step = std::clamp(y * 100 / std::max(1, card.h / 3), 0, 100);
      for (int x = 0; x < card.w; ++x) {
        const std::size_t at = static_cast<std::size_t>((y * card.w + x) * 4);
        for (int c = 0; c < 3; ++c) card.px[at + c] = static_cast<std::uint8_t>(brand.top[c] + (static_cast<int>(brand.base[c]) - brand.top[c]) * step / 100);
      }
    }
    draw_word(brand.word, card.h * 56 / 100, card);
    // a thin brand line under the wordmark keeps the card from reading as a
    // plain colour fill
    for (int x = card.w * 18 / 100; x < card.w * 82 / 100; ++x)
      for (int y = card.h * 68 / 100; y < card.h * 68 / 100 + 4; ++y) {
        const std::size_t at = static_cast<std::size_t>((y * card.w + x) * 4);
        for (int c = 0; c < 3; ++c) card.px[at + c] = static_cast<std::uint8_t>(255 - (255 - brand.top[c]) * 35 / 100);
      }
    return card;
  }

  Rgba
  placeholder_icon(std::string_view game) {
    const Brand &brand = brand_of(game);
    Rgba icon;
    icon.w = 256;
    icon.h = 256;
    icon.px.assign(static_cast<std::size_t>(icon.w * icon.h * 4), 255);
    for (int y = 0; y < icon.h; ++y)
      for (int x = 0; x < icon.w; ++x) {
        const std::size_t at = static_cast<std::size_t>((y * icon.w + x) * 4);
        for (int c = 0; c < 3; ++c) icon.px[at + c] = brand.base[c];
      }
    // the title's initial, in the lighter brand tone
    const std::uint8_t tone[3] = { brand.top[0], brand.top[1], brand.top[2] };
    if (brand.word && *brand.word) draw_text(std::string_view(brand.word).substr(0, 1), 96, 80, 16, tone, icon);
    return icon;
  }

  Rgba
  placeholder_hero(std::string_view game) {
    const Brand &brand = brand_of(game);
    Rgba hero;
    hero.w = 1920;
    hero.h = 1080;
    hero.px.assign(static_cast<std::size_t>(hero.w * hero.h * 4), 255);
    for (int y = 0; y < hero.h; ++y) {
      // the lighter tone at the top, the brand colour from the middle down
      const int step = std::clamp(y * 100 / std::max(1, hero.h / 2), 0, 100);
      for (int x = 0; x < hero.w; ++x) {
        const std::size_t at = static_cast<std::size_t>((y * hero.w + x) * 4);
        for (int c = 0; c < 3; ++c) hero.px[at + c] = static_cast<std::uint8_t>(brand.top[c] + (static_cast<int>(brand.base[c]) - brand.top[c]) * step / 100);
      }
    }
    // the name sits in the band the clients keep clear of their title scrim
    const int centre = static_cast<int>(hero.h * (kHeroSafeTop + kHeroSafeBottom) / 2);
    draw_word(brand.word, centre, hero, 14);
    const int line_y = centre + hero.h * 11 / 100;
    for (int x = hero.w * 36 / 100; x < hero.w * 64 / 100; ++x)
      for (int y = line_y; y < line_y + 6; ++y) {
        const std::size_t at = static_cast<std::size_t>((y * hero.w + x) * 4);
        for (int c = 0; c < 3; ++c) hero.px[at + c] = static_cast<std::uint8_t>(255 - (255 - brand.top[c]) * 35 / 100);
      }
    return hero;
  }

  std::vector<std::uint8_t>
  encode_png(const Rgba &image) {
    if (image.w <= 0 || image.h <= 0) return {};
    int length = 0;
    unsigned char *memory = stbi_write_png_to_mem(image.px.data(), image.w * 4, image.w, image.h, 4, &length);
    if (!memory || length <= 0) return {};
    std::vector<std::uint8_t> out(memory, memory + length);
    free(memory);
    return out;
  }
  bool
  art_due(ArtSchedule &schedule, std::chrono::system_clock::time_point now, ArtReason reason) {
    using clock = std::chrono::system_clock;
    if (reason == ArtReason::manual) return true;
    if (reason == ArtReason::host_start) {
      if (schedule.host_started) return false;
      schedule.host_started = true;
      schedule.last_check = now;
      schedule.last_fetch = now;
      return true;
    }
    if (reason == ArtReason::app_list) {
      if (schedule.last_check.time_since_epoch().count() != 0 && now - schedule.last_check < std::chrono::minutes(15)) return false;
      schedule.last_check = now;
      return true;
    }
    const auto gap = std::chrono::hours(6) + schedule.jitter;
    if (schedule.last_fetch.time_since_epoch().count() != 0 && now - schedule.last_fetch < gap) return false;
    schedule.last_fetch = now;
    schedule.last_check = now;
    return true;
  }
  ArtUrls
  art_urls_from_payload(const json &games_payload, const json &basic_payload, const std::string &biz, bool prefer_carousel) {
    ArtUrls urls;
    for (const auto &game : array_at(games_payload, "data", "games")) {
      if (!game.is_object() || game.value("biz", std::string()) != biz) continue;
      const json display = game.value("display", json::object());
      urls.icon = url_of(display.value("icon", json::object()));
      urls.logo = url_of(display.value("logo", json::object()));
      urls.logo_large = url_of(display.value("top_left_logo", json::object()));
      urls.background = url_of(display.value("background", json::object()));
      break;
    }
    if (prefer_carousel || urls.background.empty()) {
      for (const auto &info : array_at(basic_payload, "data", "game_info_list")) {
        if (!info.is_object()) continue;
        const json game = info.value("game", json::object());
        if (game.value("biz", std::string()) != biz) continue;
        const json backgrounds = info.value("backgrounds", json::array());
        if (!backgrounds.empty() && backgrounds[0].is_object()) {
          const std::string carousel = url_of(backgrounds[0].value("background", json::object()));
          if (!carousel.empty()) urls.background = carousel;
        }
        break;
      }
    }
    urls.version = art_version_of(urls.background.empty() ? urls.icon : urls.background);
    return urls;
  }
}  // namespace sparxie
