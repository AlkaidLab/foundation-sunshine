/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <filesystem>
#include <fstream>

#include <boost/property_tree/json_parser.hpp>

// local includes
#include "file_handler.h"
#include "logging.h"

#ifdef _WIN32
  #include "platform/windows/misc.h"
#endif

namespace file_handler {
  std::filesystem::path
  path_from_utf8(std::string_view path) {
#ifdef _WIN32
    return std::filesystem::path { platf::from_utf8(std::string { path }) };
#else
    return std::filesystem::path { path };
#endif
  }

  std::string
  path_to_utf8(const std::filesystem::path &path) {
#ifdef _WIN32
    return platf::to_utf8(path.wstring());
#else
    return path.string();
#endif
  }

  std::string
  get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    return path_to_utf8(path_from_utf8(trimmed_path).parent_path());
  }

  bool
  make_directory(const std::string &path) {
    const auto native_path = path_from_utf8(path);
    // first, check if the directory already exists
    if (std::filesystem::exists(native_path)) {
      return true;
    }

    return std::filesystem::create_directories(native_path);
  }

  std::string
  read_file(const char *path) {
    const auto native_path = path_from_utf8(path);
    if (!std::filesystem::exists(native_path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(native_path);
    return std::string { (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() };
  }

  int
  write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path_from_utf8(path));

    if (!out.is_open()) {
      return -1;
    }

    out << contents;
    out.flush();
    out.close();
    return out ? 0 : -1;
  }

  void
  read_json(std::string_view path, boost::property_tree::ptree &tree) {
    std::ifstream input(path_from_utf8(path), std::ios::binary);
    if (!input.is_open()) {
      throw boost::property_tree::json_parser_error("cannot open file", std::string { path }, 0);
    }

    boost::property_tree::read_json(input, tree);
  }

  void
  write_json(std::string_view path, const boost::property_tree::ptree &tree, bool pretty) {
    std::ofstream output(path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      throw boost::property_tree::json_parser_error("cannot open file", std::string { path }, 0);
    }

    boost::property_tree::write_json(output, tree, pretty);
    output.flush();
    if (!output) {
      throw boost::property_tree::json_parser_error("write failed", std::string { path }, 0);
    }
  }
}  // namespace file_handler
