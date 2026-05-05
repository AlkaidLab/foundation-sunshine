#include "capture_selection.h"

namespace platf::dxgi {

  std::vector<std::string>
  windows_capture_try_order(const std::string &configured_capture,
                            bool running_as_system_user,
                            bool prefer_cursor_plane) {
    if (configured_capture.empty()) {
      if (running_as_system_user) {
        return { "ddx" };
      }

      if (prefer_cursor_plane) {
        return { "wgc", "ddx" };
      }

      return { "ddx", "wgc" };
    }

    if (configured_capture == "wgc" && running_as_system_user) {
      return { "ddx" };
    }

    return { configured_capture };
  }

}  // namespace platf::dxgi
