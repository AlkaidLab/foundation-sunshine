#pragma once
#include "src/sparxie/art.h"
#include <string>
#include <string_view>

namespace sparxie {
  // The fill of one SVG path as a picture: `d` is the path data of a <path fill-rule="evenodd"> in a viewBox of view_w x view_h, drawn white on transparent
  // at out_w pixels wide. Supported commands: M m L l H h V v C c S s Q q A a Z z (what the official sites' inline logos use); anything else, or a malformed number,
  // gives an empty picture. Even-odd fill, 4x4 supersampled edges.
  Rgba
  rasterize_svg_path(std::string_view d, double view_w, double view_h, int out_w);

  // The path data of the first <path id="<id>" ...> of an HTML page (the inline SVG definition a site draws its logo from); empty when there is none.
  std::string
  svg_path_of_page(const std::string &page, std::string_view id);
}  // namespace sparxie
