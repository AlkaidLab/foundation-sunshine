/**
 * @file src/build_info.h
 * @brief Build identity of this binary: full commit, channel, UTC build time,
 *        variant and package, for `--version --json`, `GET /api/version` and
 *        the doctor. The same fields are custom String keys of the Windows
 *        VERSIONINFO, so the panel reads them without starting the host.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace build_info {
  /** Version of the JSON object below; additive changes keep it. */
  inline constexpr int JSON_VERSION = 1;

  struct info_t {
    std::string version;  ///< PROJECT_VER, the display version
    std::string commit;  ///< full 40-hex commit, empty when unknown
    std::string branch;
    bool dirty = false;  ///< the tree had uncommitted changes
    std::string channel;  ///< stable | beta | dev | local
    std::string build_time_utc;  ///< ISO 8601, UTC, with Z
    std::string variant;  ///< product line, e.g. foundation-sunshine
    std::string package;  ///< installer | portable (auto resolved), or auto when unresolved
    std::string package_source;  ///< build (fixed at build time) | layout (decided from the install folder)
    std::string builder;  ///< ci | local
    std::string ci_run;  ///< CI run URL or id, empty for local builds
  };

  /** The record as compiled, `package` possibly still "auto". */
  info_t
  compiled();

  /** The record with `package` decided for the install around `executable_dir`. */
  info_t
  current(const std::filesystem::path &executable_dir);

  /** The record for the running executable. */
  info_t
  current();

  /**
   * A build that makes both packages records "auto"; the running host is an
   * installer install when the Inno Setup uninstaller sits next to it.
   */
  std::string
  resolve_package(std::string_view compiled_package, bool has_inno_uninstaller);

  /** Whether `dir` holds an Inno Setup uninstaller (unins000.exe, unins001.exe, ...). */
  bool
  has_inno_uninstaller(const std::filesystem::path &dir);

  /** The first 12 hex digits of the commit, for one-line displays. */
  std::string
  short_commit(std::string_view commit);

  /**
   * UTC time this process started (ISO 8601 with Z), fixed when the binary is loaded. A restart gives a later value, which is how a screen proves the
   * host was replaced and did not just answer again (`process_started_at` of GET /api/version).
   */
  std::string
  process_started_at_utc();

  nlohmann::json
  to_json(const info_t &info);
}  // namespace build_info
