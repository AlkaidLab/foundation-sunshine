/**
 * @file tests/usbip/external_readonly_probe.cpp
 * @brief Opt-in, read-only probe of the host's actual USB/IP ownership collector.
 */
#include "src/doctor/doctor_os.h"

#include <cstdio>

int
main() {
  const auto facts = doctor::os::usbip();
  std::printf("supported=%d tool=%d driver=%d external=%d\n", facts.supported, facts.tool_found, facts.driver_found, facts.externally_managed);
  if (!facts.supported || !facts.tool_found) return 77;
  return facts.driver_found && facts.externally_managed ? 0 : 1;
}
