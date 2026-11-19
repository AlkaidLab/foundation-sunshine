#include "src/sparxie/host.h"
#include "src/sparxie/store.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
namespace fs = std::filesystem;
namespace {
  // An apps.json and Sparxie config.json captured from a real host (sanitized: the games' folders only). Index 2 is the plain 原神 app, saved before Sparxie was in use.
  constexpr int kPlainGenshin = 2;
  std::string
  fixture_text(const char *name) {
    std::ifstream in(fs::path(SPARXIE_FIXTURES_DIR) / name, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // the fixtures hold Windows paths; elsewhere the file name of a path with backslashes is the whole path, so they are slashes there
    if (fs::path::preferred_separator != '\\') {
      std::string out;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\\') {
          out += '/';
          ++i;
        }
        else
          out += text[i];
      }
      return out;
    }
    return text;
  }
  struct Sample {
    fs::path root;
    sparxie::HostEnv env;
    explicit Sample(const std::string &name) {
      root = fs::temp_directory_path() / name;
      fs::remove_all(root);
      fs::create_directories(root / "sparxie-data");
      std::ofstream(root / "apps.json", std::ios::binary) << fixture_text("sample-apps.json");
      std::ofstream(root / "sparxie-data" / "config.json", std::ios::binary) << fixture_text("sample-sparxie-config.json");
      env.host_data = root / "host";
      env.data_dir = root / "sparxie-data";
      env.apps_file = root / "apps.json";
      env.covers_dir = root / "covers";
      env.install_root = root / "install";
    }
    sparxie::json
    apps() const {
      std::ifstream in(root / "apps.json");
      sparxie::json body;
      in >> body;
      return body;
    }
    sparxie::json
    config() const {
      std::ifstream in(root / "sparxie-data" / "config.json");
      sparxie::json body;
      in >> body;
      return body;
    }
  };
  // what the editor sends for the plain 原神 app of the sample: the items the person changed (none when the app follows the game)
  sparxie::json
  editor_body(const char *mode, bool preset, bool options) {
    sparxie::json body = { { "name", "原神" }, { "mode", mode }, { "restore", "restore" }, { "cover", "official" }, { "image-path", "app_6de14b94_1794395662.png" }, { "hero", "C:/Sunshine/config/sparxie/posters/genshin-cn/banner.png" } };
    if (preset) body["settings"] = { { "fps", 60 }, { "vsync", false }, { "renderScale", 1 }, { "preset", "high" }, { "hdr", false }, { "resolution", "1920x1080" }, { "display", "fullscreen" } };
    if (options) body["options"] = { { "input", "touch" }, { "fpsUnlock", true }, { "fps", 144 }, { "bgLimit", true }, { "bgFps", 10 }, { "priority", "normal" }, { "followPreset", false }, { "preset30", 60 }, { "preset60", 1000 }, { "scaleOverride", false }, { "scale", 400 }, { "isDefault", true } };
    return body;
  }
}  // namespace

TEST(EditSave, EveryEditorBodyOfThePlainGameSaves) {
  for (const char *mode : { "pc", "touch" }) {
    for (int variant = 0; variant < 4; ++variant) {
      Sample sample(std::string("sparxie-edit-save-") + mode + std::to_string(variant));
      const auto saved = sparxie::update_app(sample.env, kPlainGenshin, editor_body(mode, variant & 1, variant & 2));
      ASSERT_TRUE(saved.status.ok) << mode << " variant " << variant << ": " << saved.status.error_code << " " << saved.status.error;
      const auto apps = sample.apps();
      EXPECT_FALSE(apps["apps"][kPlainGenshin]["sparxie"].contains("settingsMode")) << "the two-mode switch is gone: " << mode << variant;
      EXPECT_EQ(apps["apps"][kPlainGenshin]["sparxie"]["settings"].size(), (variant & 1) && std::string(mode) == "touch" && (variant & 2) ? 6u : 7u) << "sent: the items sent (the game's frame-rate tier goes while Sparxie unlocks the rate); not sent: what was stored stays: " << mode << variant;
    }
  }
}

TEST(EditSave, APlainGameAppStaysPlainAndWritesNoProfile) {
  Sample sample("sparxie-edit-save-stays-pc");
  const auto before = sample.config()["profiles"].size();
  const auto saved = sparxie::update_app(sample.env, kPlainGenshin, editor_body("pc", true, true));
  ASSERT_TRUE(saved.status.ok) << saved.status.error_code << " " << saved.status.error;
  const auto apps = sample.apps();
  const auto &app = apps["apps"][kPlainGenshin];
  EXPECT_EQ(app["sparxie"]["mode"], "pc");
  EXPECT_EQ(app["sparxie"]["profile"], "");
  EXPECT_EQ(app["cmd"].get<std::string>().find("sparxie-launch"), std::string::npos) << "the game still starts directly";
  EXPECT_EQ(sample.config()["profiles"].size(), before) << "a direct app has no profile in Sparxie";
  EXPECT_EQ(app["sparxie"]["options"]["fps"], 144) << "the Sparxie items the person changed are kept for later";
}

