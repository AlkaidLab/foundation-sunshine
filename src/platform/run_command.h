/**
 * @file src/platform/run_command.h
 * @brief Declaration for platf::run_command().
 *
 * Kept out of `platform/common.h` to avoid Windows.h / WinSock include-order issues
 * and Boost.Process v1 inline-namespace forward-declaration conflicts.
 */
#pragma once

#include <system_error>

#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>

namespace platf {
  boost::process::v1::child
  run_command(bool elevated,
              bool interactive,
              const std::string &cmd,
              boost::filesystem::path &working_dir,
              const boost::process::v1::environment &env,
              FILE *file,
              std::error_code &ec,
              boost::process::v1::group *group);
}  // namespace platf

