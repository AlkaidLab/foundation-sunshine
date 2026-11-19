#include "src/sparxie/host.h"
#include "src/sparxie/platform.h"
#include "src/sparxie/region.h"
#include "src/sparxie/scan.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
namespace fs = std::filesystem;
namespace {
  void
  touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "x";
  }
  fs::path
  fresh(const char *name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
  }
  std::vector<std::string>
  names(const sparxie::ScanResult &result) {
    std::vector<std::string> out;
    for (const auto &match : result.matches) out.push_back(match.exe.parent_path().filename().string() + "/" + match.exe.filename().string() + "@" + std::to_string(match.depth));
    return out;
  }
  sparxie::HostEnv
  env_at(const fs::path &root) {
    sparxie::HostEnv env;
    env.host_data = root / "host";
    env.data_dir = root / "sparxie-data";
    env.apps_file = root / "apps.json";
    env.covers_dir = root / "covers";
    env.install_root = root / "install";
    env.component_dir = root / "components" / "sparxie";
    return env;
  }
}  // namespace

TEST(Scan, TheChosenFolderItselfIsLevelZeroAndWinsOverDeeperInstalls) {
  const fs::path root = fresh("sparxie-scan-root");
  touch(root / "YuanShen.exe");
  touch(root / "deep" / "StarRail.exe");
  const auto result = sparxie::scan_for_games(root, {});
  ASSERT_EQ(result.matches.size(), 1u);
  EXPECT_EQ(result.matches[0].depth, 0);
  EXPECT_EQ(result.matches[0].exe.filename().string(), "YuanShen.exe");
  EXPECT_EQ(result.levels, 1);
  EXPECT_EQ(result.folders, 1u);
}

TEST(Scan, LevelsAreSearchedBreadthFirstAndTheSearchStopsAtTheFirstMatchingLevel) {
  const fs::path root = fresh("sparxie-scan-bfs");
  // A deep branch that sorts first must not beat a shallower match in a later branch.
  touch(root / "a" / "b" / "c" / "ZenlessZoneZero.exe");
  touch(root / "z" / "StarRail.exe");
  touch(root / "z" / "deeper" / "StarRail.exe");
  const auto result = sparxie::scan_for_games(root, {});
  EXPECT_EQ(names(result), (std::vector<std::string> { "z/StarRail.exe@1" }));
  EXPECT_EQ(result.levels, 2);
}

TEST(Scan, EveryMatchOfTheFirstMatchingLevelIsListed) {
  const fs::path root = fresh("sparxie-scan-several");
  touch(root / "cn" / "YuanShen.exe");
  touch(root / "global" / "GenshinImpact.exe");
  touch(root / "other" / "readme.txt");
  const auto result = sparxie::scan_for_games(root, {});
  EXPECT_EQ(names(result), (std::vector<std::string> { "cn/YuanShen.exe@1", "global/GenshinImpact.exe@1" }));
}

TEST(Scan, DepthLimit) {
  const fs::path root = fresh("sparxie-scan-depth");
  touch(root / "a" / "b" / "c" / "d" / "StarRail.exe");
  touch(root / "a" / "b" / "c" / "d" / "e" / "ZenlessZoneZero.exe");
  sparxie::ScanOptions options;
  options.max_depth = 4;
  options.game = "starrail";
  EXPECT_EQ(names(sparxie::scan_for_games(root, options)), (std::vector<std::string> { "d/StarRail.exe@4" }));
  options.max_depth = 3;
  EXPECT_TRUE(sparxie::scan_for_games(root, options).matches.empty());
  options.game = "zzz";
  options.max_depth = 4;
  EXPECT_TRUE(sparxie::scan_for_games(root, options).matches.empty());
  options.max_depth = 5;
  EXPECT_EQ(names(sparxie::scan_for_games(root, options)), (std::vector<std::string> { "e/ZenlessZoneZero.exe@5" }));
}

TEST(Scan, TimeLimitStopsTheSearchAndSaysSo) {
  const fs::path root = fresh("sparxie-scan-time");
  for (int i = 0; i < 6; ++i) touch(root / ("d" + std::to_string(i)) / "file.txt");
  touch(root / "d5" / "inner" / "StarRail.exe");
  // The clock jumps one second at every look; the limit is two and a half.
  auto tick = std::make_shared<int>(0);
  sparxie::ScanOptions options;
  options.limit = std::chrono::milliseconds(2500);
  options.now = [tick] { return std::chrono::steady_clock::time_point { std::chrono::seconds((*tick)++) }; };
  const auto result = sparxie::scan_for_games(root, options);
  EXPECT_TRUE(result.timed_out);
  EXPECT_TRUE(result.matches.empty());
  EXPECT_LT(result.folders, 4u);
}

TEST(Scan, FolderCountLimitIsReportedAsTruncated) {
  const fs::path root = fresh("sparxie-scan-cap");
  for (int i = 0; i < 5; ++i) touch(root / ("d" + std::to_string(i)) / "file.txt");
  sparxie::ScanOptions options;
  options.max_folders = 3;
  const auto result = sparxie::scan_for_games(root, options);
  EXPECT_TRUE(result.truncated);
  EXPECT_FALSE(result.timed_out);
}

TEST(Scan, GameAndRegionNarrowTheExecutableNames) {
  const fs::path root = fresh("sparxie-scan-filter");
  touch(root / "cn" / "YuanShen.exe");
  touch(root / "global" / "GenshinImpact.exe");
  touch(root / "star" / "StarRail.exe");
  sparxie::ScanOptions options;
  options.game = "genshin";
  options.region = "global";
  EXPECT_EQ(names(sparxie::scan_for_games(root, options)), (std::vector<std::string> { "global/GenshinImpact.exe@1" }));
  options.region = "bili";
  EXPECT_EQ(names(sparxie::scan_for_games(root, options)), (std::vector<std::string> { "cn/YuanShen.exe@1" }));
  options.game = "starrail";
  options.region = "";
  EXPECT_EQ(names(sparxie::scan_for_games(root, options)), (std::vector<std::string> { "star/StarRail.exe@1" }));
}

TEST(Scan, SkippedFoldersAndSystemFoldersAreNotEntered) {
  const fs::path root = fresh("sparxie-scan-skip");
  touch(root / "components" / "sparxie" / "StarRail.exe");
  touch(root / "$RECYCLE.BIN" / "StarRail.exe");
  touch(root / "Windows" / "StarRail.exe");
  sparxie::ScanOptions options;
  options.skip.push_back(root / "components" / "sparxie");
  EXPECT_TRUE(sparxie::scan_for_games(root, options).matches.empty());
  EXPECT_TRUE(sparxie::scan_for_games(root / "nothing", options).matches.empty());
}

TEST(Scan, ExecutableNamesPerRegion) {
  EXPECT_EQ(sparxie::executables_for("genshin", "cn"), (std::vector<std::string> { "YuanShen.exe" }));
  EXPECT_EQ(sparxie::executables_for("genshin", "bili"), (std::vector<std::string> { "YuanShen.exe" }));
  EXPECT_EQ(sparxie::executables_for("genshin", "global"), (std::vector<std::string> { "GenshinImpact.exe" }));
  EXPECT_EQ(sparxie::executables_for("genshin"), (std::vector<std::string> { "YuanShen.exe", "GenshinImpact.exe" }));
  EXPECT_EQ(sparxie::executables_for("zzz", "cn"), (std::vector<std::string> { "ZenlessZoneZero.exe", "ZenlessZoneZeroBeta.exe" }));
  EXPECT_EQ(sparxie::game_of_executable("yuanshen.EXE"), "genshin");
  EXPECT_EQ(sparxie::game_of_executable("Sparxie.exe"), "");
  const auto catalog = sparxie::catalog_json();
  ASSERT_EQ(catalog.size(), 9u);
  EXPECT_EQ(catalog[0]["game"], "genshin");
  EXPECT_EQ(catalog[0]["regions"].size(), 3u);
}

