/**
 * @file tests/tools/audio_devices_dump.cpp
 * @brief Prints what GET /api/audio/devices would answer on this machine. Reads endpoints and the device tree only; changes nothing.
 *   audio_devices_dump                 the JSON of the endpoint list and the VB-CABLE state
 *   audio_devices_dump --verify <exe>  the Authenticode result and signer of a file, and whether the host would accept it
 *   audio_devices_dump --instance      whether this process is elevated and the instance id of a disabled VB-CABLE device
 */
#include <iostream>
#include <string>

#include "src/audio_devices.h"
#include "src/audio_vb_cable.h"

int
main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--verify" && argc > 2) {
    auto environment = audio_devices::vb_cable::make_system_environment();
    const auto signature = environment->verify_signature(argv[2]);
    std::cout << "valid=" << signature.valid << "\nsigner=" << signature.signer << "\naccepted=" << audio_devices::vb_cable::signer_accepted(signature.signer) << std::endl;
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--instance") {
    auto environment = audio_devices::vb_cable::make_system_environment();
    const auto id = environment->disabled_instance_id();
    std::cout << "elevated=" << environment->elevated() << "\nstate=" << environment->state() << "\ndisabled_instance_id=" << (id ? *id : "") << "\nvalid_id=" << (id && audio_devices::vb_cable::valid_instance_id(*id)) << std::endl;
    return 0;
  }
  std::vector<audio_devices::endpoint_t> endpoints;
  if (!audio_devices::enumerate_endpoints(endpoints)) {
    std::cerr << "cannot create the MMDevice enumerator\n";
    return 1;
  }
  std::vector<audio_devices::driver_t> drivers;
  const bool drivers_read = audio_devices::enumerate_vb_cable_drivers(drivers);
  auto json = audio_devices::to_json(endpoints, drivers_read ? &drivers : nullptr);
  json["vb_cable"]["job"] = audio_devices::vb_cable::status_json(audio_devices::vb_cable::runner().status());
  std::cout << json.dump(2) << std::endl;
  return 0;
}
