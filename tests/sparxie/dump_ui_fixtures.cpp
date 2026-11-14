// Writes the host's own answers for the three titles to one JSON file, so the Web UI tests and the private harness draw from what the host really sends
// (catalog rows with their described settings and Sparxie fields, game-settings answers, applications the host stored) instead of hand-written fixtures.
// Usage: sparxie_ui_fixtures <out.json> [asset-root]   (the asset root holds assets/sparxie/zzz-magic.bin; default: the repository src_assets/common/assets)
#include "src/sparxie/host.h"
#include "src/sparxie/region.h"
#include "src/sparxie/zzz_file.h"
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
namespace {
  using sparxie::json;
  const char kSep = static_cast<char>(92);

  void
  touch_file(const fs::path &path, const std::string &text = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }

  void
  put_bytes(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::string
  key(std::initializer_list<const char *> parts) {
    std::string out;
    for (const char *p : parts) {
      if (!out.empty()) out += kSep;
      out += p;
    }
    return out;
  }

  // the entries a person changed in game: the file lists only what was changed, the others read as the game's own defaults
  std::string
  zzz_document(int fps, int vsync, int scale, int preset, int hdr) {
    json doc = { { "SystemSettingDataMap", { { "110", { { "Data", fps } } }, { "8", { { "Data", vsync } } }, { "9", { { "Data", scale } } }, { "3", { { "Data", preset } } } } }, { "HDRSettingRecordState", hdr } };
    return doc.dump(4);
  }

  // the fixtures are made on whatever machine builds them; the file reads as a Windows PC (the folder of this run becomes D:\Games)
  void
  as_windows(json &node, const std::string &root) {
    if (node.is_string()) {
      std::string text = node.get<std::string>();
      const auto at = text.find(root);
      if (at == std::string::npos) return;
      text.replace(at, root.size(), "D:/Games");
      for (char &c : text)
        if (c == 0x2f) c = static_cast<char>(92);
      node = text;
    }
    else if (node.is_array() || node.is_object()) {
      for (auto &child : node)
        as_windows(child, root);
    }
  }
}  // namespace

int
main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: sparxie_ui_fixtures <out.json> [asset-root]\n";
    return 2;
  }
  const fs::path out_file = argv[1];
  const fs::path assets = argc > 2 ? fs::path(argv[2]) : fs::path("src_assets/common/assets");
  const fs::path root = fs::temp_directory_path() / "sparxie-ui-fixtures";
  fs::remove_all(root);

  const auto magics = sparxie::load_zzz_magics(root / "host", { assets });
  if (magics.empty()) {
    std::cerr << "no ZZZ key under " << assets << "\n";
    return 1;
  }

  sparxie::MemoryRegistry reg;
  sparxie::HostEnv env;
  env.host_data = root / "host";
  env.data_dir = root / "sparxie-data";
  env.apps_file = root / "apps.json";
  env.covers_dir = root / "covers";
  env.install_root = root / "install";
  env.registry = &reg;
  env.zzz_magics = magics;
  env.process_running = [](const std::vector<std::string> &) { return false; };
  // the display the host streams (Zenless Zone Zero lists its resolutions from it)
  env.display_size = [] { return std::pair<int, int> { 2560, 1440 }; };
  touch_file(env.install_root / "components" / "sparxie" / "Sparxie.exe");
  touch_file(env.install_root / "components" / "sparxie" / "component.json", R"({"version":"0.9.0"})");

  const fs::path genshin = root / "Genshin" / "Genshin Impact game";
  const fs::path starrail = root / "StarRail" / "Game";
  const fs::path zzz = root / "ZZZ" / "ZenlessZoneZero Game";
  touch_file(genshin / "YuanShen.exe");
  touch_file(starrail / "StarRail.exe");
  touch_file(zzz / "ZenlessZoneZero.exe");
  const fs::path hi3 = root / "HI3" / "Honkai Impact 3rd game";
  touch_file(hi3 / "BH3.exe");
  fs::create_directories(hi3 / "BH3_Data");

  // the HoYoPlay launcher registry names the three installs
  reg.set_text(key({ "Software", "miHoYo", "HYP", "1_1", "hk4e_cn" }), "GameInstallPath", genshin.string());
  reg.set_text(key({ "Software", "miHoYo", "HYP", "1_1", "hkrpg_cn" }), "GameInstallPath", starrail.string());
  reg.set_text(key({ "Software", "miHoYo", "HYP", "1_1", "nap_cn" }), "GameInstallPath", zzz.string());
  reg.set_text(key({ "Software", "miHoYo", "HYP", "1_1", "bh3_cn" }), "GameInstallPath", hi3.string());

  // Genshin: GENERAL_DATA blob in the registry (45 fps tier, vsync on, render scale 1.2, medium, HDR off), resolution and window mode
  {
    const std::string root_key = key({ "Software", "miHoYo", "原神" });
    const json perf = { { "saveItems", json::array({ { { "entryType", 1 }, { "index", 2 }, { "itemVersion", "CNRELWin5.8.0" } }, { { "entryType", 2 }, { "index", 1 }, { "itemVersion", "CNRELWin5.8.0" } }, { { "entryType", 3 }, { "index", 7 }, { "itemVersion", "CNRELWin5.8.0" } }, { { "entryType", 4 }, { "index", 2 }, { "itemVersion", "CNRELWin5.8.0" } } }) }, { "portedVersion", "CNRELWin5.8.0" } };
    const std::string general = json({ { "enableHDR", false }, { "globalPerfData", perf.dump() } }).dump();
    reg.set_binary(root_key, "GENERAL_DATA_h2389025596", std::vector<std::uint8_t>(general.begin(), general.end()));
    reg.set_dword(root_key, "Screenmanager Resolution Width_h182942802", 2560);
    reg.set_dword(root_key, "Screenmanager Resolution Height_h2627697771", 1440);
    reg.set_dword(root_key, "Screenmanager Is Fullscreen mode_h3981298716", 1);
  }
  // Star Rail: the model blob (120 fps, vsync off, render scale 1.0) and the quality DWORD
  {
    const std::string root_key = key({ "Software", "miHoYo", "崩坏：星穹铁道" });
    const std::string model = R"({"FPS":120,"EnableVSync":false,"RenderScale":1.0})";
    reg.set_binary(root_key, "GraphicsSettings_Model_h2986158309", std::vector<std::uint8_t>(model.begin(), model.end()));
    reg.set_dword(root_key, "GraphicsSettings_GraphicsQuality_h523255858", 4);
    reg.set_dword(root_key, "Screenmanager Resolution Width_h182942802", 1920);
    reg.set_dword(root_key, "Screenmanager Resolution Height_h2627697771", 1080);
    reg.set_dword(root_key, "Screenmanager Fullscreen mode_h3630240806", 1);
  }
  // ZZZ: resolution in the registry, the five items in GENERAL_DATA.bin
  {
    const std::string root_key = key({ "Software", "miHoYo", "绝区零" });
    reg.set_dword(root_key, "Screenmanager Resolution Width_h182942802", 1920);
    reg.set_dword(root_key, "Screenmanager Resolution Height_h2627697771", 1080);
    reg.set_dword(root_key, "Screenmanager Fullscreen mode_h3630240806", 3);
  }
  // Honkai Impact 3rd: the registry blobs of its graphics and sound pages exist (empty: every item reads as the game's default)
  {
    const std::string root_key = key({ "Software", "miHoYo", "崩坏3" });
    const std::string empty = "{}";
    for (const char *name : { "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519", "GENERAL_DATA_V2_PersonalAudioSetting_h3869048096", "GENERAL_DATA_V2_PersonalAudioSettingVolume_h600615720" })
      reg.set_binary(root_key, name, std::vector<std::uint8_t>(empty.begin(), empty.end()));
    reg.set_dword(root_key, "Screenmanager Resolution Width_h182942802", 1920);
    reg.set_dword(root_key, "Screenmanager Resolution Height_h2627697771", 1080);
    reg.set_dword(root_key, "Screenmanager Fullscreen mode_h3630240806", 1);
  }
  const fs::path zzz_file = zzz / "ZenlessZoneZero_Data" / "Persistent" / "LocalStorage" / "GENERAL_DATA.bin";
  put_bytes(zzz_file, [&] { const std::string d = zzz_document(1, 0, 2, 2, 1); return sparxie::sleepy_write(d, magics[0]); }());

  json fixtures = json::object();
  fixtures["status"] = sparxie::component_status(env).body;
  fixtures["games"] = sparxie::list_games(env).body;

  // the answers of GET game-settings for each title
  json settings = json::object();
  settings["genshin"] = sparxie::game_settings_for(env, "genshin-cn", (genshin / "YuanShen.exe").string(), "cn").body;
  settings["starrail"] = sparxie::game_settings_for(env, "starrail-cn", (starrail / "StarRail.exe").string(), "cn").body;
  settings["zzz"] = sparxie::game_settings_for(env, "zzz-cn", (zzz / "ZenlessZoneZero.exe").string(), "cn").body;
  settings["hi3"] = sparxie::game_settings_for(env, "hi3-cn", (hi3 / "BH3.exe").string(), "cn").body;
  // a file the game never wrote anything into: every item reads as the game's own default (60, on, 1.0, medium)
  put_bytes(zzz_file, [&] { const std::string d = json({ { "SystemSettingDataMap", json::object() } }).dump(4); return sparxie::sleepy_write(d, magics[0]); }());
  settings["zzzDefaults"] = sparxie::game_settings_for(env, "zzz-cn", (zzz / "ZenlessZoneZero.exe").string(), "cn").body;
  // a file of a newer game version that the shipped key does not decode
  put_bytes(zzz_file, [&] { const std::string d = zzz_document(1, 0, 2, 2, 1); return sparxie::sleepy_write(d, std::vector<std::uint8_t> { 0x01, 0xC0, 0x22, 0x05, 0x7F }); }());
  settings["zzzUnreadable"] = sparxie::game_settings_for(env, "zzz-cn", (zzz / "ZenlessZoneZero.exe").string(), "cn").body;
  fixtures["gameSettings"] = settings;

  // the host's catalog without the ZZZ file key (a build that does not ship it): the title lists no file items
  fixtures["catalogNoZzzKey"] = sparxie::catalog_json(false);

  // applications the host stores: touch and direct, 自动 and 我们的预设
  json apps = json::array();
  const auto create = [&](const json &request) {
    const auto r = sparxie::create_app(env, request);
    if (!r.status.ok) {
      std::cerr << "create_app failed: " << r.status.error << "\n";
      std::exit(1);
    }
    apps.push_back(r.body["app"]);
  };
  create({ { "path", (genshin / "YuanShen.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "原神 · 国服" }, { "settings", { { "resolution", "2560×1440" }, { "display", "fullscreen" }, { "vsync", true }, { "renderScale", 1.2 }, { "preset", "medium" }, { "hdr", false } } }, { "options", { { "fpsUnlock", true }, { "fps", 120 }, { "input", "touch" } } } });
  create({ { "path", (starrail / "StarRail.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "崩坏：星穹铁道 · 国服" }, { "options", { { "fpsUnlock", false }, { "input", "kbm" } } } });
  create({ { "path", (zzz / "ZenlessZoneZero.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "绝区零 · 国服" }, { "settings", { { "resolution", "1920×1080" }, { "display", "windowed" }, { "fps", 60 }, { "vsync", true }, { "renderScale", 1.0 }, { "preset", "medium" }, { "hdr", false } } }, { "options", { { "input", "touch" } } } });
  create({ { "path", (zzz / "ZenlessZoneZero.exe").string() }, { "region", "cn" }, { "mode", "pc" }, { "name", "绝区零 · 直接启动" } });
  // Honkai Impact 3rd: no Sparxie touch launch; the host's items of its settings page, with the priority switch on
  create({ { "path", (hi3 / "BH3.exe").string() }, { "region", "cn" }, { "mode", "pc" }, { "name", "崩坏3 · 国服" }, { "settings", { { "resolution", "1920×1080" }, { "display", "fullscreen" }, { "fps", 60 }, { "masterVolume", 80 }, { "gameBoost", true } } } });
  fixtures["apps"] = apps;

  as_windows(fixtures, root.string());
  fs::create_directories(out_file.parent_path().empty() ? fs::path(".") : out_file.parent_path());
  std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
  out << fixtures.dump(2) << "\n";
  return 0;
}
