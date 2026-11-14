/**
 * @file tests/unit/test_app_scan.cpp
 * @brief Parser, discovery and directory bound tests for the app scanner.
 */
#include "src/app_scan/scanner.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  fs::path
  fixture_path(const char *name) {
    return fs::path(SUNSHINE_SOURCE_DIR) / "tests" / "fixtures" / "app_scan" / name;
  }

  std::string
  read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  }

  class TempTree {
  public:
    TempTree() {
      static std::atomic<unsigned> counter { 0 };
      static std::mt19937_64 generator { std::random_device {}() };
      std::error_code ec;
      root = fs::temp_directory_path(ec);
      if (ec) {
        root = fs::path("/scratch");
      }
      root /= "app-scan-test-" + std::to_string(generator()) + "-" + std::to_string(counter++);
      fs::create_directories(root, ec);
    }

    ~TempTree() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    const fs::path &
    path() const {
      return root;
    }

    fs::path
    make_file(const fs::path &relative, std::string_view content) const {
      const fs::path target = root / relative;
      std::error_code ec;
      fs::create_directories(target.parent_path(), ec);
      std::ofstream file(target, std::ios::binary);
      file.write(content.data(), static_cast<std::streamsize>(content.size()));
      return target;
    }

    fs::path
    make_directory(const fs::path &relative) const {
      const fs::path target = root / relative;
      std::error_code ec;
      fs::create_directories(target, ec);
      return target;
    }

  private:
    fs::path root;
  };

  class FakeRegistry final: public app_scan::RegistryView {
  public:
    std::map<std::string, std::map<std::string, std::string>> values;
    std::map<std::string, std::vector<std::string>> children;

    std::optional<std::string>
    read_string(const std::string &key, const std::string &value) const override {
      const auto key_it = values.find(key);
      if (key_it == values.end()) {
        return std::nullopt;
      }
      const auto value_it = key_it->second.find(value);
      if (value_it == key_it->second.end()) {
        return std::nullopt;
      }
      return value_it->second;
    }

    std::vector<std::string>
    subkeys(const std::string &key) const override {
      const auto it = children.find(key);
      return it == children.end() ? std::vector<std::string> {} : it->second;
    }
  };

  const app_scan::Game *
  find_game(const app_scan::ScanResult &result, const std::string &platform, const std::string &id) {
    for (const app_scan::Game &game : result.games) {
      if (game.platform == platform && game.id == id) {
        return &game;
      }
    }
    return nullptr;
  }

  bool
  contains_warning(const app_scan::ScanResult &result, std::string_view text) {
    for (const std::string &warning : result.warnings) {
      if (warning.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
}  // namespace

TEST(AppScanManifestTest, ParsesSteamAppManifest) {
  const auto manifest = app_scan::parse_steam_manifest(read_file(fixture_path("appmanifest_620.acf")));
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->app_id, "620");
  EXPECT_EQ(manifest->name, "Portal 2");
  EXPECT_EQ(manifest->install_dir_name, "Portal 2");
  ASSERT_TRUE(manifest->size_on_disk.has_value());
  EXPECT_EQ(*manifest->size_on_disk, 13000000000ull);
}

TEST(AppScanManifestTest, RejectsToolAndRuntimeManifests) {
  const std::string tool_manifest =
    "\"AppState\"\n{\n"
    "\t\"appid\"\t\t\"228980\"\n"
    "\t\"name\"\t\t\"Steamworks Common Redistributables\"\n"
    "\t\"installdir\"\t\t\"Steamworks Shared\"\n"
    "\t\"apptype\"\t\t\"tool\"\n"
    "}\n";
  EXPECT_FALSE(app_scan::parse_steam_manifest(tool_manifest).has_value());

  const std::string small_id =
    "\"AppState\"\n{\n"
    "\t\"appid\"\t\t\"5\"\n"
    "\t\"name\"\t\t\"Steam Client\"\n"
    "\t\"installdir\"\t\t\"client\"\n"
    "}\n";
  EXPECT_FALSE(app_scan::parse_steam_manifest(small_id).has_value());

  const std::string soundtrack =
    "\"AppState\"\n{\n"
    "\t\"appid\"\t\t\"900\"\n"
    "\t\"name\"\t\t\"Example Soundtrack\"\n"
    "\t\"installdir\"\t\t\"soundtrack\"\n"
    "}\n";
  EXPECT_FALSE(app_scan::parse_steam_manifest(soundtrack).has_value());
}

TEST(AppScanManifestTest, ParsesSteamLibraryFolders) {
  const std::vector<std::string> paths =
    app_scan::parse_library_folder_paths(read_file(fixture_path("libraryfolders.vdf")));
  ASSERT_EQ(paths.size(), 2u);
  EXPECT_EQ(paths[0], "C:\\Program Files (x86)\\Steam");
  EXPECT_EQ(paths[1], "D:\\SteamLibrary");
}

TEST(AppScanManifestTest, ParsesEpicManifest) {
  const auto manifest = app_scan::parse_epic_manifest(read_file(fixture_path("Fortnite.item")));
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->display_name, "Fortnite");
  EXPECT_EQ(manifest->app_name, "Fortnite");
  EXPECT_EQ(manifest->install_location, "D:\\Epic Games\\Fortnite");
  EXPECT_EQ(manifest->launch_executable, "FortniteLauncher.exe");
  ASSERT_TRUE(manifest->install_size.has_value());
  EXPECT_EQ(*manifest->install_size, 48234127360ull);
}

