/**
 * @file src/doctor/doctor_checks.h
 * @brief The health check groups of the running host.
 */
#pragma once

#include <vector>

#include "doctor.h"

namespace doctor {
  /** One check per group, collecting from the running host. */
  std::vector<check_t>
  host_checks();
}  // namespace doctor
