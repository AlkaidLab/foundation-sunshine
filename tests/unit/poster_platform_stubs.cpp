/**
 * @file poster_platform_stubs.cpp
 * @brief Stand-ins for the host entry points that src/nvhttp/poster.cpp reaches.
 *
 * The poster suite compiles poster.cpp on its own: the severity loggers, the appdata directory and the UTF-8 text of a
 * path are all it needs from the host build.
 */
#include "test_temp_paths.h"

#include <filesystem>
#include <string>

#include "src/file_handler.h"
#include "src/logging_severity.h"
#include "src/platform/common.h"

// poster.cpp logs through these severity loggers; the host build supplies them from logging.cpp.
boost::log::sources::severity_logger<int> debug;
boost::log::sources::severity_logger<int> info;
boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;
boost::log::sources::severity_logger<int> fatal;

namespace platf {
  std::filesystem::path
  appdata() {
    // One directory per process: the poster cache is a process-wide singleton and ctest -j runs the suites together.
    static const test_support::ScopedTempDir directory { "sunshine-poster-tests" };
    return directory.path();
  }
}  // namespace platf

namespace file_handler {
  std::string
  path_to_utf8(const std::filesystem::path &path) {
    // The UTF-8 text of the path, which is what the host conversion gives, without the host platform layer.
    const auto text = path.u8string();
    return { text.begin(), text.end() };
  }
}  // namespace file_handler
