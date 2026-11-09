#include "src/sparxie/host.h"
#include "src/sparxie/region.h"
#include "src/sparxie/settings.h"
#include "src/sparxie/storefront.h"
#include "src/sparxie/vendors.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <tuple>
namespace fs = std::filesystem;
namespace {
  void
  touch_file(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "x";
  }
  void
  write_text(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
  }
  std::string
  key(const char *vendor, const char *ver, const char *biz) {
    const char sep = '\\';
    return std::string("Software") + sep + vendor + sep + "HYP" + sep + ver + sep + biz;
  }
  std::string
  nl() { return std::string(1, char(10)); }
  // A Windows path inside hand-written JSON: every backslash is escaped.
  std::string
  json_path(const fs::path &path) {
    std::string out;
    for (const char c : path.string()) {
      out.push_back(c);
      if (c == char(92)) out.push_back(c);
    }
    return out;
  }
  // A Wuthering Waves tree: the executable sits three levels below the root
  // and the launcher channel record (launcherDownloadConfig.json, appId) or
  // the SDK tree lies in the root.
  fs::path
  wuwa_root(const fs::path &base, const char *name, const char *app_id) {
    const fs::path root = base / name;
    touch_file(root / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe");
    if (app_id) write_text(root / "launcherDownloadConfig.json", "{\"appId\": \"" + std::string(app_id) + "\"}");
    return root;
  }
  fs::path
  wuwa_sdk_root(const fs::path &base, const char *name, const char *sdk_dir, bool bilibili_marker) {
    const fs::path root = base / name;
    touch_file(root / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe");
    const fs::path sdk = root / "Client" / "Binaries" / "Win64" / "ThirdParty" / sdk_dir;
    touch_file(sdk / "KrPcSdk.dll");
    if (bilibili_marker) touch_file(sdk / "KRSDKRes" / "Bilibili" / "marker");
    return root;
  }
  fs::path
  nte_root(const fs::path &base, const char *name, const char *branch) {
    const fs::path root = base / name;
    touch_file(root / "NTEGame.exe");
    touch_file(root / "Client" / "WindowsNoEditor" / "HT" / "Binaries" / "Win64" / "HTGame.exe");
    std::string text = "[UPDATE_CONFIG]" + nl();
    if (branch) text += std::string("Branch=") + branch + nl();
    write_text(root / "NTELauncher" / "Config" / "Config.ini", text);
    return root;
  }
  fs::path
  endfield_root(const fs::path &base, const char *name, const char *sdk_config, bool bilibili_dll) {
    const fs::path root = base / name;
    touch_file(root / "Endfield.exe");
    touch_file(root / "config.ini");
    if (sdk_config) touch_file(root / "sdkdata" / sdk_config);
    if (bilibili_dll) touch_file(root / "PCGameSDK.dll");
    return root;
  }
  std::string
  text_of(const sparxie::RegValue &value) {
    std::string text;
    if (value.kind == sparxie::RegValue::Kind::text)
      text = value.text;
    else if (value.kind == sparxie::RegValue::Kind::binary)
      text.assign(value.bytes.begin(), value.bytes.end());
    while (!text.empty() && text.back() == 0) text.pop_back();
    return text;
  }
  sparxie::HostEnv
  env_at(const fs::path &root) {
    sparxie::HostEnv env;
    env.host_data = root / "host";
    env.data_dir = root / "sparxie-data";
    env.apps_file = root / "apps.json";
    env.covers_dir = root / "covers";
    env.install_root = root / "install";
    return env;
  }
  std::string
  vdf(const std::string &root, const std::vector<std::string> &ids) {
    const std::string q = std::string(1, char(34));
    const std::string tab = std::string(1, char(9));
    std::string text = q + "libraryfolders" + q + nl() + "{" + nl();
    text += tab + q + "0" + q + nl() + tab + "{" + nl();
    text += tab + tab + q + "path" + q + tab + tab + q + root + q + nl();
    text += tab + tab + q + "apps" + q + nl() + tab + tab + "{" + nl();
    for (const std::string &id : ids) text += tab + tab + tab + q + id + q + tab + tab + q + "1" + q + nl();
    text += tab + tab + "}" + nl() + tab + "}" + nl() + "}" + nl();
    return text;
  }
  std::string
  acf(const char *dir) {
    const std::string q = std::string(1, char(34));
    const std::string tab = std::string(1, char(9));
    return q + "AppState" + q + nl() + "{" + nl() + tab + q + "installdir" + q + tab + q + dir + q + nl() + "}" + nl();
  }
}  // namespace

TEST(Vendor, HonkaiImpact3rdRegistryAndBiz) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-hi3";
  fs::remove_all(base);
  fs::path cn = base / "bh3-cn";
  touch_file(cn / "BH3.exe");
  touch_file(cn / "BH3_Data" / "marker");
  fs::path gl = base / "bh3-global";
  touch_file(gl / "BH3.exe");
  touch_file(gl / "BH3_Data" / "marker");
  sparxie::MemoryRegistry reg;
  reg.set_text(key("miHoYo", "1_1", "bh3_cn"), "GameInstallPath", cn.string());
  reg.set_text(key("Cognosphere", "1_0", "bh3_global"), "GameInstallPath", gl.string());
  const auto cn_found = sparxie::identify_install(cn, reg);
  ASSERT_TRUE(cn_found.status.ok) << cn_found.status.error;
  EXPECT_EQ(cn_found.install.game, "hi3");
  EXPECT_EQ(cn_found.install.region, "cn");
  EXPECT_EQ(cn_found.install.source, "hoyoplay");
  const auto gl_found = sparxie::identify_install(gl, reg);
  EXPECT_EQ(gl_found.install.region, "global");
  EXPECT_EQ(gl_found.install.source, "hoyoplay");
  // Both clients write channel 1/1, so the channel pair leaves the region
  // open; only game_biz narrows it.
  fs::path open = base / "bh3-open";
  touch_file(open / "BH3.exe");
  write_text(open / "config.ini", "[General]" + nl() + "channel=1" + nl() + "sub_channel=1" + nl() + "game_biz=bh3_cn" + nl());
  const auto by_biz = sparxie::identify_install(open, reg);
  ASSERT_TRUE(by_biz.status.ok);
  EXPECT_EQ(by_biz.install.region, "cn");
  fs::path channel_only = base / "bh3-channel";
  touch_file(channel_only / "BH3.exe");
  write_text(channel_only / "config.ini", "[General]" + nl() + "channel=1" + nl() + "sub_channel=1" + nl());
  const auto open_found = sparxie::identify_install(channel_only, reg);
  ASSERT_TRUE(open_found.status.ok);
  EXPECT_TRUE(open_found.install.region.empty());
  EXPECT_EQ(open_found.install.candidates.size(), 2u);
}

TEST(Vendor, WutheringWavesLauncherRecordAndSdkTree) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-wuwa";
  fs::remove_all(base);
  sparxie::MemoryRegistry reg;
  const fs::path cn = wuwa_root(base, "wuwa-cn", "10003");
  const auto cn_found = sparxie::identify_install(cn / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg);
  ASSERT_TRUE(cn_found.status.ok) << cn_found.status.error;
  EXPECT_EQ(cn_found.install.game, "wuwa");
  EXPECT_EQ(cn_found.install.region, "cn");
  EXPECT_EQ(cn_found.install.source, "official-launcher");
  EXPECT_TRUE(sparxie::paths_equal(cn_found.install.dir, cn));
  EXPECT_EQ(cn_found.install.sources, (std::vector<std::string> { "official-launcher" }));
  const fs::path bili = wuwa_root(base, "wuwa-bili", "10004");
  EXPECT_EQ(sparxie::identify_install(bili / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg).install.region, "bili");
  const fs::path global = wuwa_root(base, "wuwa-global", "50004");
  EXPECT_EQ(sparxie::identify_install(global / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg).install.region, "global");
  const fs::path sdk_cn = wuwa_sdk_root(base, "wuwa-sdk-cn", "KrPcSdk_Mainland", false);
  const fs::path wuwa_exe = sdk_cn / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe";
  EXPECT_EQ(sparxie::identify_install(wuwa_exe, reg).install.region, "cn");
  const fs::path sdk_bili = wuwa_sdk_root(base, "wuwa-sdk-bili", "KrPcSdk_Mainland", true);
  EXPECT_EQ(sparxie::identify_install(sdk_bili / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg).install.region, "bili");
  const fs::path sdk_global = wuwa_sdk_root(base, "wuwa-sdk-global", "KrPcSdk_Global", false);
  EXPECT_EQ(sparxie::identify_install(sdk_global / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg).install.region, "global");
}