TEST(Browse, DrivesFirstThenFoldersThenExecutablesWithKnownGamesLeading) {
  const fs::path root = fresh("sparxie-browse");
  fs::create_directories(root / "beta");
  fs::create_directories(root / "Alpha");
  fs::create_directories(root / "$hidden");
  touch(root / "zeta.exe");
  touch(root / "StarRail.exe");
  touch(root / "notes.txt");
  const auto roots = [] { return std::vector<sparxie::BrowseEntry> { { "C:", "C:\\", "drive", false } }; };
  const auto drives = sparxie::browse_folder({}, true, roots);
  ASSERT_TRUE(drives.status.ok);
  ASSERT_EQ(drives.entries.size(), 1u);
  EXPECT_EQ(drives.entries[0].kind, "drive");

  const auto listed = sparxie::browse_folder(root, true, roots);
  ASSERT_TRUE(listed.status.ok);
  std::vector<std::string> order;
  for (const auto &entry : listed.entries) order.push_back(entry.name);
  EXPECT_EQ(order, (std::vector<std::string> { "Alpha", "beta", "StarRail.exe", "zeta.exe" }));
  EXPECT_TRUE(listed.entries[2].known);
  EXPECT_FALSE(listed.entries[3].known);
  EXPECT_EQ(listed.parent, root.parent_path().string());

  const auto folders_only = sparxie::browse_folder(root, false, roots);
  EXPECT_EQ(folders_only.entries.size(), 2u);

  const auto capped = sparxie::browse_folder(root, true, roots, 3);
  EXPECT_EQ(capped.entries.size(), 3u);
  EXPECT_TRUE(capped.truncated);

  EXPECT_EQ(sparxie::browse_folder("relative/path", true, roots).status.error_code, "invalid_input");
  EXPECT_EQ(sparxie::browse_folder(root / "missing", true, roots).status.error_code, "not_found");
  // listing never creates or removes anything
  EXPECT_TRUE(fs::exists(root / "$hidden"));
}

TEST(HostApi, IdentifyKeepsToTheChosenGameAndRegionAndNeverTakesSparxie) {
  const fs::path root = fresh("sparxie-host-identify");
  sparxie::HostEnv env = env_at(root);
  touch(root / "games" / "g" / "GenshinImpact.exe");
  touch(root / "games" / "s" / "StarRail.exe");
  touch(root / "components" / "sparxie" / "app-1" / "Sparxie.exe");
  env.sparxie_exe = root / "components" / "sparxie" / "app-1" / "Sparxie.exe";

  auto ok = sparxie::identify_path(env, root / "games" / "s" / "StarRail.exe", "starrail", "cn");
  EXPECT_TRUE(ok.status.ok);
  EXPECT_EQ(ok.body["install"]["game"], "starrail");

  auto wrong_game = sparxie::identify_path(env, root / "games" / "s", "genshin", "global");
  EXPECT_EQ(wrong_game.status.error_code, "wrong_game");
  EXPECT_EQ(wrong_game.body["allowed"][0], "GenshinImpact.exe");

  auto wrong_region = sparxie::identify_path(env, root / "games" / "g" / "GenshinImpact.exe", "genshin", "cn");
  EXPECT_EQ(wrong_region.status.error_code, "wrong_game");
  EXPECT_EQ(wrong_region.body["allowed"][0], "YuanShen.exe");

  EXPECT_TRUE(sparxie::identify_path(env, root / "games" / "g" / "GenshinImpact.exe", "genshin", "global").status.ok);
  EXPECT_EQ(sparxie::identify_path(env, root / "components" / "sparxie" / "app-1" / "Sparxie.exe").status.error_code, "is_sparxie");
  EXPECT_EQ(sparxie::identify_path(env, root / "components" / "sparxie").status.error_code, "is_sparxie");
  EXPECT_EQ(sparxie::identify_path(env, root / "components").status.error_code, "unrecognised");
}

TEST(HostApi, FolderScanReturnsEveryInstallOfTheNearestLevelAndAnswersWhyNot) {
  const fs::path root = fresh("sparxie-host-scan");
  sparxie::HostEnv env = env_at(root);
  touch(root / "Games" / "HoYoPlay" / "games" / "Genshin Impact Game" / "GenshinImpact.exe");
  touch(root / "Games" / "HoYoPlay" / "games" / "Genshin Impact Game" / "GenshinImpact_Data" / "x.txt");
  touch(root / "Games" / "Other" / "YuanShen.exe");
  touch(root / "components" / "sparxie" / "StarRail.exe");

  auto found = sparxie::scan_folder(env, { { "path", (root / "Games").string() }, { "game", "genshin" } });
  ASSERT_TRUE(found.status.ok);
  ASSERT_EQ(found.body["installs"].size(), 1u);
  EXPECT_EQ(found.body["installs"][0]["exe"], "YuanShen.exe");
  EXPECT_EQ(found.body["installs"][0]["depth"], 1);

  auto deep = sparxie::scan_folder(env, { { "path", (root / "Games").string() }, { "game", "genshin" }, { "region", "global" } });
  ASSERT_TRUE(deep.status.ok);
  EXPECT_EQ(deep.body["installs"][0]["exe"], "GenshinImpact.exe");
  EXPECT_EQ(deep.body["installs"][0]["depth"], 3);
  EXPECT_EQ(deep.body["installs"][0]["region"], "global");

  auto shallow_limit = sparxie::scan_folder(env, { { "path", (root / "Games").string() }, { "game", "genshin" }, { "region", "global" }, { "depth", 2 } });
  EXPECT_EQ(shallow_limit.status.error_code, "unrecognised");
  EXPECT_EQ(shallow_limit.body["depth"], 2);
  EXPECT_FALSE(shallow_limit.body["timed_out"].get<bool>());

  EXPECT_EQ(sparxie::scan_folder(env, { { "path", (root / "nothing").string() } }).status.error_code, "not_found");
  EXPECT_EQ(sparxie::scan_folder(env, { { "path", "" } }).status.error_code, "invalid_input");
  EXPECT_EQ(sparxie::scan_folder(env, { { "path", (root / "Games").string() }, { "game", "nope" } }).status.error_code, "invalid_input");
  EXPECT_EQ(sparxie::scan_folder(env, { { "path", (root / "components").string() } }).status.error_code, "unrecognised");
  EXPECT_EQ(sparxie::scan_folder(env, { { "path", (root / "components" / "sparxie").string() } }).status.error_code, "is_sparxie");
}

TEST(HostApi, NoSignedInUserMeansNothingIsReadAndNothingIsStarted) {
  const fs::path root = fresh("sparxie-host-nouser");
  sparxie::HostEnv env = env_at(root);
  touch(root / "Games" / "StarRail.exe");
  sparxie::set_user_runner([](const sparxie::UserCall &) { return false; });
  EXPECT_EQ(sparxie::scan_folder(env, { { "path", (root / "Games").string() } }).status.error_code, "no_user_session");
  EXPECT_EQ(sparxie::browse_path(env, { { "path", root.string() } }).status.error_code, "no_user_session");
  EXPECT_EQ(sparxie::identify_path(env, root / "Games").status.error_code, "no_user_session");
  sparxie::set_user_runner({});
  EXPECT_TRUE(sparxie::identify_path(env, root / "Games").status.ok);
}

