#include "src/sparxie/region.h"
#include "src/sparxie/hoyo_settings.h"
#include "src/sparxie/zzz_file.h"

#include "src/sparxie/vendors.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace sparxie {
  namespace {
    struct ExeRule {
      const char *file;
      const char *game;
      const char *data_dir;  // marker folder under the install root; "" = none
      std::vector<std::string> candidates;
      int root_up;  // folder levels between the executable and the install root
    };

    const std::vector<ExeRule> &
    rules() {
      static const std::vector<ExeRule> table = {
        { "YuanShen.exe", "genshin", "YuanShen_Data", { "cn", "bili" }, 0 },
        { "GenshinImpact.exe", "genshin", "GenshinImpact_Data", { "global" }, 0 },
        { "StarRail.exe", "starrail", "StarRail_Data", { "cn", "global", "bili" }, 0 },
        { "ZenlessZoneZero.exe", "zzz", "ZenlessZoneZero_Data", { "cn", "global", "bili" }, 0 },
        { "ZenlessZoneZeroBeta.exe", "zzz", "ZenlessZoneZero_Data", { "cn", "global", "bili" }, 0 },
        { "BH3.exe", "hi3", "BH3_Data", { "cn", "global" }, 0 },
        { "Client-Win64-Shipping.exe", "wuwa", "Client", { "cn", "bili", "global" }, 3 },
        { "PGR.exe", "pgr", "", { "cn", "global" }, 0 },
        { "Arknights.exe", "arknights", "", { "cn", "global" }, 0 },
        { "Endfield.exe", "endfield", "", { "cn", "bili", "global" }, 0 },
        { "NTEGame.exe", "nte", "NTELauncher", { "cn", "bili" }, 0 },
        { "NTEGlobalGame.exe", "nte", "NTEGlobal", { "global" }, 0 },
        { "HTGame.exe", "nte", "", { "cn", "bili", "global" }, 5 },
      };
      return table;
    }

    const ExeRule *
    rule_for(std::string_view file) {
      const std::string name = ascii_lower(file);
      for (const auto &rule : rules())
        if (ascii_lower(rule.file) == name) return &rule;
      return nullptr;
    }

    void
    add_marker(Install &install, const std::string &marker) {
      if (std::find(install.markers.begin(), install.markers.end(), marker) == install.markers.end())
        install.markers.push_back(marker);
    }

    bool
    is_space(unsigned char c) {
      return c == ' ' || c == 9 || c == 10 || c == 13;
    }

    std::string
    trim(std::string text) {
      while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) text.pop_back();
      std::size_t i = 0;
      while (i < text.size() && is_space(static_cast<unsigned char>(text[i]))) ++i;
      return text.substr(i);
    }

    struct IniGeneral {
      int channel = -1;
      int sub_channel = -1;
      std::string version;
      std::string biz;
      std::string cps;
    };

    IniGeneral
    read_config_ini(const fs::path &path) {
      IniGeneral ini;
      std::ifstream in(path);
      if (!in) return ini;
      bool general = false;
      std::string line;
      while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
          general = ascii_lower(line) == "[general]";
          continue;
        }
        if (!general) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = ascii_lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "channel")
          ini.channel = std::atoi(value.c_str());
        else if (key == "sub_channel")
          ini.sub_channel = std::atoi(value.c_str());
        else if (key == "game_version")
          ini.version = value;
        else if (key == "game_biz")
          ini.biz = value;
        else if (key == "cps")
          ini.cps = value;
      }
      return ini;
    }

    void
    finish_id(Install &install) {
      if (!install.region.empty())
        install.id = install.game + "-" + install.region;
      else
        install.id = install.game + "-open-" + fnv8(norm_path(install.dir));
      if (!install.region.empty()) install.biz = biz_for(install.game, install.region);
      if (install.candidates.size() == 1 && install.region.empty()) install.region = install.candidates[0];
      if (!install.region.empty() && install.biz.empty()) install.biz = biz_for(install.game, install.region);
      if (!install.region.empty()) install.id = install.game + "-" + install.region;
    }

    std::string
    hyp_key_text(const char *vendor, const char *version, std::string_view biz) {
      const char sep = '\\';
      return std::string("Software") + sep + vendor + sep + "HYP" + sep + version + sep + std::string(biz);
    }

    const char *
    hyp_vendor(std::string_view region) {
      // The launcher writes its records under the publisher of its region:
      // Cognosphere for the international launcher, miHoYo for the national
      // and the Bilibili launcher (collapse-bili-vendor-d478585).
      return region == "global" ? "Cognosphere" : "miHoYo";
    }

    const char *
    hyp_version(std::string_view region) {
      return region == "global" ? "1_0" : "1_1";
    }
  }  // namespace

  const RegionRow *
  region_row(std::string_view game, std::string_view region) {
    static const RegionRow table[] = {
      { "genshin", "cn", "hk4e_cn", "国服", "CN", 1, 1, "hyp_mihoyo" },
      { "genshin", "bili", "hk4e_bilibili", "B 服", "Bilibili", 14, 0, "hyp_mihoyo" },
      { "genshin", "global", "hk4e_global", "国际服", "Global", 1, 0, "hyp_hoyoverse" },
      { "starrail", "cn", "hkrpg_cn", "国服", "CN", 1, 1, "hyp_mihoyo" },
      { "starrail", "bili", "hkrpg_bilibili", "B 服", "Bilibili", 14, 0, "hyp_mihoyo" },
      { "starrail", "global", "hkrpg_global", "国际服", "Global", 1, 0, "hyp_hoyoverse" },
      { "zzz", "cn", "nap_cn", "国服", "CN", 1, 1, "hyp_mihoyo" },
      { "zzz", "bili", "nap_bilibili", "B 服", "Bilibili", 14, 0, "hyp_mihoyo" },
      { "zzz", "global", "nap_global", "国际服", "Global", 1, 0, "hyp_hoyoverse" },
      { "hi3", "cn", "bh3_cn", "国服", "CN", 1, 1, "hyp_mihoyo" },
      // Both Honkai Impact 3rd clients write channel 1/1, so the channel
      // pair never narrows this game's region; game_biz does.
      { "hi3", "global", "bh3_global", "国际服", "Global", 1, 1, "hyp_hoyoverse" },
    };
    for (const auto &row : table)
      if (row.game == game && row.region == region) return &row;
    return nullptr;
  }

  std::string
  region_display(std::string_view region) {
    for (const auto game : kGames)
      if (const RegionRow *row = region_row(game, region)) return row->name;
    return std::string(region);
  }

  std::string
  region_display_en(std::string_view region) {
    for (const auto game : kGames)
      if (const RegionRow *row = region_row(game, region)) return row->name_en;
    return std::string(region);
  }

  std::string
  hyp_biz(std::string_view game, std::string_view region) {
    const RegionRow *row = region_row(game, region);
    return row ? std::string(row->biz) : std::string();
  }

  std::vector<HypKey>
  hyp_registry_keys(const Registry &registry) {
    std::vector<HypKey> out;
    auto push = [&](const char *vendor, const char *version, const RegionRow &row) {
      HypKey key;
      key.key = hyp_key_text(vendor, version, row.biz);
      key.biz = row.biz;
      key.game = row.game;
      key.region = row.region;
      key.source = std::string(row.region) == "bili" ? "bili" : "hoyoplay";
      for (const auto &known : out)
        if (known.key == key.key) return;
      out.push_back(std::move(key));
    };
    // The keys the launchers write today (starward-hyp-keys-df8ae07).
    for (const auto game : kGames)
      for (const auto region : kRegions)
        if (const RegionRow *row = region_row(game, region)) push(hyp_vendor(row->region), hyp_version(row->region), *row);
    // And every other HYP key that names a known biz, so a launcher version
    // this table does not list is still found. The reference launcher
    // enumerates the same hives (collapse-hyp-detect-d478585); a registry
    // that cannot enumerate keeps the known-key table.
    const char *vendors[] = { "miHoYo", "Cognosphere" };
    const char sep = '\\';
    for (const char *vendor : vendors) {
      const std::string root = std::string("Software") + sep + vendor + sep + "HYP";
      for (const std::string &version : registry.subkeys(root)) {
        for (const std::string &biz : registry.subkeys(root + sep + version)) {
          for (const auto game : kGames)
            for (const auto region : kRegions)
              if (const RegionRow *row = region_row(game, region); row && row->biz == biz) push(vendor, version.c_str(), *row);
        }
      }
    }
    return out;
  }

  std::string
  game_of_executable(std::string_view file) {
    const ExeRule *rule = rule_for(file);
    return rule ? std::string(rule->game) : std::string();
  }

  std::vector<std::string>
  executables_for(std::string_view game, std::string_view region) {
    std::vector<std::string> names;
    for (const auto &rule : rules()) {
      if (game != rule.game) continue;
      if (!region.empty() && std::find(rule.candidates.begin(), rule.candidates.end(), region) == rule.candidates.end()) continue;
      names.emplace_back(rule.file);
    }
    return names;
  }

  const TitleRow *
  title_row(std::string_view game) {
    static const std::vector<TitleRow> table = {
      { "genshin", "原神", "Genshin Impact", { "gi", "ys", "yuanshen", "原神" }, true, { "resolution", "display", "fps", "vsync", "renderScale", "preset", "hdr" } },
      { "starrail", "崩坏：星穹铁道", "Honkai: Star Rail", { "hsr", "sr", "星铁", "崩铁", "星穹铁道", "honkai" }, true, { "resolution", "display", "fps", "vsync", "renderScale", "preset" } },
      { "zzz", "绝区零", "Zenless Zone Zero", { "zzz", "ze", "绝区零", "zenless" }, true, { "resolution", "display", "fps", "vsync", "renderScale", "preset", "hdr" } },
      { "hi3", "崩坏3", "Honkai Impact 3rd", { "bh3", "hi3", "崩坏3rd", "崩崩" }, false, { "resolution", "display", "fps" } },
      { "wuwa", "鸣潮", "Wuthering Waves", { "wuwa", "mc", "mingchao", "鸣潮" }, false, { "resolution", "display", "fps", "vsync", "renderScale", "preset" } },
      { "pgr", "战双帕弥什", "Punishing: Gray Raven", { "pgr", "zhanshuang", "战双", "灰鸦" }, false, {} },
      { "arknights", "明日方舟", "Arknights", { "ak", "arknights", "明日方舟", "刀客塔" }, false, {} },
      { "endfield", "明日方舟：终末地", "Arknights: Endfield", { "endfield", "zmd", "终末地" }, true, {} },
      { "nte", "异环", "NTE: Neverness to Everness", { "nte", "yihuan", "异环" }, false, { "resolution", "display", "fps", "vsync", "renderScale", "preset" } },
    };
    for (const auto &row : table)
      if (row.game == game) return &row;
    return nullptr;
  }

  std::vector<std::string>
  settings_fields(std::string_view game) {
    const TitleRow *row = title_row(game);
    std::vector<std::string> out = row ? row->settings : std::vector<std::string> {};
    if (game == "wuwa") {
      const char *extra[] = { "smooth-fps", "fixed-fps", "auto-quality", "shadow", "effects", "detail", "texture", "anti-aliasing", "anisotropy", "view-distance", "npc-density", "vegetation", "vegetation-dither", "dynamic-bones", "environment-interaction", "teammate-effects", "hit-material", "damage-effects", "lighting", "ambient-occlusion", "volumetric-fog", "volumetric-light", "bloom", "motion-blur", "auto-exposure", "dlss", "dlss-quality", "dlss-sharpness", "native-fg", "fsr", "fsr-quality", "fsr-fg", "fsr-legacy", "xess", "xess-quality", "xess-fg", "ray-tracing", "ray-quality", "ray-gi", "ray-reflections", "ray-shadows", "brightness", "saturation", "contrast", "image-display", "eye-mode", "eye-temperature", "eye-strength", "eye-brightness", "eye-texture" };
      out.insert(out.end(), std::begin(extra), std::end(extra));
    }
    return out;
  }
  namespace {
    // The described items of the settings menu (docs/sparxie-api.md): what the
    // page draws is what is listed here, and nothing else. Value domains are the
    // ones the host reads and writes; keys without a domain the repository has
    // verified are left out rather than guessed (docs/sparxie-api.md, Game settings).
    json
    option(const json &value) { return { { "value", value } }; }

    json
    field(const char *key, const char *kind, const char *section, json extra = json::object()) {
      extra["key"] = key;
      extra["kind"] = kind;
      extra["section"] = section;
      return extra;
    }

    json
    resolution_field() {
      return field("resolution", "choice", "display", { { "options", json::array({ option("1280×720"), option("1600×900"), option("1920×1080"), option("2560×1440"), option("3840×2160") }) } });
    }

    json
    display_field() { return field("display", "choice", "display", { { "options", json::array({ option("fullscreen"), option("windowed") }) } }); }

    json
    fps_field(std::initializer_list<int> tiers) {
      json options = json::array();
      for (int tier : tiers) options.push_back(option(tier));
      return field("fps", "choice", "display", { { "options", options } });
    }

    json
    labeled(const char *value, const char *label, const char *label_en) { return { { "value", value }, { "label", label }, { "labelEn", label_en } }; }

    json
    preset_field(std::initializer_list<const char *> values) {
      json options = json::array();
      for (const char *value : values) {
        // the page has words for low, medium and high; the others carry their own
        if (std::string_view(value) == "lowest")
          options.push_back(labeled(value, "最低", "Lowest"));
        else if (std::string_view(value) == "verylow")
          options.push_back(labeled(value, "较低", "Very low"));
        else if (std::string_view(value) == "veryhigh")
          options.push_back(labeled(value, "最高", "Very high"));
        else if (std::string_view(value) == "custom")
          options.push_back(labeled(value, "自定义", "Custom"));
        else
          options.push_back(option(value));
      }
      return field("preset", "choice", "quality", { { "options", options } });
    }

    json
    scale_field(double min, double max, double step, json marks) {
      return field("renderScale", "slider", "quality", { { "min", min }, { "max", max }, { "step", step }, { "decimals", 1 }, { "marks", marks } });
    }

    json
    game_settings_of(const std::string_view game, bool zzz_file_settings) {
      json out = json::array();
      out.push_back(resolution_field());
      out.push_back(display_field());
      if (game == "genshin") {
        out.push_back(fps_field({ 30, 45, 60 }));
        out.push_back(field("vsync", "toggle", "display"));
        out.push_back(preset_field({ "lowest", "low", "medium", "high" }));
        out.push_back(scale_field(0.6, 1.5, 0.1, json::array({ 0.6, 1.0, 1.5 })));
        out.push_back(field("hdr", "toggle", "quality"));
      }
      else if (game == "starrail") {
        out.push_back(fps_field({ 30, 60, 120 }));
        out.push_back(field("vsync", "toggle", "display"));
        out.push_back(preset_field({ "custom", "verylow", "low", "medium", "high", "veryhigh" }));
        out.push_back(scale_field(0.6, 1.5, 0.1, json::array({ 0.6, 1.0, 1.5 })));
      }
      else if (game == "zzz" && zzz_file_settings) {
        out.push_back(fps_field({ 30, 60, 0 }));
        out.push_back(field("vsync", "toggle", "display"));
        out.push_back(preset_field({ "high", "medium", "low", "custom" }));
        out.push_back(scale_field(0.8, 1.2, 0.2, json::array({ 0.8, 1.0, 1.2 })));
        out.push_back(field("hdr", "toggle", "quality"));
      }
      else if (game == "hi3" || game == "endfield") {
        out.push_back(fps_field({ 30, 60 }));
      }
      else if (game == "wuwa" || game == "nte") {
        out.clear();
        out.push_back(resolution_field());
        out.push_back(field("display", "choice", "display", { { "options", json::array({ labeled("fullscreen", "全屏", "Fullscreen"), labeled("borderless", "无边框", "Borderless"), labeled("windowed", "窗口", "Windowed") }) } }));
        out.push_back(fps_field({ 30, 45, 60, 90, 120, 144, 165, 240, 0 }));
        out.push_back(field("vsync", "toggle", "display"));
        out.push_back(field("renderScale", "slider", "quality", { { "min", 50 }, { "max", 200 }, { "step", 1 }, { "decimals", 0 }, { "marks", json::array({ 50, 75, 100, 150, 200 }) }, { "unit", "%" } }));
        out.push_back(preset_field({ "lowest", "low", "medium", "high", "veryhigh" }));
        if (game == "wuwa") {
          const std::pair<const char *, const char *> extra[] = {
            { "smooth-fps", "Smooth frame rate" },
            { "fixed-fps", "Fixed frame rate" },
            { "auto-quality", "Auto quality" },
            { "shadow", "Shadow quality" },
            { "effects", "Effects quality" },
            { "detail", "Image detail" },
            { "texture", "Texture quality" },
            { "anti-aliasing", "Anti-aliasing" },
            { "anisotropy", "Anisotropic filtering" },
            { "view-distance", "View distance" },
            { "npc-density", "NPC density" },
            { "vegetation", "Vegetation density" },
            { "vegetation-dither", "Vegetation dither" },
            { "dynamic-bones", "Dynamic bones" },
            { "environment-interaction", "Environment interaction" },
            { "teammate-effects", "Teammate effects" },
            { "hit-material", "Hit material" },
            { "damage-effects", "Damage effects" },
            { "lighting", "Global lighting" },
            { "ambient-occlusion", "Ambient occlusion" },
            { "volumetric-fog", "Volumetric fog" },
            { "volumetric-light", "Volumetric light" },
            { "bloom", "Bloom" },
            { "motion-blur", "Motion blur" },
            { "auto-exposure", "Auto exposure" },
            { "dlss", "DLSS" },
            { "dlss-quality", "DLSS quality" },
            { "dlss-sharpness", "DLSS sharpness" },
            { "native-fg", "DLSS frame generation" },
            { "fsr", "FSR3" },
            { "fsr-quality", "FSR3 quality" },
            { "fsr-fg", "FSR3 frame generation" },
            { "fsr-legacy", "Legacy FSR" },
            { "xess", "XeSS" },
            { "xess-quality", "XeSS quality" },
            { "xess-fg", "XeSS frame generation" },
            { "ray-tracing", "Ray tracing" },
            { "ray-quality", "Ray tracing quality" },
            { "ray-gi", "Ray traced GI" },
            { "ray-reflections", "Ray traced reflections" },
            { "ray-shadows", "Ray traced shadows" },
            { "brightness", "Brightness" },
            { "saturation", "Saturation" },
            { "contrast", "Contrast" },
            { "image-display", "Image display" },
            { "eye-mode", "Eye protection mode" },
            { "eye-temperature", "Eye temperature" },
            { "eye-strength", "Eye protection strength" },
            { "eye-brightness", "Eye protection brightness" },
            { "eye-texture", "Eye protection texture" },
          };
          for (const auto &[key, label] : extra) out.push_back(field(key, "slider", "quality", { { "min", 0 }, { "max", 100 }, { "step", 1 }, { "decimals", 0 }, { "labelEn", label } }));
        }
      }
      if (game == "starrail") {
        for (auto &item : out)
          if (item.is_object() && item.value("key", std::string()) == "fps") {
            // Star Rail's own menu on PC has no 120: Collapse writes FPS=120 into the graphics model, and the game puts it back the next time its frame rate is changed in game
            for (auto &option : item["options"])
              if (option.is_object() && option.value("value", 0) == 120) {
                option["label"] = "120（解锁）";
                option["labelEn"] = "120 (unlock)";
              }
            item["note"] = "120 由 Sunshine 写入；之后在游戏内改帧率会掉回去，请在这里改。";
            item["noteEn"] = "120 is written by Sunshine. Changing the in-game frame rate later drops it back; change it here.";
          }
      }
      for (const auto &item : hoyo_catalog(game)) out.push_back(item);
      if (game == "zzz" && zzz_file_settings)
        for (const auto &item : zzz_file_catalog()) out.push_back(item);
      return out;
    }

    json
    launch_settings_of(std::string_view game) {
      json out = json::array();
      auto add = [&](const char *key, const char *kind, json extra = json::object()) { out.push_back(field(key, kind, "launch", std::move(extra))); };
      add("launchWidth", "slider", { { "min", 1280 }, { "max", 7680 }, { "step", 1 }, { "decimals", 0 }, { "default", 0 }, { "unit", " px" }, { "label", "启动宽度" }, { "labelEn", "Launch width" } });
      add("launchHeight", "slider", { { "min", 720 }, { "max", 4320 }, { "step", 1 }, { "decimals", 0 }, { "default", 0 }, { "unit", " px" }, { "label", "启动高度" }, { "labelEn", "Launch height" } });
      add("launchFullscreen", "toggle", { { "default", true }, { "label", "启动时全屏" }, { "labelEn", "Fullscreen at launch" } });
      add("popupWindow", "toggle", { { "default", false }, { "label", "无边框窗口参数" }, { "labelEn", "Popup-window argument" } });
      add("dxVersion", "choice", { { "options", json::array({ labeled("auto", "自动", "Auto"), labeled("dx11", "DirectX 11", "DirectX 11"), labeled("dx12", "DirectX 12", "DirectX 12") }) }, { "default", "auto" }, { "label", "图形 API" }, { "labelEn", "Graphics API" } });
      if (game == "arknights" || game == "endfield") add("launcherParent", "choice", { { "options", json::array({ labeled("official", "官方启动器", "Official launcher") }) }, { "default", "official" }, { "label", "启动父进程" }, { "labelEn", "Launch parent" } });
      return out;
    }

    json
    sparxie_fields_of(const std::string_view game) {
      json out = json::array();
      out.push_back(field("input", "choice", "input", { { "options", json::array({ option("touch"), option("kbm") }) }, { "default", "touch" } }));
      if (game == "endfield") {
        // the session's resolution: the stream's own size, or a fixed one
        out.push_back(field("resolution", "choice", "advanced", { { "options", json::array({ option("auto"), option("1280×720"), option("1920×1080"), option("2560×1440"), option("3840×2160") }) }, { "default", "auto" } }));
      }
      if (game == "zzz") return out;
      json tiers = json::array();
      for (int tier : { 60, 90, 120, 144, 165, 240 }) tiers.push_back(option(tier));
      tiers.push_back(option("display"));
      out.push_back(field("fpsUnlock", "toggle", "fps", { { "default", true } }));
      out.push_back(field("fps", "choice", "fps", { { "options", tiers }, { "default", 120 }, { "enabledBy", "fpsUnlock" } }));
      out.push_back(field("bgLimit", "toggle", "fps", { { "default", true } }));
      out.push_back(field("bgFps", "slider", "fps", { { "min", 10 }, { "max", 1000 }, { "step", 1 }, { "decimals", 0 }, { "scale", "log" }, { "marks", json::array({ 10, 30, 60 }) }, { "unit", " FPS" }, { "default", 10 }, { "enabledBy", "bgLimit" } }));
      out.push_back(field("priority", "choice", "advanced", { { "options", json::array({ option("belowNormal"), option("normal"), option("aboveNormal"), option("high") }) }, { "default", "normal" } }));
      if (game == "genshin") {
        json preset30 = json::array(), preset60 = json::array();
        for (int tier : { 60, 90, 120, 144 }) preset30.push_back(option(tier));
        for (int tier : { 120, 144, 165, 240, 1000 }) preset60.push_back(option(tier));
        out.push_back(field("followPreset", "toggle", "advanced", { { "default", false } }));
        out.push_back(field("preset30", "choice", "advanced", { { "options", preset30 }, { "default", 60 }, { "enabledBy", "followPreset" } }));
        out.push_back(field("preset60", "choice", "advanced", { { "options", preset60 }, { "default", 1000 }, { "enabledBy", "followPreset" } }));
        out.push_back(field("scaleOverride", "toggle", "ui", { { "default", false }, { "inputs", json::array({ "touch" }) } }));
        out.push_back(field("scale", "slider", "ui", { { "min", 100 }, { "max", 500 }, { "step", 5 }, { "decimals", 0 }, { "marks", json::array({ 100, 250, 400, 500 }) }, { "unit", "%" }, { "default", 400 }, { "enabledBy", "scaleOverride" }, { "inputs", json::array({ "touch" }) } }));
      }
      return out;
    }
  }  // namespace

  std::vector<std::string>
  launch_fields(std::string_view game) {
    std::vector<std::string> out;
    for (const auto &item : launch_settings_of(game))
      if (item.is_object()) out.push_back(item.value("key", std::string()));
    return out;
  }

  json
  catalog_json(bool zzz_file_settings) {
    json games = json::array();
    for (const auto game : kGames) {
      const TitleRow *title = title_row(game);
      if (!title) continue;
      json regions = json::array();
      for (const auto region : kRegions) {
        const auto names = executables_for(game, region);
        if (names.empty()) continue;
        regions.push_back({ { "region", region }, { "exes", names }, { "name", region_display(region) }, { "nameEn", region_display_en(region) } });
      }
      // the title's names and whether Sparxie's touch launch supports it: the pages list the titles the host sends, so a new title is a new row here
      json settings = json::array();
      json sparxie_fields = json::array();
      // Described items for the titles whose value domains are verified.
      // The other titles keep the field names their settings adapter can
      // write (empty when this host has no sourced writer).
      if (game == "genshin" || game == "starrail" || game == "zzz" || game == "hi3" || game == "wuwa" || game == "nte") {
        settings = game_settings_of(game, game == "zzz" && zzz_file_settings);
        sparxie_fields = sparxie_fields_of(game);
      }
      else {
        settings = json(title->settings);
        // Endfield has no game-settings items (this host writes none of its settings) but it has Sparxie's own: input, frame rate, priority
        if (game == "endfield") sparxie_fields = sparxie_fields_of(game);
      }
      // the launch items (graphics API, arguments, priority) of the titles that have settings
      if (game != "endfield")
        for (const auto &item : launch_settings_of(game)) settings.push_back(item);
      // true when this host reads and writes some of the title's own settings; a title without them has no 游戏设置 items (the page says so once, it does not call it a game-version limit)
      games.push_back({ { "game", game }, { "name", title->name }, { "nameEn", title->name_en }, { "aliases", title->aliases }, { "touch", title->touch }, { "settings", settings }, { "settingsManaged", !title->settings.empty() }, { "sparxie", { { "fields", sparxie_fields } } }, { "exes", executables_for(game) }, { "regions", regions } });
    }
    return games;
  }

  IdentifyResult
  identify_install(const fs::path &input, const Registry &registry, const StorefrontEnv &stores, std::string_view source_fallback) {
    IdentifyResult result;
    std::error_code ec;
    fs::path path = fs::absolute(input, ec);
    if (ec) path = input;
    std::vector<fs::path> exes;
    fs::path dir = path;
    if (fs::is_regular_file(path, ec)) {
      dir = path.parent_path();
      if (rule_for(path.filename().string())) exes.push_back(path);
    }
    else if (fs::is_directory(path, ec)) {
      for (const auto &entry : fs::directory_iterator(path, ec)) {
        if (!entry.is_regular_file()) continue;
        if (rule_for(entry.path().filename().string())) exes.push_back(entry.path());
      }
    }
    std::set<std::string> games;
    for (const auto &exe : exes)
      if (const auto *rule = rule_for(exe.filename().string())) games.insert(rule->game);
    if (exes.empty() || games.empty()) {
      result.status = Status::fail(404, "unrecognised", "No known game executable in that location");
      return result;
    }
    if (games.size() > 1) {
      result.status = Status::fail(409, "ambiguous", "That folder contains more than one known game executable");
      return result;
    }
    const auto *rule = rule_for(exes.front().filename().string());
    Install &install = result.install;
    install.game = rule->game;
    install.exe = rule->file;
    install.exe_path = exes.front().string();
    // Some vendors keep the executable below the install root (Wuthering
    // Waves under Client\Binaries\Win64, NTE's engine build five levels
    // down); the install, its markers and its identity are the root.
    fs::path root = dir;
    for (int up = 0; up < rule->root_up; ++up)
      root = root.parent_path();
    install.dir = root.string();
    install.candidates = rule->candidates;
    add_marker(install, "exe");
    result.has_game = true;
    if (rule->data_dir && *rule->data_dir && fs::is_directory(root / rule->data_dir, ec)) add_marker(install, "dir");

    std::vector<std::string> registry_hits;
    for (const auto &hyp : hyp_registry_keys(registry)) {
      if (hyp.game != install.game) continue;
      const RegValue value = registry.get(hyp.key, "GameInstallPath");
      if (value.kind != RegValue::Kind::text || value.text.empty()) continue;
      if (!paths_equal(value.text, root)) continue;
      if (std::find(install.candidates.begin(), install.candidates.end(), hyp.region) == install.candidates.end()) continue;
      registry_hits.push_back(hyp.region);
      if (install.source != "bili") install.source = hyp.source;
    }
    if (!registry_hits.empty()) {
      add_marker(install, "registry");
      install.candidates = registry_hits;
    }
    {
      // Store builds are the international client: the Steam listing is the
      // global build (steam-zzz-store-20261111) and so is the Epic one, so an
      // install a store records is global.
      // The check runs after the registry on purpose: the launcher registry
      // also tracks a store install, and the store record is the truer answer
      // to where the install came from.
      std::string store;
      if (const int id = steam_app_id(install.game); id && !stores.steam_root.empty()) {
        if (const auto steam_dir = steam_install_dir(stores.steam_root, id); steam_dir && dir_at_or_below(root, *steam_dir)) store = "steam";
      }
      if (store.empty() && !stores.epic_manifests.empty()) {
        for (const auto &record : epic_records(stores.epic_manifests)) {
          if (dir_at_or_below(root, record.root)) {
            store = "epic";
            install.epic_app = record.app;
            break;
          }
        }
      }
      if (!store.empty()) {
        install.source = store;
        if (std::find(install.candidates.begin(), install.candidates.end(), "global") != install.candidates.end()) install.candidates = { "global" };
      }
    }
    VendorAnswer vendor;
    if (install.game == "wuwa")
      vendor = wuwa_vendor(root);
    else if (install.game == "nte")
      vendor = nte_vendor(root);
    else if (install.game == "arknights")
      vendor = arknights_vendor(root);
    else if (install.game == "endfield")
      vendor = endfield_vendor(root);
    // A launcher record of the install names its source when no store or
    // HoYoPlay record did.
    if (install.source.empty() && vendor.launcher_record) install.source = "official-launcher";
    if (install.source.empty() && rule->data_dir && *rule->data_dir) {
      const fs::path sdk = root / rule->data_dir / "Plugins" / "PCGameSDK.dll";
      if (fs::is_regular_file(sdk, ec) &&
          std::find(install.candidates.begin(), install.candidates.end(), "bili") != install.candidates.end()) {
        add_marker(install, "sdk");
        install.candidates = { "bili" };
      }
    }
    // The vendor launcher's own record: Wuthering Waves and NTE name their
    // channel in the launcher's file, Endfield in its sdkdata folder, the
    // Yostar builds of Arknights in game-launcher-config.json.
    if (!vendor.region.empty() && std::find(install.candidates.begin(), install.candidates.end(), vendor.region) != install.candidates.end()) {
      install.candidates = { vendor.region };
      for (const auto &marker : vendor.markers)
        if (!marker.empty()) add_marker(install, marker);
    }
    const IniGeneral ini = read_config_ini(root / "config.ini");
    if (!ini.version.empty()) install.version = ini.version;
    if (ini.channel >= 0) {
      install.channel = ini.channel;
      install.sub_channel = ini.sub_channel;
    }
    bool ini_used = false;
    if (!ini.biz.empty()) {
      for (const auto game : kGames)
        for (const auto region : kRegions)
          if (const RegionRow *row = region_row(game, region); row && row->biz == ini.biz && row->game == install.game &&
                                                               std::find(install.candidates.begin(), install.candidates.end(), row->region) != install.candidates.end()) {
            install.candidates = { row->region };
            ini_used = true;
          }
    }
    if (!ini_used && ini.channel >= 0) {
      // A channel pair narrows only when exactly one region of this game
      // writes it: both Honkai Impact 3rd clients write 1/1, so that pair
      // must leave the region open instead of picking the last match.
      std::set<std::string> channel_regions;
      for (const auto game : kGames)
        for (const auto region : kRegions)
          if (const RegionRow *row = region_row(game, region); row && row->game == install.game && row->channel == ini.channel &&
                                                               row->sub_channel == ini.sub_channel &&
                                                               std::find(install.candidates.begin(), install.candidates.end(), row->region) != install.candidates.end()) {
            channel_regions.insert(row->region);
          }
      if (channel_regions.size() == 1) {
        install.candidates = { *channel_regions.begin() };
        ini_used = true;
      }
    }
    if (!ini_used && !ini.cps.empty()) {
      const bool international = ini.cps == "hyp_hoyoverse";
      const bool national = ini.cps == "hyp_mihoyo";
      if (international || national) {
        std::vector<std::string> narrowed;
        for (const auto &candidate : install.candidates) {
          const bool is_global = candidate == "global";
          if (international == is_global) narrowed.push_back(candidate);
        }
        if (narrowed.size() != install.candidates.size()) {
          install.candidates = std::move(narrowed);
          ini_used = true;
        }
      }
    }
    if (ini_used) add_marker(install, "ini");
    if (install.candidates.size() == 1)
      install.region = install.candidates[0];
    else
      install.region.clear();
    // A decided region does not need a candidate list. An open one keeps the rest.
    if (!install.region.empty()) install.candidates.clear();
    if (install.source.empty()) install.source = std::string(source_fallback);
    finish_id(install);
    // where it was found: the same directory seen by two sources is one install with both
    {
      const std::string where = norm_path(root);
      auto add_source = [&](const char *source) {
        if (std::find(install.sources.begin(), install.sources.end(), source) == install.sources.end()) install.sources.emplace_back(source);
      };
      if (std::find(install.markers.begin(), install.markers.end(), "registry") != install.markers.end()) add_source("hoyoplay");
      if (where.find("/steamapps/") != std::string::npos) add_source("steam");
      if (where.find("/epic games/") != std::string::npos) add_source("epic");
      if (std::find(install.markers.begin(), install.markers.end(), "sdk") != install.markers.end()) add_source("bili");
      if (vendor.launcher_record) add_source("official-launcher");
    }
    result.status = Status::success();
    return result;
  }
}  // namespace sparxie
