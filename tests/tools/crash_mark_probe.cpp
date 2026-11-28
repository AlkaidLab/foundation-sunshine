/**
 * @file tests/tools/crash_mark_probe.cpp
 * @brief A small program that installs the crash handlers like the host does, starts the crash-loop guard, and fails on purpose (SUNSHINE_TEST_CRASH), so the driver mark and the
 *        safe mode can be checked end to end on any platform without the host (tests/tools/gpu_driver_guard_e2e.py).
 *
 *   crash_mark_probe <config dir> [phase] [driver version]
 *
 * Prints `safe=0|1` (the guard's decision for this start) and, when SUNSHINE_TEST_CRASH is not set, marks a clean exit.
 */
#include "src/crash/crash.h"
#include "src/gpu_driver_guard.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

int
main(int argc, char **argv) {
  if (argc < 2) return 2;
  const std::filesystem::path dir { argv[1] };
  const std::string_view phase = argc > 2 ? argv[2] : "startup";
  const std::string_view version = argc > 3 ? argv[3] : "32.0.101.6325";

  crash::options_t options;
  options.product = "host";
  options.version = "probe";
  options.config_dir = dir;
  crash::install(options);
  for (const auto vendor : { crash::gpu_driver::vendor_e::intel, crash::gpu_driver::vendor_e::nvidia, crash::gpu_driver::vendor_e::amd }) {
    crash::gpu_driver::set_driver_version(vendor, version);
  }

  gpu_guard::guard_t guard { dir };
  const bool safe = guard.begin();
  std::printf("safe=%d\n", safe ? 1 : 0);
  std::fflush(stdout);
  crash::gpu_driver::set_phase(crash::gpu_driver::phase_from_word(phase));

  crash::run_test_switch();  // returns only when SUNSHINE_TEST_CRASH is not set
  guard.note_clean_exit();
  crash::stop_watchdog();
  return 0;
}
