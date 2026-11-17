#include "src/sparxie/model.h"
#include "src/sparxie/stream_display.h"

#include "src/sparxie/region.h"

#include <algorithm>
#include <cctype>

namespace sparxie {
  namespace {
    bool
    key_eq(std::string_view a, std::string_view b) { return ascii_lower(a) == ascii_lower(b); }
  }  // namespace

  namespace {
    bool
    to_bool(json &value) {
      if (!value.is_string()) return false;
      const std::string text = ascii_lower(value.get<std::string>());
      if (text == "true")
        value = true;
      else if (text == "false")
        value = false;
      else
        return false;
      return true;
    }
    bool
    to_number(json &value, bool integer) {
      if (!value.is_string()) return false;
      const std::string text = value.get<std::string>();
      if (text.empty()) return false;
      std::size_t used = 0;
      try {
        if (integer) {
          const long long parsed = std::stoll(text, &used);
          if (used != text.size()) return false;
          value = parsed;
        }
        else {
          const double parsed = std::stod(text, &used);
          if (used != text.size()) return false;
          value = parsed;
        }
      }
      catch (const std::exception &) {
        return false;
      }
      return true;
    }
    bool
    empty_text(const json &value) { return value.is_string() && value.get<std::string>().empty(); }
    bool
    type_keys(json &object, std::initializer_list<const char *> booleans, std::initializer_list<const char *> integers, std::initializer_list<const char *> decimals) {
      bool changed = false;
      if (!object.is_object()) return false;
      for (const char *key : booleans)
        if (object.contains(key)) changed |= to_bool(object[key]);
      for (const char *key : integers)
        if (object.contains(key)) changed |= to_number(object[key], true);
      for (const char *key : decimals)
        if (object.contains(key)) changed |= to_number(object[key], false);
      return changed;
    }
  }  // namespace

  bool
  normalize_block(json &block) {
    if (!block.is_object()) return false;
    bool changed = false;
    for (const char *key : { "options", "settings" })
      if (block.contains(key) && empty_text(block[key])) {
        block[key] = json::object();
        changed = true;
      }
    for (const char *key : { "candidates", "markers" })
      if (block.contains(key) && empty_text(block[key])) {
        block[key] = json::array();
        changed = true;
      }
    changed |= type_keys(block, { "launchTemplateProvisional", "badge" }, {}, {});
    // fps may also be the word "display" (the tier that follows the refresh rate): only a number is converted
    if (block.contains("options")) changed |= type_keys(block["options"], { "fpsUnlock", "bgLimit", "followPreset", "scaleOverride", "isDefault" }, { "fps", "bgFps", "preset30", "preset60", "scale" }, {});
    if (block.contains("settings")) changed |= type_keys(block["settings"], { "vsync", "hdr" }, { "fps" }, { "renderScale" });
    // every application has a display choice. One that never chose follows the stream; an Endfield application that was given a fixed size keeps it.
    if (block.contains("game") && block["game"].is_string()) {
      const json before = block.contains("display") ? block["display"] : json();
      json display = normalized_display(before);
      if (!block.contains("display") && block.value("game", std::string()) == "endfield" && block.contains("options") && block["options"].is_object() && block["options"].contains("resolution") && block["options"]["resolution"].is_string()) {
        const std::string size = block["options"]["resolution"].get<std::string>();
        int width = 0, height = 0;
        if (parse_resolution(size, width, height) && width > 0 && height > 0) display = { { "mode", "fixed" }, { "resolution", resolution_text(width, height) }, { "window", "fullscreen" } };
      }
      if (display != before) {
        block["display"] = display;
        changed = true;
      }
      const json options_before = block.contains("options") ? block["options"] : json();
      mirror_display_to_options(block);
      changed |= block.contains("options") && block["options"] != options_before;
    }
    return changed;
  }

  bool
  normalize_apps(json &apps) {
    if (!apps.is_object() || !apps.contains("apps") || !apps["apps"].is_array()) return false;
    bool changed = false;
    for (auto &app : apps["apps"])
      if (app.is_object() && app.contains("sparxie")) changed |= normalize_block(app["sparxie"]);
    return changed;
  }

  std::string
  ascii_lower(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
  }

  std::string
  norm_path(const fs::path &path) {
    std::string text = path.generic_string();
    for (char &c : text)
      if (c == '\\') c = '/';
    while (text.size() > 1 && text.back() == '/') text.pop_back();
    return ascii_lower(text);
  }

  bool
  paths_equal(const fs::path &a, const fs::path &b) { return norm_path(a) == norm_path(b); }