TEST(AppScanManifestTest, RejectsEpicManifestWithoutExecutable) {
  const std::string manifest =
    "{\n"
    "  \"DisplayName\": \"Example\",\n"
    "  \"AppName\": \"ExampleApp\",\n"
    "  \"InstallLocation\": \"D:\\\\Games\\\\Example\"\n"
    "}\n";
  EXPECT_FALSE(app_scan::parse_epic_manifest(manifest).has_value());
  EXPECT_FALSE(app_scan::parse_epic_manifest("not json").has_value());
}

TEST(AppScanHeuristicsTest, AcceptsGamePaths) {
  using app_scan::detect_if_game;
  EXPECT_TRUE(detect_if_game("C:\\SteamLibrary\\steamapps\\common\\Hades\\Hades.exe", "Hades", std::nullopt));
  EXPECT_TRUE(detect_if_game("C:\\Epic Games\\Example\\Example.exe", "Example", std::nullopt));
  EXPECT_TRUE(detect_if_game("C:\\Program Files\\Games\\Example\\Example.exe", "Example", std::nullopt));
  EXPECT_TRUE(detect_if_game(
    "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Games\\Example.lnk",
    "Example",
    std::string("C:\\SteamLibrary\\steamapps\\common\\Example\\Example.exe")));
}

TEST(AppScanHeuristicsTest, RejectsToolsAndLaunchers) {
  using app_scan::detect_if_game;
  EXPECT_FALSE(detect_if_game("C:\\SteamLibrary\\steamapps\\common\\Hades\\uninstall.exe", "uninstall", std::nullopt));
  EXPECT_FALSE(detect_if_game("C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", "chrome", std::nullopt));
  EXPECT_FALSE(detect_if_game("C:\\Program Files\\Games\\Tool\\Tool.exe", "Tool", std::nullopt));
  EXPECT_FALSE(detect_if_game("C:\\Games\\readme.txt", "readme", std::nullopt));
  EXPECT_FALSE(detect_if_game(
    "C:\\Games\\Example.lnk",
    "Example",
    std::string("C:\\Windows\\System32\\notepad.exe")));
}

TEST(AppScanDirectoryTest, FindsGamesAndSkipsTools) {
  TempTree tree;
  const fs::path game = tree.make_file("library/steamapps/common/Hades/Hades.exe", "MZ");
  tree.make_file("library/steamapps/common/Hades/uninstall.exe", "MZ");
  tree.make_file("library/Documents/notes.txt", "notes");
  tree.make_file("library/.cache/steamapps/common/Hidden/Hidden.exe", "MZ");

  app_scan::ScanRequest request;
  request.directory = tree.path() / "library";
  const app_scan::ScanResult result = app_scan::scan(request, app_scan::HostContext {});

  ASSERT_EQ(result.games.size(), 1u);
  EXPECT_EQ(result.games[0].platform, "directory");
  EXPECT_EQ(result.games[0].name, "Hades");
  EXPECT_EQ(result.games[0].exe, app_scan::to_utf8(game.lexically_normal()));
  EXPECT_TRUE(result.warnings.empty());
}

