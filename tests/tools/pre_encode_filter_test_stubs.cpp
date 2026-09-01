/**
 * @file tests/tools/pre_encode_filter_test_stubs.cpp
 * @brief Minimal production logging symbols for the standalone pre-encode filter test.
 */
#include "src/logging_severity.h"

boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
boost::log::sources::severity_logger<int> error(4);
