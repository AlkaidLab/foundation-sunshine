#include "src/sparxie/sparxie_build.h"
#include "src/sparxie/sha256.h"
#include <algorithm>
#include <fstream>
#include <iterator>
namespace sparxie {
  namespace {
    constexpr std::size_t kMaxManifestBytes = 64 * 1024;
    std::string
    read_file(const fs::path &path, std::size_t limit = 0) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (limit != 0 && bytes.size() > limit) return {};
      return bytes;
    }
    bool
    is_file(const fs::path &path) {
      std::error_code ec;
      return fs::is_regular_file(path, ec);
    }
    bool
    contains(const std::string &bytes, std::string_view marker) { return bytes.find(marker) != std::string::npos; }
    // The app-<version> folder Sparxie runs from: the one version.ini names, else the newest by name.
    fs::path
    app_folder(const fs::path &dir) {
      std::error_code ec;
      const std::string ini = read_file(dir / "version.ini", 4096);
      const std::size_t at = ini.find("exe_path=");
      if (at != std::string::npos) {
        std::size_t end = ini.find_first_of("\r\n", at);
        std::string path = ini.substr(at + 9, end == std::string::npos ? std::string::npos : end - at - 9);
        std::replace(path.begin(), path.end(), '\\', '/');
        const std::size_t cut = path.find('/');
        if (cut != std::string::npos && fs::is_directory(dir / path.substr(0, cut), ec)) return dir / path.substr(0, cut);
      }
      fs::path newest;
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("app-", 0) != 0 || !entry.is_directory(ec)) continue;
        if (newest.empty() || name > newest.filename().string()) newest = entry.path();
      }
      return newest;
    }
    bool
    lists_capability(const fs::path &app, std::string_view name) {
      try {
        const json body = json::parse(read_file(app / "capabilities.json", 4096));
        if (!body.is_object() || !body.contains("capabilities") || !body["capabilities"].is_array()) return false;
        for (const auto &item : body["capabilities"])
          if (item.is_string() && item.get<std::string>() == name) return true;
      }
      catch (const std::exception &) {
      }
      return false;
    }
    // a string member, "" when it is missing or of another type (the files are another party's input)
    std::string
    text_of(const json &object, const char *key) {
      return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
    }
    bool
    same_hex(const std::string &a, const std::string &b) { return !a.empty() && ascii_lower(a) == ascii_lower(b); }
    EndfieldPlugin
    detect_endfield_plugin_unchecked(const fs::path &dir) {
      EndfieldPlugin found;
      const fs::path folder = dir / "plugins" / kEndfieldPluginId;
      const std::string manifest_bytes = read_file(folder / "plugin.json", kMaxManifestBytes);
      if (manifest_bytes.empty()) return found;
      json manifest;
      try {
        manifest = json::parse(manifest_bytes);
      }
      catch (const std::exception &) {
        return found;
      }
      // a field of another type than the manifest promises (an id that is a number, a null version) is a manifest this host cannot use: not installed
      if (!manifest.is_object() || !manifest.contains("id") || !manifest["id"].is_string() || manifest["id"].get<std::string>() != kEndfieldPluginId) return found;
      bool endfield_exe = false;
      if (manifest.contains("exeNames") && manifest["exeNames"].is_array())
        for (const auto &name : manifest["exeNames"])
          if (name.is_string() && ascii_lower(name.get<std::string>()) == "endfield.exe") endfield_exe = true;
      if (!endfield_exe || !is_file(folder / "module.dll")) return found;
      found.installed = true;
      found.version = manifest.contains("version") && manifest["version"].is_string() ? manifest["version"].get<std::string>() : std::string();
      // whatever the schema holds, the keys it declares are known: none declared means the host writes none
      found.keys_known = true;
      if (manifest.contains("settingsSchema") && manifest["settingsSchema"].is_object() && manifest["settingsSchema"].contains("fields") && manifest["settingsSchema"]["fields"].is_array())
        for (const auto &field : manifest["settingsSchema"]["fields"])
          if (field.is_object() && field.contains("key") && field["key"].is_string()) found.keys.push_back(field["key"].get<std::string>());
      // Sparxie loads a plugin only when its trust record matches the two files as they are now
      try {
        const json trust = json::parse(read_file(dir / "plugin-trust.json", 1024 * 1024));
        if (trust.is_object() && trust.contains("records") && trust["records"].is_array()) {
          const std::string manifest_hash = sha256::hash(manifest_bytes);
          std::string module_hash;
          for (const auto &record : trust["records"]) {
            if (!record.is_object() || text_of(record, "id") != kEndfieldPluginId) continue;
            if (!same_hex(text_of(record, "manifestSha256"), manifest_hash)) continue;
            if (module_hash.empty()) module_hash = sha256::hash(read_file(folder / "module.dll"));
            if (same_hex(text_of(record, "dllSha256"), module_hash)) found.trusted = true;
          }
        }
      }
      catch (const std::exception &) {
      }
      return found;
    }
    // The files of a plugin are another party's input: nothing in them may make the host fail for every game.
    EndfieldPlugin
    detect_endfield_plugin(const fs::path &dir) {
      try {
        return detect_endfield_plugin_unchecked(dir);
      }
      catch (const std::exception &) {
        return {};
      }
    }
  }  // namespace

  bool
  is_plugin_game_id(std::string_view id) {
    if (id.rfind("ext.", 0) != 0) return false;
    const std::string_view rest = id.substr(4);
    const std::size_t dot = rest.find('.');
    if (dot == std::string_view::npos) return false;
    auto part_ok = [](std::string_view part) {
      if (part.size() < 2 || part.size() > 32) return false;
      return std::all_of(part.begin(), part.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
    };
    return part_ok(rest.substr(0, dot)) && part_ok(rest.substr(dot + 1));
  }

  SparxieBuild
  detect_build(const fs::path &dir) {
    SparxieBuild build;
    if (dir.empty()) return build;
    build.endfield = detect_endfield_plugin(dir);
    const fs::path app = app_folder(dir);
    if (app.empty()) return build;
    const std::string contracts = read_file(app / "Sparxie.Contracts.dll");
    if (!contracts.empty())
      build.generation = contains(contracts, "GameIds") && contains(contracts, "SettingsJson") ? Generation::plugin : Generation::legacy;
    build.builtin_endfield = lists_capability(app, "endfield") || (is_file(app / "EndfieldCore.dll") && is_file(app / "EndfieldRuntime.dll"));
    return build;
  }

  EndfieldSupport
  endfield_support(const SparxieBuild &build) {
    switch (build.generation) {
      case Generation::plugin:
        return build.endfield.installed ? EndfieldSupport::plugin : EndfieldSupport::plugin_missing;
      case Generation::legacy:
        return build.builtin_endfield ? EndfieldSupport::builtin : EndfieldSupport::unsupported;
      case Generation::unknown:
        break;
    }
    return EndfieldSupport::builtin;
  }

  std::string_view
  endfield_support_name(EndfieldSupport support) {
    switch (support) {
      case EndfieldSupport::builtin: return "builtin";
      case EndfieldSupport::plugin: return "plugin";
      case EndfieldSupport::plugin_missing: return "plugin_missing";
      case EndfieldSupport::unsupported: return "unsupported";
    }
    return "unsupported";
  }

  std::string
  endfield_game_id(const SparxieBuild &build) {
    switch (endfield_support(build)) {
      case EndfieldSupport::builtin: return "endfield";
      case EndfieldSupport::plugin: return kEndfieldPluginId;
      default: return {};
    }
  }

  bool
  build_reads_game(const SparxieBuild &build, std::string_view game) {
    if (game == "genshin" || game == "starRail" || game == "zenlessZoneZero") return true;
    if (game == "endfield") return build.generation != Generation::plugin && (build.generation == Generation::unknown || build.builtin_endfield);
    if (is_plugin_game_id(game)) return build.generation != Generation::legacy;
    return false;
  }

  Status
  endfield_write_check(const SparxieBuild &build) {
    switch (endfield_support(build)) {
      case EndfieldSupport::plugin_missing:
        return Status::fail(409, "endfield_plugin_missing", "需要终末地插件：已安装的 Sparxie 把终末地作为插件提供，请先安装并启用终末地插件");
      case EndfieldSupport::unsupported:
        return Status::fail(409, "endfield_unsupported", "已安装的 Sparxie 不支持终末地");
      default:
        return Status::success();
    }
  }
}  // namespace sparxie
