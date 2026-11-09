#pragma once
// A fixture executable and icon files built in memory: a minimal PE (PE32+ or PE32) whose .rsrc section holds a group icon and its RT_ICON images (PNG-compressed
// and DIB entries), laid out the way the Windows resource compiler does it (type -> id -> language -> data entry).
#include "src/sparxie/art.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace pefix {
  inline void
  put16(std::string &out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
  }
  inline void
  put32(std::string &out, std::uint32_t value) {
    put16(out, value & 0xffff);
    put16(out, value >> 16);
  }
  inline void
  pad_to(std::string &out, std::size_t length) {
    if (out.size() < length) out.append(length - out.size(), '\0');
  }

  // One image of an icon: its side, its bytes (a PNG or a DIB) and the colour depth the directory states.
  struct IconImage {
    int side = 0;
    std::string data;
    int bpp = 32;
  };

  // The colour of pixel (x, y) of the fixture pictures: a left-to-right ramp in `red`, `green` marking the top half, so an upside-down or mirrored decode shows.
  inline void
  pixel_of(int x, int y, int side, std::uint8_t base, std::uint8_t out[4]) {
    out[0] = static_cast<std::uint8_t>(base + (x * 100) / std::max(1, side));
    out[1] = y < side / 2 ? 200 : 20;
    out[2] = 90;
    out[3] = 255;
  }

  inline sparxie::Rgba
  picture(int side, std::uint8_t base) {
    sparxie::Rgba image;
    image.w = side;
    image.h = side;
    image.px.resize(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4);
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x) pixel_of(x, y, side, base, &image.px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)) * 4]);
    return image;
  }

  inline IconImage
  png_image(int side, std::uint8_t base) {
    const auto png = sparxie::encode_png(picture(side, base));
    return { side, std::string(png.begin(), png.end()), 32 };
  }

  // A 32-bit DIB (bottom-up) with its AND mask; `with_alpha` false writes alpha 0 everywhere (the mask then says what is opaque). `clear_right` marks the right half transparent.
  inline IconImage
  dib_image(int side, std::uint8_t base, bool with_alpha = true, bool clear_right = false) {
    std::string d;
    put32(d, 40);
    put32(d, static_cast<std::uint32_t>(side));
    put32(d, static_cast<std::uint32_t>(side * 2));
    put16(d, 1);
    put16(d, 32);
    put32(d, 0);
    put32(d, static_cast<std::uint32_t>(side * side * 4));
    for (int i = 0; i < 4; ++i) put32(d, 0);
    for (int row = side - 1; row >= 0; --row)
      for (int x = 0; x < side; ++x) {
        std::uint8_t px[4];
        pixel_of(x, row, side, base, px);
        d.push_back(static_cast<char>(px[2]));
        d.push_back(static_cast<char>(px[1]));
        d.push_back(static_cast<char>(px[0]));
        d.push_back(static_cast<char>(with_alpha ? (clear_right && x >= side / 2 ? 0 : 255) : 0));
      }
    const int mask_stride = ((side + 31) / 32) * 4;
    for (int row = side - 1; row >= 0; --row) {
      std::string line(static_cast<std::size_t>(mask_stride), '\0');
      if (clear_right && !with_alpha)
        for (int x = side / 2; x < side; ++x) line[static_cast<std::size_t>(x / 8)] = static_cast<char>(static_cast<std::uint8_t>(line[static_cast<std::size_t>(x / 8)]) | (0x80 >> (x & 7)));
      d += line;
    }
    return { side, d, 32 };
  }

  // A 24-bit DIB with an AND mask whose right half is transparent.
  inline IconImage
  dib24_image(int side, std::uint8_t base) {
    std::string d;
    put32(d, 40);
    put32(d, static_cast<std::uint32_t>(side));
    put32(d, static_cast<std::uint32_t>(side * 2));
    put16(d, 1);
    put16(d, 24);
    put32(d, 0);
    put32(d, 0);
    for (int i = 0; i < 4; ++i) put32(d, 0);
    const int stride = ((side * 24 + 31) / 32) * 4;
    for (int row = side - 1; row >= 0; --row) {
      std::string line;
      for (int x = 0; x < side; ++x) {
        std::uint8_t px[4];
        pixel_of(x, row, side, base, px);
        line.push_back(static_cast<char>(px[2]));
        line.push_back(static_cast<char>(px[1]));
        line.push_back(static_cast<char>(px[0]));
      }
      pad_to(line, static_cast<std::size_t>(stride));
      d += line;
    }
    const int mask_stride = ((side + 31) / 32) * 4;
    for (int row = side - 1; row >= 0; --row) {
      std::string line(static_cast<std::size_t>(mask_stride), '\0');
      for (int x = side / 2; x < side; ++x) line[static_cast<std::size_t>(x / 8)] = static_cast<char>(static_cast<std::uint8_t>(line[static_cast<std::size_t>(x / 8)]) | (0x80 >> (x & 7)));
      d += line;
    }
    return { side, d, 24 };
  }

  // An 8-bit paletted DIB: two palette entries (pixel value 0 = dark blue, 1 = orange), the right half is index 1, no transparency.
  inline IconImage
  dib8_image(int side) {
    std::string d;
    put32(d, 40);
    put32(d, static_cast<std::uint32_t>(side));
    put32(d, static_cast<std::uint32_t>(side * 2));
    put16(d, 1);
    put16(d, 8);
    put32(d, 0);
    put32(d, 0);
    put32(d, 0);
    put32(d, 0);
    put32(d, 2);
    put32(d, 0);
    put32(d, 0x00800000);  // palette 0, stored B,G,R,0: red 0x80
    put32(d, 0x0000a0ff);  // palette 1: blue 0xff, green 0xa0
    const int stride = ((side * 8 + 31) / 32) * 4;
    for (int row = side - 1; row >= 0; --row) {
      std::string line;
      for (int x = 0; x < side; ++x) line.push_back(x >= side / 2 ? 1 : 0);
      pad_to(line, static_cast<std::size_t>(stride));
      d += line;
    }
    d += std::string(static_cast<std::size_t>(((side + 31) / 32) * 4 * side), '\0');
    return { side, d, 8 };
  }

  // An .ico file holding the images.
  inline std::string
  ico_file(const std::vector<IconImage> &images) {
    std::string out;
    put16(out, 0);
    put16(out, 1);
    put16(out, static_cast<std::uint32_t>(images.size()));
    std::size_t offset = 6 + 16 * images.size();
    for (const auto &image : images) {
      out.push_back(static_cast<char>(image.side >= 256 ? 0 : image.side));
      out.push_back(static_cast<char>(image.side >= 256 ? 0 : image.side));
      out.push_back(0);
      out.push_back(0);
      put16(out, 1);
      put16(out, static_cast<std::uint32_t>(image.bpp));
      put32(out, static_cast<std::uint32_t>(image.data.size()));
      put32(out, static_cast<std::uint32_t>(offset));
      offset += image.data.size();
    }
    for (const auto &image : images) out += image.data;
    return out;
  }

  // A PE executable whose only resources are the group icon (id 1) and its images (ids 1..n).
  inline std::string
  exe_file(const std::vector<IconImage> &images, bool pe32plus = true) {
    const std::uint32_t n = static_cast<std::uint32_t>(images.size());
    // ---- the resource section ----
    std::string rs;
    const std::uint32_t root = 0;
    const std::uint32_t icon_dir = root + 16 + 2 * 8;
    const std::uint32_t group_dir = icon_dir + 16 + 8 * n;
    const std::uint32_t lang_dirs = group_dir + 16 + 8;  // n icon language dirs, then the group's
    const std::uint32_t data_entries = lang_dirs + (n + 1) * 24;
    const std::uint32_t blobs = data_entries + (n + 1) * 16;
    // the group icon resource
    std::string group;
    put16(group, 0);
    put16(group, 1);
    put16(group, n);
    for (std::uint32_t i = 0; i < n; ++i) {
      group.push_back(static_cast<char>(images[i].side >= 256 ? 0 : images[i].side));
      group.push_back(static_cast<char>(images[i].side >= 256 ? 0 : images[i].side));
      group.push_back(0);
      group.push_back(0);
      put16(group, 1);
      put16(group, static_cast<std::uint32_t>(images[i].bpp));
      put32(group, static_cast<std::uint32_t>(images[i].data.size()));
      put16(group, i + 1);
    }
    const auto dir = [&](std::uint32_t entries) {
      put32(rs, 0);
      put32(rs, 0);
      put16(rs, 0);
      put16(rs, 0);
      put16(rs, 0);
      put16(rs, entries);
    };
    dir(2);
    put32(rs, 3);
    put32(rs, 0x80000000u | icon_dir);
    put32(rs, 14);
    put32(rs, 0x80000000u | group_dir);
    dir(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      put32(rs, i + 1);
      put32(rs, 0x80000000u | (lang_dirs + i * 24));
    }
    dir(1);
    put32(rs, 1);
    put32(rs, 0x80000000u | (lang_dirs + n * 24));
    for (std::uint32_t i = 0; i < n + 1; ++i) {
      dir(1);
      put32(rs, 0x409);
      put32(rs, data_entries + i * 16);
    }
    // data entries: RVA (section VA 0x2000), size, code page, reserved
    std::uint32_t at = blobs;
    std::vector<std::string> blob_list;
    for (const auto &image : images) blob_list.push_back(image.data);
    blob_list.push_back(group);
    for (const auto &blob : blob_list) {
      put32(rs, 0x2000 + at);
      put32(rs, static_cast<std::uint32_t>(blob.size()));
      put32(rs, 0);
      put32(rs, 0);
      at += static_cast<std::uint32_t>((blob.size() + 3) & ~3u);
    }
    for (const auto &blob : blob_list) {
      rs += blob;
      pad_to(rs, (rs.size() + 3) & ~static_cast<std::size_t>(3));
    }
    // ---- the file ----
    const std::uint32_t optional_size = pe32plus ? 240 : 224;
    std::string file(0x80, '\0');
    file[0] = 'M';
    file[1] = 'Z';
    file[60] = static_cast<char>(0x80);
    file.append("PE\0\0", 4);
    put16(file, pe32plus ? 0x8664 : 0x14c);
    put16(file, 2);
    put32(file, 0);
    put32(file, 0);
    put32(file, 0);
    put16(file, optional_size);
    put16(file, 0x22);
    // optional header
    put16(file, pe32plus ? 0x20b : 0x10b);
    pad_to(file, 0x80 + 24 + optional_size);
    const std::size_t dirs = 0x80 + 24 + (pe32plus ? 112 : 96);
    const auto set32 = [&](std::size_t where, std::uint32_t value) {
      for (int i = 0; i < 4; ++i) file[where + static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xff);
    };
    set32(dirs - 4, 16);
    set32(dirs + 16, 0x2000);
    set32(dirs + 20, static_cast<std::uint32_t>(rs.size()));
    // section table: .text (code), .rsrc
    const std::uint32_t raw_rsrc = 0x400;
    const auto section = [&](const char *name, std::uint32_t virtual_size, std::uint32_t va, std::uint32_t raw_size, std::uint32_t raw_ptr) {
      std::string head(name);
      head.resize(8, '\0');
      file += head;
      put32(file, virtual_size);
      put32(file, va);
      put32(file, raw_size);
      put32(file, raw_ptr);
      put32(file, 0);
      put32(file, 0);
      put32(file, 0);
      put32(file, 0x40000040);
    };
    section(".text", 0x100, 0x1000, 0x200, 0x200);
    section(".rsrc", static_cast<std::uint32_t>(rs.size()), 0x2000, static_cast<std::uint32_t>((rs.size() + 0x1ff) & ~0x1ffu), raw_rsrc);
    pad_to(file, raw_rsrc);
    file += rs;
    pad_to(file, raw_rsrc + ((rs.size() + 0x1ff) & ~static_cast<std::size_t>(0x1ff)));
    return file;
  }
}  // namespace pefix
