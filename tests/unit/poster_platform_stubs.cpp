/**
 * @file poster_platform_stubs.cpp
 * @brief Host entry points required by the standalone poster test target.
 */
#include "test_temp_paths.h"

#include <filesystem>
#include <string>

#include "src/file_handler.h"
#include "src/logging_severity.h"
#include "src/platform/common.h"

// Definitions normally supplied by logging.cpp.
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

namespace platf {
  std::filesystem::path
  appdata() {
    // The cache singleton uses one temporary directory for this test process.
    static const test_support::ScopedTempDir directory { "sunshine-poster-tests" };
    return directory.path();
  }
}  // namespace platf

namespace file_handler {
  std::string
  path_to_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return { text.begin(), text.end() };
  }
}  // namespace file_handler
