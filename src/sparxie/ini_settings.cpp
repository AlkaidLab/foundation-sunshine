#include "src/sparxie/ini_settings.h"
#include "src/sparxie/sha256.h"
#include <cstdlib>
#include <fstream>
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif
namespace sparxie::ini_settings {
  std::optional<File>
  game_file(const Install &install) {
    // [sparxie-community-settings-20261111] Unreal GameUserSettings paths
    // and the safe configuration-only boundary are documented in
    // docs/game-settings-sources.md; unknown files remain unavailable.
    if (install.game != "wuwa" && install.game != "nte" && install.game != "endfield") return std::nullopt;
    const fs::path candidates[] = {
      fs::path(install.dir) / "Client" / "Saved" / "Config" / "WindowsNoEditor" / "GameUserSettings.ini",
      fs::path(install.dir) / "Saved" / "Config" / "WindowsNoEditor" / "GameUserSettings.ini",
      fs::path(install.dir) / "Saved_Global" / "Config" / "WindowsNoEditor" / "GameUserSettings.ini",
    };
    for (const auto &path : candidates) {
      std::ifstream in(path);
      if (!in) continue;
      File file { path, {} };
      std::string line;
      while (std::getline(in, line)) file.lines.push_back(line);
      return file;
    }
    return std::nullopt;
  }

  std::string
  trim(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t at = 0;
    while (at < value.size() && (value[at] == ' ' || value[at] == '\t')) ++at;
    return value.substr(at);
  }

  std::string
  get(const File &file, const char *key) {
    bool game_section = false;
    for (const auto &line : file.lines) {
      const std::string text = trim(line);
      if (!text.empty() && text.front() == '[') {
        game_section = text.find("GameUserSettings") != std::string::npos;
        continue;
      }
      if (!game_section) continue;
      const auto eq = text.find('=');
      if (eq != std::string::npos && trim(text.substr(0, eq)) == key) return trim(text.substr(eq + 1));
    }
    return {};
  }

  bool
  set(File &file, const char *key, const std::string &value) {
    bool game_section = false;
    for (auto &line : file.lines) {
      const std::string text = trim(line);
      if (!text.empty() && text.front() == '[') {
        game_section = text.find("GameUserSettings") != std::string::npos;
        continue;
      }
      if (!game_section) continue;
      const auto eq = text.find('=');
      if (eq != std::string::npos && trim(text.substr(0, eq)) == key) {
        line = std::string(key) + "=" + value;
        return true;
      }
    }
    return false;
  }

  fs::path
  key(const char *field) {
    if (std::string_view(field) == "resolution") return "ResolutionSizeX";
    if (std::string_view(field) == "display") return "FullscreenMode";
    if (std::string_view(field) == "fps") return "FrameRateLimit";
    if (std::string_view(field) == "vsync") return "bUseVSync";
    if (std::string_view(field) == "renderScale") return "sg.ResolutionQuality";
    if (std::string_view(field) == "preset") return "OverallQuality";
    const std::pair<const char *, const char *> names[] = {
      { "smooth-fps", "bSmoothFrameRate" },
      { "fixed-fps", "bUseFixedFrameRate" },
      { "auto-quality", "AutoAdjustImageQuality" },
      { "shadow", "ShadowQuality" },
      { "effects", "NiagaraQuality" },
      { "detail", "ImageDetail" },
      { "texture", "sg.TextureQuality" },
      { "anti-aliasing", "AntiAliasing" },
      { "anisotropy", "r.MaxAnisotropy" },
      { "view-distance", "LoadingRangeScaleLevel" },
      { "npc-density", "NpcDensity" },
      { "vegetation", "VegetationDensity" },
      { "vegetation-dither", "VegetationDither" },
      { "dynamic-bones", "DynamicBones" },
      { "environment-interaction", "WaterInteract" },
      { "teammate-effects", "TeammateFx" },
      { "hit-material", "EnemyHitDisplayMode" },
      { "damage-effects", "SkinDamageMode" },
      { "lighting", "r.Kuro.GlobalLightQuality" },
      { "ambient-occlusion", "SceneAo" },
      { "volumetric-fog", "VolumeFog" },
      { "volumetric-light", "VolumeLight" },
      { "bloom", "BloomEnable" },
      { "motion-blur", "MotionBlur" },
      { "auto-exposure", "AutoExposure" },
      { "dlss", "r.NGX.DLSS.Enable" },
      { "dlss-quality", "r.NGX.DLSS.Quality" },
      { "dlss-sharpness", "NvidiaSuperSamplingSharpness" },
      { "native-fg", "r.Streamline.DLSSG.Enable" },
      { "fsr", "Fsr3Enable" },
      { "fsr-quality", "Fsr3Quality" },
      { "fsr-fg", "Fsr3Fg" },
      { "fsr-legacy", "FsrEnable" },
      { "xess", "Xess2Enable" },
      { "xess-quality", "Xess2Quality" },
      { "xess-fg", "Xess2Fg" },
      { "ray-tracing", "r.RayTracing.Enable" },
      { "ray-quality", "sg.RayTracingQuality" },
      { "ray-gi", "RayTracedGI" },
      { "ray-reflections", "RayTracedReflection" },
      { "ray-shadows", "RayTracedShadow" },
      { "brightness", "Brightness" },
      { "saturation", "SaturationNew" },
      { "contrast", "ContrastNew" },
      { "image-display", "ImageDisplayMode" },
      { "eye-mode", "EyeProtectionMode" },
      { "eye-temperature", "EyeProtectionTemp" },
      { "eye-strength", "EyeProtectionStrength" },
      { "eye-brightness", "EyeProtectionBrightness" },
      { "eye-texture", "EyeProtectionTexture" },
    };
    for (const auto &[name, key] : names)
      if (std::string_view(field) == name) return key;
    return field;
  }

