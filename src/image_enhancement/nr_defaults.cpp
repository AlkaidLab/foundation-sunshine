#include "nr_defaults.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

#include "src/platform/common.h"

#ifdef _WIN32
  #include <windows.h>
#endif

namespace image_enhancement {
  namespace {
    std::mutex defaults_mutex;

    std::filesystem::path path() {
      return platf::appdata() / "nr-defaults.json";
    }

    bool valid(const nr_defaults_t &value) {
      const auto &f = value.filter;
      return platf::valid_nr_scale(f.nr_scale_percent) &&
             std::isfinite(f.nr_intensity) && f.nr_intensity >= 0 && f.nr_intensity <= 1 &&
             std::isfinite(f.nr_skin_structure_strength) && f.nr_skin_structure_strength >= 0 && f.nr_skin_structure_strength <= 1 &&
             f.nr_style >= 0 && f.nr_style <= 4 && f.nr_motion_quality >= 0 && f.nr_motion_quality <= 3;
    }
  }  // namespace

  std::optional<nr_defaults_t> load_nr_defaults() {
    std::lock_guard lock(defaults_mutex);
    try {
      if (!std::filesystem::exists(path()) || std::filesystem::file_size(path()) > 64 * 1024) return std::nullopt;
      std::ifstream stream(path(), std::ios::binary);
      if (!stream) return std::nullopt;
      const auto document = nlohmann::json::parse(stream);
      if (document.at("version").get<int>() != 1) return std::nullopt;
      nr_defaults_t value;
      value.enabled = document.at("enabled").get<bool>();
      auto &f = value.filter;
      f.nr_scale_percent = document.at("scale_percent").get<int>();
      f.nr_intensity = document.at("intensity").get<float>();
      f.nr_style = document.at("style").get<int>();
      f.nr_skin_structure_strength = document.at("skin_structure_strength").get<float>();
      f.nr_auto_mask = document.at("auto_mask").get<bool>();
      f.nr_ui_correction = document.at("ui_correction").get<bool>();
      f.nr_motion_quality = document.at("motion_quality").get<int>();
      if (!valid(value)) return std::nullopt;
      return value;
    }
    catch (...) { return std::nullopt; }
  }

  bool save_nr_defaults(const nr_defaults_t &value) {
    if (!valid(value)) return false;
    std::lock_guard lock(defaults_mutex);
    const auto destination = path();
    auto temporary = destination;
    temporary += ".tmp";
    try {
      std::filesystem::create_directories(destination.parent_path());
      const auto &f = value.filter;
      const nlohmann::json document = {
        { "version", 1 }, { "enabled", value.enabled }, { "scale_percent", f.nr_scale_percent },
        { "intensity", f.nr_intensity }, { "style", f.nr_style },
        { "skin_structure_strength", f.nr_skin_structure_strength }, { "auto_mask", f.nr_auto_mask },
        { "ui_correction", f.nr_ui_correction }, { "motion_quality", f.nr_motion_quality },
      };
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream << document.dump(2) << '\n';
      stream.flush();
      const bool written = stream.good();
      stream.close();
      if (!written || stream.fail()) {
        std::filesystem::remove(temporary);
        return false;
      }
#ifdef _WIN32
      if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        return false;
      }
#else
      std::filesystem::rename(temporary, destination);
#endif
      return true;
    }
    catch (...) { return false; }
  }
}  // namespace image_enhancement
