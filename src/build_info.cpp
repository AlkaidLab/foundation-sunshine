/**
 * @file src/build_info.cpp
 * @brief Build identity of this binary (see build_info.h).
 */
#include "build_info.h"

#include "build_info_defs.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <system_error>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace build_info {
  namespace {
    // Initialised when the binary is loaded, before main: the start of the process.
    const std::time_t process_start = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  }  // namespace

  std::string
  process_started_at_utc() {
    std::tm broken_down {};
#ifdef _WIN32
    gmtime_s(&broken_down, &process_start);
#else
    gmtime_r(&process_start, &broken_down);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken_down);
    return buffer;
  }

  info_t
  compiled() {
    info_t info;
    info.version = PROJECT_VER;
    info.commit = SUNSHINE_BUILD_COMMIT;
    info.branch = SUNSHINE_BUILD_BRANCH;
    info.dirty = SUNSHINE_BUILD_DIRTY != 0;
    info.channel = SUNSHINE_BUILD_CHANNEL;
    info.build_time_utc = SUNSHINE_BUILD_TIME_UTC;
    info.variant = SUNSHINE_BUILD_VARIANT;
    info.package = SUNSHINE_BUILD_PACKAGE;
    info.package_source = info.package == "auto" ? "layout" : "build";
    info.builder = SUNSHINE_BUILD_BUILDER;
    info.ci_run = SUNSHINE_BUILD_CI_RUN;
    return info;
  }

  std::string
  resolve_package(std::string_view compiled_package, bool has_inno_uninstaller) {
    if (compiled_package == "installer" || compiled_package == "portable") {
      return std::string {compiled_package};
    }
    return has_inno_uninstaller ? "installer" : "portable";
  }

  bool
  has_inno_uninstaller(const std::filesystem::path &dir) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it {dir, ec}, end; !ec && it != end; it.increment(ec)) {
      auto name = it->path().filename().string();
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      // Inno Setup names its uninstaller unins###.exe in {app}
      if (name.size() == 12 && name.starts_with("unins") && name.ends_with(".exe") &&
          std::all_of(name.begin() + 5, name.begin() + 8, [](unsigned char c) { return std::isdigit(c); })) {
        return true;
      }
    }
    return false;
  }

  info_t
  current(const std::filesystem::path &executable_dir) {
    auto info = compiled();
    if (info.package == "auto") {
      info.package = resolve_package(info.package, has_inno_uninstaller(executable_dir));
    }
    return info;
  }

  static std::filesystem::path
  executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
      const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (length == 0) {
        return {};
      }
      if (length < buffer.size()) {
        buffer.resize(length);
        break;
      }
      buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path {buffer}.parent_path();
#else
    std::error_code ec;
    auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path {} : self.parent_path();
#endif
  }

  info_t
  current() {
    return current(executable_dir());
  }

  std::string
  short_commit(std::string_view commit) {
    return std::string {commit.substr(0, std::min<std::size_t>(commit.size(), 12))};
  }

  nlohmann::json
  to_json(const info_t &info) {
    return {
      {"json_version", JSON_VERSION},
      {"version", info.version},
      {"commit", info.commit},
      {"commit_short", short_commit(info.commit)},
      {"branch", info.branch},
      {"dirty", info.dirty},
      {"channel", info.channel},
      {"build_time_utc", info.build_time_utc},
      {"variant", info.variant},
      {"package", info.package},
      {"package_source", info.package_source},
      {"builder", info.builder},
      {"ci_run", info.ci_run},
    };
  }
}  // namespace build_info