TEST(AppScanDirectoryTest, RejectsUncAndDevicePaths) {
  TempTree tree;
  const auto missing = app_scan::validate_scan_root(tree.path() / "missing");
  ASSERT_TRUE(missing.has_value());
  EXPECT_EQ(*missing, "directory does not exist");

  const auto unc = app_scan::validate_scan_root(fs::path("\\\\server\\share"));
  ASSERT_TRUE(unc.has_value());
  EXPECT_EQ(*unc, "UNC paths are not allowed");

  const auto device = app_scan::validate_scan_root(fs::path("\\\\.\\PhysicalDrive0"));
  ASSERT_TRUE(device.has_value());
  EXPECT_EQ(*device, "device paths are not allowed");

  const auto namespace_path = app_scan::validate_scan_root(fs::path("\\\\?\\C:\\Games"));
  ASSERT_TRUE(namespace_path.has_value());
  EXPECT_EQ(*namespace_path, "device paths are not allowed");

#ifndef _WIN32
  EXPECT_TRUE(app_scan::validate_scan_root(fs::path("/dev/null")).has_value());
#endif
}

TEST(AppScanDirectoryTest, SkipsSymlinkEscapes) {
  TempTree tree;
  const fs::path outside = tree.path() / "outside";
  std::error_code ec;
  fs::create_directories(outside / "steamapps/common/Outside", ec);
  if (ec) {
    GTEST_SKIP() << "cannot create the outside tree";
  }
  std::ofstream(outside / "steamapps/common/Outside/Outside.exe") << "MZ";

  const fs::path root = tree.make_directory("root");
  fs::create_directory_symlink(outside, root / "linked", ec);
  if (ec) {
    GTEST_SKIP() << "directory.symlinks are not permitted on this host";
  }

  app_scan::ScanRequest request;
  request.directory = root;
  const app_scan::ScanResult result = app_scan::scan(request, app_scan::HostContext {});

  EXPECT_TRUE(result.games.empty());
  EXPECT_TRUE(contains_warning(result, "directory.symlinks"));
}

TEST(AppScanDirectoryTest, StopsAtDepthLimit) {
  TempTree tree;
  tree.make_file("library/steamapps/common/Game/Game.exe", "MZ");

  app_scan::ScanRequest request;
  request.directory = tree.path() / "library";
  request.directory_limits.max_depth = 2;
  const app_scan::ScanResult result = app_scan::scan(request, app_scan::HostContext {});

  EXPECT_TRUE(result.games.empty());
  EXPECT_TRUE(contains_warning(result, "directory.depth"));
}

TEST(AppScanDirectoryTest, StopsAtTimeLimit) {
  TempTree tree;
  tree.make_file("library/steamapps/common/Game/Game.exe", "MZ");

  app_scan::ScanRequest request;
  request.directory = tree.path() / "library";
  request.directory_limits.time_limit = std::chrono::milliseconds(0);
  const app_scan::ScanResult result = app_scan::scan(request, app_scan::HostContext {});

  EXPECT_TRUE(result.games.empty());
  EXPECT_TRUE(contains_warning(result, "directory.time"));
}

