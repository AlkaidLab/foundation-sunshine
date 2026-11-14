/**
 * @file tests/tools/ds5_matrix/ds5_tuning_matrix_sim.cpp
 * @brief DualSense tuning behaviour matrix on the host-portable rig (any OS).
 */
#include <filesystem>

#include "tests/tools/ds5_matrix/matrix_run.h"
#include "tests/tools/ds5_matrix/sim_rig.h"

int
main(int argc, char **argv) {
  const auto profile = std::filesystem::temp_directory_path() / ("ds5_matrix_sim_" + std::to_string(std::rand()) + ".json");
  ds5::matrix::sim_rig_t rig(profile);
  const auto failures = ds5::matrix::run_matrix(rig, ds5::matrix::parse_keys(argc, argv));
  std::filesystem::remove(profile);
  return failures == 0 ? 0 : 3;
}
