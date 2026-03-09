/**
 * @file globals.cpp
 * @brief Definitions for globally accessible variables and functions.
 */
#include "globals.h"

#include <filesystem>

#ifdef __APPLE__
  #include <mach-o/dyld.h>
  #include <climits>
#endif

namespace fs = std::filesystem;

safe::mail_t mail::man;
thread_pool_util::ThreadPool task_pool;
bool display_cursor = true;

const std::string &
get_assets_dir() {
  static const std::string assets_dir = []() -> std::string {
#ifdef __APPLE__
    // Detect .app bundle: executable is at Contents/MacOS/sunshine
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
      std::error_code ec;
      fs::path exe_path = fs::canonical(path, ec);
      if (!ec) {
        // exe_path = /path/to/Sunshine.app/Contents/MacOS/sunshine
        // bundle_resources = /path/to/Sunshine.app/Contents/Resources/assets
        fs::path bundle_assets = exe_path.parent_path().parent_path() / "Resources" / "assets";
        if (fs::exists(bundle_assets, ec) && fs::is_directory(bundle_assets, ec)) {
          return bundle_assets.string();
        }
      }
    }
#endif
    return SUNSHINE_ASSETS_DIR;
  }();
  return assets_dir;
}

std::string
get_web_dir() {
  return get_assets_dir() + "/web/";
}

#ifdef _WIN32
nvprefs::nvprefs_interface nvprefs_instance;
const std::string VDD_NAME = "ZakoHDR";
const std::string ZAKO_NAME = "Zako HDR";
std::string zako_device_id;
bool is_running_as_system_user = false;
#endif