TEST(AppScanScanTest, RegistryMockFindsAllSources) {
  TempTree tree;

  const fs::path steam_root = tree.path() / "steam";
  tree.make_file("steam/steamapps/appmanifest_620.acf", read_file(fixture_path("appmanifest_620.acf")));
  const fs::path portal_exe = tree.make_file("steam/steamapps/common/Portal 2/Portal2.exe", "MZ");

  const fs::path second_library = tree.path() / "library2";
  tree.make_file("library2/steamapps/appmanifest_730.acf",
    "\"AppState\"\n{\n"
    "\t\"appid\"\t\t\"730\"\n"
    "\t\"name\"\t\t\"Counter-Strike 2\"\n"
    "\t\"installdir\"\t\t\"Counter-Strike Global Offensive\"\n"
    "\t\"apptype\"\t\t\"game\"\n"
    "}\n");
  const fs::path cs_exe = tree.make_file(
    "library2/steamapps/common/Counter-Strike Global Offensive/game/cs2.exe", "MZ");
  (void) cs_exe;

  const std::string folders =
    "\"libraryfolders\"\n{\n"
    "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
    app_scan::to_utf8(steam_root) + "\"\n\t}\n"
                                    "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" +
    app_scan::to_utf8(second_library) + "\"\n\t}\n"
                                        "}\n";
  tree.make_file("steam/steamapps/libraryfolders.vdf", folders);

  tree.make_file("ProgramData/Epic/EpicGamesLauncher/Data/Manifests/Fortnite.item",
    read_file(fixture_path("Fortnite.item")));

  const fs::path gog_install = tree.path() / "GOG Games" / "The Witcher 3";
  tree.make_directory("GOG Games/The Witcher 3");
  const std::string gog_exe = app_scan::to_utf8(gog_install / "witcher3.exe");

  FakeRegistry registry;
  registry.values["SOFTWARE\\WOW6432Node\\Valve\\Steam"]["InstallPath"] = app_scan::to_utf8(steam_root);
  registry.children["SOFTWARE\\WOW6432Node\\GOG.com\\Games"] = { "1207658924" };
  registry.values["SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207658924"]["gameName"] = "The Witcher 3";
  registry.values["SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207658924"]["path"] = app_scan::to_utf8(gog_install);
  registry.values["SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1207658924"]["exe"] = gog_exe;

  app_scan::HostContext context;
  context.platform = app_scan::HostPlatform::windows_host;
  context.home = tree.path();
  context.program_data = tree.path() / "ProgramData";
  context.registry = &registry;

  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::steam, app_scan::Source::epic, app_scan::Source::gog };
  const app_scan::ScanResult result = app_scan::scan(request, context);

  ASSERT_EQ(result.games.size(), 4u) << (result.warnings.empty() ? "" : result.warnings.front());
  EXPECT_TRUE(result.warnings.empty());

  const app_scan::Game *portal = find_game(result, "steam", "620");
  ASSERT_NE(portal, nullptr);
  EXPECT_EQ(portal->name, "Portal 2");
  EXPECT_EQ(portal->launch, "steam://rungameid/620");
  EXPECT_EQ(portal->install_dir, app_scan::to_utf8((steam_root / "steamapps/common/Portal 2").lexically_normal()));
  EXPECT_EQ(portal->exe, app_scan::to_utf8(portal_exe.lexically_normal()));
  EXPECT_NE(portal->cover_hint.find("/620/header.jpg"), std::string::npos);

  const app_scan::Game *cs2 = find_game(result, "steam", "730");
  ASSERT_NE(cs2, nullptr);
  EXPECT_EQ(cs2->name, "Counter-Strike 2");

  const app_scan::Game *fortnite = find_game(result, "epic", "Fortnite");
  ASSERT_NE(fortnite, nullptr);
  EXPECT_EQ(fortnite->name, "Fortnite");
  EXPECT_EQ(fortnite->launch, "com.epicgames.launcher://apps/Fortnite?action=launch&silent=true");
  EXPECT_EQ(fortnite->install_dir, "D:\\Epic Games\\Fortnite");
  EXPECT_TRUE(fortnite->exe.ends_with("FortniteLauncher.exe"));

  const app_scan::Game *witcher = find_game(result, "gog", "1207658924");
  ASSERT_NE(witcher, nullptr);
  EXPECT_EQ(witcher->name, "The Witcher 3");
  EXPECT_EQ(witcher->install_dir, app_scan::to_utf8(gog_install));
  EXPECT_EQ(witcher->exe, gog_exe);
  EXPECT_EQ(witcher->launch, "\"" + gog_exe + "\"");
}

