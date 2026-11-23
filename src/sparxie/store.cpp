#include "src/sparxie/store.h"
#include "src/sparxie/sha256.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <thread>
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif
namespace sparxie {
  namespace {
    std::string
    read_bytes(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) return {};
      return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    bool
    write_bytes(const fs::path &path, const std::string &bytes) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      out.flush();
      return static_cast<bool>(out);
    }
    bool
    replace_file(const fs::path &from, const fs::path &to) {
#if defined(_WIN32)
      return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
      std::error_code ec;
      fs::rename(from, to, ec);
      return !ec;
#endif
    }
    std::string
    utc_now() {
      const std::time_t now = std::time(nullptr);
      std::tm tm {};
#if defined(_WIN32)
      gmtime_s(&tm, &now);
#else
      gmtime_r(&now, &tm);
#endif
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
      return buf;
    }
    bool
    allowed_exe(const std::string &game, const std::string &path) {
      // the config holds Windows paths: split on both separators, whatever system reads it
      const std::size_t cut = path.find_last_of("/\\");
      const std::string name = ascii_lower(cut == std::string::npos ? path : path.substr(cut + 1));
      if (game == "genshin") return name == "yuanshen.exe" || name == "genshinimpact.exe";
      if (game == "starRail") return name == "starrail.exe";
      if (game == "zenlessZoneZero") return name == "zenlesszonezero.exe" || name == "zenlesszonezerobeta.exe";
      if (game == "endfield" || game == kEndfieldPluginId) return name == "endfield.exe";
      // another plugin's executables are the plugin's own list (plugin.json exeNames); the host cannot judge them
      return is_plugin_game_id(game);
    }
    bool
    in_range(int value, int lo, int hi) { return value >= lo && value <= hi; }
    // Sparxie rejects a profile outside these limits and then moves the whole config aside (AppConfigValidator). The host writes nothing outside them.
    constexpr int kFpsMin = 10, kFpsMax = 1000;
    constexpr int kScaleMin = 100, kScaleMax = 500;
    constexpr int kEndfieldRefreshMin = 24, kEndfieldRefreshMax = 1000;
    constexpr int kEndfieldWidthMin = 320, kEndfieldWidthMax = 7680;
    constexpr int kEndfieldHeightMin = 240, kEndfieldHeightMax = 4320;
    // Sparxie's own default size of an Endfield profile; it stays in the file while the size follows the stream
    constexpr int kEndfieldDefaultWidth = 1920, kEndfieldDefaultHeight = 1080;
    bool
    endfield_size_ok(int width, int height) {
      return in_range(width, kEndfieldWidthMin, kEndfieldWidthMax) && in_range(height, kEndfieldHeightMin, kEndfieldHeightMax);
    }
    // "1920×1080" (or 1920x1080): the fixed size of an Endfield session. "auto", anything else, and a size Sparxie would reject follow the stream (0 × 0 here; the file never gets 0)
    std::pair<int, int>
    endfield_size(const std::string &text) {
      std::size_t cut = text.find("\xC3\x97");
      std::size_t skip = 2;
      if (cut == std::string::npos) {
        cut = text.find('x');
        skip = 1;
      }
      if (cut == std::string::npos) return { 0, 0 };
      try {
        const int w = std::stoi(text.substr(0, cut));
        const int h = std::stoi(text.substr(cut + skip));
        return endfield_size_ok(w, h) ? std::pair<int, int> { w, h } : std::pair<int, int> { 0, 0 };
      }
      catch (const std::exception &) {
        return { 0, 0 };
      }
    }
    std::string
    known_priority(const std::string &priority) {
      return priority == "belowNormal" || priority == "aboveNormal" || priority == "high" ? priority : "normal";
    }
    // The size the profile already holds when Sparxie would accept it, else Sparxie's default: the value kept in the file while the size follows the stream
    std::pair<int, int>
    kept_endfield_size(const json *existing) {
      if (existing && existing->is_object() && existing->contains("endfieldWidth") && existing->contains("endfieldHeight") && (*existing)["endfieldWidth"].is_number_integer() &&
          (*existing)["endfieldHeight"].is_number_integer()) {
        const int w = (*existing)["endfieldWidth"].get<int>();
        const int h = (*existing)["endfieldHeight"].get<int>();
        if (endfield_size_ok(w, h)) return { w, h };
      }
      return { kEndfieldDefaultWidth, kEndfieldDefaultHeight };
    }
    // The options of an Endfield application as the profile "settings" of the second generation, limited to the keys the installed plugin declares.
    // `existing` is the profile as it is now (its width and height stay while the size follows the stream).
    json
    endfield_plugin_settings(const json &options, int display_fps, const EndfieldPlugin &plugin, const json *existing) {
      const std::pair<int, int> fixed = endfield_size(options.value("resolution", std::string("auto")));
      const bool fixed_size = fixed.first > 0;
      std::pair<int, int> kept = { kEndfieldDefaultWidth, kEndfieldDefaultHeight };
      if (existing && existing->is_object() && existing->contains("settings") && (*existing)["settings"].is_object()) {
        const json &old = (*existing)["settings"];
        if (old.contains("width") && old.contains("height") && old["width"].is_number_integer() && old["height"].is_number_integer() &&
            endfield_size_ok(old["width"].get<int>(), old["height"].get<int>()))
          kept = { old["width"].get<int>(), old["height"].get<int>() };
      }
      const std::pair<int, int> size = fixed_size ? fixed : kept;
      const bool unlock = options.contains("fpsUnlock") && options["fpsUnlock"].is_boolean() ? options["fpsUnlock"].get<bool>() : false;
      const json all = {
        { "inputMode", options.value("input", std::string("touch")) == "kbm" ? "pc" : "touch" },
        { "autoResolution", !fixed_size },
        { "width", size.first },
        { "height", size.second },
        { "refreshRate", std::clamp(display_fps > 0 ? display_fps : 60, kEndfieldRefreshMin, kEndfieldRefreshMax) },
        { "fpsUnlock", unlock },
        { "targetFps", std::clamp(resolve_target_fps(options, display_fps), kFpsMin, kFpsMax) },
      };
      json settings = json::object();
      for (auto it = all.begin(); it != all.end(); ++it)
        if (!plugin.keys_known || std::find(plugin.keys.begin(), plugin.keys.end(), it.key()) != plugin.keys.end()) settings[it.key()] = it.value();
      return settings;
    }
    json
    hoyo_object(const json &options, int display_fps, const std::string &game, const json *existing = nullptr) {
      auto flag = [&](const char *key, bool fallback) {
        if (options.contains(key) && options[key].is_boolean()) return options[key].get<bool>();
        return fallback;
      };
      auto num = [&](const char *key, int fallback, int lo, int hi) {
        if (options.contains(key) && options[key].is_number_integer()) return std::clamp(options[key].get<int>(), lo, hi);
        return std::clamp(fallback, lo, hi);
      };
      const int target_fps = resolve_target_fps(options, display_fps);
      // Star Rail accepts 30/60/120 in its own GraphicsSettings model. Keep
      // HoyoTouchCore for rates above that cap; the native path is more stable
      // and also makes the game's saved setting agree with the stream.
      const bool native_starrail_fps = game == "starrail" && (target_fps == 30 || target_fps == 60 || target_fps == 120);
      const std::string priority = known_priority(options.value("priority", std::string("normal")));
      const std::string input = options.value("input", std::string("touch"));
      const std::string resolution = options.value("resolution", std::string("auto"));
      const std::pair<int, int> fixed = endfield_size(resolution);
      const bool fixed_size = fixed.first > 0;
      const std::pair<int, int> size = fixed_size ? fixed : kept_endfield_size(existing);
      return json {
        // keyboard, mouse and gamepad (kbm) is a Sparxie session without the touch interface: HoyoSettings.touch_ui_enabled
        { "touchUiEnabled", options.value("input", std::string("touch")) != "kbm" },
        { "fpsUnlockEnabled", flag("fpsUnlock", false) && !native_starrail_fps },
        { "targetFps", num("fps", target_fps, kFpsMin, kFpsMax) },
        { "backgroundFpsLimitEnabled", flag("bgLimit", true) },
        { "backgroundFps", num("bgFps", 10, kFpsMin, kFpsMax) },
        { "processPriority", priority },
        { "genshinFollowInGamePreset", flag("followPreset", false) },
        { "genshinPreset30Fps", num("preset30", 60, kFpsMin, kFpsMax) },
        { "genshinPreset60Fps", num("preset60", 1000, kFpsMin, kFpsMax) },
        { "genshinTouchUiScaleOverrideEnabled", flag("scaleOverride", false) },
        { "genshinTouchUiScalePercent", num("scale", 400, kScaleMin, kScaleMax) },
        { "endfieldInputMode", input == "kbm" ? "pc" : "touch" },  // Endfield has no automatic input any more
        { "endfieldAutoResolution", !fixed_size },
        // never 0 × 0: Sparxie's validator rejects it and discards the whole config (2026-11-12)
        { "endfieldWidth", size.first },
        { "endfieldHeight", size.second },
        { "endfieldRefreshRate", std::clamp(display_fps > 0 ? display_fps : 60, kEndfieldRefreshMin, kEndfieldRefreshMax) },
      };
    }
    // The profile for a draft in the spelling the installed build reads. An Endfield draft of a build that cannot hold one never gets here (endfield_write_check).
    // `existing` is the profile as it is now; the values the host does not manage stay as they are.
    json
    make_profile(const ProfileDraft &draft, const SparxieBuild &build, const json *existing = nullptr) {
      const bool plugin_endfield = draft.game == "endfield" && endfield_support(build) == EndfieldSupport::plugin;
      json profile = {
        { "id", draft.id },
        { "displayName", draft.display_name },
        { "game", plugin_endfield ? std::string(kEndfieldPluginId) : to_sparxie_game(draft.game) },
        { "executablePath", draft.executable },
      };
      if (plugin_endfield) {
        // a plugin game has no hoyo block: its options are the plugin's settings
        profile["settings"] = endfield_plugin_settings(draft.options, draft.display_fps, build.endfield, existing);
        return profile;
      }
      const json *existing_hoyo = existing && existing->is_object() && existing->contains("hoyo") && (*existing)["hoyo"].is_object() ? &(*existing)["hoyo"] : nullptr;
      if (draft.game != "zzz") profile["hoyo"] = hoyo_object(draft.options, draft.display_fps, draft.game, existing_hoyo);
      return profile;
    }
    void
    merge_profile(json &profile, const ProfileDraft &draft, const SparxieBuild &build) {
      const json fresh = make_profile(draft, build, &profile);
      for (auto it = fresh.begin(); it != fresh.end(); ++it) {
        // the host's keys of the block go in one by one; what else the block holds (Sparxie's own settings of the plugin, say) stays
        if ((it.key() == "hoyo" || it.key() == "settings") && profile.contains(it.key()) && profile[it.key()].is_object() && it.value().is_object()) {
          for (auto field = it.value().begin(); field != it.value().end(); ++field) profile[it.key()][field.key()] = field.value();
        }
        else {
          profile[it.key()] = it.value();
        }
      }
      if (draft.game == "zzz" && profile.contains("hoyo")) profile.erase("hoyo");
      if (profile.value("game", std::string()) == kEndfieldPluginId && profile.contains("hoyo")) profile.erase("hoyo");
    }
    void
    prune_backups(const fs::path &dir) {
      std::vector<fs::path> found;
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("config.backup-", 0) == 0 && name.ends_with(".json")) found.push_back(entry.path());
      }
      std::sort(found.begin(), found.end());
      while (found.size() > 5) {
        fs::remove(found.front(), ec);
        found.erase(found.begin());
      }
    }
    std::string
    backup_name(const fs::path &dir, const std::string &stamp) {
      fs::path path = dir / ("config.backup-" + stamp + ".json");
      if (!fs::exists(path)) return path.filename().string();
      for (int n = 2; n < 100; ++n) {
        path = dir / ("config.backup-" + stamp + "-" + std::to_string(n) + ".json");
        if (!fs::exists(path)) return path.filename().string();
      }
      return "config.backup-" + stamp + "-x.json";
    }
    WriteResult
    commit_doc(const fs::path &dir, const json &document, const std::string &profile_id, bool created, StoreHooks hooks, const SparxieBuild &build) {
      WriteResult result;
      result.profile = profile_id;
      const fs::path path = dir / "config.json";
      std::error_code ec;
      fs::create_directories(dir, ec);
      if (!created && fs::exists(path)) {
        const std::string stamp = hooks.utc_stamp ? hooks.utc_stamp() : utc_now();
        result.backup = backup_name(dir, stamp);
        fs::copy_file(path, dir / result.backup, fs::copy_options::overwrite_existing, ec);
        if (ec) {
          result.state = "write_failed";
          result.status = Status::fail(500, "write_failed", "Could not back up Sparxie config.json");
          return result;
        }
        prune_backups(dir);
      }
      const std::string body = document.dump(2);
      const fs::path tmp = dir / ("config.json.tmp." + std::to_string(std::random_device {}()));
      if (!write_bytes(tmp, body)) {
        result.state = "write_failed";
        result.status = Status::fail(500, "write_failed", "Could not write the temporary Sparxie config");
        return result;
      }
      bool replaced = false;
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (hooks.fail_replace_times > 0) {
          --hooks.fail_replace_times;
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }
        if (replace_file(tmp, path)) {
          replaced = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!replaced) {
        fs::remove(tmp, ec);
        result.state = "write_failed";
        result.status = Status::fail(500, "write_failed", "Could not replace Sparxie config.json");
        return result;
      }
      if (hooks.after_replace) hooks.after_replace(path);
      ConfigView check = read_store(dir, build);
      bool present = profile_id.empty();  // a change that removes profiles names none
      if (check.status.ok && check.document.contains("profiles")) {
        for (const auto &profile : check.document["profiles"])
          if (profile.value("id", std::string()) == profile_id) present = true;
      }
      if (!check.status.ok || !present) {
        if (!result.backup.empty())
          fs::copy_file(dir / result.backup, path, fs::copy_options::overwrite_existing, ec);
        else
          fs::remove(path, ec);
        result.state = "write_failed";
        result.status = Status::fail(500, "write_failed", "Sparxie config.json failed the read-back check and was restored");
        return result;
      }
      result.state = created ? "created" : "written";
      result.status = Status::success();
      return result;
    }
    // What an earlier host (and an earlier Sparxie) left in the file that Sparxie's validator would reject, put back to a value it accepts. Nothing else is touched.
    // The 2026-11-12 case: the host wrote endfieldWidth/Height 0 for "follow the stream", Sparxie then moved the whole config aside. Returns true when something changed.
    bool
    repair_document(json &document, const SparxieBuild &build) {
      bool changed = false;
      if (!document.is_object() || !document.contains("profiles") || !document["profiles"].is_array()) return false;
      for (auto &profile : document["profiles"]) {
        // A profile of Endfield in the first generation's spelling, in a build that reads the plugin's id: that build would put the whole file aside. The same
        // application (game, options, size) goes on under the plugin, whose settings take what the profile held.
        if (profile.is_object() && profile.value("game", std::string()) == "endfield" && endfield_support(build) == EndfieldSupport::plugin) {
          const json hoyo = profile.contains("hoyo") && profile["hoyo"].is_object() ? profile["hoyo"] : json::object();
          const json options = options_from_hoyo(hoyo);
          const json size_of = { { "settings", { { "width", hoyo.value("endfieldWidth", 0) }, { "height", hoyo.value("endfieldHeight", 0) } } } };
          profile["game"] = kEndfieldPluginId;
          // what the profile already holds as plugin settings (a key of the plugin's own, such as the interface scale) stays; the host's keys are set over it
          json settings = profile.contains("settings") && profile["settings"].is_object() ? profile["settings"] : json::object();
          const json managed = endfield_plugin_settings(options, hoyo.value("endfieldRefreshRate", 0), build.endfield, &size_of);
          for (auto field = managed.begin(); field != managed.end(); ++field) settings[field.key()] = field.value();
          profile["settings"] = settings;
          profile.erase("hoyo");
          changed = true;
          continue;
        }
        if (!profile.is_object() || !profile.contains("hoyo") || !profile["hoyo"].is_object()) continue;
        json &hoyo = profile["hoyo"];
        const bool endfield = profile.value("game", std::string()) == "endfield";
        if (hoyo.contains("endfieldWidth") && hoyo.contains("endfieldHeight") && hoyo["endfieldWidth"].is_number_integer() && hoyo["endfieldHeight"].is_number_integer() &&
            !endfield_size_ok(hoyo["endfieldWidth"].get<int>(), hoyo["endfieldHeight"].get<int>())) {
          hoyo["endfieldWidth"] = kEndfieldDefaultWidth;
          hoyo["endfieldHeight"] = kEndfieldDefaultHeight;
          // a fixed size that cannot be used any more: the size follows the stream
          hoyo["endfieldAutoResolution"] = true;
          changed = true;
        }
        if (endfield && hoyo.contains("endfieldRefreshRate") && hoyo["endfieldRefreshRate"].is_number_integer() && !in_range(hoyo["endfieldRefreshRate"].get<int>(), kEndfieldRefreshMin, kEndfieldRefreshMax)) {
          hoyo["endfieldRefreshRate"] = std::clamp(hoyo["endfieldRefreshRate"].get<int>(), kEndfieldRefreshMin, kEndfieldRefreshMax);
          changed = true;
        }
        // the removed Endfield input "auto" becomes touch, as Sparxie does on load
        if (endfield && hoyo.contains("endfieldInputMode") && hoyo["endfieldInputMode"].is_string() && hoyo["endfieldInputMode"].get<std::string>() != "touch" &&
            hoyo["endfieldInputMode"].get<std::string>() != "pc") {
          hoyo["endfieldInputMode"] = "touch";
          changed = true;
        }
      }
      return changed;
    }
    WriteResult
    edit_store(const fs::path &dir, StoreHooks hooks, const std::function<Status(json &, std::string &, const SparxieBuild &)> &edit) {
      if (hooks.running) {
        WriteResult refused;
        refused.state = "refused_running";
        refused.status = Status::fail(409, "sparxie_running", "Sparxie is running, close it before writing");
        return refused;
      }
      const fs::path path = dir / "config.json";
      std::error_code ec;
      const SparxieBuild build = hooks.build ? *hooks.build : detect_build(dir);
      for (int pass = 0; pass < 2; ++pass) {
        const bool existed = fs::exists(path, ec);
        const std::string bytes = existed ? read_bytes(path) : std::string();
        const std::string hash = sha256::hash(bytes);
        json document;
        if (!existed) {
          document = json { { "schemaVersion", 1 }, { "selectedProfileId", "" }, { "profiles", json::array() } };
        }
        else {
          try {
            document = json::parse(bytes);
          }
          catch (const std::exception &) {
            WriteResult bad;
            bad.state = "refused_unreadable";
            bad.status = Status::fail(409, "config_unreadable", "Sparxie config.json could not be parsed");
            return bad;
          }
          repair_document(document, build);
          Status valid = validate_document(document, &build);
          if (!valid.ok) {
            WriteResult bad;
            bad.state = valid.error_code == "schema_unsupported" ? "refused_schema" : "refused_unreadable";
            bad.status = valid;
            return bad;
          }
        }
        std::string profile_id;
        Status edited = edit(document, profile_id, build);
        if (!edited.ok) {
          WriteResult bad;
          bad.state = edited.error_code == "not_found" ? "not_found" : edited.error_code.rfind("endfield_", 0) == 0 ? "refused_endfield" : "refused_unreadable";
          bad.status = edited;
          return bad;
        }
        Status again = validate_document(document, &build);
        if (!again.ok) {
          WriteResult bad;
          bad.state = "refused_unreadable";
          bad.status = again;
          return bad;
        }
        if (existed && sha256::hash(read_bytes(path)) != hash) {
          if (pass == 1) {
            WriteResult race;
            race.state = "config_conflict";
            race.status = Status::fail(409, "config_conflict", "Sparxie config.json changed while it was being updated");
            return race;
          }
          continue;
        }
        return commit_doc(dir, document, profile_id, !existed, hooks, build);
      }
      WriteResult race;
      race.state = "config_conflict";
      race.status = Status::fail(409, "config_conflict", "Sparxie config.json changed while it was being updated");
      return race;
    }
  }  // namespace
  json
  default_options() {
    // A new application starts the way Sparxie's own editor starts a profile:
    // the unlock on and 120 as the target. input is the application's own
    // setting (the profile file has no field for it).
    return json {
      { "input", "touch" },
      { "fpsUnlock", true },
      { "fps", 120 },
      { "bgLimit", true },
      { "bgFps", 10 },
      { "priority", "normal" },
      { "followPreset", false },
      { "preset30", 60 },
      { "preset60", 1000 },
      { "scaleOverride", false },
      { "scale", 400 },
    };
  }
  Status
  normalize_options(json &options) {
    if (!options.is_object()) options = json::object();
    std::string input = options.value("input", std::string("touch"));
    // Endfield's automatic input (by the client's device) is gone: an older page that still sends it gets touch
    if (input == "auto") input = "touch";
    if (input != "touch" && input != "kbm") return Status::fail(400, "invalid_input", "input must be touch or kbm");
    options["input"] = input;
    return Status::success();
  }
  int
  resolve_target_fps(const json &options, int display_fps) {
    int target = display_fps > 0 ? display_fps : 120;
    if (options.contains("fps")) {
      const json &fps = options["fps"];
      if (fps.is_number_integer())
        target = fps.get<int>();
      else if (fps.is_string() && fps.get<std::string>() == "display")
        target = display_fps > 0 ? display_fps : 120;
    }
    return std::clamp(target, 10, 1000);
  }
  bool
  fps_is_display(const json &options) {
    return options.contains("fps") && options["fps"].is_string() && options["fps"].get<std::string>() == "display";
  }
  bool
  unlock_owns_fps(const json &block, int display_fps) {
    if (block.value("mode", std::string("pc")) != "touch") return false;
    const json options = block.value("options", json::object());
    if (!options.value("fpsUnlock", false)) return false;
    if (block.value("game", std::string()) != "starrail") return true;
    // The game's own 30/60/120 tiers are authoritative. Only an above-cap
    // request needs the HoyoTouchCore unlocker.
    return resolve_target_fps(options, display_fps) > 120;
  }
  json
  settings_without_game_fps(const std::string &mode, const json &options, const json &settings) {
    json out = settings;
    if (mode == "touch" && options.value("fpsUnlock", false) && out.is_object()) out.erase("fps");
    return out;
  }
  json
  app_block(const Install &install, const std::string &profile, const std::string &mode, const json &options, const std::string &templ, const json &settings, const std::string &restore, const std::string &cover) {
    return json {
      { "game", install.game },
      { "region", install.region },
      { "install", install.id },
      { "profile", profile },
      { "mode", mode },
      { "cover", cover },
      { "artVersion", "" },
      { "artUpdated", "" },
      { "restore", restore },
      { "settings", settings },
      { "options", options },
      { "launchTemplate", templ },
      { "launchTemplateProvisional", true },
      { "exe", install.exe_path },
      { "dir", install.dir },
      { "source", install.source },
      { "candidates", install.candidates },
      { "markers", install.markers },
    };
  }
  json
  make_app(const std::string &name, const std::string &cmd, bool elevated, const Install &install, const json &block) {
    json app = { { "name", name }, { "cmd", cmd }, { "elevated", elevated }, { "sparxie", block } };
    // sparxie-launch lives exactly as long as the game does and ends everything it started: Sunshine ends the app when the launcher does, not when the last process of the app is gone
    if (cmd.find("sparxie-launch") != std::string::npos) app["wait-all"] = false;
    if (!install.dir.empty()) app["working-dir"] = install.dir;
    return app;
  }
  json
  options_from_hoyo(const json &hoyo) {
    if (!hoyo.is_object()) return json::object();
    json options = json {
      { "input", hoyo.value("touchUiEnabled", true) ? "touch" : "kbm" },
      { "fpsUnlock", hoyo.value("fpsUnlockEnabled", false) },
      { "fps", hoyo.value("targetFps", 120) },
      { "bgLimit", hoyo.value("backgroundFpsLimitEnabled", true) },
      { "bgFps", hoyo.value("backgroundFps", 10) },
      { "priority", hoyo.value("processPriority", std::string("normal")) },
      { "followPreset", hoyo.value("genshinFollowInGamePreset", false) },
      { "preset30", hoyo.value("genshinPreset30Fps", 60) },
      { "preset60", hoyo.value("genshinPreset60Fps", 1000) },
      { "scaleOverride", hoyo.value("genshinTouchUiScaleOverrideEnabled", false) },
      { "scale", hoyo.value("genshinTouchUiScalePercent", 400) },
    };
    // Endfield: the input is its own mode, and a fixed resolution (absent: it follows the stream)
    if (hoyo.contains("endfieldInputMode") && hoyo["endfieldInputMode"].is_string()) {
      const std::string mode = hoyo["endfieldInputMode"].get<std::string>();
      options["input"] = mode == "pc" ? "kbm" : "touch";
    }
    if (hoyo.contains("endfieldAutoResolution") && hoyo["endfieldAutoResolution"].is_boolean() && !hoyo["endfieldAutoResolution"].get<bool>() && hoyo.value("endfieldWidth", 0) > 0)
      options["resolution"] = std::to_string(hoyo.value("endfieldWidth", 0)) + "\xC3\x97" + std::to_string(hoyo.value("endfieldHeight", 0));
    return options;
  }
  json
  options_from_settings(const json &settings) {
    // the options of a new application, then what the plugin's settings say (the schema defaults: unlock on, 120)
    json options = default_options();
    if (!settings.is_object()) return options;
    if (settings.contains("inputMode") && settings["inputMode"].is_string()) options["input"] = settings["inputMode"].get<std::string>() == "pc" ? "kbm" : "touch";
    if (settings.contains("fpsUnlock") && settings["fpsUnlock"].is_boolean()) options["fpsUnlock"] = settings["fpsUnlock"].get<bool>();
    if (settings.contains("targetFps") && settings["targetFps"].is_number_integer()) options["fps"] = std::clamp(settings["targetFps"].get<int>(), kFpsMin, kFpsMax);
    if (settings.contains("autoResolution") && settings["autoResolution"].is_boolean() && !settings["autoResolution"].get<bool>() && settings.value("width", 0) > 0 && settings.value("height", 0) > 0)
      options["resolution"] = std::to_string(settings.value("width", 0)) + "\xC3\x97" + std::to_string(settings.value("height", 0));
    return options;
  }
  json
  options_from_profile(const json &profile) {
    if (!profile.is_object()) return default_options();
    if (is_plugin_game_id(profile.value("game", std::string()))) return options_from_settings(profile.contains("settings") ? profile["settings"] : json::object());
    return profile.contains("hoyo") ? options_from_hoyo(profile["hoyo"]) : default_options();
  }
  std::string
  next_profile_id(const json &document, const std::string &prefix) {
    std::set<std::string> ids;
    if (document.contains("profiles") && document["profiles"].is_array()) {
      for (const auto &profile : document["profiles"])
        if (profile.is_object() && profile.contains("id") && profile["id"].is_string()) ids.insert(profile["id"].get<std::string>());
    }
    for (int n = 1; n < 10000; ++n) {
      const std::string id = prefix + std::to_string(n);
      if (!ids.count(id)) return id;
    }
    return prefix + "x";
  }
  Status
  validate_document(const json &document, const SparxieBuild *build_in) {
    const SparxieBuild build = build_in ? *build_in : SparxieBuild {};
    if (!document.is_object()) return Status::fail(409, "config_unreadable", "Sparxie config is not a JSON object");
    if (!document.contains("schemaVersion") || !document["schemaVersion"].is_number_integer())
      return Status::fail(409, "config_unreadable", "Sparxie config has no schemaVersion");
    if (document["schemaVersion"].get<int>() != 1)
      return Status::fail(409, "schema_unsupported", "Sparxie config schemaVersion is not 1");
    if (!document.contains("profiles") || !document["profiles"].is_array())
      return Status::fail(409, "config_unreadable", "Sparxie config profiles is missing");
    std::set<std::string> ids;
    for (const auto &profile : document["profiles"]) {
      if (!profile.is_object()) return Status::fail(409, "config_unreadable", "A Sparxie profile is not an object");
      const std::string id = profile.value("id", std::string());
      if (id.empty() || !ids.insert(id).second) return Status::fail(409, "config_unreadable", "Sparxie profile ids must be unique and non-empty");
      if (profile.value("displayName", std::string()).empty()) return Status::fail(409, "config_unreadable", "A Sparxie profile name is empty");
      const std::string game = profile.value("game", std::string());
      if (game == "endfield" && build.generation == Generation::plugin)
        return Status::fail(409, "config_unreadable", "This Sparxie reads Endfield as a plugin (" + std::string(kEndfieldPluginId) + "); a profile still says endfield");
      if (!build_reads_game(build, game)) return Status::fail(409, "config_unreadable", "A Sparxie profile game is not recognised");
      // a plugin game's settings are the plugin's own: Sparxie reads them as one JSON object and cannot read the file when it is anything else
      if (is_plugin_game_id(game) && profile.contains("settings") && !profile["settings"].is_null() && !profile["settings"].is_object())
        return Status::fail(409, "config_unreadable", "A Sparxie plugin profile's settings is not an object");
      const std::string exe = profile.value("executablePath", std::string());
      if (!exe.empty() && !allowed_exe(game, exe)) return Status::fail(409, "config_unreadable", "A Sparxie profile executable is not on the whitelist");
      const bool zzz = game == "zenlessZoneZero";
      if (zzz && profile.contains("hoyo") && !profile["hoyo"].is_null())
        return Status::fail(409, "config_unreadable", "Zenless Zone Zero profiles cannot contain hoyo settings");
      if (!zzz && !is_plugin_game_id(game) && profile.contains("hoyo") && profile["hoyo"].is_object()) {
        const json &hoyo = profile["hoyo"];
        // the same rules as Sparxie's AppConfigValidator (and the types its JSON reader needs: a wrong type makes the whole file unreadable for it)
        const char *flags[] = { "touchUiEnabled", "fpsUnlockEnabled", "backgroundFpsLimitEnabled", "genshinFollowInGamePreset", "genshinTouchUiScaleOverrideEnabled", "endfieldAutoResolution" };
        for (const char *field : flags) {
          if (hoyo.contains(field) && !hoyo[field].is_boolean()) return Status::fail(409, "config_unreadable", std::string(field) + " is not true or false");
        }
        const char *fps_fields[] = { "targetFps", "backgroundFps", "genshinPreset30Fps", "genshinPreset60Fps" };
        for (const char *field : fps_fields) {
          if (!hoyo.contains(field)) continue;
          if (!hoyo[field].is_number_integer() || !in_range(hoyo[field].get<int>(), kFpsMin, kFpsMax))
            return Status::fail(409, "config_unreadable", "A Sparxie frame-rate value is outside 10-1000");
        }
        const char *other_ints[] = { "genshinTouchUiScalePercent", "endfieldWidth", "endfieldHeight", "endfieldRefreshRate" };
        for (const char *field : other_ints) {
          if (hoyo.contains(field) && !hoyo[field].is_number_integer()) return Status::fail(409, "config_unreadable", std::string(field) + " is not a whole number");
        }
        if (hoyo.contains("genshinTouchUiScalePercent") && !in_range(hoyo.value("genshinTouchUiScalePercent", 400), kScaleMin, kScaleMax))
          return Status::fail(409, "config_unreadable", "Touch UI scale is outside 100-500");
        if (hoyo.contains("processPriority") && !hoyo["processPriority"].is_string()) return Status::fail(409, "config_unreadable", "processPriority is not text");
        const std::string priority = hoyo.value("processPriority", std::string("normal"));
        if (priority != "normal" && priority != "belowNormal" && priority != "aboveNormal" && priority != "high")
          return Status::fail(409, "config_unreadable", "processPriority is not a known value");
        if (hoyo.contains("endfieldInputMode") && !hoyo["endfieldInputMode"].is_string()) return Status::fail(409, "config_unreadable", "endfieldInputMode is not text");
        if (game == "endfield") {
          const std::string input = hoyo.value("endfieldInputMode", std::string("touch"));
          if (input != "touch" && input != "pc") return Status::fail(409, "config_unreadable", "endfieldInputMode is not touch or pc");
          // the size is only looked at when it is fixed; while the size follows the stream (auto) Sparxie ignores it
          const bool automatic = hoyo.value("endfieldAutoResolution", true);
          if (!automatic && !endfield_size_ok(hoyo.value("endfieldWidth", kEndfieldDefaultWidth), hoyo.value("endfieldHeight", kEndfieldDefaultHeight)))
            return Status::fail(409, "config_unreadable", "The Endfield size is outside 320x240 - 7680x4320");
          if (!in_range(hoyo.value("endfieldRefreshRate", 60), kEndfieldRefreshMin, kEndfieldRefreshMax))
            return Status::fail(409, "config_unreadable", "The Endfield refresh rate is outside 24-1000");
        }
      }
    }
    return Status::success();
  }
  ConfigView
  read_store(const fs::path &dir) {
    return read_store(dir, detect_build(dir));
  }
  ConfigView
  read_store(const fs::path &dir, const SparxieBuild &build) {
    ConfigView view;
    const fs::path path = dir / "config.json";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
      view.state = "empty";
      view.document = json { { "schemaVersion", 1 }, { "selectedProfileId", "" }, { "profiles", json::array() } };
      return view;
    }
    try {
      view.document = json::parse(read_bytes(path));
    }
    catch (const std::exception &) {
      view.state = "unreadable";
      view.status = Status::fail(409, "config_unreadable", "Sparxie config.json could not be parsed");
      return view;
    }
    view.status = validate_document(view.document, &build);
    if (!view.status.ok) {
      view.state = "unreadable";
      return view;
    }
    view.profile_count = static_cast<int>(view.document["profiles"].size());
    view.state = view.profile_count == 0 ? "empty" : "data";
    return view;
  }
  WriteResult
  add_profile(const fs::path &dir, ProfileDraft draft, StoreHooks hooks) {
    return edit_store(dir, hooks, [&](json &document, std::string &profile_id, const SparxieBuild &build) {
      if (draft.game == "endfield")
        if (const Status held = endfield_write_check(build); !held.ok) return held;
      if (draft.id.empty()) draft.id = next_profile_id(document, draft.game + "-");
      profile_id = draft.id;
      if (!document["selectedProfileId"].is_string() || document["selectedProfileId"].get<std::string>().empty())
        document["selectedProfileId"] = draft.id;
      document["profiles"].push_back(make_profile(draft, build));
      return Status::success();
    });
  }
  WriteResult
  update_profile(const fs::path &dir, const std::string &id, ProfileDraft draft, StoreHooks hooks) {
    draft.id = id;
    return edit_store(dir, hooks, [&](json &document, std::string &profile_id, const SparxieBuild &build) {
      profile_id = id;
      if (draft.game == "endfield")
        if (const Status held = endfield_write_check(build); !held.ok) return held;
      bool found = false;
      for (auto &profile : document["profiles"]) {
        if (profile.value("id", std::string()) == id) {
          merge_profile(profile, draft, build);
          found = true;
        }
      }
      if (!found) return Status::fail(404, "not_found", "That Sparxie profile does not exist");
      return Status::success();
    });
  }
  WriteResult
  remove_profiles(const fs::path &dir, const std::vector<std::string> &ids, StoreHooks hooks) {
    int removed = 0;
    WriteResult result = edit_store(dir, hooks, [&](json &document, std::string &profile_id, const SparxieBuild &) {
      json kept = json::array();
      for (const auto &profile : document["profiles"]) {
        if (std::find(ids.begin(), ids.end(), profile.value("id", std::string())) != ids.end())
          ++removed;
        else
          kept.push_back(profile);
      }
      if (removed == 0) return Status::fail(404, "not_found", "Those Sparxie profiles do not exist");
      document["profiles"] = kept;
      auto gone = [&](const json &value) { return value.is_string() && std::find(ids.begin(), ids.end(), value.get<std::string>()) != ids.end(); };
      if (gone(document.value("selectedProfileId", json())))
        document["selectedProfileId"] = kept.empty() ? std::string() : kept.front().value("id", std::string());
      if (document.contains("desktop") && document["desktop"].is_object() && document["desktop"].contains("lastProfileByGame") && document["desktop"]["lastProfileByGame"].is_object()) {
        json last = json::object();
        for (auto it = document["desktop"]["lastProfileByGame"].begin(); it != document["desktop"]["lastProfileByGame"].end(); ++it)
          if (!gone(it.value())) last[it.key()] = it.value();
        document["desktop"]["lastProfileByGame"] = last;
      }
      profile_id.clear();
      return Status::success();
    });
    result.profile = std::to_string(removed);
    return result;
  }
}  // namespace sparxie