TEST(HostApi, BrowseListsAsReadOnlyAndRefusesNetworkPaths) {
  const fs::path root = fresh("sparxie-host-browse");
  sparxie::HostEnv env = env_at(root);
  touch(root / "Games" / "StarRail.exe");
  auto listed = sparxie::browse_path(env, { { "path", (root / "Games").string() }, { "files", "exe" } });
  ASSERT_TRUE(listed.status.ok);
  EXPECT_EQ(listed.body["entries"][0]["name"], "StarRail.exe");
  EXPECT_EQ(listed.body["entries"][0]["known"], true);
  EXPECT_EQ(sparxie::browse_path(env, { { "path", "\\\\server\\share" } }).status.error_code, "invalid_input");
  EXPECT_EQ(sparxie::browse_path(env, { { "path", "//server/share" } }).status.error_code, "invalid_input");
  EXPECT_TRUE(sparxie::browse_path(env, sparxie::json::object()).body["entries"].is_array());
}

TEST(HostApi, OpeningSparxieGoesThroughTheUserSessionLauncherOnly) {
  const fs::path root = fresh("sparxie-host-open");
  sparxie::HostEnv env = env_at(root);
  EXPECT_EQ(sparxie::open_component(env).status.error_code, "absent");
  touch(root / "components" / "sparxie" / "Sparxie.exe");
  env.sparxie_exe = root / "components" / "sparxie" / "Sparxie.exe";
  EXPECT_EQ(sparxie::open_component(env).status.error_code, "launch_unavailable");
  int launched = 0;
  fs::path last;
  env.launch_gui = [&](const fs::path &exe) {
    ++launched;
    last = exe;
    return sparxie::Status::success();
  };
  auto started = sparxie::open_component(env);
  ASSERT_TRUE(started.status.ok);
  EXPECT_EQ(launched, 1);
  EXPECT_EQ(last, env.sparxie_exe);
  EXPECT_FALSE(started.body["already_running"].get<bool>());
  env.process_running = [](const std::vector<std::string> &) { return true; };
  auto again = sparxie::open_component(env);
  EXPECT_TRUE(again.body["already_running"].get<bool>());
  EXPECT_EQ(launched, 1);
  env.process_running = {};
  env.launch_gui = [](const fs::path &) { return sparxie::Status::fail(500, "launch_failed", "denied"); };
  EXPECT_EQ(sparxie::open_component(env).status.error_code, "launch_failed");
}

TEST(HostApi, GamesAnswerCarriesTheCatalogAndStatusTheProgramPath) {
  const fs::path root = fresh("sparxie-host-catalog");
  sparxie::HostEnv env = env_at(root);
  auto games = sparxie::list_games(env);
  ASSERT_TRUE(games.body.contains("catalog"));
  EXPECT_EQ(games.body["catalog"].size(), 9u);
  touch(root / "components" / "sparxie" / "Sparxie.exe");
  env.sparxie_exe = root / "components" / "sparxie" / "Sparxie.exe";
  EXPECT_EQ(sparxie::component_status(env).body["exe"], env.sparxie_exe.string());
}

TEST(HostApi, RefreshFetchesThePosterOfEveryGameAndRegionWithoutAnInstall) {
  const fs::path root = fresh("sparxie-host-posters");
  sparxie::HostEnv env = env_at(root);
  std::vector<std::string> launchers;
  auto get_url = [&](const std::string &url) -> std::optional<std::string> {
    if (url.find("getGames") != std::string::npos) {
      const auto at = url.find("launcher_id=");
      launchers.push_back(url.substr(at + 12, url.find('&', at) - at - 12));
      sparxie::json body;
      for (const char *biz : { "hk4e_cn", "hkrpg_cn", "nap_cn", "bh3_cn", "hk4e_global", "hkrpg_global", "nap_global", "bh3_global" }) {
        sparxie::json game;
        game["biz"] = biz;
        game["display"]["background"]["url"] = std::string("https://cdn/") + biz + "_" + launchers.back() + "_poster.webp";
        game["display"]["top_left_logo"]["url"] = std::string("https://cdn/") + biz + "_" + launchers.back() + "_big.png";
        body["data"]["games"].push_back(game);
      }
      return body.dump();
    }
    return std::string("img");
  };
  auto decode = [](std::string_view) {
    sparxie::Rgba image;
    image.w = 16;
    image.h = 16;
    image.px.assign(16 * 16 * 4, 200);
    for (std::size_t i = 0; i < image.px.size(); i += 8) image.px[i] = image.px[i + 1] = image.px[i + 2] = 90;  // some texture: the host refuses a one-colour background
    return image;
  };
  const auto first = sparxie::refresh_official(env, get_url, decode);
  ASSERT_TRUE(first.status.ok);
  // 11 HoYoverse posters plus every non-HoYoverse title and region: this fixture's
  // transport answers no official source for them, so their placeholder cards are
  // published the same way (the 12 whose titles have a source are reported skipped:
  // arknights global has none and is not).
  EXPECT_EQ(first.body["updated"], 24) << first.body.dump();
  EXPECT_EQ(first.body["skipped"].size(), 12u) << first.body.dump();
  EXPECT_EQ(launchers.size(), 11u);
  for (const char *id : { "genshin-cn", "genshin-bili", "genshin-global", "starrail-cn", "starrail-bili", "starrail-global", "zzz-cn", "zzz-bili", "zzz-global", "hi3-cn", "hi3-global" })
    EXPECT_TRUE(fs::exists(env.covers_dir) && fs::exists(env.host_data / "posters" / id / "banner.png") && fs::exists(env.host_data / "posters" / id / "hero.jpg")) << id;
  // each id keeps its own cache: nothing is fetched twice, and the national and bilibili posters do not overwrite each other
  const auto second = sparxie::refresh_official(env, get_url, decode);
  EXPECT_EQ(second.body["updated"], 0);
  EXPECT_EQ(second.body["heroes"], 0);
  // the larger launcher logo is kept for the hero, and an install cached before it was kept is fetched once more
  bool large_kept = false;
  for (const auto &entry : fs::directory_iterator(env.host_data / "posters" / "genshin-cn"))
    if (entry.path().filename().string().find("-herologo.bin") != std::string::npos) large_kept = true;
  EXPECT_TRUE(large_kept);
  fs::remove_all(env.host_data / "posters" / "zzz-global");
  const auto again = sparxie::refresh_official(env, get_url, decode);
  EXPECT_EQ(again.body["updated"], 1) << "only the install whose cache was gone";
  EXPECT_TRUE(fs::exists(env.host_data / "posters" / "zzz-global" / "hero.jpg"));
  // an install refreshed before the landscape poster existed has its pictures cached and no hero.jpg: the next refresh composes it from the cache, fetching no picture
  for (const char *id : { "genshin-cn", "zzz-bili", "starrail-global" }) fs::remove(env.host_data / "posters" / id / "hero.jpg");
  int fetched_pictures = 0;
  auto count_get = [&](const std::string &url) -> std::optional<std::string> {
    // the vendor sources ask their API each run (a few small answers); only a picture counts
    const bool api = url.find("getGames") != std::string::npos || url.find("getAllGameBasicInfo") != std::string::npos || url.find("gamestarter") != std::string::npos || url.find("IStoreBrowseService") != std::string::npos ||
                     url.find("Version.ini") != std::string::npos || url.find("steamcmd.net") != std::string::npos;
    // the program icons of a title that has none yet (this transport answers nothing a tile can use) are asked again each run: not a poster picture
    if (url.find(".ico") != std::string::npos || url.find("yo-star.com") != std::string::npos) return get_url(url);
    if (!api) ++fetched_pictures;
    return get_url(url);
  };
  const auto third = sparxie::refresh_official(env, count_get, decode);
  EXPECT_EQ(third.body["updated"], 0);
  EXPECT_EQ(third.body["heroes"], 3);
  EXPECT_EQ(fetched_pictures, 0);
  for (const char *id : { "genshin-cn", "zzz-bili", "starrail-global" }) EXPECT_TRUE(fs::exists(env.host_data / "posters" / id / "hero.jpg")) << id;
}

