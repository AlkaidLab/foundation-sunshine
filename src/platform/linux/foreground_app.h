#pragma once

#include <cstdint>
#include <string>

namespace platf::foreground_app {

  struct info_t {
    std::string window_title;
    std::string exe_name;
    std::uint32_t pid = 0;
  };

  /**
   * @brief Latest known foreground window info, empty when unknown.
   * @details On KDE Plasma a KWin script pushes active-window changes to
   *          Sunshine over D-Bus and this returns the cached report. On
   *          other desktops it stays empty, mirroring the Windows backend
   *          failing its query. The first call lazily starts the watcher.
   */
  info_t
  detect();

}  // namespace platf::foreground_app
