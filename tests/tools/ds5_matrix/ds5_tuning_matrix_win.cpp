/**
 * @file tests/tools/ds5_matrix/ds5_tuning_matrix_win.cpp
 * @brief DualSense tuning behaviour matrix on the real native virtual pad (Windows).
 *
 * Usage: ds5_tuning_matrix_win [--key N]... [--styles]
 * Attaches one virtual DualSense for the run; only run it while no streaming session is active.
 */
#ifdef _WIN32

  #include <boost/log/sources/severity_logger.hpp>

  #include "tests/tools/ds5_matrix/matrix_run.h"
  #include "tests/tools/ds5_matrix/win_rig.h"

boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> debug;

int
main(int argc, char **argv) {
  const auto profile = std::filesystem::temp_directory_path() / ("ds5_matrix_win_" + std::to_string(GetCurrentProcessId()) + ".json");
  ds5::matrix::win_rig_t rig(profile);
  if (!rig.attached()) {
    std::fputs("FAIL native DS5 allocation or usbip-win2 attach\n", stderr);
    return 2;
  }
  const auto failures = ds5::matrix::run_matrix(rig, ds5::matrix::parse_keys(argc, argv));
  std::printf("DEVICE write_failures=%u render_failures=%u\n", rig.write_failures(), rig.render_failures());
  std::filesystem::remove(profile);
  return failures == 0 ? 0 : 3;
}

#endif