namespace {
  std::string
  genshin_root() {
    return std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "原神";
  }
}  // namespace

TEST(Settings, OnlyTheChosenFieldsAreWritten) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "genshin";
  install.region = "cn";
  install.id = "genshin-cn";
  const std::string root = genshin_root();
  reg.set_dword(root, "Screenmanager Resolution Width_h182942802", 2560);
  reg.set_dword(root, "Screenmanager Resolution Height_h2627697771", 1440);
  reg.set_dword(root, "Screenmanager Is Fullscreen mode_h3981298716", 1);
  sparxie::GameValues values;
  values.display = "windowed";
  const auto wrote = sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", { "display" });
  EXPECT_TRUE(wrote.status.ok) << wrote.status.error;
  EXPECT_EQ(reg.get(root, "Screenmanager Is Fullscreen mode_h3981298716").dword, 0u);
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 2560u) << "a field nobody chose keeps the game's value";
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Height_h2627697771").dword, 1440u);
  EXPECT_EQ(reg.get(root, "WINDOWS_HDR_ON_h3132281285").kind, sparxie::RegValue::Kind::none);
  // a value that is not valid is only refused when it is one of the chosen fields: a tier the game does not have does not stop a display change
  values.fps = 45;
  EXPECT_TRUE(sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", { "display" }).status.ok);
  EXPECT_TRUE(sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", { "fps" }).status.ok) << "the game's own 45 tier is written";
  values.fps = 50;
  EXPECT_EQ(sparxie::write_game_settings(install, reg, values, {}, [] { return false; }, {}, "restore", { "fps" }).status.error_code, "invalid_input");
}

TEST(Settings, ChosenFieldsOfAStoredObject) {
  using sparxie::json;
  EXPECT_TRUE(sparxie::chosen_fields(json::object()).empty());
  EXPECT_EQ(sparxie::chosen_fields({ { "fps", 60 } }), (std::set<std::string> { "fps" }));
  EXPECT_EQ(sparxie::chosen_fields({ { "fps", 60 }, { "nonsense", 1 }, { "hdr", true } }), (std::set<std::string> { "fps", "hdr" }));
  // every key an application holds is one the person changed, even a value that equals the default
  EXPECT_EQ(sparxie::chosen_fields(sparxie::values_json(sparxie::GameValues {})).size(), 7u);
  json changed = sparxie::values_json(sparxie::GameValues {});
  changed["fps"] = 30;
  EXPECT_EQ(sparxie::chosen_fields(changed).size(), 7u);
}

TEST(HostApi, LaunchLeavesTheGameAloneUntilThePersonChoseSomething) {
  const fs::path root = fresh("sparxie-host-launch-settings");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  touch(root / "game" / "YuanShen.exe");
  auto make = [&](const sparxie::json &settings) {
    sparxie::json app = { { "name", "G" }, { "cmd", "x" }, { "sparxie", { { "install", "genshin-cn" }, { "game", "genshin" }, { "region", "cn" }, { "dir", (root / "game").string() }, { "exe", (root / "game" / "YuanShen.exe").string() }, { "settings", settings }, { "restore", "restore" } } } };
    std::ofstream out(env.apps_file);
    out << sparxie::json { { "apps", { app } } }.dump();
  };
  const std::string key = genshin_root();
  make(sparxie::json::object());
  EXPECT_TRUE(sparxie::prepare_launch(env, "G", "x").ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none) << "no settings chosen: nothing is written";
  make({ { "vsync", false } });
  EXPECT_TRUE(sparxie::prepare_launch(env, "G", "x").ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none) << "only the held item (V-sync) is written, not the size";
  // 固定 display: the person's own window mode (the display choice decides the window; the settings only say what else is written)
  make({ { "display", "windowed" } });
  {
    std::ifstream in(env.apps_file);
    sparxie::json body;
    in >> body;
    body["apps"][0]["sparxie"]["display"] = { { "mode", "fixed" }, { "window", "windowed" } };
    std::ofstream out(env.apps_file);
    out << body.dump();
  }
  const auto status = sparxie::prepare_launch(env, "G", "x");
  EXPECT_TRUE(status.ok) << status.error;
  EXPECT_EQ(reg.get(key, "Screenmanager Is Fullscreen mode_h3981298716").dword, 0u);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none) << "only the chosen field is written";
}

TEST(HostApi, CreateKeepsTheChosenSettingsTheRestorePolicyAndACustomCover) {
  const fs::path root = fresh("sparxie-host-create-r3");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto plain = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "pc" }, { "name", "plain" } });
  ASSERT_TRUE(plain.status.ok) << plain.status.error;
  EXPECT_TRUE(plain.body["app"]["sparxie"]["settings"].empty()) << "a new application follows the game's own settings";
  EXPECT_EQ(plain.body["app"]["sparxie"]["cover"], "official");
  EXPECT_FALSE(plain.body["app"].contains("image-path"));
  // only the items the person changed are stored: an application created with one item holds that one
  auto auto_mode = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "auto" }, { "settings", { { "fps", 60 } } } });
  ASSERT_TRUE(auto_mode.status.ok) << auto_mode.status.error;
  EXPECT_FALSE(auto_mode.body["app"]["sparxie"].contains("settingsMode"));
  EXPECT_EQ(auto_mode.body["app"]["sparxie"]["settings"].size(), 1u);
  EXPECT_EQ(auto_mode.body["app"]["sparxie"]["settings"]["fps"], 60);
  EXPECT_EQ(auto_mode.body["app"]["sparxie"]["mode"], "pc") << "直接启动 is stored as pc";
  auto own = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "pc" }, { "name", "own" }, { "settings", { { "fps", 60 }, { "bogus", 1 }, { "display", "windowed" } } }, { "restore", "keep" }, { "cover", "custom" }, { "image-path", "C:\\pics\\p.png" }, { "hero", "https://example.net/h.jpg" } });
  ASSERT_TRUE(own.status.ok) << own.status.error;
  const auto &block = own.body["app"]["sparxie"];
  EXPECT_FALSE(block.contains("settingsMode"));
  EXPECT_EQ(block["settings"].size(), 2u) << "only the items that were sent (and that the host knows) are held";
  EXPECT_EQ(block["settings"]["display"], "windowed");
  EXPECT_EQ(block["settings"]["fps"], 60);
  EXPECT_FALSE(block["settings"].contains("bogus"));
  EXPECT_EQ(block["restore"], "keep");
  EXPECT_EQ(block["cover"], "custom");
  EXPECT_EQ(own.body["app"]["image-path"], "C:\\pics\\p.png");
  EXPECT_EQ(own.body["app"]["hero"], "https://example.net/h.jpg");
  // an official poster that is refreshed later does not replace it
  sparxie::ArtUrls urls;
  urls.version = "v9";
  sparxie::Rgba image;
  image.w = 8;
  image.h = 8;
  image.px.assign(8 * 8 * 4, 90);
  for (std::size_t i = 0; i < image.px.size(); i += 8) image.px[i] = image.px[i + 1] = image.px[i + 2] = 200;  // some texture: the host refuses a one-colour background
  sparxie::publish_art(env, "genshin-cn", urls, image, image, {});
  std::ifstream in(env.apps_file);
  sparxie::json apps;
  in >> apps;
  EXPECT_EQ(apps["apps"][2]["image-path"], "C:\\pics\\p.png");
  EXPECT_NE(apps["apps"][0].value("image-path", std::string()).find("v9"), std::string::npos);
}

