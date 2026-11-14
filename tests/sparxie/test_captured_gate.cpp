// Applications captured from a real host (apps.json and Sparxie config.json, sanitised to the games' folders) put through the host:
// host start (cover repair, badge rule, direct apps to the launcher), the answers the Web editor is given, and what a save of each application writes.
// The Web UI tests (tests/sparxieCapturedGate.test.js) open every application in the real editor from the answers this test dumps:
//   SPARXIE_CAPTURED_GATE_OUT=<out.json> [SPARXIE_CAPTURED_GATE_BODIES=<bodies.json>] ./sparxie_unit_tests --gtest_filter='CapturedGate.*'
// writes src_assets/common/assets/web/tests/fixtures/sparxie-captured.json. `bodies.json` holds, per variant and application name, the body the editor sends for the one change the gate makes
// (the Web test writes it with SPARXIE_GATE_WRITE_BODIES=<file>); the host's answer to that save is the "afterSave" entry the editor reopens.
#include "src/sparxie/art.h"
#include "src/sparxie/host.h"
#include "src/sparxie/store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <gtest/gtest.h>
#include <tuple>
#include <vector>
namespace fs = std::filesystem;
namespace {
  using sparxie::json;

  std::string
  read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  std::string
  fixture(const char *name) {
    return read_text(fs::path(SPARXIE_FIXTURES_DIR) / name);
  }

  void
  touch_file(const fs::path &path, const std::string &text = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
  }

  // official art needs a picture that is not one colour (the host refuses a blank background)
  sparxie::Rgba
  textured(int w, int h, int seed) {
    sparxie::Rgba image;
    image.w = w;
    image.h = h;
    image.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
        image.px[at] = static_cast<std::uint8_t>(40 + (x * 180 / w + seed) % 200);
        image.px[at + 1] = static_cast<std::uint8_t>(40 + y * 180 / h);
        image.px[at + 2] = static_cast<std::uint8_t>(((x / 16 + y / 16) % 2) * 120 + 40);
      }
    return image;
  }

  sparxie::Rgba
  logo_block() {
    sparxie::Rgba logo;
    logo.w = 416;
    logo.h = 234;
    logo.px.assign(static_cast<std::size_t>(logo.w * logo.h * 4), 0);
    for (int y = 40; y < 120; ++y)
      for (int x = 30; x < 255; ++x)
        for (int c = 0; c < 4; ++c) logo.px[static_cast<std::size_t>((y * logo.w + x) * 4 + c)] = c == 3 ? ((x - 30) / 6 % 2 == 0 ? 255 : 0) : 255;
    return logo;
  }

  const std::string kCapturedConfig = "C:\\Program Files\\Sunshine\\config\\";

  // the captured paths under its config folder (covers, posters) are this run's folder
  void
  localise(json &node, const fs::path &root) {
    if (node.is_string()) {
      std::string text = node.get<std::string>();
      // the games' folders are Windows paths: elsewhere the file name of a path with backslashes is the whole path, so they are slashes here (and backslashes again in the dump)
      if (text.rfind("C:\\Games\\", 0) == 0) {
        for (char &c : text)
          if (c == 0x5c) c = 0x2f;
        node = text;
        return;
      }
      if (text.rfind(kCapturedConfig, 0) != 0) return;
      std::string rest = text.substr(kCapturedConfig.size());
      for (char &c : rest)
        if (c == '\\') c = '/';
      node = (root / rest).string();
    }
    else if (node.is_array() || node.is_object()) {
      for (auto &child : node)
        localise(child, root);
    }
  }

  // the dumped answers read as the captured host
  void
  as_captured(json &node, const fs::path &root) {
    if (node.is_string()) {
      std::string text = node.get<std::string>();
      if (text.rfind("C:/Games/", 0) == 0) {
        for (char &c : text)
          if (c == 0x2f) c = 0x5c;
        node = text;
        return;
      }
      const std::string prefix = root.string();
      if (text.rfind(prefix, 0) != 0) return;
      std::string rest = text.substr(prefix.size());
      for (char &c : rest)
        if (c == '/') c = '\\';
      node = "C:\\Program Files\\Sunshine\\config" + rest;
    }
    else if (node.is_array() || node.is_object()) {
      for (auto &child : node)
        as_captured(child, root);
    }
  }

  struct Machine {
    fs::path root;
    sparxie::HostEnv env;
    explicit Machine(const fs::path &where, bool with_switch): root(where) {
      env.host_data = root / "sparxie";
      env.data_dir = root / "sparxie-data";
      env.apps_file = root / "apps.json";
      env.covers_dir = root / "covers";
      env.install_root = root / "install";
      env.warn = [](const std::string &line) { std::cerr << "host warning: " << line << std::endl; };
      env.process_running = [](const std::vector<std::string> &) { return false; };
      fs::remove_all(root);
      const fs::path component = env.install_root / "components" / "sparxie";
      touch_file(component / "Sparxie.exe");
      env.sparxie_exe = component / "Sparxie.exe";  // the host resolves it once at start, like runtime.cpp
      touch_file(component / "version.ini", "exe_path=app-0.1.0-test.1\\Sparxie.Desktop.exe\n");
      touch_file(component / "app-0.1.0-test.1" / "capabilities.json", json({ { "schema", 1 }, { "capabilities", with_switch ? json::array({ "touch-ui-switch", "endfield" }) : json::array() } }).dump());
      touch_file(env.install_root / "tools" / "sparxie-launch.exe");
      // official art of the three installs (plain and badged covers, heroes), then the captured files: its apps.json and Sparxie config
      for (const std::pair<const char *, int> &install : { std::pair<const char *, int> { "genshin-cn", 10 }, { "starrail-cn", 60 }, { "zzz-cn", 110 } }) {
        sparxie::ArtUrls urls;
        urls.version = "v1";
        const auto published = sparxie::publish_art(env, install.first, urls, textured(2560, 1440, install.second), logo_block(), {});
        EXPECT_TRUE(published.status.ok) << published.status.error;
      }
      json apps = json::parse(fixture("sample-apps-20261111b.json"));
      localise(apps, root);
      // The file as the one-off conversion leaves it: an application holds only the items that differ from the game's own values and has no settingsMode.
      // The seven default values of the first builds and every item equal to the game's are gone; of the applications only 原神 differs from its game (HDR and V-sync on).
      for (auto &app : apps["apps"]) {
        if (!app.contains("sparxie") || !app["sparxie"].is_object()) continue;
        app["sparxie"].erase("settingsMode");
        app["sparxie"]["settings"] = app["name"] == "原神" ? json { { "hdr", true }, { "vsync", true } } : json::object();
      }
      // the installer's image migration made a byte copy of the plain official cover for every Sparxie application (covers/app_<hash>_<time>.png)
      for (const auto &app : apps["apps"]) {
        if (!app.contains("sparxie") || !app["image-path"].is_string()) continue;
        const std::string name = app["image-path"].get<std::string>();
        if (name.rfind("app_", 0) != 0) continue;
        fs::create_directories(env.covers_dir);
        fs::copy_file(env.covers_dir / ("sparxie-" + app["sparxie"]["install"].get<std::string>() + "-v1.png"), env.covers_dir / name, fs::copy_options::overwrite_existing);
      }
      std::ofstream(env.apps_file, std::ios::binary) << apps.dump(2);
      json config = json::parse(fixture("sample-sparxie-config-20261111b.json"));
      localise(config, root);
      touch_file(env.data_dir / "config.json", config.dump(2));
    }
    json
    apps() const {
      return json::parse(sparxie::typed_apps_text(read_text(env.apps_file)));
    }
  };

  // the one change the Web gate makes to each application, and the save it sends, by variant and application name
  json
  bodies() {
    const char *path = std::getenv("SPARXIE_CAPTURED_GATE_BODIES");
    if (!path || !fs::exists(path)) return json::object();
    return json::parse(read_text(path));
  }
}  // namespace

