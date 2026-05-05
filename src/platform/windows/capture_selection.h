#pragma once

#include <string>
#include <vector>

namespace platf::dxgi {

  std::vector<std::string>
  windows_capture_try_order(const std::string &configured_capture,
                            bool running_as_system_user,
                            bool prefer_cursor_plane);

}  // namespace platf::dxgi