TEST(HostApi, GameSettingsAreReadForAnInstallThatIsNotStoredYetAndPostersCarryTheirVersion) {
  const fs::path root = fresh("sparxie-host-read-r3");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  touch(root / "game" / "GenshinImpact.exe");
  const std::string intl = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "Genshin Impact";
  reg.set_dword(intl, "Screenmanager Resolution Width_h182942802", 2560);
  reg.set_dword(intl, "Screenmanager Resolution Height_h2627697771", 1440);
  const auto read = sparxie::game_settings_for(env, "genshin-open-abc", (root / "game" / "GenshinImpact.exe").string(), "global");
  ASSERT_TRUE(read.status.ok) << read.status.error;
  EXPECT_EQ(read.body["values"]["resolution"], sparxie::resolution_text(2560, 1440));
  EXPECT_EQ(sparxie::game_settings_for(env, "nope", "", "").status.error_code, "not_found");
  fs::create_directories(env.host_data / "posters" / "genshin-cn");
  {
    std::ofstream out(env.host_data / "posters" / "genshin-cn" / "index.json");
    out << sparxie::json { { "version", "abc123" } }.dump();
  }
  const auto games = sparxie::list_games(env);
  EXPECT_EQ(games.body["art"]["genshin-cn"]["version"], "abc123");
  EXPECT_FALSE(games.body["art"].contains("zzz-global"));
}

TEST(HostApi, ThePresetModeWritesEveryValueAndAutoWritesNothing) {
  const fs::path root = fresh("sparxie-host-preset-mode");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  touch(root / "game" / "YuanShen.exe");
  const std::string key = genshin_root();
  auto make = [&](const std::string &mode, const sparxie::json &settings) {
    sparxie::json block = { { "install", "genshin-cn" }, { "game", "genshin" }, { "region", "cn" }, { "dir", (root / "game").string() }, { "exe", (root / "game" / "YuanShen.exe").string() }, { "settings", settings }, { "restore", "restore" } };
    if (!mode.empty()) block["settingsMode"] = mode;  // the removed two-mode switch: "auto" still holds nothing the person chose
    sparxie::json app = { { "name", "G" }, { "cmd", "x" }, { "sparxie", block } };
    std::ofstream out(env.apps_file);
    out << sparxie::json { { "apps", { app } } }.dump();
  };
  sparxie::json preset = sparxie::values_json(sparxie::GameValues {});
  preset["resolution"] = "2560x1440";
  make("auto", preset);
  EXPECT_TRUE(sparxie::prepare_launch(env, "G", "x").ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none) << "自动 never touches the game, whatever is stored";
  EXPECT_EQ(reg.get(key, "Screenmanager Is Fullscreen mode_h3981298716").dword, 1u) << "the display choice (follow the stream) is written in every settings mode: the full-screen window";
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none) << "an unknown stream size leaves the stored size";
  make("preset", sparxie::values_json(sparxie::GameValues {}));
  EXPECT_TRUE(sparxie::prepare_launch(env, "G", "x").ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").kind, sparxie::RegValue::Kind::none) << "the size belongs to the display choice, not to the preset";
  {
    std::ifstream in(env.apps_file);
    sparxie::json body;
    in >> body;
    body["apps"][0]["sparxie"]["display"] = { { "mode", "fixed" }, { "resolution", "1920x1080" }, { "window", "fullscreen" } };
    std::ofstream out(env.apps_file);
    out << body.dump();
  }
  EXPECT_TRUE(sparxie::prepare_launch(env, "G", "x").ok);
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").dword, 1920u) << "a fixed display writes its size";
  EXPECT_EQ(reg.get(key, "Screenmanager Is Fullscreen mode_h3981298716").dword, 1u);
}

TEST(HostApi, InstallsSayWhereTheyWereFoundAndTitlesComeWithTheirNames) {
  const fs::path root = fresh("sparxie-host-sources");
  sparxie::HostEnv env = env_at(root);
  touch(root / "SteamLibrary" / "steamapps" / "common" / "ZenlessZoneZero" / "ZenlessZoneZero.exe");
  auto steam = sparxie::identify_path(env, root / "SteamLibrary" / "steamapps" / "common" / "ZenlessZoneZero");
  ASSERT_TRUE(steam.status.ok) << steam.status.error;
  EXPECT_EQ(steam.body["install"]["sources"], (sparxie::json::array({ "steam" })));
  touch(root / "plain" / "StarRail.exe");
  EXPECT_TRUE(sparxie::identify_path(env, root / "plain").body["install"]["sources"].empty());
  const auto catalog = sparxie::catalog_json();
  EXPECT_EQ(catalog[0]["name"], "原神");
  EXPECT_EQ(catalog[2]["nameEn"], "Zenless Zone Zero");
  EXPECT_TRUE(catalog[1]["touch"].get<bool>());
}