TEST(AppScanScanTest, LinuxFindsSteamAndMarksOthersUnsupported) {
  TempTree tree;
  const fs::path home = tree.path() / "home";
  tree.make_file("home/.steam/steam/steamapps/appmanifest_620.acf", read_file(fixture_path("appmanifest_620.acf")));
  tree.make_file("home/.steam/steam/steamapps/common/Portal 2/Portal2.exe", "MZ");

  app_scan::HostContext context;
  context.platform = app_scan::HostPlatform::linux_host;
  context.home = home;

  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::steam, app_scan::Source::epic, app_scan::Source::gog };
  const app_scan::ScanResult result = app_scan::scan(request, context);

  ASSERT_EQ(result.games.size(), 1u);
  EXPECT_EQ(result.games[0].platform, "steam");
  EXPECT_EQ(result.games[0].id, "620");
  EXPECT_EQ(result.warnings.size(), 2u);
  EXPECT_TRUE(contains_warning(result, "epic.unsupported"));
  EXPECT_TRUE(contains_warning(result, "gog.unsupported"));
}

TEST(AppScanScanTest, MacFindsSteamLibrary) {
  TempTree tree;
  const fs::path home = tree.path() / "home";
  tree.make_file("home/Library/Application Support/Steam/steamapps/appmanifest_620.acf",
    read_file(fixture_path("appmanifest_620.acf")));
  tree.make_file("home/Library/Application Support/Steam/steamapps/common/Portal 2/Portal2.exe", "MZ");

  app_scan::HostContext context;
  context.platform = app_scan::HostPlatform::macos_host;
  context.home = home;

  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::steam };
  const app_scan::ScanResult result = app_scan::scan(request, context);

  ASSERT_EQ(result.games.size(), 1u);
  EXPECT_EQ(result.games[0].platform, "steam");
  EXPECT_TRUE(result.warnings.empty());
}

TEST(AppScanMaskTest, MasksTheUserHomePrefix) {
  const fs::path home = fs::path("/home/example");
  const std::string masked = app_scan::mask_user_path("/home/example/Games/Title", home);
  EXPECT_EQ(masked, "~/Games/Title");
  EXPECT_EQ(app_scan::mask_user_path("/home/example", home), "~");
  EXPECT_EQ(app_scan::mask_user_path("/home/examplex/Games", home), "/home/examplex/Games");
  EXPECT_EQ(app_scan::mask_user_path("/srv/games", home), "/srv/games");
}

// Diagnostic run against the machine that executes the tests. It stays out of
// the default suite so automated runs do not depend on installed games.
TEST(AppScanScanTest, RealHostScanWhenRequested) {
  const char *enabled = std::getenv("APP_SCAN_REAL_HOST");
  if (!enabled || std::string_view(enabled) != "1") {
    GTEST_SKIP() << "set APP_SCAN_REAL_HOST=1 to scan this host";
  }

  app_scan::HostContext context;
  context.platform = app_scan::detect_host_platform();
  if (const char *home = std::getenv("USERPROFILE"); home && *home) {
    context.home = app_scan::from_utf8(home);
  }
  else if (const char *home = std::getenv("HOME"); home && *home) {
    context.home = app_scan::from_utf8(home);
  }
  if (const char *program_data = std::getenv("ProgramData"); program_data && *program_data) {
    context.program_data = app_scan::from_utf8(program_data);
  }
  const auto registry = app_scan::make_native_registry();
  context.registry = registry.get();

  app_scan::ScanRequest request;
  request.sources = { app_scan::Source::steam, app_scan::Source::epic, app_scan::Source::gog };
  const app_scan::ScanResult result = app_scan::scan(request, context);

  std::map<std::string, std::size_t> counts;
  for (const app_scan::Game &game : result.games) {
    ++counts[game.platform];
    EXPECT_FALSE(game.name.empty());
    EXPECT_FALSE(game.id.empty());
    EXPECT_FALSE(game.install_dir.empty());
    EXPECT_FALSE(game.launch.empty());
  }
  for (const auto &[platform, count] : counts) {
    std::cout << "app_scan real host: " << platform << "=" << count << '\n';
  }
  for (const std::string &warning : result.warnings) {
    std::cout << "app_scan warning: " << warning << '\n';
  }
  std::cout << "app_scan real host total=" << result.games.size() << '\n';
}
