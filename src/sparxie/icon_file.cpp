#include "src/sparxie/icon_file.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <numeric>

namespace sparxie {
  namespace {
    // An icon larger than this (on a side) is not read: real icons stop at 256.
    constexpr int kMaxIconSide = 1024;
    // The resource section is read as a whole up to this size (game launchers carry a few MB of resources at most).
    constexpr std::uint64_t kMaxResourceBytes = 96ull * 1024 * 1024;

    std::uint32_t
    u8(const std::string &s, std::size_t at) {
      return at < s.size() ? static_cast<std::uint8_t>(s[at]) : 0u;
    }
    std::uint32_t
    u16(const std::string &s, std::size_t at) {
      return u8(s, at) | (u8(s, at + 1) << 8);
    }
    std::uint32_t
    u32(const std::string &s, std::size_t at) {
      return u16(s, at) | (u16(s, at + 2) << 16);
    }

    bool
    is_png(const std::string &s) {
      return s.size() > 8 && s.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0;
    }

    // One picture an icon file or a group-icon resource lists.
    struct Entry {
      int w = 0;
      int h = 0;
      int bpp = 0;
      std::uint32_t size = 0;
      std::uint32_t where = 0;  // file offset (ico) or RT_ICON resource id (PE)
    };

    // The pictures best first: the largest, then the deepest colour, then the listing order.
    std::vector<std::size_t>
    best_first(const std::vector<Entry> &entries) {
      std::vector<std::size_t> order(entries.size());
      std::iota(order.begin(), order.end(), std::size_t(0));
      std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const long long area_a = static_cast<long long>(entries[a].w) * entries[a].h, area_b = static_cast<long long>(entries[b].w) * entries[b].h;
        if (area_a != area_b) return area_a > area_b;
        return entries[a].bpp > entries[b].bpp;
      });
      return order;
    }

    // The DIB of an icon image (a BITMAPINFOHEADER, the colour table, the XOR bitmap, then the 1-bit AND mask; the height field counts both bitmaps).
    Rgba
    decode_dib(const std::string &d) {
      Rgba out;
      const std::uint32_t header = u32(d, 0);
      if (header < 40 || header > 256 || d.size() < header) return out;
      const long long width = static_cast<std::int32_t>(u32(d, 4));
      long long height = static_cast<std::int32_t>(u32(d, 8));
      const int bpp = static_cast<int>(u16(d, 14));
      const std::uint32_t compression = u32(d, 16);
      if (width < 1 || width > kMaxIconSide || height == 0) return out;
      const bool top_down = height < 0;
      height = std::llabs(height);
      // the height counts the AND mask too; a file that wrote the plain height (no mask) is read as it is
      const long long h = height / 2 >= 1 && height / 2 <= kMaxIconSide ? height / 2 : height;
      if (h < 1 || h > kMaxIconSide) return out;
      if (compression != 0 && compression != 3) return out;
      if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) return out;
      std::size_t at = header;
      std::vector<std::uint8_t> palette;
      if (bpp <= 8) {
        std::uint32_t colours = u32(d, 32);
        if (colours == 0 || colours > (1u << bpp)) colours = 1u << bpp;
        if (at + static_cast<std::size_t>(colours) * 4 > d.size()) return out;
        for (std::uint32_t i = 0; i < colours; ++i) {
          palette.push_back(static_cast<std::uint8_t>(u8(d, at + i * 4 + 2)));
          palette.push_back(static_cast<std::uint8_t>(u8(d, at + i * 4 + 1)));
          palette.push_back(static_cast<std::uint8_t>(u8(d, at + i * 4 + 0)));
        }
        at += static_cast<std::size_t>(colours) * 4;
      }
      else if (compression == 3 && header == 40)
        at += 12;  // BI_BITFIELDS masks after a plain header; the common 32-bit BGRA layout is assumed
      const std::size_t stride = ((static_cast<std::size_t>(width) * static_cast<std::size_t>(bpp) + 31) / 32) * 4;
      const std::size_t xor_bytes = stride * static_cast<std::size_t>(h);
      if (at + xor_bytes > d.size()) return out;
      const std::size_t mask_stride = ((static_cast<std::size_t>(width) + 31) / 32) * 4;
      const std::size_t mask_at = at + xor_bytes;
      const bool has_mask = mask_at + mask_stride * static_cast<std::size_t>(h) <= d.size();
      out.w = static_cast<int>(width);
      out.h = static_cast<int>(h);
      out.px.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(h) * 4, 0);
      bool any_alpha = false;
      for (long long y = 0; y < h; ++y) {
        const long long src_row = top_down ? y : h - 1 - y;
        const std::size_t row = at + static_cast<std::size_t>(src_row) * stride;
        for (long long x = 0; x < width; ++x) {
          std::uint8_t *dst = &out.px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
          if (bpp == 32) {
            dst[0] = static_cast<std::uint8_t>(u8(d, row + x * 4 + 2));
            dst[1] = static_cast<std::uint8_t>(u8(d, row + x * 4 + 1));
            dst[2] = static_cast<std::uint8_t>(u8(d, row + x * 4 + 0));
            dst[3] = static_cast<std::uint8_t>(u8(d, row + x * 4 + 3));
            if (dst[3]) any_alpha = true;
          }
          else if (bpp == 24) {
            dst[0] = static_cast<std::uint8_t>(u8(d, row + x * 3 + 2));
            dst[1] = static_cast<std::uint8_t>(u8(d, row + x * 3 + 1));
            dst[2] = static_cast<std::uint8_t>(u8(d, row + x * 3 + 0));
            dst[3] = 255;
          }
          else {
            std::uint32_t index = 0;
            if (bpp == 8)
              index = u8(d, row + x);
            else if (bpp == 4)
              index = (u8(d, row + x / 2) >> ((x & 1) ? 0 : 4)) & 15;
            else
              index = (u8(d, row + x / 8) >> (7 - (x & 7))) & 1;
            if (index * 3 + 2 >= palette.size()) index = 0;
            dst[0] = palette[index * 3];
            dst[1] = palette[index * 3 + 1];
            dst[2] = palette[index * 3 + 2];
            dst[3] = 255;
          }
          // the AND mask: a set bit is transparent (the colour bitmap's own alpha wins when it has any)
          if (has_mask && bpp != 32) {
            const std::size_t mrow = mask_at + static_cast<std::size_t>(src_row) * mask_stride;
            if ((u8(d, mrow + x / 8) >> (7 - (x & 7))) & 1) dst[3] = 0;
          }
        }
      }
      if (bpp == 32 && !any_alpha) {
        // a 32-bit image with no alpha at all: the AND mask says what is see-through, the rest is opaque
        for (long long y = 0; y < h; ++y) {
          const long long src_row = top_down ? y : h - 1 - y;
          for (long long x = 0; x < width; ++x) {
            bool clear = false;
            if (has_mask) clear = (u8(d, mask_at + static_cast<std::size_t>(src_row) * mask_stride + x / 8) >> (7 - (x & 7))) & 1;
            out.px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4 + 3] = clear ? 0 : 255;
          }
        }
      }
      return out;
    }

    Rgba
    decode_icon_image(const std::string &data, const ImageDecodeFn &decode) {
      if (data.empty()) return {};
      if (is_png(data)) {
        if (!decode) return {};
        Rgba image = decode(std::string_view(data));
        return image.w > 0 && image.h > 0 && image.px.size() >= static_cast<std::size_t>(image.w) * static_cast<std::size_t>(image.h) * 4 ? image : Rgba {};
      }
      return decode_dib(data);
    }

    // ---- PE ----

    struct Source {
      std::uint64_t size = 0;
      std::function<bool(std::uint64_t, std::size_t, std::string &)> read;
    };

    struct Section {
      std::uint32_t virtual_size = 0;
      std::uint32_t rva = 0;
      std::uint32_t raw_size = 0;
      std::uint32_t raw_ptr = 0;
    };

    // One directory entry of the resource tree.
    struct DirEntry {
      std::uint32_t key = 0;  // the id (named entries are not used)
      bool named = false;
      bool is_dir = false;
      std::uint32_t offset = 0;  // relative to the resource root
    };

    struct Resources {
      std::string rs;  // the resource section's bytes
      std::uint32_t rva = 0;
      const std::vector<Section> *sections = nullptr;
      const Source *source = nullptr;

      std::vector<DirEntry>
      list(std::uint32_t dir) const {
        std::vector<DirEntry> out;
        if (static_cast<std::uint64_t>(dir) + 16 > rs.size()) return out;
        const std::uint32_t count = u16(rs, dir + 12) + u16(rs, dir + 14);
        if (count > 4096) return out;
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint64_t at = static_cast<std::uint64_t>(dir) + 16 + static_cast<std::uint64_t>(i) * 8;
          if (at + 8 > rs.size()) break;
          const std::uint32_t name = u32(rs, at), off = u32(rs, at + 4);
          DirEntry entry;
          entry.named = (name & 0x80000000u) != 0;
          entry.key = name & 0x7fffffffu;
          entry.is_dir = (off & 0x80000000u) != 0;
          entry.offset = off & 0x7fffffffu;
          out.push_back(entry);
        }
        return out;
      }

      // The bytes of the first resource below a name/language entry (a data entry, or a directory whose first entry leads to one; three levels at most).
      std::string
      first_data(const DirEntry &from) const {
        DirEntry entry = from;
        for (int depth = 0; depth < 4 && entry.is_dir; ++depth) {
          const auto children = list(entry.offset);
          if (children.empty()) return {};
          entry = children.front();
        }
        if (entry.is_dir || static_cast<std::uint64_t>(entry.offset) + 16 > rs.size()) return {};
        const std::uint32_t data_rva = u32(rs, entry.offset), length = u32(rs, entry.offset + 4);
        if (length == 0 || length > 16u * 1024 * 1024) return {};
        // inside the resource section: slice it; elsewhere (a data RVA the section table maps): read from the file
        if (data_rva >= rva && static_cast<std::uint64_t>(data_rva) - rva + length <= rs.size()) return rs.substr(data_rva - rva, length);
        for (const Section &section : *sections) {
          const std::uint32_t span = std::max(section.virtual_size, section.raw_size);
          if (data_rva < section.rva || data_rva - section.rva >= span) continue;
          const std::uint32_t inside = data_rva - section.rva;
          if (static_cast<std::uint64_t>(inside) + length > section.raw_size) return {};
          std::string out;
          if (!source->read(static_cast<std::uint64_t>(section.raw_ptr) + inside, length, out)) return {};
          return out;
        }
        return {};
      }

      // The type directory of an id (RT_ICON = 3, RT_GROUP_ICON = 14); entries are the names/ids below it.
      std::vector<DirEntry>
      of_type(std::uint32_t type) const {
        for (const DirEntry &entry : list(0))
          if (!entry.named && entry.key == type && entry.is_dir) return list(entry.offset);
        return {};
      }
    };

    Rgba
    icon_from_source(const Source &source, const ImageDecodeFn &decode) {
      std::string dos;
      if (source.size < 64 || !source.read(0, 64, dos) || dos.compare(0, 2, "MZ") != 0) return {};
      const std::uint32_t nt_at = u32(dos, 60);
      if (nt_at < 64 || nt_at > (1u << 20) || nt_at + 24 > source.size) return {};
      std::string nt;
      const std::size_t nt_len = static_cast<std::size_t>(std::min<std::uint64_t>(24 + 240, source.size - nt_at));
      if (!source.read(nt_at, nt_len, nt) || nt.compare(0, 4, std::string("PE\0\0", 4)) != 0) return {};
      const std::uint32_t section_count = u16(nt, 6), optional_size = u16(nt, 20), magic = u16(nt, 24);
      std::size_t directories = 0;
      if (magic == 0x10b)
        directories = 24 + 96;
      else if (magic == 0x20b)
        directories = 24 + 112;
      else
        return {};
      if (section_count == 0 || section_count > 96 || directories + 8 * 3 > nt.size() || directories + 24 > 24 + static_cast<std::size_t>(optional_size)) return {};
      const std::uint32_t dir_count = u32(nt, directories - 4);
      if (dir_count < 3) return {};
      const std::uint32_t rsrc_rva = u32(nt, directories + 16), rsrc_size = u32(nt, directories + 20);
      if (rsrc_rva == 0 || rsrc_size < 16) return {};
      std::string table;
      const std::uint64_t table_at = static_cast<std::uint64_t>(nt_at) + 24 + optional_size;
      if (table_at + static_cast<std::uint64_t>(section_count) * 40 > source.size || !source.read(table_at, static_cast<std::size_t>(section_count) * 40, table)) return {};
      std::vector<Section> sections;
      for (std::uint32_t i = 0; i < section_count; ++i) {
        Section section;
        section.virtual_size = u32(table, i * 40 + 8);
        section.rva = u32(table, i * 40 + 12);
        section.raw_size = u32(table, i * 40 + 16);
        section.raw_ptr = u32(table, i * 40 + 20);
        sections.push_back(section);
      }
      Resources resources;
      resources.rva = rsrc_rva;
      resources.sections = &sections;
      resources.source = &source;
      for (const Section &section : sections) {
        if (rsrc_rva < section.rva || rsrc_rva - section.rva >= std::max(section.virtual_size, section.raw_size)) continue;
        const std::uint32_t inside = rsrc_rva - section.rva;
        if (inside >= section.raw_size) return {};
        const std::uint64_t file_at = static_cast<std::uint64_t>(section.raw_ptr) + inside;
        std::uint64_t want = std::min<std::uint64_t>({ static_cast<std::uint64_t>(section.raw_size) - inside, kMaxResourceBytes, source.size > file_at ? source.size - file_at : 0 });
        // the whole rest of the section is read: the data entries may sit after the directory tree
        if (want < 16 || !source.read(file_at, static_cast<std::size_t>(want), resources.rs)) return {};
        break;
      }
      if (resources.rs.empty()) return {};
      const auto groups = resources.of_type(14);
      const auto images = resources.of_type(3);
      if (groups.empty() || images.empty()) return {};
      for (const DirEntry &group_entry : groups) {
        const std::string group = resources.first_data(group_entry);
        if (group.size() < 6 || u16(group, 2) != 1) continue;
        const std::uint32_t count = std::min<std::uint32_t>(u16(group, 4), 64);
        std::vector<Entry> entries;
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::size_t at = 6 + static_cast<std::size_t>(i) * 14;
          if (at + 14 > group.size()) break;
          Entry entry;
          entry.w = u8(group, at) == 0 ? 256 : static_cast<int>(u8(group, at));
          entry.h = u8(group, at + 1) == 0 ? 256 : static_cast<int>(u8(group, at + 1));
          entry.bpp = static_cast<int>(u16(group, at + 6));
          entry.size = u32(group, at + 8);
          entry.where = u16(group, at + 12);
          entries.push_back(entry);
        }
        for (const std::size_t index : best_first(entries)) {
          for (const DirEntry &image : images) {
            if (image.named || image.key != entries[index].where) continue;
            Rgba icon = decode_icon_image(resources.first_data(image), decode);
            if (icon.w > 0) return icon;
          }
        }
      }
      return {};
    }
  }  // namespace

  Rgba
  icon_from_ico(std::string_view bytes, const ImageDecodeFn &decode) {
    const std::string file(bytes);
    if (file.size() < 22 || u16(file, 0) != 0 || u16(file, 2) != 1) return {};
    const std::uint32_t count = std::min<std::uint32_t>(u16(file, 4), 256);
    std::vector<Entry> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::size_t at = 6 + static_cast<std::size_t>(i) * 16;
      if (at + 16 > file.size()) break;
      Entry entry;
      entry.w = u8(file, at) == 0 ? 256 : static_cast<int>(u8(file, at));
      entry.h = u8(file, at + 1) == 0 ? 256 : static_cast<int>(u8(file, at + 1));
      entry.bpp = static_cast<int>(u16(file, at + 6));
      entry.size = u32(file, at + 8);
      entry.where = u32(file, at + 12);
      if (entry.size == 0 || static_cast<std::uint64_t>(entry.where) + entry.size > file.size()) continue;
      entries.push_back(entry);
    }
    for (const std::size_t index : best_first(entries)) {
      Rgba icon = decode_icon_image(file.substr(entries[index].where, entries[index].size), decode);
      if (icon.w > 0) return icon;
    }
    return {};
  }

  Rgba
  icon_from_pe_bytes(std::string_view bytes, const ImageDecodeFn &decode) {
    Source source;
    source.size = bytes.size();
    source.read = [bytes](std::uint64_t off, std::size_t len, std::string &out) {
      if (off > bytes.size() || len > bytes.size() - off) return false;
      out.assign(bytes.substr(static_cast<std::size_t>(off), len));
      return true;
    };
    return icon_from_source(source, decode);
  }

  Rgba
  icon_from_pe(const std::filesystem::path &exe, const ImageDecodeFn &decode) {
    std::error_code ec;
    const std::uintmax_t length = std::filesystem::file_size(exe, ec);
    if (ec || length < 64) return {};
    std::ifstream in(exe, std::ios::binary);
    if (!in) return {};
    Source source;
    source.size = length;
    source.read = [&in](std::uint64_t off, std::size_t len, std::string &out) {
      out.assign(len, '\0');
      in.clear();
      in.seekg(static_cast<std::streamoff>(off));
      in.read(out.data(), static_cast<std::streamsize>(len));
      return static_cast<std::size_t>(in.gcount()) == len;
    };
    return icon_from_source(source, decode);
  }

  Rgba
  icon_from_file_bytes(std::string_view bytes, const ImageDecodeFn &decode) {
    if (bytes.size() >= 6 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1 && bytes[3] == 0) return icon_from_ico(bytes, decode);
    if (!decode) return {};
    return decode(bytes);
  }

  Rgba
  icon_square(const Rgba &image, int size) {
    Rgba out;
    if (image.w <= 0 || image.h <= 0 || size <= 0 || image.px.size() < static_cast<std::size_t>(image.w) * static_cast<std::size_t>(image.h) * 4) return out;
    const double scale = std::min(static_cast<double>(size) / image.w, static_cast<double>(size) / image.h);
    const int tw = std::max(1, static_cast<int>(std::lround(image.w * scale)));
    const int th = std::max(1, static_cast<int>(std::lround(image.h * scale)));
    out.w = size;
    out.h = size;
    out.px.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 0);
    const int ox = (size - tw) / 2, oy = (size - th) / 2;
    const auto pixel = [&](int x, int y, float v[4]) {
      x = std::clamp(x, 0, image.w - 1);
      y = std::clamp(y, 0, image.h - 1);
      const std::uint8_t *p = &image.px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.w) + static_cast<std::size_t>(x)) * 4];
      const float a = p[3] / 255.f;
      v[0] = p[0] / 255.f * a;
      v[1] = p[1] / 255.f * a;
      v[2] = p[2] / 255.f * a;
      v[3] = a;
    };
    for (int y = 0; y < th; ++y)
      for (int x = 0; x < tw; ++x) {
        float acc[4] = { 0, 0, 0, 0 };
        if (scale < 1.0) {
          // area average over the source rectangle this pixel covers
          const double x0 = x / scale, x1 = (x + 1) / scale, y0 = y / scale, y1 = (y + 1) / scale;
          double total = 0;
          for (int sy = static_cast<int>(std::floor(y0)); sy < static_cast<int>(std::ceil(y1)); ++sy)
            for (int sx = static_cast<int>(std::floor(x0)); sx < static_cast<int>(std::ceil(x1)); ++sx) {
              const double w = (std::min<double>(x1, sx + 1) - std::max<double>(x0, sx)) * (std::min<double>(y1, sy + 1) - std::max<double>(y0, sy));
              if (w <= 0) continue;
              float v[4];
              pixel(sx, sy, v);
              for (int c = 0; c < 4; ++c) acc[c] += v[c] * static_cast<float>(w);
              total += w;
            }
          if (total > 0)
            for (float &c : acc) c = static_cast<float>(c / total);
        }
        else {
          const double fx = (x + 0.5) / scale - 0.5, fy = (y + 0.5) / scale - 0.5;
          const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
          const float tx = static_cast<float>(fx - ix), ty = static_cast<float>(fy - iy);
          float a[4], b[4], c[4], d[4];
          pixel(ix, iy, a);
          pixel(ix + 1, iy, b);
          pixel(ix, iy + 1, c);
          pixel(ix + 1, iy + 1, d);
          for (int k = 0; k < 4; ++k) acc[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
        }
        std::uint8_t *dst = &out.px[(static_cast<std::size_t>(y + oy) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x + ox)) * 4];
        const float a = std::clamp(acc[3], 0.f, 1.f);
        for (int k = 0; k < 3; ++k) dst[k] = a > 0.f ? static_cast<std::uint8_t>(std::lround(std::clamp(acc[k] / a, 0.f, 1.f) * 255.f)) : 0;
        dst[3] = static_cast<std::uint8_t>(std::lround(a * 255.f));
      }
    return out;
  }

  bool
  icon_is_usable(const Rgba &icon) {
    if (std::min(icon.w, icon.h) < kMinIconPixels || icon.px.size() < static_cast<std::size_t>(icon.w) * static_cast<std::size_t>(icon.h) * 4) return false;
    std::size_t visible = 0;
    for (std::size_t i = 3; i < icon.px.size(); i += 4)
      if (icon.px[i] > 16) ++visible;
    return visible * 50 >= static_cast<std::size_t>(icon.w) * static_cast<std::size_t>(icon.h);
  }
}  // namespace sparxie