TEST(HostApi, AGameAppIsChangedInPlaceAndTouchLaunchGetsItsProfileThen) {
  const fs::path root = fresh("sparxie-host-update");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto created = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "Genshin" } });
  ASSERT_TRUE(created.status.ok) << created.status.error;
  EXPECT_EQ(created.body["app"]["sparxie"]["profile"], "");
  // the game asks for administrator rights in its manifest: a direct start is elevated too, else Windows refuses it (740)
  EXPECT_TRUE(created.body["app"]["elevated"].get<bool>());
  // direct -> touch: the profile is written and the command goes through Sparxie
  auto touched = sparxie::update_app(env, 0, { { "mode", "touch" }, { "name", "Genshin touch" } });
  ASSERT_TRUE(touched.status.ok) << touched.status.error;
  EXPECT_TRUE(touched.body["wrote"]);
  EXPECT_NE(touched.body["app"]["cmd"].get<std::string>().find("--profile"), std::string::npos);
  EXPECT_TRUE(touched.body["app"]["elevated"].get<bool>());
  EXPECT_EQ(touched.body["app"]["name"], "Genshin touch");
  const std::string profile = touched.body["app"]["sparxie"]["profile"];
  EXPECT_FALSE(profile.empty());
  // touch -> direct: the game's own command, the profile stays for later
  auto direct = sparxie::update_app(env, 0, { { "mode", "direct" } });
  ASSERT_TRUE(direct.status.ok);
  EXPECT_EQ(direct.body["app"]["cmd"].get<std::string>().find("--profile"), std::string::npos);
  EXPECT_EQ(direct.body["app"]["sparxie"]["profile"], profile);
  EXPECT_FALSE(direct.body["wrote"].get<bool>());
  // back to touch does not write a second profile
  auto again = sparxie::update_app(env, 0, { { "mode", "touch" } });
  EXPECT_FALSE(again.body["wrote"].get<bool>());
  EXPECT_EQ(again.body["app"]["sparxie"]["profile"], profile);
  // Sparxie's own options of the profile are written to its config
  auto options = sparxie::update_app(env, 0, { { "options", { { "fpsUnlock", true }, { "fps", 90 }, { "bgLimit", true }, { "bgFps", 10 }, { "priority", "normal" }, { "followPreset", false }, { "preset30", 60 }, { "preset60", 1000 }, { "scaleOverride", false }, { "scale", 400 } } } });
  ASSERT_TRUE(options.status.ok) << options.status.error;
  EXPECT_TRUE(options.body["wrote"].get<bool>());
  EXPECT_EQ(options.body["app"]["sparxie"]["options"]["fps"], 90);
  // settings mode and cover
  auto preset = sparxie::update_app(env, 0, { { "settings", { { "fps", 30 } } }, { "restore", "keep" }, { "cover", "custom" }, { "image-path", "C:\\p.png" }, { "hero", "C:\\h.png" } });
  ASSERT_TRUE(preset.status.ok);
  EXPECT_FALSE(preset.body["app"]["sparxie"].contains("settingsMode"));
  EXPECT_FALSE(preset.body["app"]["sparxie"]["settings"].contains("fps")) << "with the unlock on, the game's own frame-rate tier is not the application's to store";
  EXPECT_TRUE(preset.body["app"]["sparxie"]["settings"].empty());
  EXPECT_EQ(preset.body["app"]["sparxie"]["restore"], "keep");
  EXPECT_EQ(preset.body["app"]["image-path"], "C:\\p.png");
  auto unlocked = sparxie::update_app(env, 0, { { "options", { { "fpsUnlock", false }, { "fps", 90 } } } });
  ASSERT_TRUE(unlocked.status.ok);
  auto tier = sparxie::update_app(env, 0, { { "settings", { { "fps", 30 } } } });
  ASSERT_TRUE(tier.status.ok);
  EXPECT_EQ(tier.body["app"]["sparxie"]["settings"]["fps"], 30);
  EXPECT_EQ(tier.body["app"]["sparxie"]["settings"].size(), 1u);
  auto off = sparxie::update_app(env, 0, { { "settings", sparxie::json::object() }, { "cover", "official" } });
  EXPECT_TRUE(off.body["app"]["sparxie"]["settings"].empty()) << "every item back to following the game";
  EXPECT_FALSE(off.body["app"].contains("hero"));
  EXPECT_FALSE(off.body["app"].contains("image-path") && off.body["app"]["image-path"] == "C:\\p.png");
  EXPECT_EQ(sparxie::update_app(env, 5, {}).status.error_code, "not_found");
  env.process_running = [](const std::vector<std::string> &) { return true; };
  auto second = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "Genshin 2" } });
  ASSERT_TRUE(second.status.ok);
  EXPECT_EQ(sparxie::update_app(env, 1, { { "mode", "touch" } }).status.error_code, "sparxie_running") << "a running Sparxie is not written to";
  EXPECT_EQ(sparxie::catalog_json()[1]["aliases"][0], "hsr");
}

// The host keeps the application list in memory: the HTTP layer reloads it when a request rewrote apps.json (apps_writes), so a new or changed game app is there without a restart
TEST(HostApi, EveryRequestThatRewritesTheAppListIsCountedAndReadsAreNot) {
  const fs::path root = fresh("sparxie-host-reload");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  EXPECT_EQ(env.apps_writes, 0);
  sparxie::list_games(env);
  sparxie::identify_path(env, exe, "", "");
  sparxie::game_settings_for(env, "", exe, "cn");
  EXPECT_EQ(env.apps_writes, 0) << "reading changes nothing";
  auto created = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "Genshin" } });
  ASSERT_TRUE(created.status.ok) << created.status.error;
  EXPECT_EQ(env.apps_writes, 1) << "a create is one write, so one reload";
  env.apps_writes = 0;
  ASSERT_TRUE(sparxie::update_app(env, 0, { { "name", "Genshin 2" } }).status.ok);
  EXPECT_EQ(env.apps_writes, 1);
  env.apps_writes = 0;
  ASSERT_TRUE(sparxie::set_cover(env, 0, "official").status.ok);
  EXPECT_GE(env.apps_writes, 1);
  env.apps_writes = 0;
  EXPECT_FALSE(sparxie::update_app(env, 9, { { "name", "x" } }).status.ok);
  EXPECT_EQ(env.apps_writes, 0) << "a refused request leaves the file alone";
  sparxie::import_apps(env, { { "ids", sparxie::json::array() } });
  EXPECT_LE(env.apps_writes, 1) << "an import is at most one write";
}

TEST(HostApi, SparxieIsALaunchPathAndTheModeNameIsAnAliasOfTouch) {
  const fs::path root = fresh("sparxie-host-mode");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto made = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "sparxie" }, { "name", "G" }, { "options", { { "fpsUnlock", true }, { "fps", 144 } } } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  EXPECT_EQ(made.body["app"]["sparxie"]["mode"], "touch");
  EXPECT_EQ(made.body["app"]["sparxie"]["options"]["fps"], 144);
  EXPECT_TRUE(made.body["app"]["elevated"].get<bool>());
}

TEST(HostApi, AnAppMadeAfterTheRefreshGetsTheOfficialCoverOnDisk) {
  const fs::path root = fresh("sparxie-cover-attach");
  sparxie::HostEnv env = env_at(root);
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto created = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "Genshin" } });
  ASSERT_TRUE(created.status.ok) << created.status.error;
  const std::string install = created.body["app"]["sparxie"]["install"].get<std::string>();
  EXPECT_FALSE(created.body["app"].contains("image-path")) << "no cover on disk yet";
  // the refresh ran before (or another app of the same install had it): the cover is on disk, the app has no poster
  fs::create_directories(env.covers_dir);
  touch(env.covers_dir / ("sparxie-" + install + "-v1.png"));
  EXPECT_EQ(sparxie::attach_official_covers(env), 1);
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "once";
  // a new app of the same install takes it at once
  auto second = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "Genshin 2" } });
  ASSERT_TRUE(second.status.ok) << second.status.error;
  EXPECT_EQ(fs::path(second.body["app"]["image-path"].get<std::string>()).filename().string(), "sparxie-" + install + "-v1.png");
  EXPECT_TRUE(second.body["app"]["elevated"].get<bool>());
  // the landscape poster of the install is the hero (the 大图 layouts): attached to the apps that lack it, given to new ones. The old banner strip is no hero.
  fs::create_directories(env.host_data / "posters" / install);
  touch(env.host_data / "posters" / install / "banner.png");
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "a banner strip alone is not a hero";
  touch(env.host_data / "posters" / install / "hero.jpg");
  EXPECT_EQ(sparxie::attach_official_covers(env), 2);
  auto third = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "direct" }, { "name", "Genshin 3" } });
  ASSERT_TRUE(third.status.ok) << third.status.error;
  EXPECT_EQ(fs::path(third.body["app"]["hero"].get<std::string>()).filename().string(), "hero.jpg");
}