TEST(Vendor, NteLauncherConfigNamesTheChannel) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-nte";
  fs::remove_all(base);
  sparxie::MemoryRegistry reg;
  const fs::path cn = nte_root(base, "nte-cn", "publish_ob");
  const auto cn_found = sparxie::identify_install(cn, reg);
  ASSERT_TRUE(cn_found.status.ok) << cn_found.status.error;
  EXPECT_EQ(cn_found.install.game, "nte");
  EXPECT_EQ(cn_found.install.region, "cn");
  EXPECT_EQ(cn_found.install.source, "official-launcher");
  const fs::path bili = nte_root(base, "nte-bili", "publish_ob_B");
  const auto bili_found = sparxie::identify_install(bili, reg);
  EXPECT_EQ(bili_found.install.region, "bili");
  // The engine build five levels down belongs to the same install.
  const auto deep = sparxie::identify_install(cn / "Client" / "WindowsNoEditor" / "HT" / "Binaries" / "Win64" / "HTGame.exe", reg);
  ASSERT_TRUE(deep.status.ok);
  EXPECT_TRUE(sparxie::paths_equal(deep.install.dir, cn));
  EXPECT_EQ(deep.install.region, "cn");
  const fs::path global = base / "nte-global";
  touch_file(global / "NTEGlobalGame.exe");
  touch_file(global / "NTEGlobal" / "Config" / "Config.ini");
  const auto global_found = sparxie::identify_install(global, reg);
  ASSERT_TRUE(global_found.status.ok);
  EXPECT_EQ(global_found.install.region, "global");
  EXPECT_EQ(global_found.install.source, "official-launcher");
}

