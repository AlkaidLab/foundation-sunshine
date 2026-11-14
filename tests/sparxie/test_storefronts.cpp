#include "src/sparxie/host.h"
#include "src/sparxie/region.h"
#include "src/sparxie/storefront.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
namespace fs = std::filesystem;
namespace {
  void
  write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
  }
  void
  touch(const fs::path &path) {
    write(path, "x");
  }
  // A path as it appears inside a VDF file: every backslash is escaped.
  std::string
  vdf_path(const fs::path &path) {
    std::string out;
    for (const char c : path.string()) {
      out.push_back(c);
      if (c == '\\') out.push_back('\\');
    }
    return out;
  }
  struct SteamTree {
    fs::path root;
    fs::path lib2;
    fs::path game;
    SteamTree(const fs::path &base) {
      root = base / "steam";
      lib2 = base / "second-drive" / "SteamLibrary";
      game = lib2 / "steamapps" / "common" / "Zenless Zone Zero";
      std::string vdf = "\"libraryfolders\"\n{\n";
      vdf += "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + vdf_path(root) + "\"\n\t\t\"apps\"\n\t\t{\n\t\t}\n\t}\n";
      vdf += "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + vdf_path(lib2) + "\"\n";
      vdf += "\t\t\"apps\"\n\t\t{\n\t\t\t\"4162040\"\t\t\"123456789\"\n\t\t}\n\t}\n}\n";
      write(root / "steamapps" / "libraryfolders.vdf", vdf);
      write(lib2 / "steamapps" / "appmanifest_4162040.acf",
        "\"AppState\"\n{\n\t\"appid\"\t\t\"4162040\"\n\t\"name\"\t\t\"Zenless Zone Zero\"\n"
        "\t\"installdir\"\t\t\"Zenless Zone Zero\"\n}\n");
      touch(game / "ZenlessZoneZero.exe");
      touch(game / "ZenlessZoneZero_Data" / "marker.txt");
    }
  };
}  // namespace

TEST(Vdf, ParsesEscapesCommentsAndNesting) {
  const auto parsed = sparxie::parse_vdf("\"root\" // a comment\n{\n\t\"path\"\t\t\"C:\\\\Games\\\\Steam\"\n"
                                         "\t\"apps\"\n\t{\n\t\t\"4162040\"\t\"42\"\n\t}\n}\n");
  ASSERT_TRUE(parsed.has_value());
  const sparxie::VdfNode *path = parsed->find("path");
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->value, "C:\\Games\\Steam");
  const sparxie::VdfNode *apps = parsed->find("apps");
  ASSERT_NE(apps, nullptr);
  const sparxie::VdfNode *size = apps->find("4162040");
  ASSERT_NE(size, nullptr);
  EXPECT_EQ(size->value, "42");
  EXPECT_FALSE(sparxie::parse_vdf("\"root\"").has_value());
  EXPECT_FALSE(sparxie::parse_vdf("{").has_value());
}

TEST(Steam, SecondLibraryAndGlobalRegion) {
  const fs::path base = fs::temp_directory_path() / "sparxie-steam";
  fs::remove_all(base);
  const SteamTree tree(base);
  sparxie::MemoryRegistry reg;
  sparxie::StorefrontEnv env;
  env.steam_root = tree.root;
  const auto found = sparxie::find_storefront_installs(env);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].source, "steam");
  EXPECT_EQ(found[0].game, "zzz");
  EXPECT_EQ(found[0].region, "global");
  EXPECT_EQ(found[0].exe, "ZenlessZoneZero.exe");
  EXPECT_TRUE(sparxie::paths_equal(found[0].dir, tree.game));
  const auto install = sparxie::identify_install(tree.game, reg, env, "manual");
  ASSERT_TRUE(install.status.ok) << install.status.error;
  EXPECT_EQ(install.install.region, "global");
  EXPECT_EQ(install.install.source, "steam");
  const sparxie::json body = sparxie::install_json(install.install);
  EXPECT_EQ(body["source"], "steam");
  EXPECT_EQ(body["regionName"], "国际服");
  EXPECT_EQ(body["regionNameEn"], "Global");
  EXPECT_TRUE(sparxie::paths_equal(body["dir"], tree.game));
}