TEST(HostApi, TheCachedPicturesAreReadBackByteForByte) {
  // a cached WebP or PNG holds any byte, among them CR LF and 0x1A: read as text on Windows they are cut or changed, and the decoder then gets a broken picture (Star Rail went black from it)
  const fs::path root = fresh("sparxie-cache-bytes");
  sparxie::HostEnv env = env_at(root);
  std::string bytes = "RIFF....WEBPVP8L";
  for (int i = 0; i < 4000; ++i) {
    bytes += "\r\n";
    bytes.push_back(static_cast<char>(0x1A));
    bytes.push_back(static_cast<char>(i % 256));
  }
  const fs::path cache = env.host_data / "posters" / "starrail-cn";
  fs::create_directories(cache);
  {
    std::ofstream out(cache / "v1-background.bin", std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  std::string seen;
  auto decode = [&](std::string_view data) {
    if (data.size() > 1000) seen.assign(data);
    sparxie::Rgba image;
    image.w = 64;
    image.h = 36;
    image.px.assign(64 * 36 * 4, 200);
    for (std::size_t i = 0; i < image.px.size(); i += 8) image.px[i] = image.px[i + 1] = image.px[i + 2] = 60;
    return image;
  };
  auto offline = [](const std::string &) -> std::optional<std::string> { return std::nullopt; };
  sparxie::refresh_official(env, offline, decode);
  EXPECT_EQ(seen.size(), bytes.size());
  EXPECT_TRUE(seen == bytes);
}

TEST(HostApi, ABlankHeroIsRemovedAtHostStartAndComposedAgainByTheRefresh) {
  const fs::path root = fresh("sparxie-hero-heal");
  sparxie::HostEnv env = env_at(root);
  std::vector<std::string> warnings;
  env.warn = [&](const std::string &message) { warnings.push_back(message); };
  auto get_url = [&](const std::string &url) -> std::optional<std::string> {
    if (url.find("getGames") != std::string::npos) {
      sparxie::json body;
      for (const char *biz : { "hk4e_cn", "hkrpg_cn", "nap_cn" }) {
        sparxie::json game;
        game["biz"] = biz;
        game["display"]["background"]["url"] = std::string("https://cdn/") + biz + "_poster.webp";
        body["data"]["games"].push_back(game);
      }
      return body.dump();
    }
    return std::string("img");
  };
  auto decode = [](std::string_view) {
    sparxie::Rgba image;
    image.w = 64;
    image.h = 36;
    image.px.assign(64 * 36 * 4, 200);
    for (std::size_t i = 0; i < image.px.size(); i += 8) image.px[i] = image.px[i + 1] = image.px[i + 2] = 60;
    return image;
  };
  ASSERT_TRUE(sparxie::refresh_official(env, get_url, decode).status.ok);
  const fs::path hero = env.host_data / "posters" / "genshin-cn" / "hero.jpg";
  ASSERT_TRUE(fs::exists(hero));
  // an app of the install points at the poster; the poster on disk becomes a black picture (what a test host had)
  sparxie::json apps;
  apps["apps"] = sparxie::json::array();
  sparxie::json app;
  app["name"] = "Genshin";
  app["hero"] = hero.string();
  app["image-path"] = (root / "covers" / "c.png").string();
  app["sparxie"] = { { "install", "genshin-cn" }, { "game", "genshin" }, { "region", "cn" }, { "cover", "official" } };
  apps["apps"].push_back(app);
  fs::create_directories(env.apps_file.parent_path());
  std::ofstream(env.apps_file) << apps.dump();
  sparxie::Rgba black;
  black.w = 1920;
  black.h = 1080;
  black.px.assign(static_cast<std::size_t>(1920 * 1080 * 4), 0);
  const auto black_jpeg = sparxie::encode_jpeg(black, 90);
  {
    std::ofstream out(hero, std::ios::binary);
    out.write(reinterpret_cast<const char *>(black_jpeg.data()), static_cast<std::streamsize>(black_jpeg.size()));
  }
  // host start: the blank poster goes and the app lets go of it
  EXPECT_GE(sparxie::attach_official_covers(env), 1);
  EXPECT_FALSE(fs::exists(hero));
  {
    std::ifstream in(env.apps_file);
    sparxie::json after;
    in >> after;
    EXPECT_FALSE(after["apps"][0].contains("hero")) << "no black hero on the app";
  }
  // the refresh composes it again from the cached pictures, fetching nothing, and the app has it
  const auto again = sparxie::refresh_official(env, get_url, decode);
  EXPECT_EQ(again.body["updated"], 0);
  EXPECT_GE(again.body["heroes"].get<int>(), 1);
  ASSERT_TRUE(fs::exists(hero));
  std::ifstream bytes_in(hero, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(bytes_in)), std::istreambuf_iterator<char>());
  EXPECT_FALSE(sparxie::jpeg_is_blank(bytes));
  std::ifstream in(env.apps_file);
  sparxie::json final_apps;
  in >> final_apps;
  EXPECT_EQ(final_apps["apps"][0].value("hero", std::string()), hero.string());
  // a blank poster that is still on disk when the refresh runs (no host start in between) is composed again too
  {
    std::ofstream out(hero, std::ios::binary);
    out.write(reinterpret_cast<const char *>(black_jpeg.data()), static_cast<std::streamsize>(black_jpeg.size()));
  }
  EXPECT_GE(sparxie::refresh_official(env, get_url, decode).body["heroes"].get<int>(), 1);
  std::ifstream healed(hero, std::ios::binary);
  EXPECT_FALSE(sparxie::jpeg_is_blank(std::string((std::istreambuf_iterator<char>(healed)), std::istreambuf_iterator<char>())));
  // a decoder that returns nothing for a new version publishes nothing: the poster and the cover stay
  auto empty_decode = [](std::string_view) { return sparxie::Rgba {}; };
  auto new_version = [&](const std::string &url) -> std::optional<std::string> {
    auto body = get_url(url);
    if (body && url.find("getGames") != std::string::npos) {
      auto json = sparxie::json::parse(*body);
      for (auto &game : json["data"]["games"]) game["display"]["background"]["url"] = game["display"]["background"]["url"].get<std::string>() + "?new";
      return json.dump();
    }
    return body;
  };
  warnings.clear();
  fs::remove(env.host_data / "posters" / "genshin-cn" / "index.json");
  const auto refused = sparxie::refresh_official(env, new_version, empty_decode);
  EXPECT_EQ(refused.body["updated"], 0);
  EXPECT_FALSE(warnings.empty());
  EXPECT_TRUE(fs::exists(hero));
}

TEST(HostApi, AnAppWithTheOldBannerStripGetsTheLandscapePosterAndACustomHeroStays) {
  const fs::path root = fresh("sparxie-hero-migrate");
  sparxie::HostEnv env = env_at(root);
  const fs::path posters = env.host_data / "posters" / "zzz-cn";
  touch(posters / "banner.png");
  touch(posters / "hero.jpg");
  const fs::path custom_image = root / "mine" / "banner.png";
  touch(custom_image);
  sparxie::json apps;
  apps["apps"] = sparxie::json::array();
  const auto add = [&](const std::string &name, const std::string &cover, const sparxie::json &hero) {
    sparxie::json app;
    app["name"] = name;
    app["image-path"] = (root / "covers" / "c.png").string();
    if (!hero.is_null()) app["hero"] = hero;
    app["sparxie"] = { { "install", "zzz-cn" }, { "game", "zzz" }, { "region", "cn" }, { "cover", cover } };
    apps["apps"].push_back(app);
  };
  add("old strip", "official", (posters / "banner.png").string());
  add("first-round png", "official", (posters / "hero.png").string());
  add("own hero named like the strip", "official", custom_image.string());
  add("own hero address", "official", "https://example.test/hero.jpg");
  add("custom cover keeps its old hero", "custom", (posters / "banner.png").string());
  touch(posters / "hero.png");
  fs::create_directories(env.apps_file.parent_path());
  std::ofstream(env.apps_file) << apps.dump();
  EXPECT_EQ(sparxie::attach_official_covers(env), 2);
  std::ifstream in(env.apps_file);
  sparxie::json after;
  in >> after;
  EXPECT_EQ(after["apps"][0]["hero"], (posters / "hero.jpg").string()) << "the strip is replaced";
  EXPECT_EQ(after["apps"][1]["hero"], (posters / "hero.jpg").string()) << "so is the PNG of the first landscape round";
  EXPECT_EQ(after["apps"][2]["hero"], custom_image.string()) << "an image of the person's own is kept, whatever its file name";
  EXPECT_EQ(after["apps"][3]["hero"], "https://example.test/hero.jpg");
  EXPECT_EQ(after["apps"][4]["hero"], (posters / "banner.png").string()) << "a custom cover is not touched at all";
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "once";
}

TEST(HostApi, OptionsCarryInputAndTheDisplayTierResolvesThroughTheHost) {
  const fs::path root = fresh("sparxie-host-options");
  sparxie::HostEnv env = env_at(root);
  env.display_refresh = [] { return 165; };
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  auto made = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "touch" }, { "name", "G" }, { "options", { { "input", "kbm" }, { "fpsUnlock", true }, { "fps", "display" } } } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  EXPECT_EQ(made.body["app"]["sparxie"]["options"]["input"], "kbm");
  EXPECT_EQ(made.body["app"]["sparxie"]["options"]["fps"], "display");
  const auto store = sparxie::read_store(env.data_dir);
  ASSERT_EQ(store.state, "data");
  // input is the application's own setting: Sparxie's profile file has no field of that name, it says touchUiEnabled=false for kbm
  EXPECT_FALSE(store.document["profiles"][0]["hoyo"].contains("input"));
  EXPECT_EQ(store.document["profiles"][0]["hoyo"]["touchUiEnabled"], false);
  EXPECT_EQ(store.document["profiles"][0]["hoyo"]["targetFps"], 165) << "the display tier resolves to the streamed display's rate";
  // a new application starts the way Sparxie's own editor starts: the unlock on, 120 as the target
  auto plain = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "touch" }, { "name", "G2" } });
  ASSERT_TRUE(plain.status.ok) << plain.status.error;
  EXPECT_EQ(plain.body["app"]["sparxie"]["options"]["fpsUnlock"], true);
  EXPECT_EQ(plain.body["app"]["sparxie"]["options"]["fps"], 120);
  EXPECT_EQ(plain.body["app"]["sparxie"]["options"]["input"], "touch");
  auto bad = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "touch" }, { "name", "G3" }, { "options", { { "input", "gamepad" } } } });
  EXPECT_EQ(bad.status.error_code, "invalid_input");
}