TEST(Vendor, EndfieldSdkdataAndArknightsYostarRecord) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-hg";
  fs::remove_all(base);
  sparxie::MemoryRegistry reg;
  const fs::path cn = endfield_root(base, "endfield-cn", "sdk_hgsdk_config.ini", false);
  const auto cn_found = sparxie::identify_install(cn, reg);
  ASSERT_TRUE(cn_found.status.ok) << cn_found.status.error;
  EXPECT_EQ(cn_found.install.game, "endfield");
  EXPECT_EQ(cn_found.install.region, "cn");
  EXPECT_EQ(cn_found.install.source, "official-launcher");
  const fs::path global = endfield_root(base, "endfield-global", "sdk_glsdk_config.ini", false);
  EXPECT_EQ(sparxie::identify_install(global, reg).install.region, "global");
  const fs::path bili = endfield_root(base, "endfield-bili", nullptr, true);
  EXPECT_EQ(sparxie::identify_install(bili, reg).install.region, "bili");
  const fs::path yostar = base / "arknights-en";
  touch_file(yostar / "Arknights.exe");
  write_text(yostar / "game-launcher-config.json", "{\"tag\": \"Arknights_EN\"}");
  const auto global_ark = sparxie::identify_install(yostar, reg);
  ASSERT_TRUE(global_ark.status.ok);
  EXPECT_EQ(global_ark.install.game, "arknights");
  EXPECT_EQ(global_ark.install.region, "global");
  EXPECT_EQ(global_ark.install.source, "official-launcher");
  const fs::path cn_ark = base / "arknights-cn";
  touch_file(cn_ark / "Arknights.exe");
  const auto open_ark = sparxie::identify_install(cn_ark, reg);
  ASSERT_TRUE(open_ark.status.ok);
  EXPECT_TRUE(open_ark.install.region.empty());
  EXPECT_EQ(open_ark.install.candidates, (std::vector<std::string> { "cn", "global" }));
}

TEST(Vendor, EndfieldDirectLaunchUsesOfficialLauncherForEveryOfficialChannel) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-endfield-launch";
  fs::remove_all(base);
  sparxie::HostEnv env = env_at(base);
  env.process_running = [](const std::vector<std::string> &) { return false; };
  const fs::path launcher = base / "Hypergryph Launcher" / "1.0.0" / "Launcher.exe";
  touch_file(launcher);
  env.hypergryph_roots = { launcher.parent_path().parent_path() };
  const std::vector<std::tuple<const char *, const char *, bool>> channels = {
    { "cn", "sdk_hgsdk_config.ini", false },
    { "bili", nullptr, true },
    { "global", "sdk_glsdk_config.ini", false },
  };
  for (const auto &[region, sdk_config, bili] : channels) {
    const std::string name = std::string("endfield-") + region;
    const fs::path game = endfield_root(base, name.c_str(), sdk_config, bili);
    const auto result = sparxie::create_app(env, { { "path", game.string() }, { "mode", "pc" }, { "name", "Endfield" } });
    ASSERT_TRUE(result.status.ok) << result.status.error;
    EXPECT_EQ(result.body["app"]["cmd"], "\"" + launcher.string() + "\"");
  }
}

