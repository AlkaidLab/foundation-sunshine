#include "src/sparxie/zzz_file.h"

#include <string_view>
#include <vector>

namespace sparxie {
  namespace {
    struct Tok {
      const char *token;
      const char *zh;
      const char *en;
      int stored;
    };
    struct Field {
      const char *key;
      const char *slot;
      bool root;
      const char *section;
      const char *zh;
      const char *en;
      const char *kind;
      int def;
      int minv;
      int maxv;
      const Tok *toks;
      int count;
    };
    const Tok kLowHigh[] = { { "low", "低", "Low", 0 }, { "high", "高", "High", 1 } };
    const Tok k3[] = { { "low", "低", "Low", 0 }, { "medium", "中", "Medium", 1 }, { "high", "高", "High", 2 } };
    const Tok kOff4[] = { { "off", "关", "Off", 0 }, { "low", "低", "Low", 1 }, { "medium", "中", "Medium", 2 }, { "high", "高", "High", 3 } };
    const Tok kFx[] = { { "verylow", "最低", "Very low", 0 }, { "low", "低", "Low", 1 }, { "medium", "中", "Medium", 2 }, { "high", "高", "High", 3 } };
    const Tok kAa[] = { { "off", "关", "Off", 0 }, { "taa", "TAA", "TAA", 1 }, { "smaa", "SMAA", "SMAA", 2 } };
    const Tok kAniso[] = { { "1", "1x", "1x", 0 }, { "2", "2x", "2x", 1 }, { "4", "4x", "4x", 2 }, { "8", "8x", "8x", 3 }, { "16", "16x", "16x", 4 } };
    const Tok kAnim[] = { { "off", "关", "Off", 0 }, { "dynamic", "动态", "Dynamic", 1 }, { "global", "全局", "Global", 2 } };
    const Tok kRay[] = { { "low", "低", "Low", 0 }, { "medium", "中", "Medium", 1 }, { "high", "高", "High", 2 }, { "ultra", "极高", "Ultra", 3 } };
    const Tok kSr[] = { { "off", "关", "Off", 0 }, { "dlss", "DLSS", "DLSS", 1 }, { "fsr", "FSR", "FSR", 2 } };
    const Tok kSrq[] = { { "ultraperf", "超级性能", "Ultra performance", 0 }, { "performance", "性能", "Performance", 1 }, { "balanced", "均衡", "Balanced", 2 }, { "quality", "质量", "Quality", 3 }, { "dlaa", "DLAA", "DLAA", 4 } };
    const Tok kPlay[] = { { "headphones", "耳机", "Headphones", 0 }, { "speakers", "扬声器", "Speakers", 1 }, { "tv", "电视", "TV", 3 } };
    const Tok kText[] = {
      { "unset", "未设置", "Unset", -1 },
      { "en", "English", "English", 1 },
      { "zh-cn", "简体中文", "Simplified Chinese", 2 },
      { "zh-tw", "繁體中文", "Traditional Chinese", 3 },
      { "fr", "Français", "French", 4 },
      { "de", "Deutsch", "German", 5 },
      { "es", "Español", "Spanish", 6 },
      { "pt", "Português", "Portuguese", 7 },
      { "ru", "Русский", "Russian", 8 },
      { "ja", "日本語", "Japanese", 9 },
      { "ko", "한국어", "Korean", 10 },
      { "th", "ภาษาไทย", "Thai", 11 },
      { "vi", "Tiếng Việt", "Vietnamese", 12 },
      { "id", "Bahasa Indonesia", "Indonesian", 13 },
    };
    const Tok kVoice[] = { { "unset", "未设置", "Unset", -1 }, { "en", "English", "English", 1 }, { "zh-cn", "简体中文", "Simplified Chinese", 2 }, { "ja", "日本語", "Japanese", 3 }, { "ko", "한국어", "Korean", 4 } };
    const Field kFields[] = {
      { "shadow", "10", false, "quality", "阴影", "Shadow", "choice", 1, 0, 0, k3, 3 },
      { "aa", "12", false, "quality", "抗锯齿", "Anti-aliasing", "choice", 1, 0, 0, kAa, 3 },
      { "fog", "13", false, "quality", "体积雾", "Volumetric fog", "choice", 2, 0, 0, kOff4, 4 },
      { "bloom", "14", false, "quality", "泛光", "Bloom", "toggle", 1, 0, 1, nullptr, 0 },
      { "reflection", "15", false, "quality", "反射", "Reflection", "choice", 2, 0, 0, kOff4, 4 },
      { "effects", "16", false, "quality", "特效", "Effects", "choice", 2, 0, 0, kFx, 4 },
      { "colorFilter", "95", false, "quality", "调色滤镜强度", "Color filter strength", "slider", 10, 0, 20, nullptr, 0 },
      { "character", "99", false, "quality", "角色质量", "Character quality", "choice", 1, 0, 0, kLowHigh, 2 },
      { "distortion", "107", false, "quality", "扭曲", "Distortion", "toggle", 1, 0, 1, nullptr, 0 },
      { "shading", "108", false, "quality", "着色", "Shading", "choice", 1, 0, 0, k3, 3 },
      { "environment", "109", false, "quality", "环境", "Environment", "choice", 1, 0, 0, kLowHigh, 2 },
      { "anisotropic", "16184", false, "quality", "各向异性过滤", "Anisotropic filtering", "choice", 3, 0, 0, kAniso, 5 },
      { "gi", "12155", false, "quality", "全局光照", "Global illumination", "choice", 2, 0, 0, k3, 3 },
      { "motionBlur", "106", false, "quality", "动态模糊", "Motion blur", "toggle", 1, 0, 1, nullptr, 0 },
      { "charAnim", "13162", false, "quality", "高精度角色动画", "High-precision character animation", "choice", 0, 0, 0, kAnim, 3 },
      { "resolutionIndex", "5", false, "display", "分辨率序号", "Resolution index", "slider", -1, -1, 32, nullptr, 0 },
      { "rayTracing", "22003", false, "quality", "光线追踪", "Ray tracing", "toggle", 0, 0, 1, nullptr, 0 },
      { "rayQuality", "23001", false, "quality", "光追质量", "Ray tracing quality", "choice", 1, 0, 0, kRay, 4 },
      { "superRes", "22004", false, "quality", "超分辨率", "Super resolution", "choice", 0, 0, 0, kSr, 3 },
      { "superQuality", "22005", false, "quality", "超分辨率质量", "Super resolution quality", "choice", 2, 0, 0, kSrq, 5 },
      { "frameGen", "22006", false, "quality", "帧生成", "Frame generation", "choice", 0, 0, 0, kSr, 3 },
      { "masterVolume", "31", false, "audio", "总音量", "Master volume", "slider", 10, 0, 10, nullptr, 0 },
      { "musicVolume", "32", false, "audio", "音乐音量", "Music volume", "slider", 10, 0, 10, nullptr, 0 },
      { "voiceVolume", "33", false, "audio", "语音音量", "Dialogue volume", "slider", 10, 0, 10, nullptr, 0 },
      { "sfxVolume", "34", false, "audio", "音效音量", "SFX volume", "slider", 10, 0, 10, nullptr, 0 },
      { "ambientVolume", "20192", false, "audio", "环境音量", "Ambient volume", "slider", 10, 0, 10, nullptr, 0 },
      { "playback", "10104", false, "audio", "播放设备", "Playback device", "choice", 0, 0, 0, kPlay, 3 },
      { "muteMinimized", "10113", false, "audio", "最小化时静音", "Mute when minimized", "toggle", 1, 0, 1, nullptr, 0 },
      { "textLanguage", "DeviceLanguageType", true, "language", "文本", "Text", "choice", -1, 0, 0, kText, 14 },
      { "audioLanguage", "DeviceLanguageVoiceType", true, "language", "语音", "Voice", "choice", -1, 0, 0, kVoice, 5 },
    };

