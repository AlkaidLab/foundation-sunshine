/**
 * @file vdd_edid.cpp
 * @brief EDID generation for the Linux virtual display connector.
 *
 * Byte-for-byte port of sunshineVD's src/edid/generator.py (the reference
 * implementation the connector-side EDID override was validated against).
 */

#include "vdd_edid.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace vdd_edid {

  namespace {

    std::uint8_t
    checksum(const std::vector<std::uint8_t> &data, std::size_t end) {
      std::uint32_t sum = 0;
      for (std::size_t i = 0; i < end; ++i) {
        sum += data[i];
      }
      return static_cast<std::uint8_t>((256 - (sum % 256)) % 256);
    }

    void
    put16_le(std::vector<std::uint8_t> &edid, std::size_t offset, std::uint16_t value) {
      edid[offset] = value & 0xFF;
      edid[offset + 1] = (value >> 8) & 0xFF;
    }

  }  // namespace

  namespace {
    // CTA-861-H HDR Static Metadata luminance encoding.
    std::uint8_t
    encode_hdr_luminance_nits(double nits) {
      const double code = nits * 100.0 / 1499.0;
      return static_cast<std::uint8_t>(std::clamp(code, 0.0, 255.0));
    }

    std::uint8_t
    encode_hdr_min_luminance_nits(double nits) {
      // Minimum luminance is quadratic relative to the 1499 nits max range.
      const double rel = nits <= 0 ? 0.0 : std::sqrt(nits / 1499.0);
      const double code = 255.0 * rel;
      return static_cast<std::uint8_t>(std::clamp(code, 0.0, 255.0));
    }

    // Pixel clock (Hz) under the generator's blanking model — the single
    // source of truth for DTD encoding and the feasibility check.
    double
    dtd_pixel_clock_hz(unsigned int width, unsigned int height, unsigned int refresh_hz) {
      const unsigned int h_blank = std::max(80u, static_cast<unsigned int>(width * 0.08));
      const unsigned int h_total = width + h_blank;

      unsigned int v_blank = std::max(23u, static_cast<unsigned int>(height * 0.025));
      auto pixel_clock_hz = static_cast<double>(h_total) * (height + v_blank) * refresh_hz;
      v_blank = std::max(23u, static_cast<unsigned int>(pixel_clock_hz / (h_total * refresh_hz) - height));
      return static_cast<double>(h_total) * (height + v_blank) * refresh_hz;
    }

    std::vector<std::uint8_t>
    build_dtd(unsigned int width, unsigned int height, unsigned int refresh_hz, unsigned int h_size_mm, unsigned int v_size_mm) {
      const unsigned int h_active = width;
      const unsigned int v_active = height;
      const unsigned int h_blank = std::max(80u, static_cast<unsigned int>(width * 0.08));
      const unsigned int v_blank = std::max(23u, static_cast<unsigned int>(height * 0.025));

      const auto pixel_clock_hz = dtd_pixel_clock_hz(width, height, refresh_hz);
      const auto pixel_clock = static_cast<std::uint16_t>(std::min<double>(pixel_clock_hz / 10000, 65535));

      const unsigned int h_sync_offset = static_cast<unsigned int>(h_blank * 0.2);
      const unsigned int h_sync_width = static_cast<unsigned int>(h_blank * 0.4);
      const unsigned int v_sync_offset = 2;
      const unsigned int v_sync_width = 6;

      std::vector<std::uint8_t> dtd(18, 0);
      dtd[0] = pixel_clock & 0xFF;
      dtd[1] = (pixel_clock >> 8) & 0xFF;
      dtd[2] = h_active & 0xFF;
      dtd[3] = h_blank & 0xFF;
      dtd[4] = static_cast<std::uint8_t>(((h_active >> 8) << 4) | (h_blank >> 8));
      dtd[5] = v_active & 0xFF;
      dtd[6] = v_blank & 0xFF;
      dtd[7] = static_cast<std::uint8_t>(((v_active >> 8) << 4) | (v_blank >> 8));
      dtd[8] = h_sync_offset & 0xFF;
      dtd[9] = h_sync_width & 0xFF;
      dtd[10] = static_cast<std::uint8_t>(((v_sync_offset & 0x0F) << 4) | (v_sync_width & 0x0F));
      dtd[11] = static_cast<std::uint8_t>(
        (((h_sync_offset >> 8) & 0x03) << 6) | (((h_sync_width >> 8) & 0x03) << 4) | (((v_sync_offset >> 4) & 0x03) << 2) | ((v_sync_width >> 4) & 0x03));
      dtd[12] = h_size_mm & 0xFF;
      dtd[13] = v_size_mm & 0xFF;
      dtd[14] = static_cast<std::uint8_t>(((h_size_mm >> 8) << 4) | (v_size_mm >> 8));
      dtd[17] = 0x18;  // Non-interlaced, digital separate sync
      return dtd;
    }
  }  // namespace

  bool
  mode_fits_pixel_clock_limit(unsigned int width, unsigned int height, unsigned int refresh_hz) {
    return dtd_pixel_clock_hz(width, height, refresh_hz) <= 655350000.0;
  }

  std::vector<std::uint8_t>
  generate_virtual_display_edid(unsigned int width, unsigned int height, unsigned int refresh_hz, const edid_options &opts) {
    const bool enable_hdr = opts.enable_hdr;
    const std::string &display_name = opts.display_name;
    std::vector<std::uint8_t> edid(256, 0);

    // Base block header
    const std::uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    std::copy(std::begin(header), std::end(header), std::begin(edid));

    // Manufacturer ID — "VHD" for Virtual HDR Display
    edid[8] = 0x56;
    edid[9] = 0x24;

    // Product code
    put16_le(edid, 10, enable_hdr ? 0x4844 : 0x5344);

    // Serial number (unique per resolution/refresh)
    const std::uint32_t serial = (width << 16) | (height << 4) | (refresh_hz & 0x0F);
    edid[12] = serial & 0xFF;
    edid[13] = (serial >> 8) & 0xFF;
    edid[14] = (serial >> 16) & 0xFF;
    edid[15] = (serial >> 24) & 0xFF;

    edid[16] = 1;   // Week of manufacture
    edid[17] = 33;  // Year 2023
    edid[18] = 1;   // EDID version 1,
    edid[19] = 4;   // revision 4

    // Video input definition (digital, DisplayPort)
    edid[20] = enable_hdr ? 0xB5 : 0xA5;

    // Screen size in cm: caller-provided physical size, else 96-DPI derived
    unsigned int h_size_cm;
    unsigned int v_size_cm;
    if (opts.width_mm && opts.height_mm) {
      h_size_cm = opts.width_mm / 10;
      v_size_cm = opts.height_mm / 10;
    } else {
      const double diagonal_inches = std::sqrt(static_cast<double>(width) * width + static_cast<double>(height) * height) / 96;
      const double aspect_ratio = static_cast<double>(width) / height;
      h_size_cm = static_cast<unsigned int>((diagonal_inches * 2.54) / std::sqrt(1 + (1 / aspect_ratio) * (1 / aspect_ratio)));
      v_size_cm = static_cast<unsigned int>(h_size_cm / aspect_ratio);
    }
    edid[21] = static_cast<std::uint8_t>(std::min<unsigned int>(h_size_cm, 255));
    edid[22] = static_cast<std::uint8_t>(std::min<unsigned int>(v_size_cm, 255));

    edid[23] = 220;  // Gamma 2.2

    // Feature support: RGB 4:4:4, preferred timing, continuous frequency
    edid[24] = enable_hdr ? 0x1A : 0x1E;

    // Color characteristics
    const std::uint8_t chromaticity[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
    std::copy(std::begin(chromaticity), std::end(chromaticity), edid.begin() + 25);

    // Established timings: none
    edid[35] = edid[36] = edid[37] = 0x00;

    // Standard timings: all unused
    for (std::size_t i = 38; i < 54; i += 2) {
      edid[i] = 0x01;
      edid[i + 1] = 0x01;
    }

    // Detailed timing descriptor 1 — the requested mode
    const unsigned int h_active = width;
    const unsigned int v_active = height;
    const unsigned int h_blank = std::max(80u, static_cast<unsigned int>(width * 0.08));
    const unsigned int v_blank = std::max(23u, static_cast<unsigned int>(height * 0.025));

    const auto pixel_clock = static_cast<std::uint16_t>(std::min<double>(dtd_pixel_clock_hz(width, height, refresh_hz) / 10000, 65535));
    put16_le(edid, 54, pixel_clock);
    edid[56] = h_active & 0xFF;
    edid[57] = h_blank & 0xFF;
    edid[58] = static_cast<std::uint8_t>(((h_active >> 8) << 4) | (h_blank >> 8));

    edid[59] = v_active & 0xFF;
    edid[60] = v_blank & 0xFF;
    edid[61] = static_cast<std::uint8_t>(((v_active >> 8) << 4) | (v_blank >> 8));

    const unsigned int h_sync_offset = static_cast<unsigned int>(h_blank * 0.2);
    const unsigned int h_sync_width = static_cast<unsigned int>(h_blank * 0.4);
    const unsigned int v_sync_offset = 2;
    const unsigned int v_sync_width = 6;

    edid[62] = h_sync_offset & 0xFF;
    edid[63] = h_sync_width & 0xFF;
    edid[64] = static_cast<std::uint8_t>(((v_sync_offset & 0x0F) << 4) | (v_sync_width & 0x0F));
    edid[65] = static_cast<std::uint8_t>(
      (((h_sync_offset >> 8) & 0x03) << 6) | (((h_sync_width >> 8) & 0x03) << 4) | (((v_sync_offset >> 4) & 0x03) << 2) | ((v_sync_width >> 4) & 0x03));

    const unsigned int h_size_mm = h_size_cm * 10;
    const unsigned int v_size_mm = v_size_cm * 10;
    edid[66] = h_size_mm & 0xFF;
    edid[67] = v_size_mm & 0xFF;
    edid[68] = static_cast<std::uint8_t>(((h_size_mm >> 8) << 4) | (v_size_mm >> 8));

    edid[69] = 0;     // H border
    edid[70] = 0;     // V border
    edid[71] = 0x18;  // Non-interlaced, digital separate sync

    // Display product name descriptor (space-padded, reference semantics)
    std::string name = display_name.substr(0, 13);
    name.resize(13, ' ');
    std::copy(name.begin(), name.end(), edid.begin() + 77);
    const std::uint8_t name_descriptor[5] = { 0x00, 0x00, 0x00, 0xFC, 0x00 };
    std::copy(std::begin(name_descriptor), std::end(name_descriptor), edid.begin() + 72);

    // Display range limits
    const auto min_v_rate = static_cast<std::uint8_t>(std::max(24u, refresh_hz - 20));
    const auto max_v_rate = static_cast<std::uint8_t>(refresh_hz + 20);
    const std::uint8_t range_limits[18] = {
      0x00, 0x00, 0x00, 0xFD, 0x00,
      min_v_rate, max_v_rate,
      30, 160,  // H rate kHz
      220,      // Max pixel clock (x10 MHz)
      0x00, 0x0A,
      0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    };
    std::copy(std::begin(range_limits), std::end(range_limits), edid.begin() + 90);

    // Secondary detailed timings: the dummy descriptor slot takes the first
    // extra mode; the remaining ones fill the CEA DTD area after the
    // preferred-mode copy.
    auto extras = opts.extra_modes;
    extras.erase(std::remove_if(extras.begin(), extras.end(), [&](const auto &m) {
      return std::get<0>(m) == width && std::get<1>(m) == height && std::get<2>(m) == refresh_hz;
    }), extras.end());

    std::size_t extra_index = 0;
    if (!extras.empty()) {
      const auto dtd = build_dtd(std::get<0>(extras[extra_index]), std::get<1>(extras[extra_index]), std::get<2>(extras[extra_index]), h_size_mm, v_size_mm);
      std::copy(dtd.begin(), dtd.end(), edid.begin() + 108);
      ++extra_index;
    } else {
      const std::uint8_t dummy[5] = { 0x00, 0x00, 0x00, 0x10, 0x00 };
      std::copy(std::begin(dummy), std::end(dummy), edid.begin() + 108);
    }

    edid[126] = 1;  // One extension block
    edid[127] = checksum(edid, 127);

    // CEA-861 extension block
    const std::size_t cea = 128;
    edid[cea] = 0x02;      // CEA tag
    edid[cea + 1] = 0x03;  // Revision 3

    std::size_t offset = cea + 4;

    if (enable_hdr) {
      // Colorimetry Data Block: BT2020RGB, BT2020YCC, BT2020cYCC
      edid[offset] = 0xE3;
      edid[offset + 1] = 0x05;
      edid[offset + 2] = 0xE0;
      edid[offset + 3] = 0x00;
      offset += 4;

      // HDR Static Metadata Data Block: SDR+HDR+PQ, type 1. Luminance bytes
      // follow the CTA-861-H encoding; personalized when the client reports
      // its capabilities, otherwise the reference defaults.
      edid[offset] = 0xE6;
      edid[offset + 1] = 0x06;
      edid[offset + 2] = 0x07;
      edid[offset + 3] = 0x01;
      edid[offset + 4] = opts.hdr_max_nits >= 0 ? encode_hdr_luminance_nits(opts.hdr_max_nits) : 0x78;
      edid[offset + 5] = opts.hdr_max_full_nits >= 0 ? encode_hdr_luminance_nits(opts.hdr_max_full_nits) : 0x5A;
      edid[offset + 6] = opts.hdr_min_nits >= 0 ? encode_hdr_min_luminance_nits(opts.hdr_min_nits) : 0x32;
      offset += 7;
    }

    // Video Capability Data Block
    edid[offset] = 0xE2;
    edid[offset + 1] = 0x00;
    edid[offset + 2] = 0x00;
    offset += 3;

    // HDMI Forum Vendor Specific Data Block
    const std::uint8_t hdmi_forum[8] = { 0x67, 0xD8, 0x5D, 0xC4, 0x01, 0x78, 0x00, 0x00 };
    std::copy(std::begin(hdmi_forum), std::end(hdmi_forum), edid.begin() + offset);
    offset += 8;

    edid[cea + 2] = static_cast<std::uint8_t>(offset - cea);  // DTD start offset
    edid[cea + 3] = 0x70;                                     // Underscan, Basic Audio, YCbCr 4:4:4

    // Duplicate the base DTD, then append the remaining extra modes
    if (offset + 18 <= 255) {
      std::copy_n(edid.begin() + 54, 18, edid.begin() + offset);
      offset += 18;
    }
    if (extra_index > 0) {
      --extra_index;  // the first extra went into the base block
    }
    while (extra_index < extras.size() && offset + 18 <= 255) {
      const auto &m = extras[extra_index++];
      const auto dtd = build_dtd(std::get<0>(m), std::get<1>(m), std::get<2>(m), h_size_mm, v_size_mm);
      std::copy(dtd.begin(), dtd.end(), edid.begin() + offset);
      offset += 18;
    }

    edid[255] = checksum(edid, 255);

    return edid;
  }

}  // namespace vdd_edid