  std::string
  fnv8(std::string_view text) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
      hash ^= c;
      hash *= 16777619u;
    }
    static const char *digits = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
      out[i] = digits[hash & 0x0f];
      hash >>= 4;
    }
    return out;
  }

  std::string
  to_sparxie_game(std::string_view game) {
    if (game == "starrail") return "starRail";
    if (game == "zzz") return "zenlessZoneZero";
    if (game == "endfield") return "endfield";  // the Broker's game id of an Endfield session (capability "endfield")
    return "genshin";
  }

  std::string
  from_sparxie_game(std::string_view game) {
    if (game == "starRail") return "starrail";
    if (game == "zenlessZoneZero") return "zzz";
    if (game == "genshin") return "genshin";
    if (game == "endfield") return "endfield";
    return {};
  }

  std::string
  biz_for(std::string_view game, std::string_view region) {
    const char *prefix = nullptr;
    if (game == "genshin")
      prefix = "hk4e";
    else if (game == "starrail")
      prefix = "hkrpg";
    else if (game == "zzz")
      prefix = "nap";
    else if (game == "hi3")
      prefix = "bh3";
    else
      return {};
    const char *suffix = (region == "global") ? "global" : "cn";
    return std::string(prefix) + "_" + suffix;
  }

  std::string
  quote_path(std::string_view path) { return std::string("\"") + std::string(path) + "\""; }

  std::string
  apply_template(std::string_view templ, const std::vector<std::pair<std::string, std::string>> &vars) {
    std::string out(templ);
    for (const auto &var : vars) {
      const std::string key = "{" + var.first + "}";
      for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at)) {
        out.replace(at, key.size(), var.second);
        at += var.second.size();
      }
    }
    return out;
  }

  json
  install_json(const Install &install) {
    json body = {
      { "id", install.id },
      { "game", install.game },
      { "region", install.region },
      { "candidates", install.candidates },
      { "markers", install.markers },
      { "sources", install.sources },
      { "dir", install.dir },
      { "exe", install.exe },
      { "version", install.version },
      { "biz", install.biz },
      { "profile", install.profile },
      { "source", install.source },
      { "art", install.art },
    };
    if (!install.epic_app.empty()) body["epicApp"] = install.epic_app;
    if (install.channel >= 0) {
      body["channel"] = install.channel;
      body["sub_channel"] = install.sub_channel;
      body["channel_mapping"] = "sourced";
    }
    if (!install.region.empty()) {
      body["regionName"] = region_display(install.region);
      body["regionNameEn"] = region_display_en(install.region);
    }
    return body;
  }

  std::string
  resolution_text(int width, int height) {
    return std::to_string(width) + "\u00d7" + std::to_string(height);
  }

  bool
  parse_resolution(std::string_view text, int &width, int &height) {
    std::string s(text);
    for (char &c : s) {
      if (c == 'x' || c == 'X' || c == '*' || static_cast<unsigned char>(c) == 0xc3) continue;
    }
    std::string digits;
    std::vector<int> nums;
    auto flush = [&]() {
      if (!digits.empty()) {
        nums.push_back(std::atoi(digits.c_str()));
        digits.clear();
      }
    };
    for (unsigned char c : s) {
      if (c >= '0' && c <= '9')
        digits.push_back(static_cast<char>(c));
      else
        flush();
    }
    flush();
    if (nums.size() < 2 || nums[0] < 320 || nums[1] < 240) return false;
    width = nums[0];
    height = nums[1];
    return true;
  }

  MemoryRegistry::Row *
  MemoryRegistry::find(std::string_view key, std::string_view name) {
    for (auto &row : rows)
      if (key_eq(row.key, key) && key_eq(row.name, name)) return &row;
    return nullptr;
  }
  const MemoryRegistry::Row *
  MemoryRegistry::find(std::string_view key, std::string_view name) const {
    for (const auto &row : rows)
      if (key_eq(row.key, key) && key_eq(row.name, name)) return &row;
    return nullptr;
  }
  RegValue
  MemoryRegistry::get(std::string_view key, std::string_view name) const {
    if (const auto *row = find(key, name)) return row->value;
    return {};
  }
  std::vector<std::string>
  MemoryRegistry::subkeys(std::string_view key) const {
    std::vector<std::string> names;
    const std::string prefix = ascii_lower(key) + "\\";
    for (const auto &row : rows) {
      const std::string text = ascii_lower(row.key);
      if (text.rfind(prefix, 0) != 0) continue;
      const std::size_t rest = text.find('\\', prefix.size());
      // values live at any depth; the child name is the first component
      const std::string name = row.key.substr(prefix.size(), rest == std::string::npos ? std::string::npos : rest - prefix.size());
      if (name.empty()) continue;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
  }
  bool
  MemoryRegistry::set_dword(std::string_view key, std::string_view name, std::uint32_t value) {
    RegValue stored;
    stored.kind = RegValue::Kind::dword;
    stored.dword = value;
    if (auto *row = find(key, name))
      row->value = stored;
    else
      rows.push_back(Row { std::string(key), std::string(name), stored });
    return true;
  }
  bool
  MemoryRegistry::set_binary(std::string_view key, std::string_view name, const std::vector<std::uint8_t> &bytes) {
    RegValue stored;
    stored.kind = RegValue::Kind::binary;
    stored.bytes = bytes;
    if (auto *row = find(key, name))
      row->value = stored;
    else
      rows.push_back(Row { std::string(key), std::string(name), stored });
    return true;
  }
  bool
  MemoryRegistry::remove(std::string_view key, std::string_view name) {
    for (auto it = rows.begin(); it != rows.end(); ++it) {
      if (it->key == key && it->name == name) {
        rows.erase(it);
        return true;
      }
    }
    return false;
  }
  bool
  MemoryRegistry::set_text(std::string_view key, std::string_view name, std::string_view text) {
    RegValue stored;
    stored.kind = RegValue::Kind::text;
    stored.text = std::string(text);
    if (auto *row = find(key, name))
      row->value = std::move(stored);
    else
      rows.push_back(Row { std::string(key), std::string(name), std::move(stored) });
    return true;
  }
}  // namespace sparxie
