/**
 * @file src/file_mapping.h
 * @brief Core directory mapping types and path safety helpers.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace file_mapping {
  enum class access_mode_e {
    read,
    readwrite
  };

  enum class resolve_error_e {
    none,
    invalid_mapping_id,
    invalid_root,
    absolute_path,
    invalid_relative_path,
    reserved_name,
    path_escape,
    reparse_point_blocked,
    not_found,
    filesystem_error
  };

  struct mapping_t {
    std::string id;
    std::string name;
    std::filesystem::path local_root;
    access_mode_e mode = access_mode_e::read;
    bool allow_delete = false;
    bool allow_execute = false;
    bool follow_reparse_points = false;
    std::uintmax_t max_file_size = 0;
    std::vector<std::string> clients;
  };

  struct resolve_result_t {
    bool ok = false;
    resolve_error_e error = resolve_error_e::none;
    std::filesystem::path resolved_path;
    std::string message;
  };

  bool
  is_valid_mapping_id(std::string_view id);

  bool
  is_reserved_windows_name(std::string_view segment);

  bool
  is_safe_relative_path(std::string_view remote_path, std::string *message = nullptr);

  resolve_result_t
  resolve_path(const mapping_t &mapping, std::string_view remote_path, bool must_exist = false);
}  // namespace file_mapping
