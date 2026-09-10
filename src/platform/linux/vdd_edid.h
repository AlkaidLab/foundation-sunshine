/**
 * @file vdd_edid.h
 * @brief EDID generation for the Linux virtual display connector.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vdd_edid {

  /**
   * @brief Optional personalization for the generated EDID.
   */
  struct edid_options {
    bool enable_hdr { true };
    std::string display_name { "Foundation VDD" };

    // Physical size override in millimetres for the screen size descriptors;
    // 0 = derive from the resolution assuming a 96 dpi panel.
    unsigned int width_mm { 0 };
    unsigned int height_mm { 0 };

    // HDR static metadata luminance override in cd/m2 (CTA-861-H encoding);
    // negative = emit the reference defaults (1000 / 400 / 0.05 nits class).
    int hdr_max_nits { -1 };
    int hdr_max_full_nits { -1 };
    int hdr_min_nits { -1 };

    // Additional modes advertised besides the preferred timing. These let
    // the compositor switch modes without an EDID rewrite. Must fit the
    // CEA-861 pixel-clock limit (655.35 MHz).
    std::vector<std::tuple<unsigned int, unsigned int, unsigned int>> extra_modes;
  };

  /**
   * @brief Build a 256-byte EDID (base block + CEA-861 extension) describing
   *        a virtual display whose preferred mode is exactly the given one.
   */
  std::vector<std::uint8_t>
  generate_virtual_display_edid(unsigned int width, unsigned int height, unsigned int refresh_hz, const edid_options &opts = {});

  /**
   * @brief Convenience overload matching the historical call signature.
   */
  inline std::vector<std::uint8_t>
  generate_virtual_display_edid(unsigned int width, unsigned int height, unsigned int refresh_hz, bool enable_hdr, const std::string &display_name) {
    edid_options opts;
    opts.enable_hdr = enable_hdr;
    opts.display_name = display_name;
    return generate_virtual_display_edid(width, height, refresh_hz, opts);
  }

}  // namespace vdd_edid
