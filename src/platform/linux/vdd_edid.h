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
   * @brief Build a 256-byte EDID (base block + CEA-861 extension) describing
   *        a virtual display whose preferred mode is exactly the given one.
   * @param enable_hdr Adds BT.2020 colorimetry and an HDR static metadata
   *        block (SDR+HDR+PQ, 1000-nit class) to the extension.
   */
  std::vector<std::uint8_t>
  generate_virtual_display_edid(unsigned int width, unsigned int height, unsigned int refresh_hz, bool enable_hdr, const std::string &display_name);

}  // namespace vdd_edid
