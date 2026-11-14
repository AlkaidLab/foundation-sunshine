/**
 * @file src/log_retention.h
 * @brief Keep the log of the previous run as sunshine.log.1 when the host starts, so neither a crash restart nor an upgrade loses it.
 *
 * With `restore_log` off (the default) the start used to delete sunshine.log or truncate it. The service restarts the host a few seconds after a crash, so the log
 * of the crashed run was gone before anyone could read it (2026-11-12, the 0xc0000005 at 14:50:15 restarted at 14:50:22). The file is now moved to
 * sunshine.log.1, the older ones shift up to `keep`, the oldest is dropped. Header only, std::filesystem only.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace log_retention {

  inline constexpr int default_keep = 5;

  inline std::filesystem::path
  backup_path(const std::filesystem::path &log, int index) {
    auto name = log.filename().string();
    name += "." + std::to_string(index);
    return log.parent_path() / name;
  }

  /** What a start found of the previous run. */
  enum class kept_e {
    nothing,  ///< no previous log anywhere (first start, or only empty files)
    leftover,  ///< sunshine.log was still in place (the process was killed or crashed): moved to .1
    from_archive,  ///< the previous run ended cleanly and its log was already moved into the archive folder: copied to .1
  };

  inline void
  shift_backups(const std::filesystem::path &log, int keep, std::error_code &error) {
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::remove(backup_path(log, keep), ignored);
    for (int index = keep - 1; index >= 1; --index) {
      const auto from = backup_path(log, index);
      if (fs::exists(from, ignored)) {
        std::error_code shift_error;
        fs::rename(from, backup_path(log, index + 1), shift_error);
        if (shift_error && !error) {
          error = shift_error;
        }
      }
    }
  }

  /**
   * @brief The newest non-empty file in `archive_dir` whose name starts with the log's file name (the log sink's collector names them sunshine.log00004), or empty.
   * @details The write time decides, but a tie may not fall to the order the directory iterator happens to return: that order is unspecified, and the write
   * time the ucrt stat path behind std::filesystem::last_write_time reports on Windows has whole seconds
   * [gcc-libstdc++-fs-dir-16.2.0] [gcc-libstdc++-fs-ops-16.2.0], so files stored within one second compare equal (a file's time is only guaranteed once
   * every writing handle is closed [ms-file-times-20261112]). The log sink's collector appends a growing zero-padded counter when the archived name is
   * already taken [boost-log-text-file-backend-1.92.0], so the greater name is the later store.
   */
  inline std::filesystem::path
  newest_archived(const std::filesystem::path &log, const std::filesystem::path &archive_dir) {
    namespace fs = std::filesystem;
    const auto prefix = log.filename().string();
    fs::path best;
    std::string best_name;
    fs::file_time_type best_time {};
    std::error_code ec;
    for (fs::directory_iterator it(archive_dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code entry_error;
      if (!it->is_regular_file(entry_error) || entry_error) {
        continue;
      }
      const auto name = it->path().filename().string();
      if (name.compare(0, prefix.size(), prefix) != 0) {
        continue;
      }
      const auto size = it->file_size(entry_error);
      const auto time = it->last_write_time(entry_error);
      if (entry_error || size == 0) {
        continue;
      }
      if (best.empty() || time > best_time || (time == best_time && name > best_name)) {
        best = it->path();
        best_name = name;
        best_time = time;
      }
    }
    return best;
  }

  /**
   * @brief Make the log of the previous run `log.1`, whichever way that run ended; older ones shift up to `log.<keep>`, the one beyond is deleted.
   * @details A process that crashed or was killed leaves `log` in place: it is moved. A process that stopped cleanly (an upgrade stops the service, so does a
   * restart) has already had its log moved into `archive_dir` by the log sink (Boost's file collector moves the closed file there), and `log` does not exist: the newest
   * archived file is copied, the archive keeps its own. Nothing is touched when there is no previous log.
   * @param error Receives the first failure; nothing is thrown.
   */
  inline kept_e
  retain_previous_run(const std::filesystem::path &log, const std::filesystem::path &archive_dir, int keep, std::error_code &error) {
    namespace fs = std::filesystem;
    error.clear();
    if (keep < 1) {
      return kept_e::nothing;
    }

    std::error_code status;
    const bool present = fs::is_regular_file(log, status);
    const auto size = present ? fs::file_size(log, status) : std::uintmax_t { 0 };
    if (present && (status || size > 0)) {
      shift_backups(log, keep, error);
      std::error_code move_error;
      fs::rename(log, backup_path(log, 1), move_error);
      if (move_error) {
        // Another process may hold the file open without delete sharing: keep the content anyway.
        std::error_code copy_error;
        fs::copy_file(log, backup_path(log, 1), fs::copy_options::overwrite_existing, copy_error);
        if (!error) {
          error = copy_error ? copy_error : move_error;
        }
      }
      return kept_e::leftover;
    }

    const auto archived = archive_dir.empty() ? fs::path {} : newest_archived(log, archive_dir);
    if (archived.empty()) {
      return kept_e::nothing;
    }
    shift_backups(log, keep, error);
    std::error_code copy_error;
    fs::copy_file(archived, backup_path(log, 1), fs::copy_options::overwrite_existing, copy_error);
    if (copy_error) {
      if (!error) {
        error = copy_error;
      }
      return kept_e::nothing;
    }
    return kept_e::from_archive;
  }

}  // namespace log_retention
