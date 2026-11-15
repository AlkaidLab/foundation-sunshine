/**
 * @file src/crash/crash_env.cpp
 * @brief The machine, for the "env" object of a crash report (see crash_env.h).
 */
#include "crash_env.h"

#include "src/system_info.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <nlohmann/json.hpp>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace crash::env {
  std::string
  describe_machine() {
    nlohmann::json env = nlohmann::json::object();
    try {
      const auto snap = system_info::sample("", false);
      if (const auto cpu = sanitize_model(snap.cpu.model); !cpu.empty()) env["cpu"] = cpu;
      if (snap.cpu.threads > 0) env["cores"] = snap.cpu.threads;
      auto gpus = nlohmann::json::array();
      for (const auto &g : snap.gpus) {
        if (gpus.size() >= 4) break;
        nlohmann::json gpu { { "vendor", gpu_vendor_word(g.vendor.empty() ? g.name : g.vendor) } };
        if (const auto model = sanitize_model(g.name); !model.empty()) gpu["model"] = model;
        if (const auto driver = sanitize_version(g.driver_version); !driver.empty()) gpu["driver"] = driver;
        gpus.push_back(std::move(gpu));
      }
      if (!gpus.empty()) env["gpus"] = std::move(gpus);
    }
    catch (...) {
    }
  #if defined(__x86_64__) || defined(_M_X64)
    env["arch"] = "x64";
  #elif defined(__aarch64__) || defined(_M_ARM64)
    env["arch"] = "arm64";
  #endif
  #ifdef _WIN32
    MEMORYSTATUSEX mem {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) env["ram_mb"] = (std::uint64_t) (mem.ullTotalPhys / (1024 * 1024));
    if (const int monitors = GetSystemMetrics(SM_CMONITORS); monitors > 0) env["displays"] = monitors;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
      std::string name;
      for (const wchar_t *p = locale; *p; ++p) name += (*p < 128 ? (char) *p : '?');
      if (name.size() >= 2 && name.size() <= 20) env["locale"] = name;
    }
    // the installer puts an uninstaller next to the program; a portable copy has none
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
      std::error_code ec;
      env["install_source"] = std::filesystem::exists(std::filesystem::path(exe).parent_path() / L"unins000.exe", ec) ? "installer" : "portable";
    }
  #else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page > 0) env["ram_mb"] = (std::uint64_t) pages * (std::uint64_t) page / (1024 * 1024);
  #endif
    return env.dump();
  }
}  // namespace crash::env