TEST(HostApi, LaunchResolvesTheDisplayTierAgainAndNeverWritesTheGamesOwnFpsWhileUnlocked) {
  const fs::path root = fresh("sparxie-host-unlock");
  sparxie::HostEnv env = env_at(root);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  int refresh = 60;
  env.display_refresh = [&refresh] { return refresh; };
  touch(root / "game" / "YuanShen.exe");
  const std::string exe = (root / "game" / "YuanShen.exe").string();
  const std::string key = genshin_root();
  const std::string perf = R"({"saveItems":[{"entryType":1,"index":1,"itemVersion":"CNRELWin5.0.0"}],"portedVersion":"CNRELWin5.0.0"})";
  sparxie::json general = { { "enableHDR", false }, { "globalPerfData", perf } };
  const std::string dumped = general.dump();
  reg.set_binary(key, "GENERAL_DATA_h2389025596", std::vector<std::uint8_t>(dumped.begin(), dumped.end()));
  auto made = sparxie::create_app(env, { { "path", exe }, { "region", "cn" }, { "mode", "touch" }, { "name", "G" }, { "options", { { "fpsUnlock", true }, { "fps", "display" } } }, { "display", { { "mode", "fixed" }, { "resolution", "1280x720" } } }, { "settings", { { "fps", 30 } } } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  EXPECT_FALSE(made.body["app"]["sparxie"]["settings"].contains("fps")) << "the game's own tier is not the application's to store";
  const std::string profile = made.body["app"]["sparxie"]["profile"];
  auto first = sparxie::read_store(env.data_dir);
  EXPECT_EQ(first.document["profiles"][0]["hoyo"]["targetFps"], 60);
  refresh = 165;
  EXPECT_TRUE(sparxie::prepare_launch(env, "G", made.body["app"]["cmd"]).ok);
  auto second = sparxie::read_store(env.data_dir);
  for (const auto &row : second.document["profiles"])
    if (row.value("id", std::string()) == profile) EXPECT_EQ(row["hoyo"]["targetFps"], 165) << "the display tier is resolved again at launch";
  EXPECT_EQ(reg.get(key, "Screenmanager Resolution Width_h182942802").dword, 1280u) << "the other preset values are written";
  const auto blob = reg.get(key, "GENERAL_DATA_h2389025596");
  const auto after = sparxie::json::parse(std::string(blob.bytes.begin(), blob.bytes.end()));
  const auto saved_perf = sparxie::json::parse(after["globalPerfData"].get<std::string>());
  int game_fps = -1;
  for (const auto &item : saved_perf["saveItems"])
    if (item.value("entryType", -1) == 1) game_fps = item.value("index", -1);
  EXPECT_EQ(game_fps, 1) << "the game's own 60 fps tier is untouched while Sparxie owns the frame rate";
  // a settings request that carries fps does not write it either
  auto posted = sparxie::write_game_settings_for(env, { { "app", 0 }, { "values", { { "fps", 30 }, { "display", "windowed" } } } });
  ASSERT_TRUE(posted.status.ok) << posted.status.error;
  EXPECT_EQ(std::find(posted.body["applied"].begin(), posted.body["applied"].end(), sparxie::json("fps")), posted.body["applied"].end());
  EXPECT_EQ(reg.get(key, "Screenmanager Is Fullscreen mode_h3981298716").dword, 0u);
}

TEST(HostApi, AnOldTouchAppGetsTheLauncherCommandOnceTheHostShipsIt) {
  const fs::path root = fresh("sparxie-old-touch");
  sparxie::HostEnv env = env_at(root);
  env.install_root = root / "install";
  fs::create_directories(env.install_root / "tools");
  {
    nlohmann::json block = { { "mode", "touch" }, { "profile", "starrail-cn-1" }, { "install", "starrail-cn" }, { "game", "starrail" }, { "region", "cn" }, { "exe", "C:\\g\\StarRail.exe" }, { "dir", "C:\\g" }, { "launchTemplate", "\"{sparxie}\" --profile {profile}" }, { "cover", "custom" } };
    nlohmann::json apps = { { "apps", nlohmann::json::array({ { { "name", "星铁触屏" }, { "cmd", "\"C:\\x\\Sparxie.exe\" --profile starrail-cn-1" }, { "sparxie", block } } }) } };
    std::ofstream(env.apps_file) << apps.dump();
  }
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "no launcher shipped: the command stays";
  touch(env.install_root / "tools" / "sparxie-launch.exe");
  EXPECT_EQ(sparxie::attach_official_covers(env), 1);
  std::ifstream in(env.apps_file);
  const auto apps = nlohmann::json::parse(in);
  const std::string cmd = apps["apps"][0]["cmd"].get<std::string>();
  EXPECT_NE(cmd.find("sparxie-launch.exe"), std::string::npos) << cmd;
  EXPECT_NE(cmd.find("--profile starrail-cn-1"), std::string::npos) << cmd;
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "once";
}
