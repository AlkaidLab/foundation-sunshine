/**
 * @file tests/tools/audio_devices_dump.cpp
 * @brief Prints what GET /api/audio/devices would answer on this machine. Reads endpoints and the device tree only; changes nothing.
 *   audio_devices_dump                 the JSON of the endpoint list and the VB-CABLE state
 */
#include <iostream>

#include "src/audio_devices.h"

int
main() {
  std::vector<audio_devices::endpoint_t> endpoints;
  if (!audio_devices::enumerate_endpoints(endpoints)) {
    std::cerr << "cannot create the MMDevice enumerator\n";
    return 1;
  }
  std::vector<audio_devices::driver_t> drivers;
  const bool drivers_read = audio_devices::enumerate_vb_cable_drivers(drivers);
  auto json = audio_devices::to_json(endpoints, drivers_read ? &drivers : nullptr);
  std::cout << json.dump(2) << std::endl;
  return 0;
}
