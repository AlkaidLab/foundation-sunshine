/**
 * @file tests/tools/ds5_sidecar_test_stubs.cpp
 * @brief Minimal production-symbol stubs for the standalone DS5 client test.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "src/config.h"
#include "src/logging.h"

boost::log::sources::severity_logger<int> verbose;
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

namespace config {
  input_t input {};
}

namespace platf {
  std::wstring from_utf8(const std::string &input) {
    if (input.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                        static_cast<int>(input.size()), output.data(), size);
    return output;
  }
}
