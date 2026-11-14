#pragma once
#include "src/sparxie/art.h"
#include <functional>
#include <string>
#include <string_view>

namespace sparxie {
  // The decoder the host hands the art code (WIC on Windows, stb for PNG and JPEG elsewhere): bytes of a picture file in, RGBA out (w == 0 when it is not one).
  using ImageDecodeFn = std::function<Rgba(std::string_view)>;

  // Smallest icon (in pixels, on its shorter side) a tile may show: a source below it is not an icon for our purposes (Steam's 32x32 community icon).
  inline constexpr int kMinIconPixels = 64;
  // The size a published icon is stored at (square, transparent where the source is).
  inline constexpr int kIconPixels = 256;
  // Bumped when the icon choice or its processing changes: the icons made by an older revision are made again.
  inline constexpr int kIconRevision = 1;

  // The best picture of an .ico file (the largest entry, then the deepest colour): a PNG-compressed entry is handed to `decode`, a BMP (DIB) entry is decoded here
  // (32 bit with alpha, 24, 8, 4 and 1 bit with the AND mask). An entry that does not decode is skipped for the next best. Empty when the file holds no icon.
  Rgba
  icon_from_ico(std::string_view bytes, const ImageDecodeFn &decode);

  // The program icon of a PE executable (exe/dll): the first RT_GROUP_ICON resource, its best RT_ICON image (prefers a 256 px PNG-compressed entry). Only the
  // headers and the resource section are read, so a multi-hundred-MB game executable costs a few KB of IO. Empty when the file is not a PE, has no icon, or is cut
  // short; every offset is bounds-checked.
  Rgba
  icon_from_pe(const std::filesystem::path &exe, const ImageDecodeFn &decode);
  // The same over an executable held in memory (tests, a download).
  Rgba
  icon_from_pe_bytes(std::string_view bytes, const ImageDecodeFn &decode);

  // A downloaded icon file: an .ico goes through icon_from_ico, anything else through `decode` (png, jpg, webp).
  Rgba
  icon_from_file_bytes(std::string_view bytes, const ImageDecodeFn &decode);

  // `image` fitted into a size x size square (aspect kept, centred, transparent padding), scaled with an area average when shrinking and bilinear when enlarging.
  // An empty image stays empty.
  Rgba
  icon_square(const Rgba &image, int size = kIconPixels);

  // True when the icon is acceptable as a tile icon: at least kMinIconPixels on its shorter side, and something visible in it. Judge the picture as the source
  // carries it (before icon_square: an enlarged 32 px icon is still a 32 px icon).
  bool
  icon_is_usable(const Rgba &icon);
}  // namespace sparxie
