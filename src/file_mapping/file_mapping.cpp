/**
 * @file src/file_mapping.cpp
 * @brief Core directory mapping types and path safety helpers.
 */
#include "file_mapping.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace file_mapping {
  namespace {
    namespace fs = std::filesystem;

    std::string
    ascii_lower(std::string value) {
      std::transform(std::begin(value), std::end(value), std::begin(value), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return value;
    }

    bool
    equal_path_component(const fs::path &left, const fs::path &right) {
#ifdef _WIN32
      return ascii_lower(left.generic_string()) == ascii_lower(right.generic_string());
#else
      return left == right;
#endif
    }

    bool
    path_starts_with(const fs::path &path, const fs::path &root) {
      auto path_it = path.begin();
      auto root_it = root.begin();
      for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || !equal_path_component(*path_it, *root_it)) {
          return false;
        }
      }
      return true;
    }

    bool
    is_forbidden_windows_char(unsigned char ch) {
      return ch < 0x20 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '\\' ||
             ch == '|' || ch == '?' || ch == '*';
    }

    bool
    has_drive_prefix(std::string_view path) {
      return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
    }

    bool
    is_unc_like(std::string_view path) {
      return path.size() >= 2 &&
             ((path[0] == '/' && path[1] == '/') || (path[0] == '\\' && path[1] == '\\'));
    }

    std::vector<std::string_view>
    split_remote_path(std::string_view remote_path) {
      std::vector<std::string_view> parts;
      std::size_t start = 0;
      while (start <= remote_path.size()) {
        const auto end = remote_path.find('/', start);
        if (end == std::string_view::npos) {
          parts.emplace_back(remote_path.substr(start));
          break;
        }
        parts.emplace_back(remote_path.substr(start, end - start));
        start = end + 1;
      }
      return parts;
    }

    fs::path
    make_relative_path(std::string_view remote_path) {
      fs::path out;
      for (const auto segment : split_remote_path(remote_path)) {
        if (segment.empty()) {
          continue;
        }
        out /= std::string { segment };
      }
      return out;
    }

    bool
    is_reparse_point_or_symlink(const fs::path &path, std::error_code &ec) {
#ifdef _WIN32
      const auto attrs = GetFileAttributesW(path.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
      }
      ec.clear();
      return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
      auto status = fs::symlink_status(path, ec);
      if (ec) {
        return false;
      }
      return fs::is_symlink(status);
#endif
    }

    bool
    contains_reparse_point(const fs::path &path) {
      fs::path current;
      std::error_code ec;

      for (const auto &part : path) {
        current /= part;
        if (current.empty() || !fs::exists(current, ec)) {
          ec.clear();
          continue;
        }
        if (is_reparse_point_or_symlink(current, ec)) {
          return true;
        }
        ec.clear();
      }
      return false;
    }

    resolve_result_t
    fail(resolve_error_e error, std::string message) {
      return { false, error, {}, std::move(message) };
    }
  }  // namespace

  bool
  is_valid_mapping_id(std::string_view id) {
    if (id.empty() || id.size() > 64) {
      return false;
    }

    return std::all_of(std::begin(id), std::end(id), [](unsigned char ch) {
      return std::isalnum(ch) || ch == '_' || ch == '-';
    });
  }

  bool
  is_reserved_windows_name(std::string_view segment) {
    while (!segment.empty() && (segment.back() == ' ' || segment.back() == '.')) {
      segment.remove_suffix(1);
    }
    if (segment.empty()) {
      return true;
    }

    const auto dot = segment.find('.');
    std::string base { segment.substr(0, dot) };
    base = ascii_lower(std::move(base));

    static constexpr std::array reserved {
      "con",
      "prn",
      "aux",
      "nul",
      "com1",
      "com2",
      "com3",
      "com4",
      "com5",
      "com6",
      "com7",
      "com8",
      "com9",
      "lpt1",
      "lpt2",
      "lpt3",
      "lpt4",
      "lpt5",
      "lpt6",
      "lpt7",
      "lpt8",
      "lpt9"
    };

    return std::find(std::begin(reserved), std::end(reserved), base) != std::end(reserved);
  }

  bool
  is_safe_relative_path(std::string_view remote_path, std::string *message) {
    auto set_message = [message](std::string text) {
      if (message) {
        *message = std::move(text);
      }
    };

    if (remote_path.empty()) {
      return true;
    }
    if (remote_path.front() == '/' || remote_path.front() == '\\' || has_drive_prefix(remote_path) || is_unc_like(remote_path)) {
      set_message("remote path must be relative");
      return false;
    }

    for (const auto segment : split_remote_path(remote_path)) {
      if (segment.empty()) {
        continue;
      }
      if (segment == "." || segment == "..") {
        set_message("remote path may not contain dot segments");
        return false;
      }
      if (segment.back() == ' ' || segment.back() == '.') {
        set_message("remote path segment may not end with space or dot");
        return false;
      }
      if (std::any_of(std::begin(segment), std::end(segment), is_forbidden_windows_char)) {
        set_message("remote path contains a Windows-forbidden character");
        return false;
      }
      if (is_reserved_windows_name(segment)) {
        set_message("remote path contains a reserved Windows device name");
        return false;
      }
    }

    return true;
  }

  resolve_result_t
  resolve_path(const mapping_t &mapping, std::string_view remote_path, bool must_exist) {
    if (!is_valid_mapping_id(mapping.id)) {
      return fail(resolve_error_e::invalid_mapping_id, "invalid mapping id");
    }

    std::string safety_message;
    if (!is_safe_relative_path(remote_path, &safety_message)) {
      const auto error =
        safety_message == "remote path must be relative" ? resolve_error_e::absolute_path :
        safety_message == "remote path contains a reserved Windows device name" ? resolve_error_e::reserved_name :
                                                                                resolve_error_e::invalid_relative_path;
      return fail(error, std::move(safety_message));
    }

    std::error_code ec;
    if (mapping.local_root.empty() || !fs::is_directory(mapping.local_root, ec)) {
      return fail(resolve_error_e::invalid_root, "mapping root is not an existing directory");
    }
    ec.clear();

    if (!mapping.follow_reparse_points && contains_reparse_point(mapping.local_root)) {
      return fail(resolve_error_e::reparse_point_blocked, "mapping root contains a reparse point");
    }

    const auto root = fs::weakly_canonical(mapping.local_root, ec);
    if (ec) {
      return fail(resolve_error_e::filesystem_error, ec.message());
    }

    const auto relative = make_relative_path(remote_path);
    const auto raw_candidate = root / relative;
    if (!mapping.follow_reparse_points && contains_reparse_point(raw_candidate)) {
      return fail(resolve_error_e::reparse_point_blocked, "resolved path contains a reparse point");
    }

    const auto candidate = fs::weakly_canonical(raw_candidate, ec);
    if (ec) {
      return fail(resolve_error_e::filesystem_error, ec.message());
    }

    if (!path_starts_with(candidate, root)) {
      return fail(resolve_error_e::path_escape, "resolved path escapes mapping root");
    }

    if (must_exist && !fs::exists(candidate, ec)) {
      if (ec) {
        return fail(resolve_error_e::filesystem_error, ec.message());
      }
      return fail(resolve_error_e::not_found, "resolved path does not exist");
    }
    ec.clear();

    if (!mapping.follow_reparse_points && contains_reparse_point(candidate)) {
      return fail(resolve_error_e::reparse_point_blocked, "resolved path contains a reparse point");
    }

    return { true, resolve_error_e::none, candidate, {} };
  }
}  // namespace file_mapping
