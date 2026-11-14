#include "src/sparxie/hoyo_settings.h"

#include "src/sparxie/region.h"
#include "src/sparxie/zzz_file.h"

#include <algorithm>
#include <optional>

namespace sparxie {
  namespace {
    char
    sep() { return static_cast<char>(92); }

    std::string
    registry_root(const Install &install) {
      const bool global = install.region == "global";
      std::string out = std::string("Software") + sep();
      if (install.game == "genshin") return out + "miHoYo" + sep() + (global ? "Genshin Impact" : "原神");
      if (install.game == "starrail") return global ? out + "Cognosphere" + sep() + "Star Rail" : out + "miHoYo" + sep() + "崩坏：星穹铁道";
      if (install.game == "hi3") return out + "miHoYo" + sep() + (global ? "Honkai Impact 3rd" : "崩坏3");
      return global ? out + "miHoYo" + sep() + "ZenlessZoneZero" : out + "miHoYo" + sep() + "绝区零";
    }

    bool
    hoyo_game(std::string_view game) { return game == "genshin" || game == "starrail" || game == "zzz" || game == "hi3"; }

    bool
    wanted(const std::set<std::string> &fields, const GameValues &values, const char *key) {
      if (!fields.empty()) return fields.count(key) > 0;
      return values.extra.contains(key);
    }

    void
    mark(SettingsResult &result, const std::string &field) {
      if (std::find(result.unavailable.begin(), result.unavailable.end(), field) == result.unavailable.end()) result.unavailable.push_back(field);
    }

    void
    applied(SettingsResult &result, const char *field) {
      if (std::find(result.applied.begin(), result.applied.end(), field) == result.applied.end()) result.applied.push_back(field);
    }

    std::string
    blob_text(const RegValue &value) {
      std::string text;
      if (value.kind == RegValue::Kind::text)
        text = value.text;
      else if (value.kind == RegValue::Kind::binary)
        text.assign(value.bytes.begin(), value.bytes.end());
      while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) {
        if (text.back() != '\0' && text.back() != ' ') break;
        if (text.back() == ' ' && text.find('\0') == std::string::npos) break;
        if (text.back() == '\0')
          text.pop_back();
        else
          break;
      }
      while (!text.empty() && text.back() == '\0') text.pop_back();
      return text;
    }

    enum class Blob { missing,
      bad,
      ok };

    Blob
    load_object(const RegValue &value, json &out) {
      if (value.kind != RegValue::Kind::binary && value.kind != RegValue::Kind::text) return Blob::missing;
      const std::string text = blob_text(value);
      if (text.empty()) return Blob::missing;
      try {
        out = json::parse(text);
      }
      catch (const std::exception &) {
        return Blob::bad;
      }
      return out.is_object() ? Blob::ok : Blob::bad;
    }

    void
    store_object(Registry &registry, const std::string &root, const char *name, const json &body) {
      const std::string dumped = body.dump();
      std::vector<std::uint8_t> bytes(dumped.begin(), dumped.end());
      bytes.push_back(0);
      registry.set_binary(root, name, bytes);
    }

    json
    embedded(const json &root, const char *key) {
      if (!root.contains(key)) return {};
      if (root[key].is_object()) return root[key];
      if (!root[key].is_string()) return {};
      try {
        return json::parse(root[key].get<std::string>());
      }
      catch (const std::exception &) {
        return {};
      }
    }

    void
    store_embedded(json &root, const char *key, const json &value) {
      if (root.contains(key) && root[key].is_object())
        root[key] = value;
      else
        root[key] = value.dump();
    }

    struct Lab {
      const char *value;
      const char *zh;
      const char *en;
    };

    json
    choice_field(const char *key, const char *section, const char *zh, const char *en, const char *def, const Lab *labs, int count, const char *enabled = nullptr) {
      json options = json::array();
      for (int i = 0; i < count; ++i) options.push_back({ { "value", labs[i].value }, { "label", labs[i].zh }, { "labelEn", labs[i].en } });
      json field = { { "key", key }, { "kind", "choice" }, { "section", section }, { "label", zh }, { "labelEn", en }, { "def", def }, { "options", options } };
      if (enabled) field["enabledBy"] = enabled;
      return field;
    }

    json
    toggle_field(const char *key, const char *section, const char *zh, const char *en, bool def, const char *enabled = nullptr) {
      json field = { { "key", key }, { "kind", "toggle" }, { "section", section }, { "label", zh }, { "labelEn", en }, { "def", def } };
      if (enabled) field["enabledBy"] = enabled;
      return field;
    }

    json
    slider_field(const char *key, const char *section, const char *zh, const char *en, int min, int max, int def, const char *unit = nullptr, const char *enabled = nullptr) {
      json field = { { "key", key }, { "kind", "slider" }, { "section", section }, { "label", zh }, { "labelEn", en }, { "min", min }, { "max", max }, { "step", 1 }, { "decimals", 0 }, { "def", def } };
      if (unit) field["unit"] = unit;
      if (enabled) field["enabledBy"] = enabled;
      return field;
    }

    const Lab kApi[] = {
      { "10.1", "DirectX 11 10.1", "DirectX 11 10.1" },
      { "11.0", "DirectX 11", "DirectX 11" },
      { "11.1", "DirectX 11 11.1", "DirectX 11 11.1" },
      { "11.1ns", "DirectX 11 11.1（多线程关闭）", "DirectX 11 11.1, no single thread" },
      { "12", "DirectX 12", "DirectX 12" },
    };

    int
    api_index(const std::string &token) {
      for (int i = 0; i < 5; ++i)
        if (token == kApi[i].value) return i;
      return -1;
    }

    const char *
    api_token(int index) { return index >= 0 && index <= 4 ? kApi[index].value : kApi[3].value; }