// Endfield's direct start is the Collapse Launcher plugin's method: the launcher shipped with the host creates the game under a suspended Games.exe parent.
TEST(Vendor, EndfieldDirectLaunchIsTheDecoyParentMethodThroughTheShippedLauncher) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-endfield-decoy";
  fs::remove_all(base);
  sparxie::HostEnv env = env_at(base);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.process_running = [](const std::vector<std::string> &) { return false; };
  const fs::path shipped = env.install_root / "tools" / "sparxie-launch.exe";
  touch_file(shipped);
  // the official launcher is installed as well: the decoy-parent method still wins for an Endfield launcher build
  const fs::path official = base / "Hypergryph Launcher" / "1.0.0" / "Launcher.exe";
  touch_file(official);
  env.hypergryph_roots = { official.parent_path().parent_path() };
  const std::vector<std::tuple<const char *, const char *, bool>> channels = {
    { "cn", "sdk_hgsdk_config.ini", false },
    { "bili", nullptr, true },
    { "global", "sdk_glsdk_config.ini", false },
  };
  for (const auto &[region, sdk_config, bili] : channels) {
    const std::string name = std::string("endfield-") + region;
    const fs::path game = endfield_root(base, name.c_str(), sdk_config, bili);
    const auto result = sparxie::create_app(env, { { "path", game.string() }, { "mode", "pc" }, { "name", "Endfield" } });
    ASSERT_TRUE(result.status.ok) << result.status.error;
    const std::string cmd = result.body["app"]["cmd"].get<std::string>();
    EXPECT_EQ(cmd, "\"" + shipped.string() + "\" --endfield-direct \"" + (game / "Endfield.exe").string() + "\"") << region;
    EXPECT_EQ(cmd.find("Launcher.exe"), std::string::npos) << region;
    EXPECT_TRUE(result.body["app"]["elevated"].get<bool>()) << region;  // the game's manifest asks for administrator rights (740 otherwise)
  }
  // the launcher beside the host executable (no tools folder) is found as well
  fs::remove(shipped);
  const fs::path beside = env.install_root / "sparxie-launch.exe";
  touch_file(beside);
  const fs::path again = endfield_root(base, "endfield-again", "sdk_hgsdk_config.ini", false);
  const auto beside_result = sparxie::create_app(env, { { "path", again.string() }, { "mode", "pc" }, { "name", "Endfield 2" } });
  ASSERT_TRUE(beside_result.status.ok) << beside_result.status.error;
  EXPECT_EQ(beside_result.body["app"]["cmd"], "\"" + beside.string() + "\" --endfield-direct \"" + (again / "Endfield.exe").string() + "\"");
  // without a shipped launcher the previous behaviour stays: the official launcher
  fs::remove(beside);
  const fs::path third = endfield_root(base, "endfield-third", "sdk_hgsdk_config.ini", false);
  const auto none = sparxie::create_app(env, { { "path", third.string() }, { "mode", "pc" }, { "name", "Endfield 3" } });
  ASSERT_TRUE(none.status.ok) << none.status.error;
  EXPECT_EQ(none.body["app"]["cmd"], "\"" + official.string() + "\"");
  // Arknights is not Endfield: it keeps starting through the official launcher even when the launcher is shipped
  touch_file(shipped);
  const fs::path ark = base / "arknights";
  touch_file(ark / "Arknights.exe");
  write_text(ark / "game-launcher-config.json", "{\"tag\": \"Arknights_EN\"}");
  const auto ark_result = sparxie::create_app(env, { { "path", ark.string() }, { "mode", "pc" }, { "name", "AK" } });
  ASSERT_TRUE(ark_result.status.ok) << ark_result.status.error;
  EXPECT_EQ(ark_result.body["app"]["cmd"], "\"" + official.string() + "\"");
  // a Steam build starts through Steam, as before
  const fs::path steam_root = base / "steam";
  const fs::path steam_game = steam_root / "steamapps" / "common" / "Endfield";
  touch_file(steam_game / "Endfield.exe");
  write_text(steam_root / "steamapps" / "libraryfolders.vdf", vdf(steam_root.string(), { "4732690" }));
  write_text(steam_root / "steamapps" / "appmanifest_4732690.acf", acf("Endfield"));
  env.steam_root = steam_root;
  const auto steam_result = sparxie::create_app(env, { { "path", steam_game.string() }, { "mode", "pc" }, { "name", "Endfield Steam" } });
  ASSERT_TRUE(steam_result.status.ok) << steam_result.status.error;
  EXPECT_EQ(steam_result.body["app"]["cmd"], "steam://rungameid/4732690");
}

