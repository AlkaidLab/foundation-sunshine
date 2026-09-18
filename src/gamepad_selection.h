#pragma once

#include <string_view>

namespace input {
  // Evaluate an immutable session preference for each controller allocation.
  // 1=auto, 2=Xbox 360, 3=DS4, 4=DS5. Explicit host modes beat auto hints.
  inline int
  select_gamepad_mode(std::string_view client, bool allow_override, int host_mode, bool prefers_ds5) {
    auto mode = host_mode;
    if (allow_override && !client.empty()) {
      if (client == "x360")
        mode = 2;
      else if (client == "ds4")
        mode = 3;
      else if (client == "ds5")
        mode = 4;
      else if (client == "auto")
        mode = 1;
    }
    return mode == 1 && allow_override && prefers_ds5 ? 4 : mode;
  }
}  // namespace input
