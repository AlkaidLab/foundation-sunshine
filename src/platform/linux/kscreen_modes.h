/**
 * @file src/platform/linux/kscreen_modes.h
 * @brief Advertised mode list of a single output, as the compositor reports it.
 *
 * The Wayland analogue of the Windows mode list that EnumDisplaySettings()
 * returns: on KDE the compositor is the display server, so its view is what
 * decides whether a requested mode can actually be applied. Used by the VDD
 * backend to verify that a freshly written EDID really published the mode a
 * client asked for, instead of assuming the generator was right.
 *
 * Non-KDE sessions (niri, wlroots, X11) have no kscreen-doctor: the list comes
 * back empty, which callers must read as "unknown", never as "not advertised".
 */
#pragma once

#include <string>
#include <vector>

namespace platf::kscreen {
  struct advertised_mode_t {
    unsigned int width { 0 };
    unsigned int height { 0 };
    unsigned int refresh_hz { 0 };
  };

  /**
   * @brief Every mode the compositor advertises for @p output_name.
   * @return The mode list, or an empty vector when kscreen-doctor cannot be
   *         queried or does not know the output.
   */
  std::vector<advertised_mode_t>
  advertised_modes(const std::string &output_name);
}  // namespace platf::kscreen