TEST(Vendor, PgrStaysOpenWithoutARecord) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-pgr";
  fs::remove_all(base);
  sparxie::MemoryRegistry reg;
  const fs::path game = base / "pgr";
  touch_file(game / "PGR.exe");
  const auto found = sparxie::identify_install(game, reg);
  ASSERT_TRUE(found.status.ok) << found.status.error;
  EXPECT_EQ(found.install.game, "pgr");
  EXPECT_TRUE(found.install.region.empty());
  EXPECT_EQ(found.install.candidates, (std::vector<std::string> { "cn", "global" }));
}

TEST(Vendor, SteamIdsOfEveryStoreBuild) {
  EXPECT_EQ(sparxie::steam_app_id("zzz"), 4162040);
  EXPECT_EQ(sparxie::steam_app_id("hi3"), 1671200);
  EXPECT_EQ(sparxie::steam_app_id("wuwa"), 3513350);
  EXPECT_EQ(sparxie::steam_app_id("pgr"), 4125930);
  EXPECT_EQ(sparxie::steam_app_id("endfield"), 4732690);
  EXPECT_EQ(sparxie::steam_app_id("nte"), 4508340);
  EXPECT_EQ(sparxie::steam_app_id("genshin"), 0);
}

TEST(Vendor, SteamFindsTheNestedExeOfWutheringWavesAndNte) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-steam";
  fs::remove_all(base);
  const fs::path root = base / "steam";
  fs::path wuwa = root / "steamapps" / "common" / "Wuthering Waves";
  touch_file(wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe");
  fs::path nte = root / "steamapps" / "common" / "NTE";
  touch_file(nte / "NTEGlobalGame.exe");
  write_text(root / "steamapps" / "libraryfolders.vdf", vdf(root.string(), { "3513350", "4508340" }));
  write_text(root / "steamapps" / "appmanifest_3513350.acf", acf("Wuthering Waves"));
  write_text(root / "steamapps" / "appmanifest_4508340.acf", acf("NTE"));
  sparxie::StorefrontEnv env;
  env.steam_root = root;
  const auto found = sparxie::find_storefront_installs(env);
  ASSERT_EQ(found.size(), 2u);
  for (const auto &install : found) {
    EXPECT_EQ(install.source, "steam");
    EXPECT_EQ(install.region, "global");
    if (install.game == "wuwa") {
      EXPECT_EQ(install.exe, "Client-Win64-Shipping.exe");
      EXPECT_TRUE(sparxie::paths_equal(install.dir, wuwa));
    }
    else {
      EXPECT_EQ(install.game, "nte");
      EXPECT_EQ(install.exe, "NTEGlobalGame.exe");
    }
  }
  sparxie::MemoryRegistry reg;
  const auto identified = sparxie::identify_install(wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg, env, "manual");
  ASSERT_TRUE(identified.status.ok) << identified.status.error;
  EXPECT_EQ(identified.install.region, "global");
  EXPECT_EQ(identified.install.source, "steam");
}

TEST(Vendor, EpicManifestCarriesTheAppNameForTheLaunchLink) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-epic";
  fs::remove_all(base);
  const fs::path manifests = base / "manifests";
  const fs::path wuwa = base / "epic" / "WutheringWaves";
  touch_file(wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe");
  write_text(manifests / "wuwa.item", "{\"AppName\": \"e885327ce4414509bff4c10757f88334\", \"InstallLocation\": \"" + json_path(wuwa) + "\"}");
  sparxie::StorefrontEnv env;
  env.epic_manifests = manifests;
  const auto records = sparxie::epic_records(manifests);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].app, "e885327ce4414509bff4c10757f88334");
  const auto found = sparxie::find_storefront_installs(env);
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found[0].game, "wuwa");
  EXPECT_EQ(found[0].app, "e885327ce4414509bff4c10757f88334");
  sparxie::MemoryRegistry reg;
  const auto identified = sparxie::identify_install(wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe", reg, env, "manual");
  ASSERT_TRUE(identified.status.ok) << identified.status.error;
  EXPECT_EQ(identified.install.source, "epic");
  EXPECT_EQ(identified.install.epic_app, "e885327ce4414509bff4c10757f88334");
  const sparxie::json body = sparxie::install_json(identified.install);
  EXPECT_EQ(body["epicApp"], "e885327ce4414509bff4c10757f88334");
}