    const char *
    token_of(const Field &field, int stored) {
      for (int i = 0; i < field.count; ++i)
        if (field.toks[i].stored == stored) return field.toks[i].token;
      for (int i = 0; i < field.count; ++i)
        if (field.toks[i].stored == field.def) return field.toks[i].token;
      return field.count ? field.toks[0].token : "";
    }

    int
    store_of(const Field &field, const json &value) {
      if (std::string_view(field.kind) == "toggle") return value.is_boolean() && value.get<bool>() ? 1 : 0;
      if (std::string_view(field.kind) == "slider") return value.is_number() ? value.get<int>() : field.def;
      if (!value.is_string()) return field.def;
      const std::string token = value.get<std::string>();
      for (int i = 0; i < field.count; ++i)
        if (token == field.toks[i].token) return field.toks[i].stored;
      return field.def;
    }
  }  // namespace

  json
  zzz_file_catalog() {
    json out = json::array();
    for (const Field &field : kFields) {
      json item = { { "key", field.key }, { "kind", field.kind }, { "section", field.section }, { "label", field.zh }, { "labelEn", field.en } };
      if (std::string_view(field.kind) == "toggle")
        item["def"] = field.def != 0;
      else if (std::string_view(field.kind) == "slider")
        item.update({ { "min", field.minv }, { "max", field.maxv }, { "step", 1 }, { "decimals", 0 }, { "def", field.def } });
      else {
        json options = json::array();
        for (int i = 0; i < field.count; ++i) options.push_back({ { "value", field.toks[i].token }, { "label", field.toks[i].zh }, { "labelEn", field.toks[i].en } });
        item["options"] = std::move(options);
        item["def"] = token_of(field, field.def);
      }
      out.push_back(std::move(item));
    }
    return out;
  }

  void
  zzz_fill_extra(const std::string &text, GameValues &values) {
    json body;
    try {
      body = json::parse(text);
    }
    catch (const std::exception &) {
      return;
    }
    if (!body.is_object()) return;
    const json map = body.value("SystemSettingDataMap", json::object());
    for (const Field &field : kFields) {
      int stored = field.def;
      if (field.root) {
        if (body.contains(field.slot) && body[field.slot].is_number()) stored = body[field.slot].get<int>();
      }
      else if (map.is_object() && map.contains(field.slot) && map[field.slot].is_object() && map[field.slot].contains("Data") && map[field.slot]["Data"].is_number())
        stored = map[field.slot]["Data"].get<int>();
      if (std::string_view(field.kind) == "toggle")
        values.extra[field.key] = stored != 0;
      else if (std::string_view(field.kind) == "slider")
        values.extra[field.key] = stored;
      else
        values.extra[field.key] = token_of(field, stored);
    }
  }

  std::vector<ZzzEdit>
  zzz_extra_edits(const GameValues &values, const std::set<std::string> &fields) {
    std::vector<ZzzEdit> out;
    for (const Field &field : kFields) {
      const bool want = fields.empty() || fields.count(field.key) > 0;
      if (!want || !values.extra.contains(field.key)) continue;
      out.push_back({ field.root, field.slot, store_of(field, values.extra[field.key]) });
    }
    return out;
  }
}  // namespace sparxie