TEST(Steam, MissingManifestFindsNothing) {
  const fs::path base = fs::temp_directory_path() / "sparxie-steam-empty";
  fs::remove_all(base);
  const fs::path root = base / "steam";
  fs::create_directories(root / "steamapps");
  sparxie::StorefrontEnv env;
  env.steam_root = root;
  EXPECT_TRUE(sparxie::find_storefront_installs(env).empty());
}

TEST(Epic, ManifestsFindGenshinAndStarRail) {
  const fs::path base = fs::path(fs::temp_directory_path()) / "sparxie-epic";
  fs::remove_all(base);
  const fs::path manifests = base / "Manifests";
  const fs::path genshin = base / "epic-genshin";
  const fs::path rail_root = base / "epic-starrail";
  const fs::path rail_game = rail_root / "StarRail Game";
  touch(genshin / "GenshinImpact.exe");
  touch(genshin / "GenshinImpact_Data" / "marker.txt");
  touch(rail_root / "launcher.exe");
  touch(rail_game / "StarRail.exe");
  touch(rail_game / "StarRail_Data" / "marker.txt");
  touch(base / "epic-other" / "something.exe");
  const auto item = [](const char *app, const char *display, const fs::path &location, const char *executable) {
    return sparxie::json { { "AppName", app }, { "DisplayName", display }, { "InstallLocation", location.string() }, { "LaunchExecutable", executable } }.dump();
  };
  write(manifests / "a1b2c3.item", item("GenshinImpact", "Genshin Impact", genshin, "GenshinImpact.exe"));
  write(manifests / "d4e5f6.item", item("StarRail", "Honkai: Star Rail", rail_root, "launcher.exe"));
  write(manifests / "not-json.item", "{ broken");
  write(manifests / "other.item", item("OtherGame", "Other", base / "epic-other", "something.exe"));
  sparxie::StorefrontEnv env;
  env.epic_manifests = manifests;
  const auto found = sparxie::find_storefront_installs(env);
  ASSERT_EQ(found.size(), 2u);
  EXPECT_EQ(found[0].source, "epic");
  EXPECT_TRUE(sparxie::paths_equal(found[0].dir, genshin) || sparxie::paths_equal(found[0].dir, rail_game));
  for (const auto &row : found) {
    EXPECT_EQ(row.region, "global");
    EXPECT_TRUE(row.game == "genshin" || row.game == "starrail");
    const auto install = sparxie::identify_install(row.dir / row.exe, sparxie::MemoryRegistry(), env, "manual");
    ASSERT_TRUE(install.status.ok) << install.status.error;
    EXPECT_EQ(install.install.region, "global");
    EXPECT_EQ(install.install.source, "epic");
  }
}

TEST(Detection, ListGamesMarksTheStoreAndDeduplicates) {
  const fs::path base = fs::path(fs::temp_directory_path()) / "sparxie-list-games";
  fs::remove_all(base);
  const SteamTree tree(base);
  sparxie::HostEnv env;
  env.host_data = base / "host";
  env.data_dir = base / "sparxie-data";
  env.apps_file = base / "apps.json";
  env.covers_dir = base / "covers";
  env.install_root = base / "install";
  env.steam_root = tree.root;
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  // The launcher registry also points at the Steam install: HoYoPlay keeps
  // the game up to date there. The row stays one and keeps the store badge.
  reg.set_text("Software\\Cognosphere\\HYP\\1_0\\nap_global", "GameInstallPath", tree.game.string());
  const auto listed = sparxie::list_games(env);
  ASSERT_TRUE(listed.status.ok) << listed.status.error;
  ASSERT_EQ(listed.body["detected"].size(), 1u);
  EXPECT_EQ(listed.body["detected"][0]["source"], "steam");
  EXPECT_EQ(listed.body["detected"][0]["region"], "global");
}
