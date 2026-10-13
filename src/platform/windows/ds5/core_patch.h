/** @file core_patch.h
 * Hash-bound atomic upgrade of the optional HIDMaestro managed runtime.
 */
#pragma once
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>

namespace platf::ds5 {
  template<class Hash>
  bool ensure_core_patch(const std::filesystem::path &source,
                         const std::filesystem::path &destination,
                         std::string_view original_digest,
                         std::string_view patched_digest,
                         Hash hash, std::string &error) {
    static std::mutex patch_mutex;
    const std::lock_guard lock(patch_mutex);
    std::error_code ec;
    if (std::filesystem::symlink_status(destination, ec).type() != std::filesystem::file_type::regular || ec) {
      error = "HIDMaestro Core is missing or is not a regular file";
      return false;
    }
    const auto current = hash(destination);
    if (current && *current == patched_digest) return true;
    if (!current || *current != original_digest) {
      error = "HIDMaestro Core does not match the supported upstream runtime";
      return false;
    }
    if (std::filesystem::symlink_status(source, ec).type() != std::filesystem::file_type::regular || ec ||
        hash(source) != std::optional { std::string(patched_digest) }) {
      error = "The bundled HIDMaestro Core patch failed verification";
      return false;
    }
    static std::atomic_uint64_t sequence { 0 };
    auto staging = destination;
    staging += L".foundation-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(sequence.fetch_add(1)) + L".tmp";
    struct cleanup_t {
      std::filesystem::path path;
      ~cleanup_t() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup { staging };
    if (!std::filesystem::copy_file(source, staging, std::filesystem::copy_options::none, ec) || ec ||
        hash(staging) != std::optional { std::string(patched_digest) }) {
      error = "Unable to stage the verified HIDMaestro Core patch";
      return false;
    }
    if (hash(destination) != current) {
      error = "HIDMaestro Core changed during patch activation";
      return false;
    }
    if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      error = "Unable to activate HIDMaestro Core patch (Windows error " + std::to_string(GetLastError()) + ")";
      return false;
    }
    if (hash(destination) != std::optional { std::string(patched_digest) }) {
      error = "Activated HIDMaestro Core patch failed verification";
      return false;
    }
    return true;
  }
}  // namespace platf::ds5
