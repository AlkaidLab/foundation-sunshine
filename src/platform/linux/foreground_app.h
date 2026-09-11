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
   * @brief Parse the JSON that `niri msg --json focused-window` prints.
   * @details Maps niri's window fields onto info_t: `title` -> window_title,
   *          `app_id` -> exe_name, `pid` -> pid. niri prints `null` when no
   *          window is focused, which yields false. The parser is deliberately
   *          tolerant: a missing or mistyped field is ignored rather than
   *          treated as an error, so a niri release that renames a field only
   *          loses that field instead of the whole report.
   * @returns true when a focused window with at least a title or app id was
   *          parsed.
   */
  bool
  parse_niri_focused_window(const std::string &json_text, info_t &out);

  /**
   * @brief Latest known foreground window info, empty when unknown.
   * @details On KDE Plasma a KWin script pushes active-window changes to
   *          Sunshine over D-Bus and this returns the cached report; on niri
   *          the focused window is polled through its IPC CLI. On other
   *          desktops it stays empty, mirroring the Windows backend failing
   *          its query. The first call lazily starts the watcher.
   */
  info_t
  detect();

}  // namespace platf::foreground_app
