#pragma once

#include <cstdlib>
#include <string>

#if defined(SUNSHINE_HAVE_SDBUS)
  #include <systemd/sd-bus.h>
#else
  // Without libsystemd the header is not installed at all, so the type is only
  // forward declared: callers that merely carry a bus pointer still compile,
  // and the ones that would talk to it are compiled out.
  struct sd_bus;
#endif

namespace platf::sdbus {

#if defined(SUNSHINE_HAVE_SDBUS)

  /**
   * @brief Open a connection to the user session bus.
   * @details The address is built explicitly: Sunshine runs with file
   *          capabilities, which sets AT_SECURE and makes sd-bus's
   *          secure_getenv() hide XDG_RUNTIME_DIR from the process, so
   *          sd_bus_open_user() would always fail here.
   */
  inline sd_bus *
  open_user_bus() {
    const char *runtime_dir = ::getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir) {
      return nullptr;
    }

    sd_bus *bus = nullptr;
    if (sd_bus_new(&bus) < 0) {
      return nullptr;
    }

    const std::string address = std::string { "unix:path=" } + runtime_dir + "/bus";
    // Mark the connection as a bus client so sd-bus performs the full
    // handshake (AUTH + Hello) the way sd_bus_open_user() would. Without this
    // flag the socket connects but never acquires a unique name, and
    // dbus-broker resets the connection when the first method call arrives
    // (surfaced as ECONNRESET from every klipper call).
    if (sd_bus_set_bus_client(bus, 1) < 0 ||
        sd_bus_set_address(bus, address.c_str()) < 0 || sd_bus_start(bus) < 0) {
      sd_bus_unref(bus);
      return nullptr;
    }
    return bus;
  }

#else

  /**
   * @brief Open a connection to the user session bus.
   * @details Compiled without libsystemd there is no sd-bus to open a
   *          connection with, so this reports "no bus". Every caller already
   *          treats a null bus as "this producer is unavailable" and falls back
   *          to another one, which is the right behavior on a system without
   *          session-bus headers.
   */
  inline sd_bus *
  open_user_bus() {
    return nullptr;
  }

#endif

}  // namespace platf::sdbus
