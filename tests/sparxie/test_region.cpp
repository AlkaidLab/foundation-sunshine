#include "src/sparxie/region.h"
#include "src/sparxie/sha256.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
namespace fs = std::filesystem;
namespace {
  void
  touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "x";
  }
  void
  ini(const fs::path &dir, int channel, int sub, const char *version) {
    std::ofstream out(dir / "config.ini");
    out << "[General]" << std::endl;
    out << "channel=" << channel << std::endl;
    out << "sub_channel=" << sub << std::endl;
    out << "game_version=" << version << std::endl;
  }
  void
  ini_lines(const fs::path &dir, const std::string &lines) {
    std::ofstream out(dir / "config.ini");
    out << "[General]" << std::endl;
    out << lines << std::endl;
  }
  std::string
  key(const char *vendor, const char *ver, const char *biz) {
    const char sep = '\\';
    return std::string("Software") + sep + vendor + sep + "HYP" + sep + ver + sep + biz;
  }
  struct Fix {
    fs::path root;
    sparxie::MemoryRegistry reg;
    fs::path
    make(const char *name, const char *exe, const char *data, bool sdk) {
      fs::path dir = root / name;
      touch(dir / exe);
      touch(dir / data / "marker.txt");
      if (sdk) touch(dir / data / "Plugins" / "PCGameSDK.dll");
      return dir;
    }
  };
}  // namespace
TEST(Sha256, Abc) {
  EXPECT_EQ(sparxie::sha256::hash(std::string("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Catalog, DescribesTheSettingsAndSparxieItemsOfEachTitle) {
  const sparxie::json games = sparxie::catalog_json();
  auto keys = [](const sparxie::json &row, const char *group) {
    std::vector<std::string> out;
    const sparxie::json &list = std::string_view(group) == "settings" ? row.value("settings", sparxie::json::array()) : row.value("sparxie", sparxie::json::object()).value("fields", sparxie::json::array());
    for (const auto &field : list)
      if (field.is_object()) out.push_back(field.value("key", std::string()));
    return out;
  };
  const sparxie::json &genshin = games[0];
  const std::vector<std::string> genshin_keys = keys(genshin, "settings");
  auto in_order = [](const std::vector<std::string> &hay, std::initializer_list<const char *> needles) {
    auto it = hay.begin();
    for (const char *needle : needles) {
      it = std::find(it, hay.end(), needle);
      if (it == hay.end()) return false;
      ++it;
    }
    return true;
  };
  EXPECT_TRUE(in_order(genshin_keys, { "resolution", "display", "fps", "vsync", "preset", "renderScale", "hdr", "customArgs", "shadow", "masterVolume", "textLanguage", "launchWidth", "dxVersion" }));
  auto option_values = [](const sparxie::json &field) {
    std::vector<sparxie::json> out;
    for (const auto &option : field.value("options", sparxie::json::array())) out.push_back(option.value("value", sparxie::json()));
    return out;
  };
  for (const auto &field : genshin["settings"])
    if (field.value("key", std::string()) == "fps")
      EXPECT_EQ(option_values(field), (std::vector<sparxie::json> { 30, 45, 60 })) << "the game's own tiers include the 45 the host writes";
  const sparxie::json &starrail = games[1];
  const std::vector<std::string> rail_settings = keys(starrail, "settings");
  EXPECT_EQ(std::find(rail_settings.begin(), rail_settings.end(), "hdr"), rail_settings.end()) << "Star Rail has no HDR item";
  EXPECT_EQ(keys(starrail, "sparxie"), (std::vector<std::string> { "input", "fpsUnlock", "fps", "bgLimit", "bgFps", "priority" })) << "the Genshin-only items are not Star Rail's";
  const sparxie::json &zzz = games[2];
  EXPECT_EQ(keys(zzz, "sparxie"), (std::vector<std::string> { "input" })) << "Sparxie has no frame-rate items for Zenless Zone Zero";
  const std::vector<std::string> zzz_plain = keys(zzz, "settings");
  EXPECT_TRUE(in_order(zzz_plain, { "resolution", "display", "customArgs", "launchWidth", "dxVersion" })) << "registry items stay without the file key";
  EXPECT_EQ(std::find(zzz_plain.begin(), zzz_plain.end(), "shadow"), zzz_plain.end());
  EXPECT_EQ(std::find(zzz_plain.begin(), zzz_plain.end(), "fps"), zzz_plain.end());
  const std::vector<std::string> with_file = keys(sparxie::catalog_json(true)[2], "settings");
  EXPECT_TRUE(in_order(with_file, { "resolution", "display", "fps", "vsync", "preset", "renderScale", "hdr", "shadow", "masterVolume", "textLanguage", "launchWidth", "dxVersion" })) << "with the magic the file-backed items are there";
  for (const auto &field : genshin["sparxie"]["fields"])
    if (field.value("key", std::string()) == "scale")
      EXPECT_EQ(field.value("inputs", sparxie::json::array()), sparxie::json::array({ "touch" })) << "the touch interface scale is for touch only";
  for (const auto &field : genshin["sparxie"]["fields"]) {
    if (field.value("key", std::string()) != "fps") continue;
    const std::vector<sparxie::json> tiers = option_values(field);
    EXPECT_EQ(tiers.back(), sparxie::json("display")) << "the target tiers end with the display's own rate";
    EXPECT_NE(std::find(tiers.begin(), tiers.end(), sparxie::json(120)), tiers.end());
  }
}
TEST(Region, EveryRowOfTheTable) {
  Fix fix;
  fix.root = fs::temp_directory_path() / "sparxie-region-fixtures";
  fs::remove_all(fix.root);
  auto cn_key = [](const char *biz) { return key("miHoYo", "1_1", biz); };
  auto gl_key = [](const char *biz) { return key("Cognosphere", "1_0", biz); };
  fs::path gcn = fix.make("genshin-cn", "YuanShen.exe", "YuanShen_Data", false);
  fix.reg.set_text(cn_key("hk4e_cn"), "GameInstallPath", gcn.string());
  fs::path ggl = fix.make("genshin-global", "GenshinImpact.exe", "GenshinImpact_Data", false);
  fix.reg.set_text(gl_key("hk4e_global"), "GameInstallPath", ggl.string());
  fs::path gbi = fix.make("genshin-bili", "YuanShen.exe", "YuanShen_Data", true);
  fs::path scn = fix.make("starrail-cn", "StarRail.exe", "StarRail_Data", false);
  fix.reg.set_text(cn_key("hkrpg_cn"), "GameInstallPath", scn.string());
  fs::path sgl = fix.make("starrail-global", "StarRail.exe", "StarRail_Data", false);
  fix.reg.set_text(gl_key("hkrpg_global"), "GameInstallPath", sgl.string());
  fs::path sbi = fix.make("starrail-bili", "StarRail.exe", "StarRail_Data", true);
  fs::path zcn = fix.make("zzz-cn", "ZenlessZoneZero.exe", "ZenlessZoneZero_Data", false);
  fix.reg.set_text(cn_key("nap_cn"), "GameInstallPath", zcn.string());
  fs::path zgl = fix.make("zzz-global", "ZenlessZoneZero.exe", "ZenlessZoneZero_Data", false);
  fix.reg.set_text(gl_key("nap_global"), "GameInstallPath", zgl.string());
  fs::path zbi = fix.make("zzz-bili", "ZenlessZoneZero.exe", "ZenlessZoneZero_Data", true);
  struct Expect {
    fs::path dir;
    const char *game;
    const char *region;
  };
  const Expect rows[] = {
    { gcn, "genshin", "cn" },
    { ggl, "genshin", "global" },
    { gbi, "genshin", "bili" },
    { scn, "starrail", "cn" },
    { sgl, "starrail", "global" },
    { sbi, "starrail", "bili" },
    { zcn, "zzz", "cn" },
    { zgl, "zzz", "global" },
    { zbi, "zzz", "bili" },
  };
  for (const auto &row : rows) {
    auto found = sparxie::identify_install(row.dir, fix.reg);
    EXPECT_TRUE(found.status.ok) << found.status.error;
    EXPECT_EQ(found.install.game, row.game);
    EXPECT_EQ(found.install.region, row.region) << row.dir;
    EXPECT_TRUE(found.has_game);
  }
}
TEST(Region, AmbiguousUnknownIniAndRegistryOrder) {
  Fix fix;
  fix.root = fs::temp_directory_path() / "sparxie-region-fixtures-2";
  fs::remove_all(fix.root);
  fs::path open = fix.make("open-ys", "YuanShen.exe", "YuanShen_Data", false);
  auto amb = sparxie::identify_install(open, fix.reg);
  EXPECT_TRUE(amb.status.ok);
  EXPECT_TRUE(amb.install.region.empty());
  ASSERT_EQ(amb.install.candidates.size(), 2u);
  fs::path none = fix.root / "notes";
  fs::create_directories(none);
  touch(none / "readme.txt");
  auto miss = sparxie::identify_install(none, fix.reg);
  EXPECT_FALSE(miss.status.ok);
  EXPECT_EQ(miss.status.error_code, "unrecognised");
  fs::path by_ini = fix.make("ys-ini", "YuanShen.exe", "YuanShen_Data", false);
  ini(by_ini, 14, 0, "5.1.0");
  auto bili = sparxie::identify_install(by_ini, fix.reg);
  EXPECT_EQ(bili.install.region, "bili");
  EXPECT_EQ(bili.install.version, "5.1.0");
  fs::path both = fix.make("ys-both", "YuanShen.exe", "YuanShen_Data", true);
  fix.reg.set_text(key("miHoYo", "1_1", "hk4e_cn"), "GameInstallPath", both.string());
  auto reg_wins = sparxie::identify_install(both, fix.reg);
  EXPECT_EQ(reg_wins.install.region, "cn");
  fs::path star = fix.make("sr-open", "StarRail.exe", "StarRail_Data", false);
  auto sr = sparxie::identify_install(star, fix.reg);
  EXPECT_TRUE(sr.install.region.empty());
  EXPECT_GE(sr.install.candidates.size(), 2u);
}

TEST(Region, GameBizFromConfigIniDecidesTheRegion) {
  Fix fix;
  fix.root = fs::temp_directory_path() / "sparxie-region-biz";
  fs::remove_all(fix.root);
  fs::path ys = fix.make("ys-bili-ini", "YuanShen.exe", "YuanShen_Data", false);
  ini_lines(ys, "game_biz=hk4e_bilibili");
  auto bili = sparxie::identify_install(ys, fix.reg);
  EXPECT_TRUE(bili.status.ok);
  EXPECT_EQ(bili.install.region, "bili");
  EXPECT_EQ(bili.install.source, "manual");
  fs::path sr = fix.make("sr-global-ini", "StarRail.exe", "StarRail_Data", false);
  ini_lines(sr, "game_biz=hkrpg_global");
  auto global = sparxie::identify_install(sr, fix.reg);
  EXPECT_EQ(global.install.region, "global");
  // A biz of another game never decides this game's region.
  fs::path zzz = fix.make("zzz-open", "ZenlessZoneZero.exe", "ZenlessZoneZero_Data", false);
  ini_lines(zzz, "game_biz=hk4e_cn");
  auto open = sparxie::identify_install(zzz, fix.reg);
  EXPECT_TRUE(open.install.region.empty());
}

TEST(Region, ChannelAndCpsAreSourcedSignals) {
  Fix fix;
  fix.root = fs::temp_directory_path() / "sparxie-region-channel";
  fs::remove_all(fix.root);
  fs::path sr = fix.make("sr-bili", "StarRail.exe", "StarRail_Data", false);
  ini_lines(sr, "channel=14\nsub_channel=0");
  auto bili = sparxie::identify_install(sr, fix.reg);
  EXPECT_EQ(bili.install.region, "bili");
  EXPECT_EQ(bili.install.channel, 14);
  EXPECT_EQ(bili.install.sub_channel, 0);
  fs::path ys = fix.make("ys-global-numbers", "YuanShen.exe", "YuanShen_Data", false);
  ini_lines(ys, "channel=1\nsub_channel=0");
  // channel 1/0 is the international pair, which this executable does not ship
  auto none = sparxie::identify_install(ys, fix.reg);
  EXPECT_TRUE(none.install.region.empty());
  fs::path rail = fix.make("sr-cps-overseas", "StarRail.exe", "StarRail_Data", false);
  ini_lines(rail, "cps=hyp_hoyoverse");
  auto overseas = sparxie::identify_install(rail, fix.reg);
  EXPECT_EQ(overseas.install.region, "global");
  EXPECT_TRUE(overseas.install.candidates.empty());
  fs::path rail2 = fix.make("sr-cps-mihoyo", "StarRail.exe", "StarRail_Data", false);
  ini_lines(rail2, "cps=hyp_mihoyo");
  auto national = sparxie::identify_install(rail2, fix.reg);
  ASSERT_EQ(national.install.candidates.size(), 2u);
  EXPECT_EQ(national.install.candidates[0], "cn");
  EXPECT_EQ(national.install.candidates[1], "bili");
}

TEST(Region, BilibiliRegistryKeyAndEnumeration) {
  Fix fix;
  fix.root = fs::temp_directory_path() / "sparxie-region-bili-key";
  fs::remove_all(fix.root);
  fs::path ys = fix.make("ys-bili", "YuanShen.exe", "YuanShen_Data", false);
  fix.reg.set_text(key("miHoYo", "1_1", "hk4e_bilibili"), "GameInstallPath", ys.string());
  auto found = sparxie::identify_install(ys, fix.reg);
  EXPECT_TRUE(found.status.ok);
  EXPECT_EQ(found.install.region, "bili");
  EXPECT_EQ(found.install.source, "bili");
  // A launcher version this host does not know is still found by enumerating.
  fs::path zzz = fix.make("zzz-cn", "ZenlessZoneZero.exe", "ZenlessZoneZero_Data", false);
  fix.reg.set_text(key("miHoYo", "1_2", "nap_cn"), "GameInstallPath", zzz.string());
  bool saw_nap_1_2 = false;
  for (const auto &hyp : sparxie::hyp_registry_keys(fix.reg))
    if (hyp.biz == "nap_cn" && hyp.key.find("1_2") != std::string::npos) saw_nap_1_2 = true;
  EXPECT_TRUE(saw_nap_1_2);
  auto zzz_found = sparxie::identify_install(zzz, fix.reg);
  EXPECT_EQ(zzz_found.install.region, "cn");
  EXPECT_EQ(zzz_found.install.source, "hoyoplay");
}

TEST(Region, DisplayNamesAndCatalog) {
  EXPECT_EQ(sparxie::region_display("cn"), "国服");
  EXPECT_EQ(sparxie::region_display("bili"), "B 服");
  EXPECT_EQ(sparxie::region_display("global"), "国际服");
  EXPECT_EQ(sparxie::region_display_en("cn"), "CN");
  EXPECT_EQ(sparxie::region_display_en("bili"), "Bilibili");
  EXPECT_EQ(sparxie::hyp_biz("zzz", "bili"), "nap_bilibili");
  const sparxie::json catalog = sparxie::catalog_json();
  ASSERT_EQ(catalog.size(), 9u);
  for (const auto &game : catalog)
    for (const auto &region : game["regions"]) {
      EXPECT_TRUE(region.contains("name"));
      EXPECT_TRUE(region.contains("nameEn"));
    }
  // The new titles arrive with their names, their touch support and the
  // settings fields their adapter can write.
  EXPECT_EQ(catalog[3]["game"], "hi3");
  EXPECT_EQ(catalog[3]["name"], "崩坏3");
  EXPECT_EQ(catalog[3]["touch"], false);
  EXPECT_EQ(catalog[4]["game"], "wuwa");
  EXPECT_EQ(catalog[4]["nameEn"], "Wuthering Waves");
  EXPECT_FALSE(catalog[4]["settings"].empty());
  EXPECT_EQ(catalog[8]["game"], "nte");
  EXPECT_EQ(catalog[8]["touch"], false);
}

TEST(Catalog, EndfieldHasNoGameSettingsAndSaysSoInsteadOfAnUnavailableList) {
  const sparxie::json games = sparxie::catalog_json();
  const sparxie::json *endfield = nullptr;
  const sparxie::json *genshin = nullptr;
  for (const auto &row : games) {
    if (row["game"] == "endfield") endfield = &row;
    if (row["game"] == "genshin") genshin = &row;
  }
  ASSERT_NE(endfield, nullptr);
  ASSERT_NE(genshin, nullptr);
  EXPECT_EQ((*endfield)["settingsManaged"], false);
  EXPECT_TRUE((*endfield)["settings"].empty()) << "no items that come back unavailable";
  EXPECT_EQ((*genshin)["settingsManaged"], true);
  // Sparxie's own items for Endfield: the input (touch, keyboard/mouse; there is no automatic choice), the resolution (the stream's by default), the target frame rate, the launch options
  std::vector<std::string> keys;
  for (const auto &field : (*endfield)["sparxie"]["fields"]) keys.push_back(field["key"]);
  for (const char *key : { "input", "resolution", "fpsUnlock", "fps", "bgLimit", "bgFps", "priority" }) EXPECT_NE(std::find(keys.begin(), keys.end(), key), keys.end()) << key;
  const auto &input = (*endfield)["sparxie"]["fields"][0];
  EXPECT_EQ(input["key"], "input");
  EXPECT_EQ(input["default"], "touch");
  EXPECT_EQ(input["options"].size(), 2u);
  for (const auto &field : (*endfield)["sparxie"]["fields"])
    if (field["key"] == "resolution") EXPECT_EQ(field["default"], "auto");
  bool has_240 = false;
  for (const auto &field : (*endfield)["sparxie"]["fields"])
    if (field["key"] == "fps")
      for (const auto &option : field["options"])
        if (option["value"] == 240) has_240 = true;
  EXPECT_TRUE(has_240) << "the target frame rate goes above 120";
}