    const Lab kOnOff[] = { { "off", "关", "Off" }, { "on", "开", "On" } };
    const Lab kLowHigh[] = { { "low", "低", "Low" }, { "high", "高", "High" } };
    const Lab k3[] = { { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const Lab k4[] = { { "lowest", "最低", "Lowest" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const Lab k5[] = { { "lowest", "最低", "Lowest" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" }, { "highest", "极高", "Highest" } };
    const Lab kOff4[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const Lab kQual[] = { { "off", "关", "Off" }, { "verylow", "最低", "Very low" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" }, { "veryhigh", "最高", "Very high" } };

    int
    lab_index(const Lab *labs, int count, const std::string &token) {
      for (int i = 0; i < count; ++i)
        if (token == labs[i].value) return i;
      return -1;
    }

    const char *
    lab_token(const Lab *labs, int count, int index, int fallback) {
      if (index < 0 || index >= count) index = fallback;
      return labs[index].value;
    }

    bool
    quote_args(std::string_view raw, std::string &out) {
      out.clear();
      if (raw.empty()) return true;
      auto ok = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' || c == ':' || c == '=' || c == '+' || c == '-';
      };
      for (std::size_t i = 0; i < raw.size();) {
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\r' || raw[i] == '\n')) ++i;
        if (i >= raw.size()) break;
        std::string token;
        if (raw[i] == '"') {
          ++i;
          while (i < raw.size() && raw[i] != '"') {
            if (!ok(static_cast<unsigned char>(raw[i]))) return false;
            token.push_back(raw[i++]);
          }
          if (i >= raw.size() || raw[i] != '"') return false;
          ++i;
        }
        else {
          while (i < raw.size() && raw[i] != ' ' && raw[i] != '\t' && raw[i] != '\r' && raw[i] != '\n') {
            if (!ok(static_cast<unsigned char>(raw[i]))) return false;
            token.push_back(raw[i++]);
          }
        }
        if (token.empty()) return false;
        if (!out.empty()) out.push_back(' ');
        out.push_back('"');
        out += token;
        out.push_back('"');
      }
      return true;
    }

    json
    screen_catalog() {
      json out = json::array();
      out.push_back(toggle_field("exclusiveFullscreen", "display", "独占全屏", "Exclusive fullscreen", false));
      out.push_back(toggle_field("borderless", "display", "无边框窗口", "Borderless window", false));
      out.push_back(toggle_field("resizable", "display", "可调整大小的窗口", "Resizable window", false));
      out.push_back(toggle_field("customResolution", "display", "自定义分辨率", "Custom resolution", false));
      out.push_back(slider_field("customWidth", "display", "自定义宽度", "Custom width", 640, 7680, 1920, " px", "customResolution"));
      out.push_back(slider_field("customHeight", "display", "自定义高度", "Custom height", 480, 4320, 1080, " px", "customResolution"));
      out.push_back(choice_field("graphicsApi", "launch", "图形 API", "Graphics API", "11.1ns", kApi, 5));
      out.push_back(toggle_field("gameBoost", "launch", "提升游戏优先级", "Raise game priority", false));
      json args = { { "key", "customArgs" }, { "kind", "text" }, { "section", "launch" }, { "label", "自定义启动参数" }, { "labelEn", "Custom launch arguments" }, { "def", "" } };
      out.push_back(std::move(args));
      return out;
    }

    const char *kScreen = "CollapseLauncher_ScreenSetting";
    const char *kMisc = "CollapseLauncher_Misc";
    const char *kArgs = "CollapseLauncher_CustomArgs";

    void
    read_screen(const std::string &root, Registry &registry, SettingsResult &result) {
      json body;
      const Blob blob = load_object(registry.get(root, kScreen), body);
      if (blob == Blob::bad) {
        for (const char *key : { "exclusiveFullscreen", "borderless", "resizable", "customResolution", "customWidth", "customHeight", "graphicsApi" }) mark(result, key);
        return;
      }
      const bool present = blob == Blob::ok;
      result.values.extra["exclusiveFullscreen"] = present && body.value("UseExclusiveFullscreen", false);
      result.values.extra["borderless"] = present && body.value("UseBorderlessScreen", false);
      result.values.extra["resizable"] = present && body.value("UseResizableWindow", false);
      result.values.extra["customResolution"] = present && body.value("UseCustomResolution", false);
      result.values.extra["graphicsApi"] = api_token(present ? body.value("GameGraphicsAPI", 3) : 3);
      json misc;
      const Blob misc_blob = load_object(registry.get(root, kMisc), misc);
      if (misc_blob == Blob::bad)
        mark(result, "gameBoost");
      else
        result.values.extra["gameBoost"] = misc_blob == Blob::ok && misc.value("UseGameBoost", false);
      const RegValue args = registry.get(root, kArgs);
      std::string text = blob_text(args);
      if (misc_blob == Blob::ok && misc.contains("UseCustomArguments") && misc["UseCustomArguments"].is_boolean() && !misc["UseCustomArguments"].get<bool>()) text.clear();
      result.values.extra["customArgs"] = text;
    }

    void
    write_screen(const std::string &root, Registry &registry, const GameValues &values, const std::set<std::string> &fields, SettingsResult &result) {
      const char *screen_keys[] = { "exclusiveFullscreen", "borderless", "resizable", "customResolution", "graphicsApi" };
      bool screen = false;
      for (const char *key : screen_keys)
        if (wanted(fields, values, key)) screen = true;
      if (screen) {
        json body;
        const Blob blob = load_object(registry.get(root, kScreen), body);
        if (blob == Blob::bad) {
          for (const char *key : screen_keys) mark(result, key);
        }
        else {
          if (!body.is_object()) body = json::object();
          if (wanted(fields, values, "exclusiveFullscreen")) body["UseExclusiveFullscreen"] = values.extra.value("exclusiveFullscreen", false);
          if (wanted(fields, values, "borderless")) body["UseBorderlessScreen"] = values.extra.value("borderless", false);
          if (wanted(fields, values, "resizable")) body["UseResizableWindow"] = values.extra.value("resizable", false);
          if (wanted(fields, values, "customResolution")) body["UseCustomResolution"] = values.extra.value("customResolution", false);
          if (wanted(fields, values, "graphicsApi") && values.extra.contains("graphicsApi")) body["GameGraphicsAPI"] = api_index(values.extra.value("graphicsApi", std::string("11.1ns")));
          if (!body.contains("GameGraphicsAPI")) body["GameGraphicsAPI"] = 3;
          store_object(registry, root, kScreen, body);
          for (const char *key : screen_keys)
            if (wanted(fields, values, key)) applied(result, key);
        }
      }
      if (wanted(fields, values, "customWidth") && values.extra.contains("customWidth")) {
        registry.set_dword(root, "Screenmanager Resolution Width_h182942802", static_cast<std::uint32_t>(values.extra.value("customWidth", 1920)));
        applied(result, "customWidth");
      }
      if (wanted(fields, values, "customHeight") && values.extra.contains("customHeight")) {
        registry.set_dword(root, "Screenmanager Resolution Height_h2627697771", static_cast<std::uint32_t>(values.extra.value("customHeight", 1080)));
        applied(result, "customHeight");
      }
      if (wanted(fields, values, "gameBoost") || wanted(fields, values, "customArgs")) {
        json misc;
        const Blob blob = load_object(registry.get(root, kMisc), misc);
        if (blob == Blob::bad) {
          if (wanted(fields, values, "gameBoost")) mark(result, "gameBoost");
          if (wanted(fields, values, "customArgs")) mark(result, "customArgs");
        }
        else {
          if (!misc.is_object()) misc = json::object();
          if (wanted(fields, values, "gameBoost")) {
            misc["UseGameBoost"] = values.extra.value("gameBoost", false);
            applied(result, "gameBoost");
          }
          if (wanted(fields, values, "customArgs") && values.extra.contains("customArgs")) {
            const std::string raw = values.extra.value("customArgs", std::string());
            std::string quoted;
            quote_args(raw, quoted);
            misc["UseCustomArguments"] = !raw.empty();
            registry.set_text(root, kArgs, raw);
            applied(result, "customArgs");
          }
          store_object(registry, root, kMisc, misc);
        }
      }
    }

    struct Grade {
      const char *key;
      const char *zh;
      const char *en;
      int type;
      const Lab *labs;
      int count;
      int def;
    };

    const Lab kGiAa[] = { { "off", "关", "Off" }, { "fsr2", "FSR 2", "FSR 2" }, { "smaa", "SMAA", "SMAA" } };
    const Lab kGiMotion[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "high", "高", "High" }, { "extreme", "极高", "Extreme" } };
    const Lab kGiGi[] = { { "off", "关", "Off" }, { "medium", "中", "Medium" }, { "high", "高", "High" }, { "extreme", "极高", "Extreme" } };
    const Lab kGiSub[] = { { "off", "关", "Off" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const Lab kGiCoop[] = { { "off", "关", "Off" }, { "partial", "部分关闭", "Partially off" }, { "on", "开", "On" } };
    const Lab kGiCrowd[] = { { "low", "低", "Low" }, { "high", "高", "High" } };
    const Lab kAniso[] = { { "1", "1x", "1x" }, { "2", "2x", "2x" }, { "4", "4x", "4x" }, { "8", "8x", "8x" }, { "16", "16x", "16x" } };
    const Lab kGiText[] = {
      { "en", "English", "English" },
      { "zh-cn", "简体中文", "Simplified Chinese" },
      { "zh-tw", "繁體中文", "Traditional Chinese" },
      { "fr", "Français", "French" },
      { "de", "Deutsch", "German" },
      { "es", "Español", "Spanish" },
      { "pt", "Português", "Portuguese" },
      { "ru", "Русский", "Russian" },
      { "ja", "日本語", "Japanese" },
      { "ko", "한국어", "Korean" },
      { "th", "ภาษาไทย", "Thai" },
      { "vi", "Tiếng Việt", "Vietnamese" },
      { "id", "Bahasa Indonesia", "Indonesian" },
      { "tr", "Türkçe", "Turkish" },
      { "it", "Italiano", "Italian" },
    };
    const Lab kGiVoice[] = { { "zh-cn", "中文", "Chinese" }, { "en", "English", "English" }, { "ja", "日本語", "Japanese" }, { "ko", "한국어", "Korean" } };

    const Grade kGenshin[] = {
      { "shadow", "阴影质量", "Shadow quality", 3, k4, 4, 3 },
      { "effects", "特效质量", "Effects quality", 4, k4, 4, 3 },
      { "sfxQuality", "场景特效", "SFX quality", 5, k4, 4, 3 },
      { "environment", "环境细节", "Environment detail", 6, k5, 5, 3 },
      { "aa", "抗锯齿", "Anti-aliasing", 8, kGiAa, 3, 1 },
      { "fog", "体积雾", "Volumetric fog", 9, kOnOff, 2, 1 },
      { "reflections", "反射", "Reflections", 10, kOnOff, 2, 1 },
      { "motionBlur", "动态模糊", "Motion blur", 11, kGiMotion, 4, 3 },
      { "bloom", "泛光", "Bloom", 12, kOnOff, 2, 1 },
      { "crowd", "人群密度", "Crowd density", 13, kGiCrowd, 2, 1 },
      { "subsurface", "次表面散射", "Subsurface scattering", 15, kGiSub, 3, 2 },
      { "coop", "联机队友特效", "Co-op teammate effects", 16, kGiCoop, 3, 2 },
      { "anisotropic", "各向异性过滤", "Anisotropic filtering", 17, kAniso, 5, 3 },
      { "gi", "全局光照", "Global illumination", 19, kGiGi, 4, 2 },
      { "dynamicCharacter", "角色动态分辨率", "Dynamic character resolution", 21, kOnOff, 2, 1 },
    };

    int
    item_index(const json &items, int type, int fallback) {
      if (!items.is_array()) return fallback;
      for (const auto &item : items)
        if (item.is_object() && item.value("entryType", -1) == type) return item.value("index", fallback);
      return fallback;
    }

    void
    upsert_item(json &items, int type, int index) {
      if (!items.is_array()) items = json::array();
      for (auto &item : items) {
        if (item.is_object() && item.value("entryType", -1) == type) {
          item["index"] = index;
          return;
        }
      }
      items.push_back({ { "entryType", type }, { "index", index }, { "itemVersion", "" } });
    }

    void
    upsert_grade(json &grades, int type, int index) {
      if (!grades.is_array()) grades = json::array();
      const int stored = index + 1;
      for (auto &grade : grades) {
        if (grade.value("key", -1) == type) {
          grade["value"] = stored;
          return;
        }
      }
      grades.push_back({ { "key", type }, { "value", stored } });
    }

    json
    genshin_catalog() {
      json out = screen_catalog();
      for (const Grade &grade : kGenshin) out.push_back(choice_field(grade.key, "quality", grade.zh, grade.en, grade.labs[grade.def].value, grade.labs, grade.count));
      out.push_back(slider_field("masterVolume", "audio", "总音量", "Master volume", 0, 10, 10));
      out.push_back(slider_field("musicVolume", "audio", "音乐音量", "Music volume", 0, 10, 10));
      out.push_back(slider_field("sfxVolume", "audio", "音效音量", "SFX volume", 0, 10, 10));
      out.push_back(slider_field("voiceVolume", "audio", "语音音量", "Voice volume", 0, 10, 10));
      out.push_back(choice_field("audioLanguage", "language", "语音", "Audio language", "en", kGiVoice, 4));
      out.push_back(choice_field("textLanguage", "language", "文本", "Text language", "en", kGiText, 15));
      return out;
    }

    const char *kGeneral = "GENERAL_DATA_h2389025596";

    void
    read_genshin(const std::string &root, Registry &registry, SettingsResult &result) {
      json general;
      const Blob blob = load_object(registry.get(root, kGeneral), general);
      if (blob != Blob::ok) {
        for (const Grade &grade : kGenshin) mark(result, grade.key);
        for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume", "audioLanguage", "textLanguage" }) mark(result, key);
        return;
      }
      const json perf = embedded(general, "globalPerfData");
      const json items = perf.is_object() ? perf.value("saveItems", json::array()) : json::array();
      for (const Grade &grade : kGenshin) result.values.extra[grade.key] = lab_token(grade.labs, grade.count, item_index(items, grade.type, grade.def), grade.def);
      result.values.extra["masterVolume"] = general.value("volumeGlobal", 10);
      result.values.extra["musicVolume"] = general.value("volumeMusic", 10);
      result.values.extra["sfxVolume"] = general.value("volumeSFX", 10);
      result.values.extra["voiceVolume"] = general.value("volumeVoice", 10);
      result.values.extra["audioLanguage"] = lab_token(kGiVoice, 4, general.value("deviceVoiceLanguageType", 1), 1);
      const int text = general.value("deviceLanguageType", 1);
      result.values.extra["textLanguage"] = lab_token(kGiText, 15, text - 1, 0);
    }

    void
    write_genshin(const std::string &root, Registry &registry, const GameValues &values, const std::set<std::string> &fields, SettingsResult &result) {
      bool need = false;
      for (const Grade &grade : kGenshin)
        if (wanted(fields, values, grade.key)) need = true;
      for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume", "audioLanguage", "textLanguage" })
        if (wanted(fields, values, key)) need = true;
      if (!need) return;
      json general;
      const Blob blob = load_object(registry.get(root, kGeneral), general);
      if (blob != Blob::ok) {
        for (const Grade &grade : kGenshin)
          if (wanted(fields, values, grade.key)) mark(result, grade.key);
        for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume", "audioLanguage", "textLanguage" })
          if (wanted(fields, values, key)) mark(result, key);
        return;
      }
      json perf = embedded(general, "globalPerfData");
      if (!perf.is_object()) perf = json::object();
      json items = perf.value("saveItems", json::array());
      json graphics = embedded(general, "graphicsData");
      json grades = graphics.is_object() ? graphics.value("customVolatileGrades", json::array()) : json::array();
      bool grades_touched = false;
      for (const Grade &grade : kGenshin) {
        if (!wanted(fields, values, grade.key) || !values.extra.contains(grade.key)) continue;
        const int index = lab_index(grade.labs, grade.count, values.extra.value(grade.key, std::string()));
        if (index < 0) continue;
        upsert_item(items, grade.type, index);
        if (graphics.is_object() || general.contains("graphicsData")) {
          upsert_grade(grades, grade.type, index);
          grades_touched = true;
        }
        applied(result, grade.key);
      }
      perf["saveItems"] = items;
      store_embedded(general, "globalPerfData", perf);
      if (grades_touched) {
        if (!graphics.is_object()) graphics = json::object();
        graphics["customVolatileGrades"] = grades;
        store_embedded(general, "graphicsData", graphics);
      }
      auto volume = [&](const char *key, const char *json_key) {
        if (!wanted(fields, values, key) || !values.extra.contains(key)) return;
        general[json_key] = values.extra.value(key, 10);
        applied(result, key);
      };
      volume("masterVolume", "volumeGlobal");
      volume("musicVolume", "volumeMusic");
      volume("sfxVolume", "volumeSFX");
      volume("voiceVolume", "volumeVoice");
      if (wanted(fields, values, "audioLanguage") && values.extra.contains("audioLanguage")) {
        const int index = lab_index(kGiVoice, 4, values.extra.value("audioLanguage", std::string("en")));
        if (index >= 0) {
          general["deviceVoiceLanguageType"] = index;
          applied(result, "audioLanguage");
        }
      }
      if (wanted(fields, values, "textLanguage") && values.extra.contains("textLanguage")) {
        const int index = lab_index(kGiText, 15, values.extra.value("textLanguage", std::string("en")));
        if (index >= 0) {
          general["deviceLanguageType"] = index + 1;
          applied(result, "textLanguage");
        }
      }
      store_object(registry, root, kGeneral, general);
    }

    const Lab kSrAa[] = { { "off", "关", "Off" }, { "taa", "TAA", "TAA" }, { "fxaa", "FXAA", "FXAA" } };
    const Lab kSrDlss[] = { { "off", "禁用", "Off" }, { "ultraperf", "超级性能", "Ultra performance" }, { "performance", "性能", "Performance" }, { "balanced", "均衡", "Balanced" }, { "quality", "质量", "Quality" }, { "dlaa", "DLAA", "DLAA" } };
    const Lab kSrAudio[] = { { "en", "英语", "English" }, { "jp", "日语", "Japanese" }, { "cn", "汉语", "Chinese" }, { "kr", "韩语", "Korean" } };
    const Lab kSrText[] = {
      { "en", "English", "English" },
      { "jp", "日本語", "Japanese" },
      { "cn", "简体中文", "Simplified Chinese" },
      { "cht", "繁體中文", "Traditional Chinese" },
      { "kr", "한국어", "Korean" },
      { "es", "Español", "Spanish" },
      { "ru", "Русский", "Russian" },
      { "th", "ภาษาไทย", "Thai" },
      { "vi", "Tiếng Việt", "Vietnamese" },
      { "id", "Bahasa Indonesia", "Indonesian" },
      { "fr", "Français", "French" },
      { "de", "Deutsch", "German" },
      { "pt", "Português", "Portuguese" },
    };

    struct SrField {
      const char *key;
      const char *json_key;
      const char *zh;
      const char *en;
      const Lab *labs;
      int count;
      int def;
      bool boolean;
    };

    const SrField kStar[] = {
      { "shadow", "ShadowQuality", "阴影质量", "Shadow quality", kQual, 6, 0, false },
      { "light", "LightQuality", "光照质量", "Lighting quality", kQual, 6, 1, false },
      { "character", "CharacterQuality", "角色质量", "Character quality", kQual, 6, 2, false },
      { "environment", "EnvDetailQuality", "场景细节", "Scene detail", kQual, 6, 1, false },
      { "reflection", "ReflectionQuality", "反射质量", "Reflection quality", kQual, 6, 1, false },
      { "effects", "SFXQuality", "特效质量", "Effects quality", kQual, 6, 4, false },
      { "bloom", "BloomQuality", "泛光效果", "Bloom", kQual, 6, 0, false },
      { "aa", "AAMode", "抗锯齿模式", "Anti-aliasing", kSrAa, 3, 0, false },
      { "dlss", "DlssQuality", "NVIDIA DLSS", "NVIDIA DLSS", kSrDlss, 6, 0, false },
      { "resolutionQuality", "ResolutionQuality", "分辨率质量", "Resolution quality", kQual, 6, 0, false },
    };

    const char *kModel = "GraphicsSettings_Model_h2986158309";
    const char *kQuality = "GraphicsSettings_GraphicsQuality_h523255858";

    json
    starrail_catalog() {
      json out = screen_catalog();
      for (const SrField &field : kStar) out.push_back(choice_field(field.key, "quality", field.zh, field.en, field.labs[field.def].value, field.labs, field.count));
      out.push_back(toggle_field("selfShadow", "quality", "地图探索中角色实时阴影", "Real-time character shadow in exploration", false));
      out.push_back(toggle_field("halfResTransparent", "quality", "半分辨率透明", "Half-resolution transparency", false));
      out.push_back(slider_field("masterVolume", "audio", "总音量", "Master volume", 0, 10, 10));
      out.push_back(slider_field("musicVolume", "audio", "音乐音量", "Music volume", 0, 10, 10));
      out.push_back(slider_field("sfxVolume", "audio", "音效音量", "SFX volume", 0, 10, 10));
      out.push_back(slider_field("voiceVolume", "audio", "语音音量", "Voice volume", 0, 10, 10));
      out.push_back(choice_field("audioLanguage", "language", "音频", "Audio", "en", kSrAudio, 4));
      out.push_back(choice_field("textLanguage", "language", "文本", "Text", "en", kSrText, 13));
      return out;
    }

    int
    dword_or(const RegValue &value, int fallback) { return value.kind == RegValue::Kind::dword ? static_cast<int>(value.dword) : fallback; }

    void
    read_starrail(const std::string &root, Registry &registry, SettingsResult &result) {
      json model;
      const Blob blob = load_object(registry.get(root, kModel), model);
      if (blob != Blob::ok) {
        for (const SrField &field : kStar) mark(result, field.key);
        mark(result, "selfShadow");
        mark(result, "halfResTransparent");
      }
      else {
        for (const SrField &field : kStar) result.values.extra[field.key] = lab_token(field.labs, field.count, model.value(field.json_key, field.def), field.def);
        const int shadow = model.value("EnableSelfShadow", 0);
        result.values.extra["selfShadow"] = shadow == 1;
        result.values.extra["halfResTransparent"] = model.value("EnableHalfResTransparent", false);
      }
      result.values.extra["masterVolume"] = dword_or(registry.get(root, "AudioSettings_MasterVolume_h1622207037"), 10);
      result.values.extra["musicVolume"] = dword_or(registry.get(root, "AudioSettings_BGMVolume_h240914409"), 10);
      result.values.extra["sfxVolume"] = dword_or(registry.get(root, "AudioSettings_SFXVolume_h2753520268"), 10);
      result.values.extra["voiceVolume"] = dword_or(registry.get(root, "AudioSettings_VOVolume_h805685304"), 10);
      std::string audio = blob_text(registry.get(root, "LanguageSettings_LocalAudioLanguage_h882585060"));
      if (lab_index(kSrAudio, 4, audio) < 0) audio = "en";
      result.values.extra["audioLanguage"] = audio;
      std::string text = blob_text(registry.get(root, "LanguageSettings_LocalTextLanguage_h2764291023"));
      if (lab_index(kSrText, 13, text) < 0) text = "en";
      result.values.extra["textLanguage"] = text;
    }

    void
    store_lang(Registry &registry, const std::string &root, const char *name, const std::string &text) {
      std::vector<std::uint8_t> bytes(text.begin(), text.end());
      bytes.push_back(0);
      registry.set_binary(root, name, bytes);
    }

    void
    write_starrail(const std::string &root, Registry &registry, const GameValues &values, const std::set<std::string> &fields, SettingsResult &result) {
      bool model_need = wanted(fields, values, "selfShadow") || wanted(fields, values, "halfResTransparent");
      for (const SrField &field : kStar)
        if (wanted(fields, values, field.key)) model_need = true;
      if (model_need) {
        json model;
        const Blob blob = load_object(registry.get(root, kModel), model);
        if (blob != Blob::ok) {
          for (const SrField &field : kStar)
            if (wanted(fields, values, field.key)) mark(result, field.key);
          if (wanted(fields, values, "selfShadow")) mark(result, "selfShadow");
          if (wanted(fields, values, "halfResTransparent")) mark(result, "halfResTransparent");
        }
        else {
          bool wrote = false;
          for (const SrField &field : kStar) {
            if (!wanted(fields, values, field.key) || !values.extra.contains(field.key)) continue;
            const int index = lab_index(field.labs, field.count, values.extra.value(field.key, std::string()));
            if (index < 0) continue;
            model[field.json_key] = index;
            applied(result, field.key);
            wrote = true;
          }
          if (wanted(fields, values, "selfShadow") && values.extra.contains("selfShadow")) {
            model["EnableSelfShadow"] = values.extra.value("selfShadow", false) ? 1 : 2;
            applied(result, "selfShadow");
            wrote = true;
          }
          if (wanted(fields, values, "halfResTransparent") && values.extra.contains("halfResTransparent")) {
            model["EnableHalfResTransparent"] = values.extra.value("halfResTransparent", false);
            applied(result, "halfResTransparent");
            wrote = true;
          }
          if (wrote) {
            store_object(registry, root, kModel, model);
            registry.set_dword(root, kQuality, 0);
          }
        }
      }
      auto volume = [&](const char *key, const char *name) {
        if (!wanted(fields, values, key) || !values.extra.contains(key)) return;
        registry.set_dword(root, name, static_cast<std::uint32_t>(values.extra.value(key, 10)));
        applied(result, key);
      };
      volume("masterVolume", "AudioSettings_MasterVolume_h1622207037");
      volume("musicVolume", "AudioSettings_BGMVolume_h240914409");
      volume("sfxVolume", "AudioSettings_SFXVolume_h2753520268");
      volume("voiceVolume", "AudioSettings_VOVolume_h805685304");
      if (wanted(fields, values, "audioLanguage") && values.extra.contains("audioLanguage")) {
        store_lang(registry, root, "LanguageSettings_LocalAudioLanguage_h882585060", values.extra.value("audioLanguage", std::string("en")));
        applied(result, "audioLanguage");
      }
      if (wanted(fields, values, "textLanguage") && values.extra.contains("textLanguage")) {
        store_lang(registry, root, "LanguageSettings_LocalTextLanguage_h2764291023", values.extra.value("textLanguage", std::string("en")));
        applied(result, "textLanguage");
      }
    }

    const Lab kHiRes[] = { { "0.6", "0.6", "0.6" }, { "0.8", "0.8", "0.8" }, { "0.9", "0.9", "0.9" }, { "1.0", "1.0", "1.0" }, { "1.1", "1.1", "1.1" }, { "1.2", "1.2", "1.2" }, { "1.3", "1.3", "1.3" }, { "1.4", "1.4", "1.4" }, { "1.5", "1.5", "1.5" }, { "1.6", "1.6", "1.6" } };
    const char *kHiResName[] = { "Quality06", "Quality08", "Quality09", "Low", "Quality11", "Middle", "Quality13", "Quality14", "High", "VHigh" };
    const Lab kHiShadow[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" }, { "ultra", "极高", "Ultra" } };
    const char *kHiShadowName[] = { "DISABLED", "LOW", "MIDDLE", "HIGH", "ULTRA" };
    const Lab kHiRefl[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const char *kHiReflName[] = { "DISABLED", "LOW", "MIDDLE", "HIGH" };
    const Lab kHiLight[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" }, { "ultra", "极高", "Ultra" } };
    const char *kHiLightName[] = { "Off", "Low", "Middle", "High", "Ultra" };
    const Lab kHiPost[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const char *kHiPostName[] = { "Off", "Low", "Middle", "High" };
    const Lab kHiAa[] = { { "off", "关", "Off" }, { "fxaa", "FXAA", "FXAA" }, { "taa", "TAA", "TAA" } };
    const char *kHiAaName[] = { "Off", "FXAA", "TAA" };
    const Lab kHiChar[] = { { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const char *kHiCharName[] = { "Low", "Middle", "High" };
    const Lab kHiAo[] = { { "off", "关", "Off" }, { "low", "低", "Low" }, { "high", "高", "High" } };
    const char *kHiAoName[] = { "OFF", "LOW", "HIGH" };
    const Lab kHiVol[] = { { "low", "低", "Low" }, { "medium", "中", "Medium" }, { "high", "高", "High" } };
    const char *kHiVolName[] = { "Low", "Medium", "High" };
    const Lab kHiGrade[] = { { "low", "低", "Low" }, { "high", "高", "High" } };
    const char *kHiGradeName[] = { "Low", "High" };
    const Lab kHiGi[] = { { "low", "低", "Low" }, { "high", "高", "High" } };
    const char *kHiGiName[] = { "Low", "High" };
    const Lab kHiLod[] = { { "high", "高", "High" }, { "medium", "中", "Medium" }, { "low", "低", "Low" } };
    const Lab kHiVoice[] = { { "Japanese", "日语", "Japanese" }, { "Chinese(PRC)", "汉语", "Chinese" } };

    struct HiStr {
      const char *key;
      const char *json_key;
      const char *zh;
      const char *en;
      const Lab *labs;
      const char *const *stored;
      int count;
      int def;
    };

    const HiStr kHiStr[] = {
      { "resolutionQuality", "ResolutionQuality", "渲染精度", "Render scale", kHiRes, kHiResName, 10, 5 },
      { "shadow", "ShadowLevel", "阴影", "Shadow", kHiShadow, kHiShadowName, 5, 2 },
      { "reflection", "ReflectionQuality", "反射", "Reflection", kHiRefl, kHiReflName, 4, 1 },
      { "light", "LightingQuality", "光照", "Lighting", kHiLight, kHiLightName, 5, 1 },
      { "postFx", "PostFXQuality", "后期", "Post FX", kHiPost, kHiPostName, 4, 1 },
      { "aa", "AAType", "抗锯齿", "Anti-aliasing", kHiAa, kHiAaName, 3, 1 },
      { "character", "CharacterQuality", "角色", "Character", kHiChar, kHiCharName, 3, 0 },
      { "weather", "WeatherQuality", "天气", "Weather", kHiChar, kHiCharName, 3, 0 },
      { "particle", "ParticleEmitLevel", "粒子", "Particles", kHiChar, kHiCharName, 3, 0 },
      { "gi", "GlobalIllumination", "全局光照", "Global illumination", kHiGi, kHiGiName, 2, 0 },
      { "ao", "AmbientOcclusion", "环境光遮蔽", "Ambient occlusion", kHiAo, kHiAoName, 3, 1 },
      { "volumetric", "VolumetricLight", "体积光", "Volumetric light", kHiVol, kHiVolName, 3, 1 },
      { "postGrade", "PostFXGrade", "后期等级", "Post FX grade", kHiGrade, kHiGradeName, 2, 0 },
    };

    const char *kHiGraphics = "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519";
    const char *kHiAudio = "GENERAL_DATA_V2_PersonalAudioSetting_h3869048096";
    const char *kHiAudioVol = "GENERAL_DATA_V2_PersonalAudioSettingVolume_h600615720";

    int
    stored_index(const char *const *stored, int count, const std::string &name, int fallback) {
      for (int i = 0; i < count; ++i)
        if (name == stored[i]) return i;
      return fallback;
    }

    json
    hi3_catalog() {
      json out = screen_catalog();
      for (const HiStr &field : kHiStr) out.push_back(choice_field(field.key, "quality", field.zh, field.en, field.labs[field.def].value, field.labs, field.count));
      out.push_back(slider_field("fpsMenu", "display", "界面帧率", "Menu frame rate", 30, 60, 60));
      out.push_back(toggle_field("useFxaa", "quality", "FXAA", "FXAA", true));
      out.push_back(toggle_field("usePostFx", "quality", "后期效果", "Post FX", true));
      out.push_back(toggle_field("useHdr", "quality", "HDR", "HDR", true));
      out.push_back(toggle_field("useDistortion", "quality", "扭曲", "Distortion", true));
      out.push_back(choice_field("lod", "quality", "模型细节", "LOD", "medium", kHiLod, 3));
      out.push_back(slider_field("masterVolume", "audio", "总音量", "Master volume", 0, 100, 100));
      out.push_back(slider_field("musicVolume", "audio", "音乐", "Music", 0, 100, 100));
      out.push_back(slider_field("sfxVolume", "audio", "音效", "SFX", 0, 100, 100));
      out.push_back(slider_field("voiceVolume", "audio", "语音", "Voice", 0, 100, 100));
      out.push_back(slider_field("elfVolume", "audio", "妖精语音", "ELF voice", 0, 100, 100));
      out.push_back(slider_field("cgVolume", "audio", "过场", "CG", 0, 100, 100));
      out.push_back(toggle_field("mute", "audio", "静音", "Mute", false));
      out.push_back(choice_field("audioLanguage", "language", "语音", "Voice-over", "Japanese", kHiVoice, 2));
      return out;
    }

    double
    hi_float(int value, double out_max) { return static_cast<double>(value) / 100.0 * out_max; }

    void
    read_hi3(const std::string &root, Registry &registry, SettingsResult &result) {
      json graphics;
      const Blob blob = load_object(registry.get(root, kHiGraphics), graphics);
      if (blob != Blob::ok) {
        for (const HiStr &field : kHiStr) mark(result, field.key);
        for (const char *key : { "fpsMenu", "useFxaa", "usePostFx", "useHdr", "useDistortion", "lod" }) mark(result, key);
      }
      else {
        for (const HiStr &field : kHiStr) {
          const std::string stored = graphics.value(field.json_key, std::string(field.stored[field.def]));
          const int index = stored_index(field.stored, field.count, stored, field.def);
          result.values.extra[field.key] = field.labs[index].value;
        }
        result.values.extra["fpsMenu"] = graphics.value("TargetFrameRateForOthers", 60);
        result.values.extra["useFxaa"] = graphics.value("UseFXAA", true);
        result.values.extra["usePostFx"] = graphics.value("UsePostFX", true);
        result.values.extra["useHdr"] = graphics.value("UseHDR", true);
        result.values.extra["useDistortion"] = graphics.value("UseDistortion", true);
        const int lod = graphics.value("LodGrade", 1);
        result.values.extra["lod"] = lod == 0 ? "high" : lod == 2 ? "low" :
                                                                    "medium";
      }
      json audio;
      const Blob audio_blob = load_object(registry.get(root, kHiAudio), audio);
      if (audio_blob == Blob::bad) {
        for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume", "elfVolume", "cgVolume", "mute", "audioLanguage" }) mark(result, key);
      }
      else {
        if (!audio.is_object()) audio = json::object();
        result.values.extra["masterVolume"] = audio.value("MasterVolume", 100);
        result.values.extra["musicVolume"] = audio.value("BGMVolume", 100);
        result.values.extra["sfxVolume"] = audio.value("SoundEffectVolume", 100);
        result.values.extra["voiceVolume"] = audio.value("VoiceVolume", 100);
        result.values.extra["elfVolume"] = audio.value("ElfVolume", 100);
        result.values.extra["cgVolume"] = audio.value("CGVolumeV2", 100);
        result.values.extra["mute"] = audio.value("Mute", false);
        const std::string voice = audio.value("_userCVLanguage", std::string("Japanese"));
        result.values.extra["audioLanguage"] = voice == "Chinese(PRC)" ? "Chinese(PRC)" : "Japanese";
      }
    }

    void
    write_hi3(const std::string &root, Registry &registry, const GameValues &values, const std::set<std::string> &fields, SettingsResult &result) {
      bool graphics_need = wanted(fields, values, "fpsMenu") || wanted(fields, values, "useFxaa") || wanted(fields, values, "usePostFx") || wanted(fields, values, "useHdr") || wanted(fields, values, "useDistortion") || wanted(fields, values, "lod");
      for (const HiStr &field : kHiStr)
        if (wanted(fields, values, field.key)) graphics_need = true;
      if (graphics_need) {
        json graphics;
        const Blob blob = load_object(registry.get(root, kHiGraphics), graphics);
        if (blob != Blob::ok) {
          for (const HiStr &field : kHiStr)
            if (wanted(fields, values, field.key)) mark(result, field.key);
          for (const char *key : { "fpsMenu", "useFxaa", "usePostFx", "useHdr", "useDistortion", "lod" })
            if (wanted(fields, values, key)) mark(result, key);
        }
        else {
          for (const HiStr &field : kHiStr) {
            if (!wanted(fields, values, field.key) || !values.extra.contains(field.key)) continue;
            const int index = lab_index(field.labs, field.count, values.extra.value(field.key, std::string()));
            if (index < 0) continue;
            graphics[field.json_key] = field.stored[index];
            applied(result, field.key);
          }
          if (wanted(fields, values, "fpsMenu") && values.extra.contains("fpsMenu")) {
            graphics["TargetFrameRateForOthers"] = values.extra.value("fpsMenu", 60);
            applied(result, "fpsMenu");
          }
          auto flag = [&](const char *key, const char *json_key) {
            if (!wanted(fields, values, key) || !values.extra.contains(key)) return;
            graphics[json_key] = values.extra.value(key, false);
            applied(result, key);
          };
          flag("useFxaa", "UseFXAA");
          flag("usePostFx", "UsePostFX");
          flag("useHdr", "UseHDR");
          flag("useDistortion", "UseDistortion");
          if (wanted(fields, values, "lod") && values.extra.contains("lod")) {
            const std::string lod = values.extra.value("lod", std::string("medium"));
            graphics["LodGrade"] = lod == "high" ? 0 : lod == "low" ? 2 :
                                                                      1;
            applied(result, "lod");
          }
          store_object(registry, root, kHiGraphics, graphics);
        }
      }
      bool audio_need = false;
      for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume", "elfVolume", "cgVolume", "mute", "audioLanguage" })
        if (wanted(fields, values, key)) audio_need = true;
      if (!audio_need) return;
      json audio;
      const Blob blob = load_object(registry.get(root, kHiAudio), audio);
      if (blob != Blob::ok) {
        for (const char *key : { "masterVolume", "musicVolume", "sfxVolume", "voiceVolume", "elfVolume", "cgVolume", "mute", "audioLanguage" })
          if (wanted(fields, values, key)) mark(result, key);
        return;
      }
      json floats;
      if (load_object(registry.get(root, kHiAudioVol), floats) != Blob::ok) floats = json::object();
      auto vol = [&](const char *key, const char *json_key, const char *float_key, double span) {
        if (!wanted(fields, values, key) || !values.extra.contains(key)) return;
        const int value = values.extra.value(key, 100);
        audio[json_key] = value;
        if (float_key) floats[float_key] = hi_float(value, span);
        applied(result, key);
      };
      vol("masterVolume", "MasterVolume", "MasterVolumeValue", 100);
      vol("musicVolume", "BGMVolume", "BGMVolumeValue", 3);
      vol("sfxVolume", "SoundEffectVolume", "SoundEffectVolumeValue", 3);
      vol("voiceVolume", "VoiceVolume", "VoiceVolumeValue", 3);
      vol("elfVolume", "ElfVolume", "ElfVolumeValue", 3);
      vol("cgVolume", "CGVolumeV2", "CGVolumeValue", 1.8);
      if (wanted(fields, values, "mute") && values.extra.contains("mute")) {
        audio["Mute"] = values.extra.value("mute", false);
        applied(result, "mute");
      }
      if (wanted(fields, values, "audioLanguage") && values.extra.contains("audioLanguage")) {
        const std::string voice = values.extra.value("audioLanguage", std::string("Japanese"));
        audio["_userCVLanguage"] = voice == "Chinese(PRC)" ? "Chinese(PRC)" : "Japanese";
        if (!audio.contains("CVLanguage")) audio["CVLanguage"] = "Japanese";
        applied(result, "audioLanguage");
      }
      store_object(registry, root, kHiAudio, audio);
      store_object(registry, root, kHiAudioVol, floats);
    }

    const json &
    catalog_of(std::string_view game) {
      static const json genshin = genshin_catalog();
      static const json starrail = starrail_catalog();
      static const json hi3 = hi3_catalog();
      static const json zzz = [] {
        return screen_catalog();
      }();
      static const json empty = json::array();
      if (game == "genshin") return genshin;
      if (game == "starrail") return starrail;
      if (game == "hi3") return hi3;
      if (game == "zzz") return zzz;
      return empty;
    }
  }  // namespace

  bool
  hoyo_is_field(std::string_view key) {
    static const std::set<std::string> keys = [] {
      std::set<std::string> out;
      for (const char *game : { "genshin", "starrail", "zzz", "hi3" })
        for (const auto &item : hoyo_catalog(game))
          if (item.is_object()) out.insert(item.value("key", std::string()));
      for (const auto &item : zzz_file_catalog())
        if (item.is_object()) out.insert(item.value("key", std::string()));
      return out;
    }();
    return keys.count(std::string(key)) > 0;
  }

  json
  hoyo_catalog(std::string_view game) { return catalog_of(game); }

  const char *const *
  hoyo_registry_names(std::size_t &count) {
    static const char *names[] = {
      "CollapseLauncher_ScreenSetting",
      "CollapseLauncher_Misc",
      "CollapseLauncher_CustomArgs",
      "AudioSettings_MasterVolume_h1622207037",
      "AudioSettings_BGMVolume_h240914409",
      "AudioSettings_SFXVolume_h2753520268",
      "AudioSettings_VOVolume_h805685304",
      "LanguageSettings_LocalAudioLanguage_h882585060",
      "LanguageSettings_LocalTextLanguage_h2764291023",
      "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519",
      "GENERAL_DATA_V2_PersonalAudioSetting_h3869048096",
      "GENERAL_DATA_V2_PersonalAudioSettingVolume_h600615720",
      "GENERAL_DATA_V2_ScreenSettingData_h1916288658",
    };
    count = sizeof(names) / sizeof(names[0]);
    return names;
  }

  Status
  hoyo_validate(const Install &install, const GameValues &values, const std::set<std::string> &fields) {
    if (!hoyo_game(install.game)) return Status::success();
    for (const auto &item : catalog_of(install.game)) {
      if (!item.is_object()) continue;
      const std::string key = item.value("key", std::string());
      if (!wanted(fields, values, key.c_str()) || !values.extra.contains(key)) continue;
      const json &value = values.extra[key];
      const std::string kind = item.value("kind", std::string());
      if (kind == "toggle" && !value.is_boolean()) return Status::fail(400, "invalid_input", key + " must be on or off");
      if (kind == "slider") {
        if (!value.is_number()) return Status::fail(400, "invalid_input", key + " must be a number");
        const double number = value.get<double>();
        if (number < item.value("min", 0.0) || number > item.value("max", 0.0)) return Status::fail(400, "invalid_input", key + " is outside the game's range");
      }
      if (kind == "choice") {
        if (!value.is_string()) return Status::fail(400, "invalid_input", key + " must be one of the game's values");
        bool found = false;
        for (const auto &option : item.value("options", json::array()))
          if (option.is_object() && option.value("value", std::string()) == value.get<std::string>()) found = true;
        if (!found) return Status::fail(400, "invalid_input", key + " must be one of the game's values");
      }
      if (kind == "text") {
        if (!value.is_string()) return Status::fail(400, "invalid_input", "custom launch arguments must be text");
        std::string quoted;
        if (!quote_args(value.get<std::string>(), quoted)) return Status::fail(400, "invalid_input", "custom launch arguments may only contain quoted tokens without shell metacharacters");
      }
    }
    return Status::success();
  }

  void
  hoyo_read(const Install &install, Registry &registry, SettingsResult &result) {
    if (!hoyo_game(install.game)) return;
    const std::string root = registry_root(install);
    read_screen(root, registry, result);
    if (install.game == "genshin")
      read_genshin(root, registry, result);
    else if (install.game == "starrail")
      read_starrail(root, registry, result);
    else if (install.game == "hi3")
      read_hi3(root, registry, result);
    else if (std::find(result.unavailable.begin(), result.unavailable.end(), "fps") != result.unavailable.end()) {
      for (const auto &item : zzz_file_catalog())
        if (item.is_object()) mark(result, item.value("key", std::string()));
    }
  }

  void
  hoyo_write(const Install &install, Registry &registry, const GameValues &values, const std::set<std::string> &fields, SettingsResult &result) {
    if (!hoyo_game(install.game)) return;
    const std::string root = registry_root(install);
    write_screen(root, registry, values, fields, result);
    if (install.game == "genshin")
      write_genshin(root, registry, values, fields, result);
    else if (install.game == "starrail")
      write_starrail(root, registry, values, fields, result);
    else if (install.game == "hi3")
      write_hi3(root, registry, values, fields, result);
  }

  json
  game_launch_options(const json &settings, std::string_view game) {
    json out = json::object();
    if (!settings.is_object()) return out;
    auto take = [&](const std::string &key) {
      if (!settings.contains(key)) return;
      const json &value = settings[key];
      if (value.is_boolean() || value.is_number() || value.is_string()) out[key] = value;
    };
    for (const auto &key : launch_fields(game)) take(key);
    for (const char *key : { "resolution", "exclusiveFullscreen", "borderless", "customResolution", "customWidth", "customHeight", "graphicsApi", "customArgs" }) take(key);
    return out;
  }

  std::string
  game_launch_command(std::string command, const Install &install, const json &options) {
    if (!options.is_object() || options.empty()) return command;
    const bool store = install.source == "steam" || install.source == "epic" || std::find(install.sources.begin(), install.sources.end(), "steam") != install.sources.end() || std::find(install.sources.begin(), install.sources.end(), "epic") != install.sources.end();
    const bool parent_launcher = install.game == "arknights" || install.game == "endfield";
    if (store || parent_launcher) return command;
    auto number = [&](const char *key) { return options.contains(key) && options[key].is_number_integer() ? options[key].get<int>() : 0; };
    const bool sized = number("launchWidth") > 0;
    if (sized) command += " -screen-width " + std::to_string(number("launchWidth"));
    if (number("launchHeight") > 0) command += " -screen-height " + std::to_string(number("launchHeight"));
    if (options.value("launchFullscreen", true) && options.contains("launchFullscreen"))
      command += " -screen-fullscreen 1";
    else if (options.contains("launchFullscreen"))
      command += " -screen-fullscreen 0";
    if (options.value("popupWindow", false) || options.value("borderless", false)) command += " -popupwindow";
    const bool hoyo = hoyo_game(install.game);
    const bool api_set = options.contains("graphicsApi") && options["graphicsApi"].is_string();
    const int api = api_set ? api_index(options.value("graphicsApi", std::string())) : -1;
    const bool api_owns_dx = api_set && (install.game == "hi3" || install.game == "zzz");
    if (!api_owns_dx) {
      const std::string dx = options.value("dxVersion", std::string("auto"));
      if (dx == "dx11") command += install.game == "wuwa" || install.game == "nte" ? " -dx11" : " -force-d3d11";
      if (dx == "dx12") command += install.game == "wuwa" || install.game == "nte" ? " -dx12" : " -force-d3d12";
    }
    if (!hoyo) return command;
    if (options.value("exclusiveFullscreen", false)) {
      if (install.game == "hi3")
        command += " -window-mode exclusive";
      else if (install.game != "zzz")
        command += " -window-mode exclusive -screen-fullscreen 1";
    }
    int width = 0, height = 0;
    const bool custom = options.value("customResolution", false);
    bool have = false;
    if (custom && number("customWidth") > 0 && number("customHeight") > 0) {
      width = number("customWidth");
      height = number("customHeight");
      have = true;
    }
    if (!have && options.contains("resolution") && options["resolution"].is_string()) have = parse_resolution(options["resolution"].get<std::string>(), width, height);
    if (have && !sized && (install.game != "zzz" || custom)) {
      command += " -screen-width " + std::to_string(width);
      command += " -screen-height " + std::to_string(height);
    }
    if (install.game == "hi3" && api >= 0) {
      if (api == 0)
        command += " -force-feature-level-10-1";
      else if (api == 1)
        command += " -force-feature-level-11-0 -force-d3d11-no-singlethreaded";
      else if (api == 2)
        command += " -force-feature-level-11-1";
      else if (api == 3)
        command += " -force-feature-level-11-1 -force-d3d11-no-singlethreaded";
      else if (api == 4)
        command += " -force-d3d12";
    }
    if (install.game == "zzz" && api == 4) command += " -use-d3d12";
    if (options.contains("customArgs") && options["customArgs"].is_string()) {
      std::string quoted;
      if (quote_args(options["customArgs"].get<std::string>(), quoted) && !quoted.empty()) command += " " + quoted;
    }
    return command;
  }

  bool
  game_boost_on(const json &settings) {
    return settings.is_object() && settings.contains("gameBoost") && settings["gameBoost"].is_boolean() && settings["gameBoost"].get<bool>();
  }

  json
  options_with_boost(json options, bool boost) {
    if (!boost) return options;
    if (!options.is_object()) options = json::object();
    const std::string priority = options.value("priority", std::string("normal"));
    if (priority == "normal" || priority == "belowNormal") options["priority"] = "aboveNormal";
    return options;
  }

  std::string
  custom_args_problem(std::string_view raw) {
    std::string quoted;
    if (quote_args(raw, quoted)) return {};
    return "custom launch arguments may only contain quoted tokens without shell metacharacters";
  }

}  // namespace sparxie
