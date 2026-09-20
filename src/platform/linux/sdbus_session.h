#pragma once

#include <cstdlib>
#include <string>

#include <systemd/sd-bus.h>

namespace platf::sdbus {

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

}  // namespace platf::sdbus
