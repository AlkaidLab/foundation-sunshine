/**
 * @file src/confighttp/asset_paths.h
 * @brief Fail-closed path resolution for public image requests.
 */
#pragma once

#include <filesystem>
#include <optional>
#ifdef _WIN32
  #include <boost/winapi/error_codes.hpp>
  #include <boost/winapi/file_management.hpp>
  #include <boost/winapi/get_last_error.hpp>
#endif

namespace confighttp::assets {
  // Compare components, never string prefixes or fs::relative (which can be
  // empty for different Windows roots). Callers must reject lexical traversal.
  inline bool
  is_child_path(const std::filesystem::path &child, const std::filesystem::path &parent) {
    if (child.empty() || parent.empty()) return false;
    auto part = child.begin();
    for (const auto &component : parent) {
      if (part == child.end() || *part != component) return false;
      ++part;
    }
    if (part == child.end()) return false;
#ifdef _WIN32
    // MinGW canonicalization does not resolve directory junctions. Inspect each
    // component before following it, including the final file (symlinks/ADS).
    auto current = parent;
    for (; part != child.end(); ++part) {
      current /= *part;
      const auto attributes = boost::winapi::GetFileAttributesW(current.c_str());
      if (attributes == boost::winapi::INVALID_FILE_ATTRIBUTES_) {
        const auto error = boost::winapi::GetLastError();
        // Let the caller report ordinary missing assets as 404. Every existing
        // ancestor has already been inspected before reaching this component.
        return error == boost::winapi::ERROR_FILE_NOT_FOUND_ || error == boost::winapi::ERROR_PATH_NOT_FOUND_;
      }
      if (attributes & boost::winapi::FILE_ATTRIBUTE_REPARSE_POINT_) return false;
    }
#endif
    return true;
  }

  inline std::optional<std::filesystem::path>
  resolve_boxart_path(const std::filesystem::path &assets, const std::filesystem::path &covers, const std::filesystem::path &requested) {
    namespace fs = std::filesystem;
    // Reject roots before canonicalization: a UNC request must never initiate
    // a network filesystem lookup. Colons also exclude Windows drive-relative
    // paths and alternate data streams. NULs must not reach filesystem APIs.
    const auto &name = requested.native();
    if (requested.empty() || requested.has_root_path() ||
        name.find(fs::path::value_type(':')) != fs::path::string_type::npos ||
        name.find(fs::path::value_type('\0')) != fs::path::string_type::npos) return std::nullopt;
    for (const auto &part : requested) {
      if (part == "..") return std::nullopt;
    }

    for (const auto &root : { assets, covers }) {
      std::error_code ec;
      const auto canonical_root = fs::weakly_canonical(root, ec);
      if (ec || canonical_root.empty()) continue;
      if (!is_child_path(canonical_root / requested, canonical_root)) continue;
      const auto candidate = fs::weakly_canonical(canonical_root / requested, ec);
      if (ec || !is_child_path(candidate, canonical_root)) continue;
      if (root == assets && candidate.parent_path() != canonical_root) continue;
      if (fs::is_regular_file(candidate, ec) && !ec) return candidate;
    }
    return std::nullopt;
  }
}  // namespace confighttp::assets
