#include <cstdint>
#include "src/app_scan/scanner.h"
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
namespace {
  namespace fs = std::filesystem;
  class TempTree {
  public:
    TempTree() {
      std::error_code ec;
      root = fs::temp_directory_path(ec);
      if (ec) root = fs::path("/scratch");
      root /= "app-scan-lib-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
      fs::create_directories(root, ec);
    }
    ~TempTree() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }
    const fs::path &
    path() const { return root; }
    fs::path
    make_file(const fs::path &relative, std::string_view content) const {
      const fs::path target = root / relative;
      std::error_code ec;
      fs::create_directories(target.parent_path(), ec);
      std::ofstream file(target, std::ios::binary);
      file.write(content.data(), static_cast<std::streamsize>(content.size()));
      return target;
    }

  private:
    fs::path root;
  };
  class FakeRegistry final: public app_scan::RegistryView {
  public:
    std::map<std::string, std::map<std::string, std::string>> values;
    std::map<std::string, std::vector<std::string>> children;
    std::map<std::string, std::map<std::string, std::string>> user_values;
    std::map<std::string, std::vector<std::string>> user_children;
    std::optional<std::string>
    read_string(const std::string &key, const std::string &value) const override {
      const auto key_it = values.find(key);
      if (key_it == values.end()) return std::nullopt;
      const auto value_it = key_it->second.find(value);
      if (value_it == key_it->second.end()) return std::nullopt;
      return value_it->second;
    }
    std::vector<std::string>
    subkeys(const std::string &key) const override {
      const auto it = children.find(key);
      return it == children.end() ? std::vector<std::string> {} : it->second;
    }
    std::optional<std::string>
    read_user_string(const std::string &key, const std::string &value) const override {
      const auto key_it = user_values.find(key);
      if (key_it == user_values.end()) return std::nullopt;
      const auto value_it = key_it->second.find(value);
      if (value_it == key_it->second.end()) return std::nullopt;
      return value_it->second;
    }
    std::vector<std::string>
    user_subkeys(const std::string &key) const override {
      const auto it = user_children.find(key);
      return it == user_children.end() ? std::vector<std::string> {} : it->second;
    }
  };
  class FakePackages final: public app_scan::PackageView {
  public:
    std::vector<app_scan::PackageRecord> rows;
    std::vector<app_scan::PackageRecord>
    packages() const override { return rows; }
  };
  app_scan::HostContext
  windows_host(const TempTree &tree, const FakeRegistry &registry) {
    app_scan::HostContext context;
    context.platform = app_scan::HostPlatform::windows_host;
    context.program_data = tree.path() / "ProgramData";
    context.local_app_data = tree.path() / "Local";
    context.roaming_app_data = tree.path() / "Roaming";
    context.registry = &registry;
    context.drive_roots = { tree.path() / "D" };
    return context;
  }
  const app_scan::Game *
  find_platform(const app_scan::ScanResult &result, const std::string &platform) {
    for (const auto &game : result.games)
      if (game.platform == platform || std::find(game.sources.begin(), game.sources.end(), platform) != game.sources.end()) return &game;
    return nullptr;
  }
  bool
  warned(const app_scan::ScanResult &result, const std::string &code) {
    return std::find(result.warnings.begin(), result.warnings.end(), code) != result.warnings.end();
  }
  std::string
  ksep(std::initializer_list<const char *> parts) {
    std::string out;
    for (const char *part : parts) {
      if (!out.empty()) out.push_back(char(92));
      out += part;
    }
    return out;
  }
}  // namespace
TEST(AppScanPackageTest, ReadsFamilyFromFullName) {
  const auto family = app_scan::package_family_from_full_name("Example.Game_1.0.0.0_x64__8wekyb3d8bbwe");
  ASSERT_TRUE(family.has_value());
  EXPECT_EQ(*family, "Example.Game_8wekyb3d8bbwe");
  EXPECT_FALSE(app_scan::package_family_from_full_name("not-a-package").has_value());
}
TEST(AppScanLibraryTest, XboxReadsGamingRootAndLaunchesThePackage) {
  TempTree tree;
  tree.make_file("D/.GamingRoot", "XboxGames");
  const std::string xml = "<?xml version=\"1.0\"?><Game configVersion=\"1\"><Identity Name=\"Example.Game\" Publisher=\"CN=Example\" Version=\"1.0.0.0\"/><ExecutableList><Executable Name=\"Game.exe\" Id=\"Game\" TargetDeviceFamily=\"PC\"/></ExecutableList><ShellVisuals DefaultDisplayName=\"Example Game\" Square150x150Logo=\"Logo.png\"/></Game>";
  tree.make_file("D/XboxGames/Example/Content/MicrosoftGame.config", xml);
  tree.make_file("D/XboxGames/Example/Content/Game.exe", "MZ");
  tree.make_file("D/XboxGames/Example/Content/Logo.png", "png");
  FakeRegistry registry;
  FakePackages packages;
  app_scan::PackageRecord package;
  package.family_name = "Example.Game_8wekyb3d8bbwe";
  package.app_id = "Game";
  package.install_path = app_scan::to_utf8(tree.path() / "D/XboxGames/Example/Content");
  packages.rows.push_back(package);
  auto context = windows_host(tree, registry);
  context.packages = &packages;
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::xbox };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "xbox");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->name, "Example Game");
  EXPECT_EQ(game->launch, std::string("shell:AppsFolder") + std::string(1, char(92)) + "Example.Game_8wekyb3d8bbwe!Game");
  EXPECT_TRUE(game->cover_hint.ends_with("Logo.png"));
}
TEST(AppScanLibraryTest, XboxMissingSaysSo) {
  TempTree tree;
  FakeRegistry registry;
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::xbox };
  const auto result = app_scan::scan(request, context);
  EXPECT_TRUE(result.games.empty());
  EXPECT_TRUE(warned(result, "xbox.not_found"));
}
TEST(AppScanLibraryTest, EaUsesLink2EaFromInstallerData) {
  TempTree tree;
  tree.make_file("Local/Electronic Arts/EA Desktop/user_1.ini", std::string("user.downloadinplacedir=") + app_scan::to_utf8(tree.path() / "EA"));
  tree.make_file("EA/Dead Space/__Installer/installerdata.xml", "<DiPManifest><gameTitles><gameTitle locale=\"en_US\">Dead Space</gameTitle></gameTitles><contentIDs><contentID>1002975</contentID></contentIDs></DiPManifest>");
  FakeRegistry registry;
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::ea };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "ea");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->name, "Dead Space");
  EXPECT_EQ(game->launch, "link2ea://launchgame/1002975");
}
TEST(AppScanLibraryTest, UbisoftReadsInstallsAndLocalArt) {
  TempTree tree;
  FakeRegistry registry;
  const std::string key = ksep({ "SOFTWARE", "WOW6432Node", "Ubisoft", "Launcher", "Installs" });
  registry.children[key] = { "42" };
  registry.values[key + std::string(1, char(92)) + "42"]["InstallDir"] = "D:/Games/Anno";
  registry.values[key + std::string(1, char(92)) + "42"]["DisplayName"] = "Anno";
  tree.make_file("Local/Ubisoft Game Launcher/cache/assets/42/thumb.jpg", "jpg");
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::ubisoft };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "ubisoft");
  ASSERT_NE(game, nullptr);
  EXPECT_EQ(game->launch, "uplay://launch/42/0");
  EXPECT_EQ(game->name, "Anno");
  EXPECT_TRUE(game->cover_hint.ends_with("thumb.jpg"));
}
TEST(AppScanLibraryTest, BattleNetReadsProductStrings) {
  TempTree tree;
  const auto game_dir = tree.make_file("Games/Diablo IV/placeholder.txt", "x").parent_path();
  tree.make_file("ProgramData/Battle.net/Agent/.product.db", std::string("fenris") + std::string(1, char(10)) + app_scan::to_utf8(game_dir));
  FakeRegistry registry;
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::battlenet };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "battlenet");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->name, "Diablo IV");
  EXPECT_EQ(game->launch, "battlenet://fenris");
}
TEST(AppScanLibraryTest, RiotLaunchesThroughTheClient) {
  TempTree tree;
  tree.make_file("ProgramData/Riot Games/RiotClientInstalls.json", "{\"rc_live\":\"C:/Riot Games/Riot Client/RiotClientServices.exe\"}");
  tree.make_file("ProgramData/Riot Games/Metadata/league_of_legends.live/product.yaml", std::string("product_install_full_path: \"D:/Riot Games/League of Legends\"") + std::string(1, char(10)) + "shortcut_name: \"League of Legends.lnk\"");
  FakeRegistry registry;
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::riot };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "riot");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->name, "League of Legends");
  EXPECT_EQ(game->id, "league_of_legends");
  EXPECT_NE(game->launch.find("--launch-product=league_of_legends"), std::string::npos);
  EXPECT_NE(game->launch.find("--launch-patchline=live"), std::string::npos);
  EXPECT_NE(game->launch.find("RiotClientServices.exe"), std::string::npos);
}
TEST(AppScanLibraryTest, HoyoPlayUsesTheSameRegistryDetection) {
  TempTree tree;
  const auto dir = tree.make_file("Genshin/YuanShen.exe", "MZ").parent_path();
  FakeRegistry registry;
  const std::string key = ksep({ "Software", "miHoYo", "HYP", "1_1", "hk4e_cn" });
  registry.user_values[key]["GameInstallPath"] = app_scan::to_utf8(dir);
  registry.user_children[ksep({ "Software", "miHoYo", "HYP" })] = { "1_1" };
  auto context = windows_host(tree, registry);
  tree.make_file("covers/sparxie-genshin-cn-2026.png", "png");
  context.covers_dir = tree.path() / "covers";
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::hoyoplay };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "hoyoplay");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->name, "原神");
  EXPECT_EQ(game->region, "cn");
  EXPECT_NE(game->exe.find("YuanShen.exe"), std::string::npos);
  EXPECT_NE(game->cover_hint.find("sparxie-genshin-cn-2026.png"), std::string::npos);
}
TEST(AppScanLibraryTest, WeGameReadsRailApps) {
  TempTree tree;
  FakeRegistry registry;
  const std::string key = ksep({ "SOFTWARE", "WOW6432Node", "Tencent", "WeGame" });
  const auto root = tree.path() / "WeGame";
  registry.values[key]["InstallPath"] = app_scan::to_utf8(root);
  tree.make_file("WeGame/rail_apps.json", "[{\"game_id\":\"2001\",\"name\":\"Example\",\"exe\":\"D:/Games/Example.exe\"}]");
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::wegame };
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "wegame");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->name, "Example");
  EXPECT_EQ(game->launch, "wegame://launch_game/2001");
}
TEST(AppScanLibraryTest, SparxieAbsentIsNotAnErrorAndProfilesOfferBothLaunches) {
  TempTree tree;
  FakeRegistry registry;
  auto context = windows_host(tree, registry);
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::sparxie };
  auto missing = app_scan::scan(request, context);
  EXPECT_TRUE(missing.games.empty());
  EXPECT_TRUE(warned(missing, "sparxie.absent"));
  const auto exe = tree.make_file("Genshin/YuanShen.exe", "MZ");
  const auto dir = exe.parent_path();
  const std::string hyp = ksep({ "Software", "miHoYo", "HYP", "1_1", "hk4e_cn" });
  registry.user_values[hyp]["GameInstallPath"] = app_scan::to_utf8(dir);
  tree.make_file("component/Sparxie.exe", "MZ");
  const std::string config = std::string("{\"schemaVersion\":1,\"profiles\":[{\"id\":\"genshin-cn-1\",\"displayName\":\"原神\",\"game\":\"genshin\",\"executablePath\":\"") + app_scan::to_utf8(exe) + "\"}]}";
  tree.make_file("sparxie-data/config.json", config);
  context.sparxie_exe = tree.path() / "component/Sparxie.exe";
  context.sparxie_data = tree.path() / "sparxie-data";
  const auto result = app_scan::scan(request, context);
  const auto *game = find_platform(result, "sparxie");
  ASSERT_NE(game, nullptr) << (result.warnings.empty() ? std::string() : result.warnings.front());
  EXPECT_EQ(game->profile, "genshin-cn-1");
  EXPECT_EQ(game->region, "cn");
  ASSERT_EQ(game->launches.size(), 2u);
  EXPECT_EQ(game->launches[0].id, "touch");
  EXPECT_EQ(game->launches[1].id, "pc");
  EXPECT_NE(game->launches[1].launch.find("YuanShen.exe"), std::string::npos);
}
TEST(AppScanLibraryTest, DuplicateInstallPrefersTheStoreLaunch) {
  app_scan::Game steam;
  steam.platform = "steam";
  steam.name = "Hades";
  steam.id = "1145360";
  steam.install_dir = "D:/Games/Hades";
  steam.exe = "D:/Games/Hades/Hades.exe";
  steam.launch = "steam://rungameid/1145360";
  app_scan::Game gog = steam;
  gog.platform = "gog";
  gog.id = "1";
  gog.launch = "gog-direct";
  gog.sources.clear();
  std::vector<app_scan::Game> games { gog, steam };
  app_scan::dedupe_games(games);
  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].platform, "steam");
  EXPECT_EQ(games[0].launch, "steam://rungameid/1145360");
  EXPECT_EQ(games[0].sources.front(), "steam");
  EXPECT_NE(std::find(games[0].sources.begin(), games[0].sources.end(), "gog"), games[0].sources.end());
}
TEST(AppScanLibraryTest, NonWindowsLibrariesSayUnsupported) {
  app_scan::HostContext context;
  context.platform = app_scan::HostPlatform::linux_host;
  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::xbox, app_scan::Source::sparxie };
  const auto result = app_scan::scan(request, context);
  EXPECT_TRUE(warned(result, "xbox.unsupported"));
  EXPECT_TRUE(warned(result, "sparxie.unsupported"));
}