TEST(EditSave, ASaveThatFailsAfterTheProfileLeavesNoProfile) {
  Sample sample("sparxie-edit-save-rollback");
  const std::string apps_before = sample.apps().dump();
  const std::string config_before = sample.config().dump();
  // the list is written through apps.json.tmp: a folder of that name makes the write fail after Sparxie's profile was written
  fs::create_directories(sample.root / "apps.json.tmp");
  const auto saved = sparxie::update_app(sample.env, kPlainGenshin, editor_body("touch", true, false));
  ASSERT_FALSE(saved.status.ok);
  EXPECT_EQ(saved.status.error_code, "apps_write_failed");
  EXPECT_EQ(sample.config().dump(), config_before) << "the profile that was written is taken back";
  EXPECT_EQ(sample.apps().dump(), apps_before);
}

TEST(EditSave, TheCreateThatFailsToSaveLeavesNoProfile) {
  Sample sample("sparxie-create-rollback");
  const fs::path game = sample.root / "game";
  fs::create_directories(game);
  std::ofstream(game / "StarRail.exe") << "x";
  const std::string config_before = sample.config().dump();
  fs::create_directories(sample.root / "apps.json.tmp");
  const auto made = sparxie::create_app(sample.env, { { "path", (game / "StarRail.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "星铁" } });
  ASSERT_FALSE(made.status.ok);
  EXPECT_EQ(made.status.error_code, "apps_write_failed");
  EXPECT_EQ(sample.config().dump(), config_before);
}

TEST(EditSave, ProfilesNoApplicationUsesGoAtHostStart) {
  Sample sample("sparxie-orphans");
  auto ids = [&] {
    std::vector<std::string> out;
    const auto config = sample.config();
    for (const auto &profile : config["profiles"]) out.push_back(profile["id"]);
    return out;
  };
  // a profile of Sparxie's own making, and one only a command line names
  auto config = sample.config();
  config["profiles"].push_back({ { "displayName", "mine" }, { "executablePath", "" }, { "game", "genshin" }, { "hoyo", nullptr }, { "id", "my-own" } });
  config["profiles"].push_back({ { "displayName", "named" }, { "executablePath", "" }, { "game", "starRail" }, { "hoyo", nullptr }, { "id", "starrail-cn-9" } });
  std::ofstream(sample.root / "sparxie-data" / "config.json") << config.dump(2);
  auto apps = sample.apps();
  apps["apps"].push_back({ { "name", "by hand" }, { "cmd", "\"sparxie-launch.exe\" --sparxie \"Sparxie.exe\" --profile starrail-cn-9" } });
  std::ofstream(sample.root / "apps.json") << apps.dump(2);

  sample.env.process_running = [](const std::vector<std::string> &) { return true; };
  EXPECT_EQ(sparxie::remove_orphan_profiles(sample.env), 0) << "nothing is written while Sparxie is open";
  EXPECT_EQ(ids().size(), 10u);
  sample.env.process_running = nullptr;

  EXPECT_EQ(sparxie::remove_orphan_profiles(sample.env), 4);
  const std::vector<std::string> kept { "genshin-cn-1", "starrail-cn-1", "zzz-cn-1", "starrail-cn-2", "my-own", "starrail-cn-9" };
  EXPECT_EQ(ids(), kept) << "genshin-cn-2/3/4 and zzz-cn-2 of the failed saves go; every other profile stays";
  EXPECT_EQ(sample.config()["selectedProfileId"], "genshin-cn-1");
  bool backed_up = false;
  for (const auto &entry : fs::directory_iterator(sample.root / "sparxie-data"))
    if (entry.path().filename().string().rfind("config.backup-", 0) == 0) backed_up = true;
  EXPECT_TRUE(backed_up) << "Sparxie's config is backed up before profiles are removed";
  EXPECT_EQ(sparxie::remove_orphan_profiles(sample.env), 0) << "a second start finds nothing";
}

TEST(EditSave, AnUnreadableApplicationListRemovesNothing) {
  Sample sample("sparxie-orphans-unreadable");
  std::ofstream(sample.root / "apps.json") << "{ not json";
  EXPECT_EQ(sparxie::remove_orphan_profiles(sample.env), 0);
  EXPECT_EQ(sample.config()["profiles"].size(), 8u);
  fs::remove(sample.root / "apps.json");
  EXPECT_EQ(sparxie::remove_orphan_profiles(sample.env), 0) << "no list at all is not a list without apps";
  EXPECT_EQ(sample.config()["profiles"].size(), 8u);
}
