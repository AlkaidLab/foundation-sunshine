/**
 * @file src/platform/linux/compositor_output.cpp
 * @brief Compositor-side output enablement, Linux side.
 */
#include "compositor_output.h"

#include <cstdlib>
#include <unistd.h>

namespace platf::compositor_output {
  bool
  niri_session() {
    const char *socket = ::getenv("NIRI_SOCKET");
    return socket && *socket;
  }

  bool
  tool_available(const char *name) {
    for (const char *dir : { "/usr/bin", "/usr/local/bin", "/bin" }) {
      if (::access((std::string { dir } + "/" + name).c_str(), X_OK) == 0) {
        return true;
      }
    }
    return false;
  }

  std::string
  enable_command(const std::string &desktop,
                 const std::string &connector,
                 bool niri,
                 bool wlr_randr,
                 bool x11_display,
                 bool xrandr) {
    if (desktop.find("KDE") != std::string::npos || desktop.find("plasma") != std::string::npos) {
      return "kscreen-doctor output." + connector + ".enable";
    }

    if (niri) {
      return "niri msg output " + connector + " on";
    }

    if (wlr_randr) {
      return "wlr-randr --output " + connector + " --on";
    }

    if (x11_display && xrandr) {
      return "xrandr --output " + connector + " --auto";
    }

    return {};
  }
}  // namespace platf::compositor_output
