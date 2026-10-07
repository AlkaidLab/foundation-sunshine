/**
 * @file tests/tools/pyrowave_test_stubs.cpp
 * @brief Minimal production logging symbols for the standalone PyroWave tests.
 */
#include "src/logging_severity.h"

boost::log::sources::severity_logger<int> verbose(0);
boost::log::sources::severity_logger<int> warning(3);