TEST(Vendor, DirectLaunchUsesTheStoresOwnLinksAndTheHypergryphLauncher) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-launch";
  fs::remove_all(base);
  sparxie::HostEnv env = env_at(base);
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.process_running = [](const std::vector<std::string> &) { return false; };
  // A Steam install of NTE starts through the store own link.
  const fs::path steam_root = base / "steam";
  const fs::path nte = steam_root / "steamapps" / "common" / "NTE";
  touch_file(nte / "NTEGlobalGame.exe");
  write_text(steam_root / "steamapps" / "libraryfolders.vdf", vdf(steam_root.string(), { "4508340" }));
  write_text(steam_root / "steamapps" / "appmanifest_4508340.acf", acf("NTE"));
  env.steam_root = steam_root;
  auto steam_app = sparxie::create_app(env, { { "path", nte.string() }, { "mode", "pc" }, { "name", "NTE" } });
  ASSERT_TRUE(steam_app.status.ok) << steam_app.status.error;
  EXPECT_EQ(steam_app.body["app"]["cmd"], "steam://rungameid/4508340");
  // An Epic install starts through the store own link with its AppName.
  const fs::path manifests = base / "manifests";
  const fs::path wuwa = base / "epic" / "WutheringWaves";
  touch_file(wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe");
  write_text(manifests / "wuwa.item", "{\"AppName\": \"e885327ce4414509bff4c10757f88334\", \"InstallLocation\": \"" + json_path(wuwa) + "\"}");
  env.epic_manifests = manifests;
  auto epic_app = sparxie::create_app(env, { { "path", (wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe").string() }, { "mode", "pc" }, { "name", "wuwa" } });
  ASSERT_TRUE(epic_app.status.ok) << epic_app.status.error;
  EXPECT_EQ(epic_app.body["app"]["cmd"], "com.epicgames.launcher://apps/e885327ce4414509bff4c10757f88334?action=launch&silent=true");
  // A launcher build of Arknights starts through the official launcher,
  // because the game checks its parent process.
  const fs::path launcher_dir = base / "pf" / "Hypergryph Launcher" / "1.6.0";
  touch_file(launcher_dir / "Launcher.exe");
  env.hypergryph_roots = { base / "pf" / "Hypergryph Launcher" };
  const fs::path ark = base / "arknights";
  touch_file(ark / "Arknights.exe");
  write_text(ark / "game-launcher-config.json", "{\"tag\": \"Arknights_EN\"}");
  auto launcher_app = sparxie::create_app(env, { { "path", ark.string() }, { "mode", "pc" }, { "name", "AK" } });
  ASSERT_TRUE(launcher_app.status.ok) << launcher_app.status.error;
  EXPECT_EQ(launcher_app.body["app"]["cmd"], "\"" + (launcher_dir / "Launcher.exe").string() + "\"");
  // Without the launcher installed the executable itself is the command.
  env.hypergryph_roots.clear();
  const fs::path ark2 = base / "arknights-2";
  touch_file(ark2 / "Arknights.exe");
  auto plain_app = sparxie::create_app(env, { { "path", ark2.string() }, { "region", "cn" }, { "mode", "pc" }, { "name", "AK2" } });
  ASSERT_TRUE(plain_app.status.ok) << plain_app.status.error;
  EXPECT_NE(plain_app.body["app"]["cmd"].get<std::string>().find("Arknights.exe"), std::string::npos);
  env.epic_manifests.clear();
  auto configured = sparxie::create_app(env, { { "path", (wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe").string() }, { "region", "global" }, { "mode", "pc" }, { "name", "wuwa-configured" }, { "display", { { "mode", "fixed" }, { "resolution", "2560x1440" }, { "window", "windowed" } } }, { "settings", { { "dxVersion", "dx12" } } } });
  ASSERT_TRUE(configured.status.ok) << configured.status.error;
  const std::string launch = configured.body["app"]["cmd"].get<std::string>();
  // the size and the window mode are the app's display choice: a fixed one is in the command
  EXPECT_NE(launch.find("-screen-width 2560"), std::string::npos);
  EXPECT_NE(launch.find("-screen-height 1440"), std::string::npos);
  EXPECT_NE(launch.find("-screen-fullscreen 0"), std::string::npos);
  EXPECT_EQ(launch.find("-popupwindow"), std::string::npos);
  EXPECT_NE(launch.find("-dx12"), std::string::npos);
  auto borderless = sparxie::create_app(env, { { "path", (wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe").string() }, { "region", "global" }, { "mode", "pc" }, { "name", "wuwa-borderless" }, { "display", { { "mode", "fixed" }, { "resolution", "1920x1080" }, { "window", "borderless" } } } });
  ASSERT_TRUE(borderless.status.ok) << borderless.status.error;
  EXPECT_NE(borderless.body["app"]["cmd"].get<std::string>().find("-popupwindow"), std::string::npos);
  // following the stream stores no size: it is resolved at launch
  auto follow = sparxie::create_app(env, { { "path", (wuwa / "Client" / "Binaries" / "Win64" / "Client-Win64-Shipping.exe").string() }, { "region", "global" }, { "mode", "pc" }, { "name", "wuwa-follow" }, { "settings", { { "launchWidth", 800 } } } });
  ASSERT_TRUE(follow.status.ok) << follow.status.error;
  EXPECT_EQ(follow.body["app"]["cmd"].get<std::string>().find("-screen-width"), std::string::npos);
}

TEST(Vendor, LauncherRecordsFindTheHypergryphGames) {
  const fs::path base = fs::temp_directory_path() / "sparxie-vendors-launcher-games";
  fs::remove_all(base);
  const fs::path cn = base / "hypergryph" / "Hypergryph Launcher";
  const fs::path global = base / "GRYPHLINK";
  touch_file(cn / "games" / "Arknights Endfield" / "Endfield.exe");
  touch_file(cn / "games" / "Arknights" / "Arknights.exe");
  write_text(cn / "games" / "Arknights Endfield" / "sdkdata" / "sdk_hgsdk_config.json", "{}");
  touch_file(global / "Games" / "Arknights Endfield" / "Endfield.exe");
  write_text(global / "Games" / "Arknights Endfield" / "sdkdata" / "sdk_glsdk_config.json", "{}");
  // The three uninstall keys answer in table order (cn, global, bili); a
  // record whose folder is gone is dropped instead of naming a channel for a
  // folder that does not exist.
  const auto launchers = sparxie::hypergryph_launchers_from_records({ cn, global, {} });
  ASSERT_EQ(launchers.size(), 2u);
  EXPECT_EQ(launchers[0].channel, "cn");
  EXPECT_EQ(launchers[1].channel, "global");
  const auto games = sparxie::hypergryph_launcher_games(launchers);
  ASSERT_EQ(games.size(), 3u);
  int cn_games = 0;
  const sparxie::LauncherGame *global_endfield = nullptr;
  for (const auto &game : games) {
    if (game.channel == "cn") ++cn_games;
    if (game.channel == "global" && game.game == "endfield") global_endfield = &game;
  }
  EXPECT_EQ(cn_games, 2);
  ASSERT_NE(global_endfield, nullptr);
  EXPECT_EQ(global_endfield->dir.filename().string(), "Arknights Endfield");

  // The library scan reports the launcher installs as official-launcher rows,
  // narrowed by each game's own channel record, and a store install of the
  // same folder keeps its store row.
  sparxie::HostEnv env = env_at(base / "host");
  sparxie::MemoryRegistry reg;
  env.registry = &reg;
  env.process_running = [](const std::vector<std::string> &) { return false; };
  env.uninstall_locations = [&](const std::vector<std::string> &keys) {
    EXPECT_EQ(keys, (std::vector<std::string> { "33a0a6296a20400d503c59ac0fd6341e", "d9f71f86325c5b9733268efda40413c8", "d957df99efbdf610162fd47c00ceaba7" }));
    return std::vector<fs::path> { cn, global, {} };
  };
  const fs::path steam_root = base / "steam";
  write_text(steam_root / "steamapps" / "libraryfolders.vdf", vdf(steam_root.string(), { "4732690" }));
  write_text(steam_root / "steamapps" / "appmanifest_4732690.acf", acf("Arknights Endfield"));
  touch_file(steam_root / "steamapps" / "common" / "Arknights Endfield" / "Endfield.exe");
  env.steam_root = steam_root;
  const auto listed = sparxie::list_games(env);
  ASSERT_TRUE(listed.status.ok) << listed.status.error;
  sparxie::json rows;
  for (const auto &row : listed.body["detected"]) rows[row["game"].get<std::string>() + "@" + row["dir"].get<std::string>()] = row;
  // cn Endfield, cn Arknights, global Endfield and the store Endfield.
  ASSERT_EQ(rows.size(), 4u);
  const sparxie::json *endfield_cn = nullptr;
  for (const auto &item : rows)
    if (item["game"] == "endfield" && item["dir"].get<std::string>().find("Hypergryph Launcher") != std::string::npos) endfield_cn = &item;
  ASSERT_NE(endfield_cn, nullptr);
  EXPECT_EQ((*endfield_cn)["source"], "official-launcher");
  EXPECT_EQ((*endfield_cn)["region"], "cn");
  for (const auto &item : rows)
    if (item["game"] == "endfield" && item["dir"].get<std::string>().find("GRYPHLINK") != std::string::npos) {
      EXPECT_EQ(item["source"], "official-launcher");
      EXPECT_EQ(item["region"], "global");
    }
  for (const auto &item : rows)
    if (item["dir"].get<std::string>().find("steamapps") != std::string::npos) EXPECT_EQ(item["source"], "steam");
}

TEST(Vendor, Hi3SettingsAreReadAndWrittenInTheRegistryBlob) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "hi3";
  install.region = "cn";
  install.id = "hi3-cn";
  const std::string root = std::string("Software") + static_cast<char>(92) + "miHoYo" + static_cast<char>(92) + "崩坏3";
  reg.set_text(root, "GENERAL_DATA_V2_ScreenSettingData_h1916288658", "{\"width\":1600,\"height\":900,\"isfullScreen\":true}");
  reg.set_text(root, "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519", "{\"TargetFrameRateForInLevel\":60,\"TargetFrameRateForOthers\":30}");
  const auto read = sparxie::read_game_settings(install, reg, {}, {});
  ASSERT_TRUE(read.status.ok);
  EXPECT_EQ(read.values.resolution, "1600×900");
  EXPECT_EQ(read.values.display, "fullscreen");
  EXPECT_EQ(read.values.fps, 60);
  EXPECT_EQ(read.unavailable, (std::vector<std::string> { "vsync", "renderScale", "preset", "hdr" }));
  sparxie::GameValues values;
  values.resolution = "2560x1440";
  values.display = "windowed";
  values.fps = 30;
  const auto written = sparxie::write_game_settings(install, reg, values, {}, {}, {}, "restore", {});
  ASSERT_TRUE(written.status.ok) << written.status.error;
  EXPECT_EQ(written.applied, (std::vector<std::string> { "resolution", "display", "fps" }));
  EXPECT_EQ(reg.get(root, "Screenmanager Resolution Width_h182942802").dword, 2560u);
  EXPECT_EQ(reg.get(root, "Screenmanager Is Fullscreen mode_h3981298716").dword, 0u);
  const auto screen = reg.get(root, "GENERAL_DATA_V2_ScreenSettingData_h1916288658");
  EXPECT_NE(text_of(screen).find("2560"), std::string::npos);
  const auto graphics = reg.get(root, "GENERAL_DATA_V2_PersonalGraphicsSettingV2_h3480068519");
  EXPECT_NE(text_of(graphics).find("\"TargetFrameRateForInLevel\":30"), std::string::npos);
  const auto back = sparxie::read_game_settings(install, reg, {}, {});
  EXPECT_EQ(back.values.resolution, "2560×1440");
  EXPECT_EQ(back.values.display, "windowed");
  EXPECT_EQ(back.values.fps, 30);
}

TEST(Vendor, TitlesWithoutASourcedSettingsWriterAnswerUnavailable) {
  sparxie::MemoryRegistry reg;
  sparxie::Install install;
  install.game = "wuwa";
  install.region = "cn";
  install.id = "wuwa-cn";
  const auto read = sparxie::read_game_settings(install, reg, {}, {});
  ASSERT_TRUE(read.status.ok);
  EXPECT_EQ(read.unavailable.size(), 57u);
  sparxie::GameValues values;
  const auto written = sparxie::write_game_settings(install, reg, values, {}, {}, {}, "restore", {});
  EXPECT_FALSE(written.status.ok);
  EXPECT_EQ(written.status.error_code, "settings_unsupported");
}
