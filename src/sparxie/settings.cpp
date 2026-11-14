#include "src/sparxie/settings.h"
#include "src/sparxie/hoyo_settings.h"
#include "src/sparxie/ini_settings.h"
#include "src/sparxie/region.h"
#include "src/sparxie/sha256.h"
#include "src/sparxie/zzz_file.h"
#include <algorithm>
#include <cmath>
#include <fstream>
namespace sparxie {
  namespace {
    char
    sep() { return static_cast<char>(92); }
    std::string
    key_path(std::initializer_list<const char *> parts) {
      std::string out;
      for (const char *part : parts) {
        if (!out.empty()) out.push_back(sep());
        out += part;
      }
      return out;
    }
    std::string
    registry_root(const Install &install) {
      const bool global = install.region == "global";
      if (install.game == "genshin") return key_path({ "Software", "miHoYo", global ? "Genshin Impact" : "原神" });
      if (install.game == "starrail") return global ? key_path({ "Software", "Cognosphere", "Star Rail" }) : key_path({ "Software", "miHoYo", "崩坏：星穹铁道" });
      if (install.game == "hi3") return key_path({ "Software", "miHoYo", global ? "Honkai Impact 3rd" : "崩坏3" });
      // Arknights: Endfield keeps its display in Unity's PlayerPrefs under the publisher's key (observed on a test machine, 2026-11-12: HKCU\\Software\\Hypergryph\\Endfield)
      if (install.game == "endfield") return key_path({ "Software", "Hypergryph", "Endfield" });
      return global ? key_path({ "Software", "miHoYo", "ZenlessZoneZero" }) : key_path({ "Software", "miHoYo", "绝区零" });
    }
    void
    append_i32(std::vector<std::uint8_t> &out, std::int32_t value) {
      const auto u = static_cast<std::uint32_t>(value);
      out.push_back(static_cast<std::uint8_t>(u));
      out.push_back(static_cast<std::uint8_t>(u >> 8));
      out.push_back(static_cast<std::uint8_t>(u >> 16));
      out.push_back(static_cast<std::uint8_t>(u >> 24));
    }
    void
    append_7bit(std::vector<std::uint8_t> &out, std::uint32_t value) {
      while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
      }
      out.push_back(static_cast<std::uint8_t>(value));
    }
    bool
    read_i32(const std::vector<std::uint8_t> &in, std::size_t &at, std::int32_t &value) {
      if (at + 4 > in.size()) return false;
      const std::uint32_t u = std::uint32_t(in[at]) | (std::uint32_t(in[at + 1]) << 8) | (std::uint32_t(in[at + 2]) << 16) | (std::uint32_t(in[at + 3]) << 24);
      at += 4;
      value = static_cast<std::int32_t>(u);
      return true;
    }
    bool
    read_7bit(const std::vector<std::uint8_t> &in, std::size_t &at, std::uint32_t &value) {
      value = 0;
      int shift = 0;
      while (at < in.size() && shift <= 28) {
        const std::uint8_t byte = in[at++];
        value |= std::uint32_t(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
      }
      return false;
    }
    std::string
    blob_text(const RegValue &value) {
      std::string text;
      if (value.kind == RegValue::Kind::text)
        text = value.text;
      else if (value.kind == RegValue::Kind::binary)
        text.assign(value.bytes.begin(), value.bytes.end());
      while (!text.empty() && text.back() == 0) text.pop_back();
      return text;
    }
    std::vector<std::uint8_t>
    text_blob(const std::string &text) {
      std::vector<std::uint8_t> bytes(text.begin(), text.end());
      bytes.push_back(0);
      return bytes;
    }
    json
    parse_blob(const RegValue &value) {
      const std::string text = blob_text(value);
      if (text.empty()) return {};
      try {
        return json::parse(text);
      }
      catch (const std::exception &) {
        return {};
      }
    }
    void
    put_json_blob(Registry &registry, const std::string &root, const char *name, const json &body) {
      registry.set_binary(root, name, text_blob(body.dump()));
    }
    int
    fps_index_genshin(int fps) {
      if (fps == 30) return 0;
      if (fps == 60) return 1;
      if (fps == 45) return 2;
      return -1;
    }
    int
    fps_from_genshin(int index) {
      if (index == 0) return 30;
      if (index == 1) return 60;
      if (index == 2) return 45;
      return 60;
    }
    int
    scale_index_genshin(double scale) {
      const std::pair<double, int> table[] = { { 0.6, 0 }, { 0.8, 1 }, { 0.9, 8 }, { 1.0, 2 }, { 1.1, 3 }, { 1.2, 4 }, { 1.3, 5 }, { 1.4, 6 }, { 1.5, 7 } };
      int best = 2;
      double err = 100;
      for (const auto &row : table) {
        const double d = std::abs(scale - row.first);
        if (d < err) {
          err = d;
          best = row.second;
        }
      }
      return best;
    }
    double
    scale_from_genshin(int index) {
      switch (index) {
        case 0:
          return 0.6;
        case 1:
          return 0.8;
        case 8:
          return 0.9;
        case 3:
          return 1.1;
        case 4:
          return 1.2;
        case 5:
          return 1.3;
        case 6:
          return 1.4;
        case 7:
          return 1.5;
        default:
          return 1.0;
      }
    }
    int
    preset_index(const std::string &preset, const std::string &game) {
      if (game == "zzz") {
        if (preset == "high") return 0;
        if (preset == "medium") return 1;
        if (preset == "low" || preset == "lowest") return 2;
        if (preset == "custom") return 3;
        return -1;
      }
      if (game == "starrail") {
        if (preset == "custom") return 0;
        if (preset == "lowest" || preset == "verylow") return 1;
        if (preset == "low") return 2;
        if (preset == "medium") return 3;
        if (preset == "high") return 4;
        if (preset == "veryhigh") return 5;
        return -1;
      }
      if (preset == "lowest") return 0;
      if (preset == "low") return 1;
      if (preset == "medium") return 2;
      if (preset == "high") return 3;
      if (preset == "veryhigh") return 4;
      return -1;
    }
    std::string
    preset_name(int index, const std::string &game) {
      if (game == "zzz") return index == 0 ? "high" : index == 1 ? "medium" :
                                                    index == 2   ? "low" :
                                                                   "custom";
      if (game == "starrail") {
        const char *names[] = { "custom", "verylow", "low", "medium", "high", "veryhigh" };
        if (index >= 0 && index <= 5) return names[index];
        return "high";
      }
      const char *names[] = { "lowest", "low", "medium", "high" };
      if (index >= 0 && index <= 3) return names[index];
      return "high";
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
    int
    read_item(const json &items, int type, int fallback) {
      if (!items.is_array()) return fallback;
      for (const auto &item : items)
        if (item.is_object() && item.value("entryType", -1) == type) return item.value("index", fallback);
      return fallback;
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
    bool
    allowed_fps(const std::string &game, int fps) {
      if (game == "genshin") return fps == 30 || fps == 45 || fps == 60;
      if (game == "starrail") return fps == 30 || fps == 60 || fps == 120;
      if (game == "hi3") return fps == 30 || fps == 60;
      if (game == "wuwa" || game == "nte") return fps == 0 || fps == 30 || fps == 45 || fps == 60 || fps == 90 || fps == 120 || fps == 144 || fps == 165 || fps == 240;
      return fps == 30 || fps == 60 || fps == 0;
    }
    void
    mark(SettingsResult &result, const std::string &field) {
      if (std::find(result.unavailable.begin(), result.unavailable.end(), field) == result.unavailable.end())
        result.unavailable.push_back(field);
    }
  }  // namespace
  std::vector<std::uint8_t>
  sleepy_write(std::string_view text, const std::vector<std::uint8_t> &magic) {
    std::vector<bool> evil(magic.size());
    for (std::size_t i = 0; i < magic.size(); ++i) evil[i] = (magic[i] & 0xC0) == 0xC0;
    std::vector<std::uint8_t> payload;
    std::size_t i = 0;
    for (std::size_t j = 0; j < text.size();) {
      const std::size_t n = i % magic.size();
      auto ch = static_cast<std::uint8_t>(text[j]);
      if (evil[n]) {
        std::uint8_t flag = 0;
        if (static_cast<std::uint8_t>(text[j]) >= 0x40) {
          ch = static_cast<std::uint8_t>(ch - 0x40);
          flag = 1;
        }
        payload.push_back(static_cast<std::uint8_t>(flag ^ magic[n]));
        ++i;
      }
      const std::size_t n2 = i % magic.size();
      payload.push_back(static_cast<std::uint8_t>(ch ^ magic[n2]));
      ++i;
      ++j;
    }
    std::vector<std::uint8_t> out;
    out.push_back(0);
    append_i32(out, 1);
    append_i32(out, -1);
    append_i32(out, 1);
    append_i32(out, 0);
    out.push_back(6);
    append_i32(out, 1);
    append_7bit(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    out.push_back(11);
    return out;
  }
  std::string
  sleepy_read(const std::vector<std::uint8_t> &bytes, const std::vector<std::uint8_t> &magic) {
    if (magic.empty()) return {};
    std::size_t at = 0;
    auto need = [&](std::uint8_t expect) {
      if (at >= bytes.size() || bytes[at++] != expect) return false;
      return true;
    };
    std::int32_t n = 0;
    if (!need(0) || !read_i32(bytes, at, n) || n != 1 || !read_i32(bytes, at, n) || n != -1 || !read_i32(bytes, at, n) || n != 1 || !read_i32(bytes, at, n) || n != 0 || !need(6) || !read_i32(bytes, at, n) || n != 1)
      return {};
    std::uint32_t length = 0;
    if (!read_7bit(bytes, at, length) || at + length > bytes.size()) return {};
    std::vector<bool> evil(magic.size());
    for (std::size_t i = 0; i < magic.size(); ++i) evil[i] = (magic[i] & 0xC0) == 0xC0;
    std::string text;
    bool flagged = false;
    for (std::uint32_t i = 0; i < length; ++i) {
      const std::size_t slot = i % magic.size();
      const auto ch = static_cast<std::uint8_t>(bytes[at + i] ^ magic[slot]);
      if (evil[slot])
        flagged = ch != 0;
      else {
        auto out = ch;
        if (flagged) {
          out = static_cast<std::uint8_t>(out + 0x40);
          flagged = false;
        }
        text.push_back(static_cast<char>(out));
      }
    }
    return text;
  }
  json
  values_json(const GameValues &values) {
    json out = {
      { "resolution", values.resolution },
      { "display", values.display },
      { "fps", values.fps },
      { "vsync", values.vsync },
      { "renderScale", values.render_scale },
      { "preset", values.preset },
      { "hdr", values.hdr },
    };
    for (auto it = values.extra.begin(); it != values.extra.end(); ++it) out[it.key()] = it.value();
    return out;
  }
  GameValues
  values_from_json(const json &body, const GameValues &base) {
    GameValues values = base;
    if (!body.is_object()) return values;
    if (body.contains("resolution") && body["resolution"].is_string()) values.resolution = body["resolution"].get<std::string>();
    if (body.contains("display") && body["display"].is_string()) values.display = body["display"].get<std::string>();
    if (body.contains("fps") && body["fps"].is_number_integer()) values.fps = body["fps"].get<int>();
    if (body.contains("vsync") && body["vsync"].is_boolean()) values.vsync = body["vsync"].get<bool>();
    if (body.contains("renderScale") && body["renderScale"].is_number()) values.render_scale = body["renderScale"].get<double>();
    if (body.contains("preset") && body["preset"].is_string()) values.preset = body["preset"].get<std::string>();
    if (body.contains("hdr") && body["hdr"].is_boolean()) values.hdr = body["hdr"].get<bool>();
    for (auto it = body.begin(); it != body.end(); ++it)
      if (it.key() != "resolution" && it.key() != "display" && it.key() != "fps" && it.key() != "vsync" && it.key() != "renderScale" && it.key() != "preset" && it.key() != "hdr") values.extra[it.key()] = it.value();
    return values;
  }
  namespace {
    void
    read_display(const Install &install, Registry &registry, SettingsResult &result) {
      const std::string root = registry_root(install);
      if (install.game == "starrail") {
        // The game itself starts from GraphicsSettings_PCResolution ({width, height, isFullScreen}); Screenmanager keeps Unity's own copy, which an older tool may have left
        // saying something else (Collapse reads this object for the same reason). The Screenmanager keys answer only when the object is missing.
        const json pc = parse_blob(registry.get(root, "GraphicsSettings_PCResolution_h431323223"));
        if (pc.is_object() && pc.value("width", 0) > 0 && pc.value("height", 0) > 0 && pc.contains("isFullScreen") && pc["isFullScreen"].is_boolean()) {
          result.values.resolution = resolution_text(pc.value("width", 0), pc.value("height", 0));
          result.values.display = pc["isFullScreen"].get<bool>() ? "fullscreen" : "windowed";
          return;
        }
      }
      const RegValue width = registry.get(root, "Screenmanager Resolution Width_h182942802");
      const RegValue height = registry.get(root, "Screenmanager Resolution Height_h2627697771");
      if (width.kind == RegValue::Kind::dword && height.kind == RegValue::Kind::dword)
        result.values.resolution = resolution_text(static_cast<int>(width.dword), static_cast<int>(height.dword));
      if (install.game == "genshin" || install.game == "hi3") {
        const RegValue full = registry.get(root, "Screenmanager Is Fullscreen mode_h3981298716");
        if (full.kind == RegValue::Kind::dword) result.values.display = full.dword ? "fullscreen" : "windowed";
      }
      else {
        const RegValue full = registry.get(root, "Screenmanager Fullscreen mode_h3630240806");
        if (full.kind == RegValue::Kind::dword) result.values.display = full.dword == 3 ? "windowed" : "fullscreen";
      }
    }
  }  // namespace
  SettingsResult
  read_game_settings(const Install &install, Registry &registry, const std::vector<std::vector<std::uint8_t>> &zzz_magics, const std::function<bool()> &running) {
    SettingsResult result;
    result.status = Status::success();
    result.running = running && running();
    const auto supported = settings_fields(install.game);
    const auto manages = [&supported](const char *field) {
      return std::find(supported.begin(), supported.end(), field) != supported.end();
    };
    for (const char *field : { "resolution", "display", "fps", "vsync", "renderScale", "preset", "hdr" }) {
      // Star Rail stores no HDR value. It is absent from the described items,
      // not an unreadable field.
      if (install.game == "starrail" && std::string_view(field) == "hdr") continue;
      if (!manages(field)) mark(result, field);
    }
    if (supported.empty()) return result;
    if (install.game == "wuwa" || install.game == "nte" || install.game == "endfield") {
      const auto file = ini_settings::game_file(install);
      if (!file || !ini_settings::has_game_section(*file)) {
        for (const auto &key : supported) mark(result, key);
        return result;
      }
      double number = 0;
      if (ini_settings::number(ini_settings::get(*file, "ResolutionSizeX"), number)) {
        const int width = static_cast<int>(number);
        if (ini_settings::number(ini_settings::get(*file, "ResolutionSizeY"), number)) result.values.resolution = resolution_text(width, static_cast<int>(number));
      }
      const std::string mode = ini_settings::get(*file, "FullscreenMode");
      if (!mode.empty()) result.values.display = mode == "2" ? "borderless" : mode == "3" ? "windowed" :
                                                                                            "fullscreen";
      if (ini_settings::number(ini_settings::get(*file, "FrameRateLimit"), number)) result.values.fps = static_cast<int>(number);
      result.values.vsync = ini_settings::get(*file, "bUseVSync") == "True" || ini_settings::get(*file, "bUseVSync") == "true";
      if (ini_settings::number(ini_settings::get(*file, "sg.ResolutionQuality"), number)) result.values.render_scale = number;
      return result;
    }
    read_display(install, registry, result);
    const std::string root = registry_root(install);
    if (install.game == "genshin") {
      const json general = parse_blob(registry.get(root, "GENERAL_DATA_h2389025596"));
      if (!general.is_object()) {
        mark(result, "fps");
        mark(result, "vsync");
        mark(result, "renderScale");
        mark(result, "preset");
      }
      else {
        const json perf = embedded(general, "globalPerfData");
        const json items = perf.is_object() ? perf.value("saveItems", json::array()) : json::array();
        result.values.fps = fps_from_genshin(read_item(items, 1, 1));
        result.values.vsync = read_item(items, 7, 0) != 0;
        result.values.render_scale = scale_from_genshin(read_item(items, 2, 2));
        result.values.preset = preset_name(read_item(items, 18, 3), "genshin");
        if (general.contains("enableHDR") && general["enableHDR"].is_boolean()) result.values.hdr = general["enableHDR"].get<bool>();
      }
      const RegValue hdr = registry.get(root, "WINDOWS_HDR_ON_h3132281285");
      if (hdr.kind == RegValue::Kind::dword) result.values.hdr = hdr.dword != 0;
    }
    else if (install.game == "hi3") {
      // The screen data blob and the two frame rates are plain JSON in the
      // registry (collapse-hi3-screen-d478585, collapse-hi3-graphics-d478585).
      // The rest of the managed fields stay unavailable for this title.
      const json screen = parse_blob(registry.get(root, "GENERAL_DATA_V2_ScreenSettingData_h1916288658"));
      if (screen.is_object() && screen.contains("width") && screen.contains("height"))
        result.values.resolution = resolution_text(screen.value("width", 1920), screen.value("height", 1080));
      if (screen.is_object() && screen.contains("isfullScreen"))
        result.values.display = screen.value("isfullScreen", true) ? "fullscreen" : "windowed";
      const json graphics = parse_blob(registry.get(root, "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519"));
      if (graphics.is_object() && graphics.contains("TargetFrameRateForInLevel"))
        result.values.fps = graphics.value("TargetFrameRateForInLevel", 60);
      else
        mark(result, "fps");
    }
    else if (install.game == "starrail") {
      const json model = parse_blob(registry.get(root, "GraphicsSettings_Model_h2986158309"));
      if (!model.is_object()) {
        mark(result, "fps");
        mark(result, "vsync");
        mark(result, "renderScale");
      }
      else {
        result.values.fps = model.value("FPS", 60);
        result.values.vsync = model.value("EnableVSync", false);
        result.values.render_scale = model.value("RenderScale", 1.0);
      }
      const RegValue quality = registry.get(root, "GraphicsSettings_GraphicsQuality_h523255858");
      if (quality.kind == RegValue::Kind::dword)
        result.values.preset = preset_name(static_cast<int>(quality.dword), "starrail");
      else
        mark(result, "preset");
      // Star Rail has no HDR setting: the game never stores one, so it is not
      // reported as an unreadable value either (the described items omit it).
    }
    else {
      for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
        mark(result, field);
      if (!zzz_magics.empty()) {
        GameValues stored;
        if (zzz_read_general_data(fs::path(install.dir) / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin", zzz_magics, stored)) {
          result.values.fps = stored.fps;
          result.values.vsync = stored.vsync;
          result.values.render_scale = stored.render_scale;
          result.values.preset = stored.preset;
          result.values.hdr = stored.hdr;
          for (auto it = stored.extra.begin(); it != stored.extra.end(); ++it) result.values.extra[it.key()] = it.value();
          for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
            result.unavailable.erase(std::remove(result.unavailable.begin(), result.unavailable.end(), std::string(field)), result.unavailable.end());
        }
      }
    }
    hoyo_read(install, registry, result);
    return result;
  }
  namespace {
    // Every write of a game's settings goes through this: a value the registry refused is counted, so a launch never reports success for settings that were not stored.
    class CheckedRegistry: public Registry {
    public:
      explicit CheckedRegistry(Registry &inner): inner_(inner) {}
      int failed = 0;
      RegValue
      get(std::string_view key, std::string_view name) const override { return inner_.get(key, name); }
      std::vector<std::string>
      subkeys(std::string_view key) const override { return inner_.subkeys(key); }
      bool
      set_dword(std::string_view key, std::string_view name, std::uint32_t value) override { return count(inner_.set_dword(key, name, value)); }
      bool
      set_binary(std::string_view key, std::string_view name, const std::vector<std::uint8_t> &bytes) override { return count(inner_.set_binary(key, name, bytes)); }
      bool
      set_text(std::string_view key, std::string_view name, std::string_view text) override { return count(inner_.set_text(key, name, text)); }

    private:
      bool
      count(bool ok) {
        if (!ok) ++failed;
        return ok;
      }
      Registry &inner_;
    };
    const char *kManagedNames[] = {
      "Screenmanager Resolution Width_h182942802",
      "Screenmanager Resolution Height_h2627697771",
      "Screenmanager Is Fullscreen mode_h3981298716",
      "Screenmanager Fullscreen mode_h3630240806",
      "Screenmanager Resolution Use Native_h1405027254",
      "Screenmanager Resolution Window Width_h2524650974",
      "Screenmanager Resolution Window Height_h1684712807",
      "video_resolution_width_h583690364",
      "video_resolution_height_h2517654917",
      "video_full_screen_h1998742411",
      "Screenmanager Resolution Width Default_h680557497",
      "Screenmanager Resolution Height Default_h1380706816",
      "GENERAL_DATA_h2389025596",
      "WINDOWS_HDR_ON_h3132281285",
      "GraphicsSettings_Model_h2986158309",
      "GraphicsSettings_PCResolution_h431323223",
      "GraphicsSettings_GraphicsQuality_h523255858",
    };
    const char *const *
    managed_names(std::size_t &count) {
      static const std::vector<const char *> names = [] {
        std::vector<const char *> out(std::begin(kManagedNames), std::end(kManagedNames));
        std::size_t extra = 0;
        const char *const *hoyo = hoyo_registry_names(extra);
        out.insert(out.end(), hoyo, hoyo + extra);
        return out;
      }();
      count = names.size();
      return names.data();
    }

    json
    snapshot_values(Registry &registry, const std::string &root) {
      json snap = json::object();
      std::size_t count = 0;
      const char *const *names = managed_names(count);
      for (std::size_t i = 0; i < count; ++i) {
        const char *name = names[i];
        const RegValue value = registry.get(root, name);
        if (value.kind == RegValue::Kind::dword)
          snap[name] = value.dword;
        else if (value.kind == RegValue::Kind::binary || value.kind == RegValue::Kind::text)
          snap[name] = blob_text(value);
      }
      return snap;
    }

    bool
    json_matches(const RegValue &value, const json &expected) {
      if (expected.is_number()) {
        if (value.kind != RegValue::Kind::dword) return false;
        return static_cast<std::int64_t>(value.dword) == expected.get<std::int64_t>();
      }
      if (expected.is_string()) return blob_text(value) == expected.get<std::string>();
      return false;
    }

    void
    apply_snapshot(Registry &registry, const std::string &root, const json &snap) {
      if (!snap.is_object()) return;
      for (auto it = snap.begin(); it != snap.end(); ++it) {
        if (it.value().is_number())
          registry.set_dword(root, it.key(), static_cast<std::uint32_t>(it.value().get<std::int64_t>()));
        else if (it.value().is_string() && it.key() == "CollapseLauncher_CustomArgs")
          registry.set_text(root, it.key(), it.value().get<std::string>());
        else if (it.value().is_string())
          registry.set_binary(root, it.key(), text_blob(it.value().get<std::string>()));
      }
    }

    // ---- Restore at the end of a session: what the host wrote goes back, what the game changed stays ----
    // A value the host wrote is put back only where the live value still equals what was written. The Genshin-style objects are one registry value holding many
    // items (and JSON inside JSON), and the game rewrites the whole value when it quits: so those are compared item by item, not as one blob.
    bool
    json_missing(const json &value) { return value.is_discarded(); }
    json
    json_text_or_missing(const json &value) {
      if (!value.is_string()) return json::value_t::discarded;
      const std::string &text = value.get_ref<const std::string &>();
      if (text.size() < 2 || (text.front() != '{' && text.front() != '[')) return json::value_t::discarded;
      return json::parse(text, nullptr, false);
    }
    const char *
    identity_key(const json &array) {
      if (!array.is_array() || array.empty()) return nullptr;
      for (const char *key : { "entryType", "key" }) {
        bool all = true;
        for (const auto &item : array)
          if (!item.is_object() || !item.contains(key)) all = false;
        if (all) return key;
      }
      return nullptr;
    }
    json
    restore_value(const json &before, const json &after, const json &live);
    // one member of an object or one identity-keyed element of an array
    void
    restore_member(const json &before, const json &after, const json &live, json &out_slot, bool &erase) {
      erase = false;
      if (json_missing(after)) return;  // the host never wrote it
      if (json_missing(before)) {  // the host added it
        if (!json_missing(live) && live == after) erase = true;
        return;
      }
      if (json_missing(live)) return;  // the game removed it
      out_slot = restore_value(before, after, live);
    }
    json
    restore_value(const json &before, const json &after, const json &live) {
      if (before == after) return live;
      if (live == after) return before;
      if (before.is_string() && after.is_string() && live.is_string()) {
        const json b = json_text_or_missing(before), a = json_text_or_missing(after), l = json_text_or_missing(live);
        if (!json_missing(b) && !json_missing(a) && !json_missing(l)) return restore_value(b, a, l).dump();
        return live;
      }
      if (before.is_object() && after.is_object() && live.is_object()) {
        json out = live;
        std::set<std::string> keys;
        for (auto it = before.begin(); it != before.end(); ++it) keys.insert(it.key());
        for (auto it = after.begin(); it != after.end(); ++it) keys.insert(it.key());
        for (const std::string &key : keys) {
          const json b = before.contains(key) ? before[key] : json(json::value_t::discarded);
          const json a = after.contains(key) ? after[key] : json(json::value_t::discarded);
          const json l = live.contains(key) ? live[key] : json(json::value_t::discarded);
          json slot = l;
          bool erase = false;
          restore_member(b, a, l, slot, erase);
          if (erase) out.erase(key);
          else if (live.contains(key)) out[key] = slot;
        }
        return out;
      }
      if (before.is_array() && after.is_array() && live.is_array()) {
        const char *id = identity_key(before.empty() ? after : before);
        if (!id || identity_key(after) == nullptr || (!live.empty() && identity_key(live) == nullptr)) return live;
        auto find = [&](const json &array, const json &key) -> json {
          for (const auto &item : array)
            if (item[id] == key) return item;
          return json::value_t::discarded;
        };
        json out = live;
        std::vector<json> ids;
        for (const json *array : { &before, &after })
          for (const auto &item : *array)
            if (std::find(ids.begin(), ids.end(), item[id]) == ids.end()) ids.push_back(item[id]);
        for (const json &key : ids) {
          const json b = find(before, key), a = find(after, key), l = find(live, key);
          json slot = l;
          bool erase = false;
          restore_member(b, a, l, slot, erase);
          for (std::size_t at = 0; at < out.size(); ++at) {
            if (out[at][id] != key) continue;
            if (erase) out.erase(out.begin() + static_cast<std::ptrdiff_t>(at));
            else out[at] = slot;
            break;
          }
        }
        return out;
      }
      return live;
    }
    // Puts one registry value back: whole when it is still what was written, item by item when it is a JSON blob the game rewrote, left alone otherwise.
    void
    restore_registry_value(Registry &registry, const std::string &root, const std::string &key, const json &before, const json &after) {
      if (before == after) return;
      const RegValue live = registry.get(root, key);
      if (json_matches(live, after)) {
        apply_snapshot(registry, root, json { { key, before } });
        return;
      }
      if (!before.is_string() || !after.is_string() || key == "CollapseLauncher_CustomArgs") return;
      const std::string text = blob_text(live);
      const json live_json = text.empty() ? json(json::value_t::discarded) : json::parse(text, nullptr, false);
      const json b = json_text_or_missing(before), a = json_text_or_missing(after);
      if (json_missing(live_json) || json_missing(b) || json_missing(a)) return;
      registry.set_binary(root, key, text_blob(restore_value(b, a, live_json).dump()));
    }

    fs::path
    zzz_general_data(const Install &install) {
      return fs::path(install.dir) / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
    }

    std::string
    hash_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      return sha256::hash(bytes);
    }

    void
    write_backup(const fs::path &path, const json &body) {
      std::ofstream out(path);
      out << body.dump(2);
    }
  }  // namespace

  SettingsResult
  write_game_settings(const Install &install, Registry &base_registry, const GameValues &values, const std::vector<std::vector<std::uint8_t>> &zzz_magics, const std::function<bool()> &running, const fs::path &backup_dir, const std::string &policy, const std::set<std::string> &fields) {
    CheckedRegistry checked(base_registry);
    Registry &registry = checked;
    SettingsResult result;
    result.values = values;
    // Only the fields the person chose are written; the others keep the game's own value (an empty set means every field).
    auto want = [&](const char *key) { return fields.empty() || fields.count(key) > 0; };
    result.running = running && running();
    if (result.running) {
      result.status = Status::fail(409, "game_running", "The game is running, so its settings were not written");
      return result;
    }
    // The fields this title's adapter can write; a title with none answers
    // that its settings stay the game's own (自动 keeps them untouched).
    const auto supported = settings_fields(install.game);
    const auto manages = [&supported](const char *field) {
      return std::find(supported.begin(), supported.end(), field) != supported.end();
    };
    for (const char *field : { "resolution", "display", "fps", "vsync", "renderScale", "preset", "hdr" }) {
      // Star Rail stores no HDR value. It is absent from the described items,
      // not an unreadable field.
      if (install.game == "starrail" && std::string_view(field) == "hdr") continue;
      if (!manages(field)) mark(result, field);
    }
    if (supported.empty()) {
      result.status = Status::fail(400, "settings_unsupported", "This title's settings stay the game's own; this host writes none of them");
      return result;
    }
    const bool borderless = install.game == "wuwa" || install.game == "nte" || install.game == "endfield";
    if (want("display") && manages("display") && values.display != "fullscreen" && values.display != "windowed" && (!borderless || values.display != "borderless")) {
      result.status = Status::fail(400, "invalid_input", "display must be fullscreen, borderless or windowed");
      return result;
    }
    if (want("fps") && manages("fps") && !allowed_fps(install.game, values.fps)) {
      result.status = Status::fail(400, "invalid_input", "That frame rate is not one of the game tiers");
      return result;
    }
    int width = 1920, height = 1080;
    if (want("resolution") && manages("resolution") && !parse_resolution(values.resolution, width, height)) {
      result.status = Status::fail(400, "invalid_input", "resolution must contain a width and a height");
      return result;
    }
    const int preset = want("preset") ? preset_index(values.preset, install.game) : 0;
    if (want("preset") && manages("preset") && preset < 0) {
      result.status = Status::fail(400, "invalid_input", "preset is not a value this game stores");
      return result;
    }
    if (const Status bad = hoyo_validate(install, values, fields); !bad.ok) {
      result.status = bad;
      return result;
    }
    const std::string root = registry_root(install);
    const std::string stored_policy = policy == "keep" ? "keep" : "restore";
    std::string zzz_before_name;
    const fs::path zzz_file = zzz_general_data(install);
    const auto ini_file = ini_settings::game_file(install);
    std::string ini_before_name;
    if (!backup_dir.empty()) {
      std::error_code ec;
      fs::create_directories(backup_dir, ec);
      if (fs::is_regular_file(zzz_file)) {
        zzz_before_name = install.id + ".general-data.before";
        fs::copy_file(zzz_file, backup_dir / zzz_before_name, fs::copy_options::overwrite_existing, ec);
        if (ec) zzz_before_name.clear();
      }
      if (ini_file && fs::is_regular_file(ini_file->path)) {
        ini_before_name = install.id + ".ini.before";
        fs::copy_file(ini_file->path, backup_dir / ini_before_name, fs::copy_options::overwrite_existing, ec);
        if (ec) ini_before_name.clear();
      }
      json backup = {
        { "policy", stored_policy },
        { "root", root },
        { "before", snapshot_values(registry, root) },
      };
      if (!zzz_before_name.empty()) backup["zzzBefore"] = zzz_before_name;
      if (!ini_before_name.empty()) backup["iniBefore"] = ini_before_name;
      if (ini_file) backup["iniPath"] = ini_file->path.string();
      write_backup(backup_dir / (install.id + ".json"), backup);
    }
    if (install.game == "wuwa" || install.game == "nte" || install.game == "endfield")
      return ini_settings::write_game_file(install, values, supported, fields, backup_dir);
    if (want("resolution") && manages("resolution")) {
      registry.set_dword(root, "Screenmanager Resolution Width_h182942802", static_cast<std::uint32_t>(width));
      registry.set_dword(root, "Screenmanager Resolution Height_h2627697771", static_cast<std::uint32_t>(height));
      // Unity starts at the screen's own size, whatever size is stored, while this flag is 1 (the player sets it when a session ended at the screen's size). A size that is written is the size to start at.
      if (registry.get(root, "Screenmanager Resolution Use Native_h1405027254").kind == RegValue::Kind::dword) registry.set_dword(root, "Screenmanager Resolution Use Native_h1405027254", 0);
      result.applied.push_back("resolution");
    }
    if (install.game == "genshin") {
      if (want("display")) {
        registry.set_dword(root, "Screenmanager Is Fullscreen mode_h3981298716", values.display == "fullscreen" ? 1u : 0u);
        result.applied.push_back("display");
      }
      RegValue general_value = registry.get(root, "GENERAL_DATA_h2389025596");
      json general = parse_blob(general_value);
      if (!general.is_object()) {
        mark(result, "fps");
        mark(result, "vsync");
        mark(result, "renderScale");
        mark(result, "preset");
      }
      else {
        json perf = embedded(general, "globalPerfData");
        if (!perf.is_object()) perf = json::object();
        json items = perf.value("saveItems", json::array());
        if (want("fps")) upsert_item(items, 1, fps_index_genshin(values.fps));
        if (want("vsync")) upsert_item(items, 7, values.vsync ? 1 : 0);
        if (want("renderScale")) upsert_item(items, 2, scale_index_genshin(values.render_scale));
        if (want("preset")) upsert_item(items, 18, preset);
        perf["saveItems"] = items;
        store_embedded(general, "globalPerfData", perf);
        json graphics = embedded(general, "graphicsData");
        if (graphics.is_object() && graphics.contains("customVolatileGrades") && graphics["customVolatileGrades"].is_array()) {
          for (int type : { 1, 2, 7, 18 }) {
            if (!want(type == 1 ? "fps" : type == 2 ? "renderScale" :
                                        type == 7   ? "vsync" :
                                                      "preset")) continue;
            int index = type == 1 ? fps_index_genshin(values.fps) : type == 7 ? (values.vsync ? 1 : 0) :
                                                                  type == 2   ? scale_index_genshin(values.render_scale) :
                                                                                preset;
            bool found = false;
            for (auto &grade : graphics["customVolatileGrades"]) {
              if (grade.value("key", -1) == type) {
                grade["value"] = index + 1;
                found = true;
              }
            }
            if (!found) graphics["customVolatileGrades"].push_back({ { "key", type }, { "value", index + 1 } });
          }
          store_embedded(general, "graphicsData", graphics);
        }
        if (want("hdr")) general["enableHDR"] = values.hdr;
        put_json_blob(registry, root, "GENERAL_DATA_h2389025596", general);
        for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
          if (want(field)) result.applied.push_back(field);
      }
      if (want("hdr")) registry.set_dword(root, "WINDOWS_HDR_ON_h3132281285", values.hdr ? 1u : 0u);
      if (want("hdr") && std::find(result.applied.begin(), result.applied.end(), "hdr") == result.applied.end()) result.applied.push_back("hdr");
    }
    else if (install.game == "hi3") {
      // Screen data and the two frame rates, the same values the reference
      // launcher writes (collapse-hi3-screen-d478585,
      // collapse-hi3-graphics-d478585); everything else stays the game's own.
      if (want("display")) {
        registry.set_dword(root, "Screenmanager Is Fullscreen mode_h3981298716", values.display == "fullscreen" ? 1u : 0u);
        result.applied.push_back("display");
      }
      if (want("resolution") || want("display")) {
        json screen = parse_blob(registry.get(root, "GENERAL_DATA_V2_ScreenSettingData_h1916288658"));
        if (!screen.is_object()) screen = json::object();
        if (want("resolution")) {
          screen["width"] = width;
          screen["height"] = height;
        }
        if (want("display")) screen["isfullScreen"] = values.display == "fullscreen";
        put_json_blob(registry, root, "GENERAL_DATA_V2_ScreenSettingData_h1916288658", screen);
      }
      if (want("fps")) {
        json graphics = parse_blob(registry.get(root, "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519"));
        if (!graphics.is_object()) {
          mark(result, "fps");
        }
        else {
          graphics["TargetFrameRateForInLevel"] = values.fps;
          graphics["TargetFrameRateForOthers"] = values.fps;
          put_json_blob(registry, root, "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519", graphics);
          result.applied.push_back("fps");
        }
      }
    }
    else if (install.game == "starrail") {
      if (want("display")) {
        registry.set_dword(root, "Screenmanager Fullscreen mode_h3630240806", values.display == "windowed" ? 3u : 1u);
        result.applied.push_back("display");
      }
      if (want("resolution") || want("display")) {
        json resolution = parse_blob(registry.get(root, "GraphicsSettings_PCResolution_h431323223"));
        if (!resolution.is_object()) resolution = json::object();
        if (want("resolution")) {
          resolution["width"] = width;
          resolution["height"] = height;
        }
        if (want("display")) resolution["isFullScreen"] = values.display == "fullscreen";
        put_json_blob(registry, root, "GraphicsSettings_PCResolution_h431323223", resolution);
      }
      json model = parse_blob(registry.get(root, "GraphicsSettings_Model_h2986158309"));
      if (!model.is_object()) {
        mark(result, "fps");
        mark(result, "vsync");
        mark(result, "renderScale");
      }
      else {
        if (want("fps")) model["FPS"] = values.fps;
        if (want("vsync")) model["EnableVSync"] = values.vsync;
        if (want("renderScale")) model["RenderScale"] = values.render_scale;
        put_json_blob(registry, root, "GraphicsSettings_Model_h2986158309", model);
        for (const char *field : { "fps", "vsync", "renderScale" })
          if (want(field)) result.applied.push_back(field);
      }
      if (want("preset")) {
        registry.set_dword(root, "GraphicsSettings_GraphicsQuality_h523255858", static_cast<std::uint32_t>(preset));
        result.applied.push_back("preset");
      }
    }
    else {
      if (want("display")) {
        registry.set_dword(root, "Screenmanager Fullscreen mode_h3630240806", values.display == "windowed" ? 3u : 1u);
        result.applied.push_back("display");
      }
      if (want("resolution")) {
        // with the size written above this is the pair Collapse's Zenless page saves: the size the game reads and its Default twin
        registry.set_dword(root, "Screenmanager Resolution Width Default_h680557497", static_cast<std::uint32_t>(width));
        registry.set_dword(root, "Screenmanager Resolution Height Default_h1380706816", static_cast<std::uint32_t>(height));
      }
      const fs::path file = fs::path(install.dir) / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
      const bool file_items = !zzz_magics.empty() && fs::is_regular_file(file);
      if (!file_items) {
        for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
          mark(result, field);
      }
      else {
        std::string error;
        // The edit replaces only the stored numbers and keeps every other
        // byte of the document; a file the shipped key cannot decode is
        // never written and reports the fields unavailable instead.
        if (zzz_write_general_data(file, zzz_magics, values, fields, error)) {
          for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
            if (want(field)) result.applied.push_back(field);
        }
        else {
          for (const char *field : { "fps", "vsync", "renderScale", "preset", "hdr" })
            mark(result, field);
        }
      }
    }
    hoyo_write(install, registry, values, fields, result);
    if (checked.failed > 0) {
      result.status = Status::fail(500, "registry_write_failed", "The game's settings could not be stored");
      return result;
    }
    if (!backup_dir.empty()) {
      json backup;
      const fs::path backup_path = backup_dir / (install.id + ".json");
      std::ifstream in(backup_path);
      try {
        in >> backup;
      }
      catch (const std::exception &) {
        backup = { { "policy", stored_policy }, { "root", root }, { "before", json::object() } };
      }
      backup["after"] = snapshot_values(registry, root);
      if (!zzz_before_name.empty() && fs::is_regular_file(zzz_file))
        backup["zzzAfter"] = hash_file(zzz_file);
      write_backup(backup_path, backup);
    }
    result.status = Status::success();
    return result;
  }

  Status
  write_endfield_display(const Install &install, Registry &registry, int width, int height, const std::string &window, const fs::path &backup_dir, const std::string &policy) {
    const std::string root = registry_root(install);
    const bool sized = width >= 320 && height >= 240;
    if (!sized && window.empty()) return Status::success();
    if (!window.empty() && window != "fullscreen" && window != "windowed" && window != "borderless") return Status::fail(400, "invalid_input", "display must be fullscreen, borderless or windowed");
    json backup = { { "policy", policy == "keep" ? "keep" : "restore" }, { "root", root }, { "before", snapshot_values(registry, root) } };
    if (!backup_dir.empty()) {
      std::error_code ec;
      fs::create_directories(backup_dir, ec);
      write_backup(backup_dir / (install.id + ".json"), backup);
    }
    // The game applies its own saved display settings (video_*) after it has started: that is how a launch with -screen-fullscreen 1 turned into 1280x720 in a window.
    // So the game's own keys carry the choice, and Unity's Screenmanager copy agrees with them.
    bool ok = true;
    if (sized) {
      ok = registry.set_dword(root, "Screenmanager Resolution Width_h182942802", static_cast<std::uint32_t>(width)) && ok;
      ok = registry.set_dword(root, "Screenmanager Resolution Height_h2627697771", static_cast<std::uint32_t>(height)) && ok;
      ok = registry.set_dword(root, "video_resolution_width_h583690364", static_cast<std::uint32_t>(width)) && ok;
      ok = registry.set_dword(root, "video_resolution_height_h2517654917", static_cast<std::uint32_t>(height)) && ok;
      if (window == "windowed" || window == "borderless") {
        ok = registry.set_dword(root, "Screenmanager Resolution Window Width_h2524650974", static_cast<std::uint32_t>(width)) && ok;
        ok = registry.set_dword(root, "Screenmanager Resolution Window Height_h1684712807", static_cast<std::uint32_t>(height)) && ok;
      }
    }
    if (!window.empty()) {
      const bool full = window == "fullscreen";
      // Unity's FullScreenMode: 1 the full-screen window, 3 a window; a borderless window is a window (-popupwindow on the command line takes the frame off)
      ok = registry.set_dword(root, "Screenmanager Fullscreen mode_h3630240806", full ? 1u : 3u) && ok;
      ok = registry.set_dword(root, "video_full_screen_h1998742411", full ? 1u : 0u) && ok;
    }
    if (registry.get(root, "Screenmanager Resolution Use Native_h1405027254").kind == RegValue::Kind::dword) ok = registry.set_dword(root, "Screenmanager Resolution Use Native_h1405027254", 0) && ok;
    if (!backup_dir.empty()) {
      backup["after"] = snapshot_values(registry, root);
      write_backup(backup_dir / (install.id + ".json"), backup);
    }
    return ok ? Status::success() : Status::fail(500, "registry_write_failed", "The game's display settings could not be stored");
  }

  std::set<std::string>
  chosen_fields(const json &settings) {
    std::set<std::string> out;
    if (!settings.is_object()) return out;
    static const std::set<std::string> known = { "resolution", "display", "fps", "vsync", "renderScale", "preset", "hdr", "launchWidth", "launchHeight", "launchFullscreen", "popupWindow", "dxVersion", "launcherParent", "smooth-fps", "fixed-fps", "auto-quality", "shadow", "effects", "detail", "texture", "anti-aliasing", "anisotropy", "view-distance", "npc-density", "vegetation", "vegetation-dither", "dynamic-bones", "environment-interaction", "teammate-effects", "hit-material", "damage-effects", "lighting", "ambient-occlusion", "volumetric-fog", "volumetric-light", "bloom", "motion-blur", "auto-exposure", "dlss", "dlss-quality", "dlss-sharpness", "native-fg", "fsr", "fsr-quality", "fsr-fg", "fsr-legacy", "xess", "xess-quality", "xess-fg", "ray-tracing", "ray-quality", "ray-gi", "ray-reflections", "ray-shadows", "brightness", "saturation", "contrast", "image-display", "eye-mode", "eye-temperature", "eye-strength", "eye-brightness", "eye-texture" };
    for (auto it = settings.begin(); it != settings.end(); ++it)
      if ((known.count(it.key()) || hoyo_is_field(it.key())) && (it.value().is_boolean() || it.value().is_number() || it.value().is_string())) out.insert(it.key());
    return out;
  }

  void
  reconcile_pending_settings(const Install &install, Registry &registry, const fs::path &backup_dir, const std::vector<std::vector<std::uint8_t>> &zzz_magics) {
    (void) zzz_magics;
    std::error_code ec;
    if (backup_dir.empty() || !fs::is_directory(backup_dir, ec)) return;
    const std::string root = registry_root(install);
    const fs::path zzz_file = zzz_general_data(install);
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(backup_dir, ec)) {
      if (entry.path().extension() == ".json") files.push_back(entry.path());
    }
    for (const fs::path &path : files) {
      json body;
      try {
        std::ifstream in(path);
        in >> body;
      }
      catch (const std::exception &) {
        continue;
      }
      if (!body.is_object() || body.value("root", std::string()) != root) continue;
      const bool keep = body.value("policy", std::string("restore")) == "keep";
      // Each value the host wrote goes back on its own; a value the game changed since stays as the game has it.
      if (!keep && body.contains("before") && body["before"].is_object() && body.contains("after") && body["after"].is_object()) {
        for (auto it = body["after"].begin(); it != body["after"].end(); ++it) {
          const json &before_value = body["before"].contains(it.key()) ? body["before"][it.key()] : json(json::value_t::discarded);
          if (json_missing(before_value)) {
            // a value this launch created: it goes again, unless the game has written it since
            if (json_matches(registry.get(root, it.key()), it.value())) registry.remove(root, it.key());
            continue;
          }
          restore_registry_value(registry, root, it.key(), before_value, it.value());
        }
      }
      else if (!keep && body.contains("before") && body["before"].is_object()) {
        // a backup of a write that never finished (no after was recorded): the previous values go back whole
        apply_snapshot(registry, root, body["before"]);
      }
      if (!keep && body.contains("zzzBefore") && body["zzzBefore"].is_string()) {
        const bool unchanged = !body.contains("zzzAfter") || !body["zzzAfter"].is_string() || (fs::is_regular_file(zzz_file) && hash_file(zzz_file) == body["zzzAfter"].get<std::string>());
        const fs::path saved = backup_dir / body["zzzBefore"].get<std::string>();
        if (unchanged && fs::is_regular_file(saved)) {
          fs::create_directories(zzz_file.parent_path(), ec);
          fs::copy_file(saved, zzz_file, fs::copy_options::overwrite_existing, ec);
        }
      }
      if (!keep && body.contains("iniBefore") && body["iniBefore"].is_string()) {
        const fs::path ini_path = body.value("iniPath", std::string());
        const fs::path saved = backup_dir / body["iniBefore"].get<std::string>();
        if (!ini_path.empty() && fs::is_regular_file(saved)) fs::copy_file(saved, ini_path, fs::copy_options::overwrite_existing, ec);
      }
      if (body.contains("zzzBefore") && body["zzzBefore"].is_string())
        fs::remove(backup_dir / body["zzzBefore"].get<std::string>(), ec);
      if (body.contains("iniBefore") && body["iniBefore"].is_string()) fs::remove(backup_dir / body["iniBefore"].get<std::string>(), ec);
      fs::remove(path, ec);
    }
  }
}  // namespace sparxie