TEST(CapturedGate, HostStartSettlesTheCapturedApplicationsAndIsStableAfterwards) {
  json out = json::object();
  const json saves = bodies();
  for (const bool with_switch : { true, false }) {
    const std::string variant = with_switch ? "withSwitch" : "withoutSwitch";
    Machine machine(fs::temp_directory_path() / ("sparxie-captured-gate-" + variant), with_switch);
    const json before = machine.apps();
    EXPECT_EQ(sparxie::component_status(machine.env).body["touchUiSwitch"], with_switch) << variant;
    // host start
    const int changed = sparxie::attach_official_covers(machine.env);
    EXPECT_GT(changed, 0);
    const std::string after_start = read_text(machine.env.apps_file);
    EXPECT_EQ(sparxie::attach_official_covers(machine.env), 0) << variant << ": a second start changes nothing";
    EXPECT_EQ(read_text(machine.env.apps_file), after_start) << variant << ": and writes nothing";

    const json apps = machine.apps();
    const fs::path plain = machine.env.covers_dir / "sparxie-genshin-cn-v1.png";
    const auto named = [&apps](const char *name) -> const json & {
      for (const auto &app : apps["apps"])
        if (app.value("name", std::string()) == name) return app;
      ADD_FAILURE() << name;
      return apps["apps"][0];
    };
    // 原神触屏 and the others of the touch input keep the badged posters; 星穹铁道 (touch application, kbm input) has the plain ones; no application keeps an app_* copy of an official cover
    for (const char *name : { "原神触屏", "星铁触屏", "绝区零触屏" }) {
      const json &app = named(name);
      EXPECT_NE(app["image-path"].get<std::string>().find("-badge"), std::string::npos) << variant << " " << name << ": " << app["image-path"];
      EXPECT_NE(app["hero"].get<std::string>().find("hero-badge.jpg"), std::string::npos) << variant << " " << name;
    }
    for (const char *name : { "原神", "绝区零" }) {
      const json &app = named(name);
      const std::string cover = app["image-path"].get<std::string>();
      EXPECT_EQ(cover.find("app_"), std::string::npos) << variant << " " << name << ": the official cover is not an app_* copy any more: " << cover;
      EXPECT_EQ(cover.find("-badge"), std::string::npos) << variant << " " << name << ": a keyboard-and-mouse application has the plain cover";
    }
    const json &sr = named("星穹铁道");
    EXPECT_EQ(sr["image-path"].get<std::string>().find("-badge"), std::string::npos) << variant << ": 星穹铁道 loses the badge";
    EXPECT_EQ(sr["image-path"].get<std::string>().find("app_"), std::string::npos);
    EXPECT_NE(sr["hero"].get<std::string>().find("hero.jpg"), std::string::npos);
    EXPECT_EQ(sr["hero"].get<std::string>().find("hero-badge"), std::string::npos);
    // the direct 原神 / 绝区零: through the launcher with a Sparxie that has the switch (Genshin only: Zenless Zone Zero has no PC-interface switch), direct without it
    const json &gi = named("原神");
    EXPECT_EQ(gi["sparxie"]["mode"], with_switch ? "touch" : "pc") << variant;
    if (with_switch) {
      EXPECT_EQ(gi["sparxie"]["options"]["input"], "kbm");
      EXPECT_NE(gi["cmd"].get<std::string>().find("sparxie-launch"), std::string::npos);
      EXPECT_FALSE(gi["sparxie"]["profile"].get<std::string>().empty());
    }
    EXPECT_EQ(named("绝区零")["sparxie"]["mode"], "pc") << variant;

    json v = json::object();
    v["status"] = sparxie::component_status(machine.env).body;
    v["games"] = sparxie::list_games(machine.env).body;
    json settings = json::object();
    const std::vector<std::tuple<std::string, std::string, std::string>> installs = { { "genshin-cn", "C:/Games/mihoyo/Genshin/YuanShen.exe", "cn" }, { "starrail-cn", "C:/Games/mihoyo/StarRail/StarRail.exe", "cn" }, { "zzz-cn", "C:/Games/mihoyo/ZZZ/ZenlessZoneZero.exe", "cn" } };
    for (const auto &[id, exe, region] : installs)
      settings[id] = sparxie::game_settings_for(machine.env, id, exe, region).body;
    v["gameSettings"] = settings;
    v["apps"] = apps["apps"];
    v["startChanged"] = changed;

    // the host's answer to the save the editor sends for each application (the Web test records them)
    json after = json::object();
    const fs::path snapshot = fs::temp_directory_path() / ("sparxie-captured-gate-" + variant + "-snapshot");
    fs::remove_all(snapshot);
    fs::copy(machine.root, snapshot, fs::copy_options::recursive);
    if (saves.contains(variant)) {
      for (const auto &[name, entry] : saves[variant].items()) {
        int index = -1;
        for (std::size_t i = 0; i < apps["apps"].size(); ++i)
          if (apps["apps"][i].value("name", std::string()) == name) index = static_cast<int>(i);
        ASSERT_GE(index, 0) << name;
        fs::remove_all(machine.root);
        fs::copy(snapshot, machine.root, fs::copy_options::recursive);
        const auto result = sparxie::update_app(machine.env, index, entry["body"]);
        EXPECT_TRUE(result.status.ok) << variant << " " << name << ": " << result.status.error;
        after[name] = { { "result", result.body }, { "apps", machine.apps()["apps"] } };
      }
    }
    // Endfield, added through the host's own path (a game of the next test): the app and the answers the editor is given for it
    {
      fs::remove_all(machine.root);
      fs::copy(snapshot, machine.root, fs::copy_options::recursive);
      const fs::path endfield = machine.root / "games" / "endfield-cn";
      touch_file(endfield / "Endfield.exe");
      touch_file(endfield / "config.ini");
      touch_file(endfield / "sdkdata" / "sdk_hgsdk_config.ini");
      const auto made = sparxie::create_app(machine.env, { { "path", (endfield / "Endfield.exe").string() }, { "mode", "touch" }, { "name", "终末地" } });
      EXPECT_TRUE(made.status.ok) << made.status.error;
      json app = made.body["app"];
      EXPECT_EQ(app["sparxie"]["options"]["input"], "touch") << variant;
      v["endfield"] = { { "app", app }, { "install", app["sparxie"]["install"] }, { "gameSettings", sparxie::game_settings_for(machine.env, app["sparxie"]["install"].get<std::string>(), (endfield / "Endfield.exe").string(), "cn").body }, { "games", sparxie::list_games(machine.env).body } };
    }
    v["afterSave"] = after;
    out[variant] = v;
  }
  if (const char *path = std::getenv("SPARXIE_CAPTURED_GATE_OUT")) {
    json written = out;
    as_captured(written, fs::temp_directory_path() / "sparxie-captured-gate-withSwitch");
    as_captured(written, fs::temp_directory_path() / "sparxie-captured-gate-withoutSwitch");
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << written.dump(2) << "\n";
  }
}