  bool
  write(const File &file) {
    const fs::path tmp = file.path.string() + ".sparxie.tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      for (const auto &line : file.lines) out << line << '\n';
      out.flush();
      if (!out) return false;
    }
#if defined(_WIN32)
    return MoveFileExW(tmp.c_str(), file.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(tmp, file.path, ec);
    return !ec;
#endif
  }

  bool
  number(const std::string &text, double &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end && *end == '\0';
  }

  bool
  has_game_section(const File &file) {
    for (const auto &line : file.lines)
      if (trim(line).find("GameUserSettings") != std::string::npos) return true;
    return false;
  }

  SettingsResult
  write_game_file(const Install &install, const GameValues &values, const std::vector<std::string> &supported, const std::set<std::string> &fields, const fs::path &backup_dir) {
    SettingsResult result;
    result.values = values;
    const auto want = [&](const char *field) { return fields.empty() || fields.count(field) != 0; };
    auto file = game_file(install);
    if (!file || !has_game_section(*file)) {
      for (const auto &field : supported) result.unavailable.push_back(field);
      result.status = Status::fail(400, "settings_unsupported", "This title's settings file is missing or has an unknown format");
      return result;
    }
    if (want("resolution")) {
      int width = 0, height = 0;
      if (parse_resolution(values.resolution, width, height)) {
        set(*file, "ResolutionSizeX", std::to_string(width));
        set(*file, "ResolutionSizeY", std::to_string(height));
        result.applied.push_back("resolution");
      }
    }
    if (want("display")) {
      set(*file, "FullscreenMode", values.display == "fullscreen" ? "1" : values.display == "borderless" ? "2" :
                                                                                                           "3");
      result.applied.push_back("display");
    }
    if (want("fps")) {
      set(*file, "FrameRateLimit", std::to_string(values.fps));
      result.applied.push_back("fps");
    }
    if (want("vsync")) {
      set(*file, "bUseVSync", values.vsync ? "True" : "False");
      result.applied.push_back("vsync");
    }
    if (want("renderScale")) {
      set(*file, "sg.ResolutionQuality", std::to_string(values.render_scale));
      result.applied.push_back("renderScale");
    }
    if (want("preset")) {
      int preset = values.preset == "lowest" ? 0 : values.preset == "low"    ? 1 :
                                                 values.preset == "medium"   ? 2 :
                                                 values.preset == "veryhigh" ? 4 :
                                                                               3;
      set(*file, "OverallQuality", std::to_string(preset));
      result.applied.push_back("preset");
    }
    for (const auto &field : supported) {
      if (field == "resolution" || field == "display" || field == "fps" || field == "vsync" || field == "renderScale" || field == "preset" || !want(field.c_str())) continue;
      const auto value = values.extra.find(field);
      if (value == values.extra.end() || (!value->is_boolean() && !value->is_number() && !value->is_string())) {
        result.unavailable.push_back(field);
        continue;
      }
      const std::string text = value->is_boolean() ? (value->get<bool>() ? "True" : "False") : value->is_string() ? value->get<std::string>() :
                                                                                                                    value->dump();
      if (set(*file, key(field.c_str()).string().c_str(), text))
        result.applied.push_back(field);
      else
        result.unavailable.push_back(field);
    }
    if (!write(*file)) {
      result.status = Status::fail(500, "write_failed", "Could not atomically replace the game settings file");
      return result;
    }
    const auto verify = game_file(install);
    if (!verify || (want("fps") && get(*verify, "FrameRateLimit") != std::to_string(values.fps))) {
      result.status = Status::fail(500, "write_failed", "The game settings file did not pass read-back verification");
      return result;
    }
    if (!backup_dir.empty() && verify) {
      json backup;
      std::ifstream in(backup_dir / (install.id + ".json"));
      try {
        in >> backup;
      }
      catch (const std::exception &) {
        backup = json::object();
      }
      std::ifstream bytes(verify->path, std::ios::binary);
      const std::string body((std::istreambuf_iterator<char>(bytes)), std::istreambuf_iterator<char>());
      backup["iniAfter"] = sha256::hash(body);
      std::ofstream out(backup_dir / (install.id + ".json"));
      out << backup.dump(2);
    }
    result.status = Status::success();
    return result;
  }

}  // namespace sparxie::ini_settings
