#include "src/sparxie/art.h"
#include "src/sparxie/art_sources.h"
#include "src/sparxie/model.h"
#include "src/sparxie/region.h"
#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {
  // The answers the official sources gave on 2026-11-11 (docs/sparxie-games.md).
  const char *
  batch_answer(bool endfield) {
    if (!endfield)
      return "{\"proxy_rsps\":[{\"kind\":\"get_main_bg_image\",\"get_main_bg_image_rsp\":{\"data_version\":\"\",\"main_bg_image\":{\"url\":\"https://hg-utils-public.hycdn.cn/hg-utils/prod/eppcsuwqpaueijqk/GzD1CpaWgmSq1wew/53/a2/53a2be88a3371a1473ecd41e89fb0ad5.png\",\"md5\":\"cdbfe3c53a1cd160c4b268d398cccb4f\"}}}]}";
    return "{\"proxy_rsps\":[{\"kind\":\"get_main_bg_image\",\"get_main_bg_image_rsp\":{\"data_version\":\"\",\"main_bg_image\":{\"url\":\"https://hg-utils-public.hycdn.cn/hg-utils/prod/eppcsuwqpaueijqk/6LL0KJuqHBVz33WK/15/d4/15d43bac17c83b83d53f667aeb639952.webp\",\"md5\":\"b406f13a3b71684ebe8c243546b5f2af\"}}}]}";
  }

  const char *
  kuro_index() {
    return "{\"resourcesGray\":{\"graySwitch\":0},\"default\":{\"cdnList\":[]},\"functionCode\":{\"background\":\"UhLmZ3F8YxC2bFFm1c1CFApz6kkJSJQi\"}}";
  }

  const char *
  kuro_background() {
    return "{\"functionSwitch\":1,\"backgroundFile\":\"https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/4v8i229niqadyqq532.mp4\",\"backgroundFileType\":2,"
           "\"firstFrameImage\":\"https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/z9eh4ixe3jlnk7wx8a.webp\","
           "\"slogan\":\"https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/sh81q4z9f428xrpo8d.png\"}";
  }

  const char *
  steam_items() {
    return "{\"response\":{\"store_items\":[{\"item_type\":0,\"id\":3513350,\"appid\":3513350,\"name\":\"Wuthering Waves\",\"assets\":{"
           "\"asset_url_format\":\"steam/apps/3513350/${FILENAME}?t=1791864800\","
           "\"library_capsule\":\"07948a96389614937d331335b9dca86bd5166d7c/library_capsule.jpg\","
           "\"library_capsule_2x\":\"07948a96389614937d331335b9dca86bd5166d7c/library_capsule_2x.jpg\","
           "\"community_icon\":\"b2065aa2b618ff4b310e929024baf237a3d58ff9\","
           "\"raw_page_background\":\"7c80b6040bdbd6d48471c33dbc55e7fbca8abc36/page_bg_raw.jpg\"}}]}}";
  }

  // The mirror's answer for a Steam app (the fields the host reads of it).
  std::string
  steam_pics(int app_id) {
    const std::string id = std::to_string(app_id);
    return "{\"data\":{\"" + id + "\":{\"common\":{\"name\":\"x\",\"clienticon\":\"b2065aa2b618ff4b310e929024baf237a3d58ff9\",\"library_assets_full\":{"
           "\"library_capsule\":{\"image\":{\"english\":\"07948a96389614937d331335b9dca86bd5166d7c/library_capsule.jpg\",\"schinese\":\"fcb169eb2c41ba8fa4e7a78d40ea57b7a635df03/library_capsule_schinese.jpg\"},"
           "\"image2x\":{\"english\":\"07948a96389614937d331335b9dca86bd5166d7c/library_capsule_2x.jpg\",\"schinese\":\"fcb169eb2c41ba8fa4e7a78d40ea57b7a635df03/library_capsule_schinese_2x.jpg\"}},"
           "\"library_hero\":{\"image\":{\"english\":\"71dc6e3d1b733e31187f06c547576c9916f10bdb/library_hero.jpg\"}},"
           "\"library_logo\":{\"image\":{\"english\":\"42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png\",\"schinese\":\"8555af917c101826af81713c250cc74a929a00d8/logo_schinese.png\"}}}}}}}";
  }

  const char *
  nte_ini() {
    return "[VERSION]\r\nVersion=1.0.7.1102\r\nBuild=ce6bd117\r\nFileListURL=https://yhcdn1.wmupd.com/hd/publish_ob/launcher/1.0.7.1102_1/AllFiles.xml\r\n\r\n[UPDATEINFO]\r\n";
  }

  const char *
  nte_files() {
    return "<?xml version='1.0' encoding='UTF-8'?>\n<All_Files>\n  <BuildInfo Time=\"2026-11-04 15:58:21\" />\n  <ProductVersion Version=\"1.0.7.1102_1\" />\n"
           "  <Url BaseUrl=\"https://yhcdn1.wmupd.com/hd/publish_ob/launcher\" />\n"
           "  <File Checksum=\"8e3d1bcd6615ac94022b1675900c15a0\" Path=\"/ResFilesM/1289/bgimgs/bg.mp4\" Size=\"8033029\" />\n"
           "  <File Checksum=\"cf54a64e41f1435e162948f77e768b67\" Path=\"/ResFilesM/1289/bgimgs/bg_0.png\" Size=\"3337070\" ZipChecksum=\"cb57\" ZipSize=\"3337678\" />\n</All_Files>\n";
  }

  const char *
  nte_site() {
    return "<html><body><div><img src=\"https://yh.wanmei.com/images/cover261105/logo.png\" class=\"abs logoCover\"><img class=\"icon-age\" src=\"https://yh.wanmei.com/images/cover260203/icon-age.png\"></div></body></html>";
  }

  // The network the sources see in the tests: every answer above, nothing else; each URL asked is recorded.
  struct FakeNet {
    std::vector<std::string> asked;
    bool kuro_up = true;
    bool steam_up = true;
    bool pics_up = true;
    bool ak_site_up = false;

    sparxie::GetUrlFn
    get() {
      return [this](const std::string &url) -> std::optional<std::string> {
        asked.push_back(url);
        if (url.find("kurogame.com/launcher/launcher/") != std::string::npos && url.find("index.json") != std::string::npos) return kuro_up ? std::optional<std::string>(kuro_index()) : std::nullopt;
        if (url.find("/background/UhLmZ3F8YxC2bFFm1c1CFApz6kkJSJQi/") != std::string::npos) return kuro_up ? std::optional<std::string>(kuro_background()) : std::nullopt;
        if (url.find("IStoreBrowseService") != std::string::npos) return steam_up ? std::optional<std::string>(steam_items()) : std::nullopt;
        if (url.rfind("https://api.steamcmd.net/v1/info/", 0) == 0) return pics_up ? std::optional<std::string>(steam_pics(std::stoi(url.substr(33)))) : std::nullopt;
        if (url == "https://ak.hypergryph.com/") return ak_site_up ? std::optional<std::string>("<html><defs><path id=\"svg_def-title_arknights\" fill-rule=\"evenodd\" fill=\"currentColor\" d=\"M0 0h254v119H0z\"/></defs></html>") : std::nullopt;
        if (url.find("Version.ini") != std::string::npos) return nte_ini();
        if (url.find("AllFiles.xml") != std::string::npos) return nte_files();
        if (url == "https://yh.wanmei.com/") return nte_site();
        return std::nullopt;
      };
    }

    sparxie::PostUrlFn
    post() {
      return [this](const std::string &url, const std::string &body) -> std::optional<std::string> {
        asked.push_back(url + " " + body);
        if (url.find("batch_proxy") == std::string::npos) return std::nullopt;
        return batch_answer(body.find("6LL0KJuqHBVz33WK") != std::string::npos);
      };
    }
  };

  sparxie::ArtUrls
  first_of(std::string_view game, std::string_view region, FakeNet &net, std::string *label = nullptr) {
    const auto sources = sparxie::vendor_sources(game, region, net.get(), net.post());
    for (const auto &source : sources) {
      const sparxie::ArtUrls urls = source.resolve();
      if (!urls.background.empty()) {
        if (label) *label = source.label;
        return urls;
      }
    }
    return {};
  }
}  // namespace

TEST(ArtSources, BatchProxyRequestAndAnswer) {
  const std::string body = sparxie::batch_proxy_body("6LL0KJuqHBVz33WK");
  EXPECT_NE(body.find("\"kind\":\"get_main_bg_image\""), std::string::npos);
  EXPECT_NE(body.find("\"appcode\":\"6LL0KJuqHBVz33WK\""), std::string::npos);
  const sparxie::ArtUrls urls = sparxie::art_from_batch_proxy(batch_answer(true));
  EXPECT_EQ(urls.background, "https://hg-utils-public.hycdn.cn/hg-utils/prod/eppcsuwqpaueijqk/6LL0KJuqHBVz33WK/15/d4/15d43bac17c83b83d53f667aeb639952.webp");
  EXPECT_EQ(urls.version, "15d43bac17c83b83d53f667aeb639952");
  EXPECT_TRUE(sparxie::art_from_batch_proxy("not json").background.empty());
  EXPECT_TRUE(sparxie::art_from_batch_proxy("{\"proxy_rsps\":[]}").background.empty());
}

TEST(ArtSources, KuroLauncherAnswers) {
  EXPECT_EQ(sparxie::kuro_background_id(kuro_index()), "UhLmZ3F8YxC2bFFm1c1CFApz6kkJSJQi");
  EXPECT_TRUE(sparxie::kuro_background_id("not json").empty());
  EXPECT_TRUE(sparxie::kuro_background_id("{\"functionCode\":{}}").empty());
  const sparxie::ArtUrls urls = sparxie::art_from_kuro_background(kuro_background());
  EXPECT_EQ(urls.background, "https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/z9eh4ixe3jlnk7wx8a.webp");
  EXPECT_EQ(urls.logo, "https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/sh81q4z9f428xrpo8d.png");
  // a new slogan on the same background is a new version
  EXPECT_EQ(urls.version, "z9eh4ixe3jlnk7wx8a-sh81q4z9f428xrpo8d");
  // the switch off, a picture not on TLS and a broken answer all mean no art
  EXPECT_TRUE(sparxie::art_from_kuro_background("{\"functionSwitch\":0,\"firstFrameImage\":\"https://x/y.webp\"}").background.empty());
  EXPECT_TRUE(sparxie::art_from_kuro_background("{\"functionSwitch\":1,\"firstFrameImage\":\"http://x/y.webp\"}").background.empty());
  EXPECT_TRUE(sparxie::art_from_kuro_background("{").background.empty());
  // a slogan that is not on TLS is dropped, the picture stays
  const sparxie::ArtUrls plain = sparxie::art_from_kuro_background("{\"functionSwitch\":1,\"firstFrameImage\":\"https://x/y.webp\",\"slogan\":\"http://x/z.png\"}");
  EXPECT_EQ(plain.background, "https://x/y.webp");
  EXPECT_TRUE(plain.logo.empty());
}

TEST(ArtSources, SteamAnswerNamesPageArtCapsuleAndIcon) {
  const sparxie::ArtUrls urls = sparxie::art_from_steam(steam_items());
  EXPECT_EQ(urls.background, "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/3513350/7c80b6040bdbd6d48471c33dbc55e7fbca8abc36/page_bg_raw.jpg?t=1791864800");
  EXPECT_EQ(urls.cover, "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/3513350/07948a96389614937d331335b9dca86bd5166d7c/library_capsule_2x.jpg?t=1791864800");
  EXPECT_EQ(urls.icon, "https://shared.akamai.steamstatic.com/community_assets/images/apps/3513350/b2065aa2b618ff4b310e929024baf237a3d58ff9.jpg");
  EXPECT_EQ(urls.version, "7c80b6040bdbd6d4-07948a963896");
  EXPECT_TRUE(sparxie::art_from_steam("{}").background.empty());
  EXPECT_TRUE(sparxie::art_from_steam("{\"response\":{\"store_items\":[]}}").background.empty());
  EXPECT_TRUE(sparxie::art_from_steam("{\"response\":{\"store_items\":[{\"assets\":{\"asset_url_format\":\"x\"}}]}}").background.empty());
  const std::string url = sparxie::steam_items_url(3513350);
  EXPECT_EQ(url.rfind("https://api.steampowered.com/IStoreBrowseService/GetItems/v1?input_json=", 0), 0u);
  EXPECT_NE(url.find("3513350"), std::string::npos);
  EXPECT_EQ(url.find('{'), std::string::npos);
}

TEST(ArtSources, NteLauncherFileListNamesTheZippedPicture) {
  EXPECT_EQ(sparxie::ini_value(nte_ini(), "FileListURL"), "https://yhcdn1.wmupd.com/hd/publish_ob/launcher/1.0.7.1102_1/AllFiles.xml");
  EXPECT_TRUE(sparxie::ini_value(nte_ini(), "Missing").empty());
  const sparxie::ArtUrls urls = sparxie::art_from_nte_file_list(nte_files());
  EXPECT_EQ(urls.background, "https://yhcdn1.wmupd.com/hd/publish_ob/launcher/1.0.7.1102_1/ResFilesM/1289/bgimgs/bg_0.png.zip");
  EXPECT_EQ(urls.version, "cf54a64e41f1435e162948f77e768b67");
  EXPECT_TRUE(sparxie::art_background_zipped(urls));
  EXPECT_FALSE(sparxie::art_background_zipped(sparxie::art_from_kuro_background(kuro_background())));
  EXPECT_TRUE(sparxie::art_from_nte_file_list("<All_Files></All_Files>").background.empty());
  EXPECT_TRUE(sparxie::art_from_nte_file_list("<Url BaseUrl=\"http://insecure/\"/><ProductVersion Version=\"1\"/><File Checksum=\"a\" Path=\"/ResFilesM/1289/bgimgs/bg_0.png\"/>").background.empty());
}

TEST(ArtSources, SitePagesNameTheirAssets) {
  EXPECT_EQ(sparxie::page_image_url(nte_site(), "logo.png"), "https://yh.wanmei.com/images/cover261105/logo.png");
  EXPECT_TRUE(sparxie::page_image_url(nte_site(), "missing.png").empty());
}

TEST(ArtSources, SteamPicsNameTheIconTheLogoTheCleanHeroAndTheCapsule) {
  const sparxie::SteamPics global = sparxie::steam_pics_art(steam_pics(4732690), 4732690, "global");
  EXPECT_EQ(global.icon, "https://shared.akamai.steamstatic.com/community_assets/images/apps/4732690/b2065aa2b618ff4b310e929024baf237a3d58ff9.ico");
  EXPECT_EQ(global.logo, "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/4732690/42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png");
  EXPECT_EQ(global.hero, "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/4732690/71dc6e3d1b733e31187f06c547576c9916f10bdb/library_hero.jpg");
  EXPECT_EQ(global.capsule, "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/4732690/07948a96389614937d331335b9dca86bd5166d7c/library_capsule_2x.jpg");
  EXPECT_EQ(global.version, "71dc6e3d1b73-42099b9aa260");
  EXPECT_EQ(global.logo_version, "42099b9aa260");
  // the national and bilibili clients take the Simplified Chinese logo and capsule
  for (const char *region : { "cn", "bili" }) {
    const sparxie::SteamPics chinese = sparxie::steam_pics_art(steam_pics(4732690), 4732690, region);
    EXPECT_NE(chinese.logo.find("8555af917c101826af81713c250cc74a929a00d8/logo_schinese.png"), std::string::npos) << region;
    EXPECT_NE(chinese.capsule.find("fcb169eb2c41ba8fa4e7a78d40ea57b7a635df03/library_capsule_schinese_2x.jpg"), std::string::npos) << region;
    EXPECT_EQ(chinese.hero, global.hero) << "the key art has no language";
  }
  // a language the app has no art in falls to English
  const std::string no_chinese = "{\"data\":{\"1\":{\"common\":{\"library_assets_full\":{\"library_logo\":{\"image\":{\"english\":\"42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png\"}}}}}}}";
  EXPECT_NE(sparxie::steam_pics_art(no_chinese, 1, "cn").logo.find("42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png"), std::string::npos);
  // the answer is not trusted for paths: only "<40 hex>/<file>" names are used
  const std::string hostile = "{\"data\":{\"1\":{\"common\":{\"clienticon\":\"../../x\",\"library_assets_full\":{\"library_logo\":{\"image\":{\"english\":\"../evil/logo.png\"}}}}}}}";
  const sparxie::SteamPics hostile_pics = sparxie::steam_pics_art(hostile, 1, "global");
  EXPECT_TRUE(hostile_pics.logo.empty());
  EXPECT_TRUE(hostile_pics.icon.empty());
  EXPECT_TRUE(sparxie::steam_pics_art("not json", 1, "global").logo.empty());
  EXPECT_TRUE(sparxie::steam_pics_art("{\"data\":{}}", 1, "global").logo.empty());
  EXPECT_EQ(sparxie::steam_pics_url(4732690), "https://api.steamcmd.net/v1/info/4732690");
}

TEST(ArtSources, EveryTitleAndRegionHasItsSourcesOrNone) {
  FakeNet net;
  // Endfield cn: the launcher's picture has the title painted in, so the clean variant leads: Steam's library hero with the Chinese library logo and capsule.
  std::string label;
  const sparxie::ArtUrls endfield = first_of("endfield", "cn", net, &label);
  EXPECT_EQ(label, "steam-clean");
  EXPECT_NE(endfield.background.find("71dc6e3d1b733e31187f06c547576c9916f10bdb/library_hero.jpg"), std::string::npos);
  EXPECT_NE(endfield.logo.find("logo_schinese.png"), std::string::npos);
  EXPECT_NE(endfield.cover.find("library_capsule_schinese_2x.jpg"), std::string::npos);
  EXPECT_EQ(endfield.version, "71dc6e3d1b73-8555af917c10");
  EXPECT_TRUE(endfield.icon.empty()) << "the icon has its own sources";
  EXPECT_EQ(first_of("endfield", "bili", net).version, endfield.version);
  // Arknights cn: the launcher's picture is the title's own (no clean variant exists), no logo is laid over it.
  const sparxie::ArtUrls arknights = first_of("arknights", "cn", net, &label);
  EXPECT_EQ(label, "hypergryph");
  EXPECT_EQ(arknights.version, "53a2be88a3371a1473ecd41e89fb0ad5");
  EXPECT_TRUE(arknights.logo.empty());
  // Wuthering Waves cn and bilibili: the Kuro launcher's first frame with Steam's Chinese library logo, not the version banner.
  const sparxie::ArtUrls wuwa = first_of("wuwa", "cn", net, &label);
  EXPECT_EQ(label, "kuro");
  EXPECT_NE(wuwa.logo.find("logo_schinese.png"), std::string::npos);
  EXPECT_EQ(wuwa.version, "z9eh4ixe3jlnk7wx8a-8555af917c10");
  EXPECT_EQ(first_of("wuwa", "bili", net).version, wuwa.version);
  // Wuthering Waves global: the Kuro launcher again with the English logo.
  const sparxie::ArtUrls wuwa_global = first_of("wuwa", "global", net, &label);
  EXPECT_EQ(label, "kuro");
  EXPECT_NE(wuwa_global.logo.find("42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png"), std::string::npos);
  // the Steam mirror does not answer: the launcher's slogan stays the logo
  net.pics_up = false;
  const sparxie::ArtUrls slogan = first_of("wuwa", "cn", net, &label);
  EXPECT_EQ(slogan.logo, "https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/sh81q4z9f428xrpo8d.png");
  EXPECT_EQ(slogan.version, "z9eh4ixe3jlnk7wx8a-sh81q4z9f428xrpo8d");
  // ... and Endfield cn falls from the clean variant to the launcher's own picture
  first_of("endfield", "cn", net, &label);
  EXPECT_EQ(label, "hypergryph");
  net.pics_up = true;
  // Punishing: Gray Raven cn and global: both Kuro launchers.
  EXPECT_EQ(first_of("pgr", "cn", net, &label).background.empty(), false);
  EXPECT_EQ(label, "kuro");
  EXPECT_EQ(first_of("pgr", "global", net, &label).background.empty(), false);
  // NTE: the national launcher's zipped picture with the site's logo, and the store build globally (page art, the library logo laid over it).
  const sparxie::ArtUrls nte = first_of("nte", "cn", net, &label);
  EXPECT_EQ(label, "nte-launcher");
  EXPECT_TRUE(sparxie::art_background_zipped(nte));
  EXPECT_EQ(nte.logo, "https://yh.wanmei.com/images/cover261105/logo.png");
  const sparxie::ArtUrls nte_global = first_of("nte", "global", net, &label);
  EXPECT_EQ(label, "steam");
  EXPECT_FALSE(nte_global.cover.empty());
  EXPECT_NE(nte_global.logo.find("42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png"), std::string::npos);
  // Endfield global is the store build with the library logo.
  const sparxie::ArtUrls endfield_global = first_of("endfield", "global", net, &label);
  EXPECT_FALSE(endfield_global.background.empty());
  EXPECT_FALSE(endfield_global.logo.empty());
}

TEST(ArtSources, ArknightsCnLaysTheSitesOwnWordmarkOnItsPosters) {
  FakeNet net;
  // the site does not answer: the launcher picture alone (its painted title), no logo
  const sparxie::ArtUrls plain = first_of("arknights", "cn", net);
  EXPECT_TRUE(plain.logo_svg_path.empty());
  EXPECT_EQ(plain.version, "53a2be88a3371a1473ecd41e89fb0ad5");
  // the site's inline SVG wordmark (viewBox 254 x 119) is the logo, and its hash is part of the version
  net.ak_site_up = true;
  const sparxie::ArtUrls drawn = first_of("arknights", "cn", net);
  EXPECT_EQ(drawn.logo_svg_path, "M0 0h254v119H0z");
  EXPECT_EQ(drawn.logo_svg_w, 254);
  EXPECT_EQ(drawn.logo_svg_h, 119);
  EXPECT_TRUE(drawn.logo.empty());
  EXPECT_TRUE(drawn.logo_cover_only);
  EXPECT_FALSE(first_of("wuwa", "cn", net).logo_cover_only);
  EXPECT_EQ(drawn.version.rfind("53a2be88a3371a1473ecd41e89fb0ad5-", 0), 0u);
  EXPECT_EQ(drawn.version.size(), 32u + 1u + 8u);
  // Endfield's launcher picture gets no logo laid over it (the clean Steam variant leads)
  EXPECT_TRUE(first_of("endfield", "bili", net).logo_svg_path.empty());
}

TEST(ArtSources, TheProgramIconHasItsOwnSourcesBestFirst) {
  FakeNet net;
  // Steam's client icon (the .ico of the game's program) for every title that is on Steam, whatever the region
  for (const char *game : { "endfield", "wuwa", "pgr", "nte" })
    for (const char *region : { "cn", "bili", "global" }) {
      const auto sources = sparxie::icon_sources(game, region, net.get());
      ASSERT_FALSE(sources.empty()) << game << " " << region;
      EXPECT_EQ(sources[0].label, "steam-clienticon");
      EXPECT_NE(sources[0].url().find("community_assets/images/apps/"), std::string::npos);
      EXPECT_NE(sources[0].url().find(".ico"), std::string::npos);
    }
  // Arknights is not on Steam: the Yostar site's own icon (a PNG), not the Hypergryph company favicon of the national site
  const auto arknights = sparxie::icon_sources("arknights", "cn", net.get());
  ASSERT_EQ(arknights.size(), 1u);
  EXPECT_EQ(arknights[0].label, "official-site");
  EXPECT_NE(arknights[0].url().find("yo-star.com"), std::string::npos);
  // the mirror does not answer: the Steam source answers no address, the next one is tried
  net.pics_up = false;
  EXPECT_TRUE(sparxie::icon_sources("wuwa", "cn", net.get())[0].url().empty());
  // a HoYoverse title has none here (its launcher API answers the icon)
  EXPECT_TRUE(sparxie::icon_sources("genshin", "cn", net.get()).empty());
}

TEST(ArtSources, ASourceThatDoesNotAnswerFallsToTheNextOne) {
  FakeNet net;
  net.kuro_up = false;
  std::string label;
  // the Kuro launcher is down: the store build answers instead
  const sparxie::ArtUrls urls = first_of("wuwa", "global", net, &label);
  EXPECT_EQ(label, "steam");
  EXPECT_FALSE(urls.background.empty());
  // the national build has no second source: nothing answers, the caller draws the placeholder
  EXPECT_TRUE(first_of("wuwa", "cn", net).background.empty());
  net.steam_up = false;
  EXPECT_TRUE(first_of("wuwa", "global", net).background.empty());
  // Endfield bilibili: the clean variant, the launcher API and the store build in that order
  sparxie::PostUrlFn dead_post = [](const std::string &, const std::string &) { return std::optional<std::string> {}; };
  net.steam_up = true;
  const auto sources = sparxie::vendor_sources("endfield", "bili", net.get(), dead_post);
  ASSERT_EQ(sources.size(), 3u);
  EXPECT_FALSE(sources[0].resolve().background.empty());
  EXPECT_TRUE(sources[1].resolve().background.empty());
  EXPECT_FALSE(sources[2].resolve().background.empty());
}

TEST(ArtSources, AnUnsourcedRegionNeverTouchesTheNetwork) {
  const auto fail_get = [](const std::string &) { ADD_FAILURE(); return std::optional<std::string> {}; };
  const auto fail_post = [](const std::string &, const std::string &) { ADD_FAILURE(); return std::optional<std::string> {}; };
  EXPECT_TRUE(sparxie::vendor_sources("arknights", "global", fail_get, fail_post).empty());
  EXPECT_TRUE(sparxie::vendor_sources("genshin", "cn", fail_get, fail_post).empty());
  // a source asks only when it is resolved
  EXPECT_FALSE(sparxie::vendor_sources("pgr", "global", fail_get, fail_post).empty());
}

TEST(ArtSources, PlaceholdersAndMonogramIcons) {
  for (const auto game : sparxie::kGames) {
    const sparxie::Rgba card = sparxie::placeholder_background(game);
    ASSERT_EQ(card.w, 600);
    ASSERT_EQ(card.h, 900);
    EXPECT_FALSE(sparxie::encode_png(card).empty()) << game;
    const sparxie::Rgba icon = sparxie::placeholder_icon(game);
    ASSERT_EQ(icon.w, 256);
    ASSERT_EQ(icon.h, 256);
    EXPECT_FALSE(sparxie::encode_png(icon).empty()) << game;
    const sparxie::Rgba hero = sparxie::placeholder_hero(game);
    ASSERT_EQ(hero.w, 1920) << game;
    ASSERT_EQ(hero.h, 1080) << game;
    // the hero poster of a placeholder composes to the same 16:9 frame the clients read
    EXPECT_FALSE(sparxie::encode_jpeg(sparxie::compose_hero(hero, {}), sparxie::kHeroJpegQuality).empty()) << game;
  }
  // The card carries the drawn wordmark (lit pixels around the text band) and
  // the brand tone (base colour dominates the lower third).
  const sparxie::Rgba card = sparxie::placeholder_background("arknights");
  int lit = 0;
  for (int y = card.h * 45 / 100; y < card.h * 65 / 100; ++y)
    for (int x = 0; x < card.w; ++x)
      if (card.px[(static_cast<std::size_t>(y * card.w + x)) * 4] > 200) ++lit;
  EXPECT_GT(lit, 2000);
  EXPECT_NEAR(card.px[(static_cast<std::size_t>(card.h * 9 / 10 * card.w + card.w / 2)) * 4], 14, 2);
  // the landscape card draws the name inside the band the clients keep clear of the title scrim
  const sparxie::Rgba hero = sparxie::placeholder_hero("arknights");
  int lit_in_band = 0;
  int lit_below_band = 0;
  for (int y = 0; y < hero.h; ++y)
    for (int x = 0; x < hero.w; ++x)
      if (hero.px[(static_cast<std::size_t>(y * hero.w + x)) * 4] > 200) (y < static_cast<int>(hero.h * sparxie::kHeroSafeBottom) ? lit_in_band : lit_below_band)++;
  EXPECT_GT(lit_in_band, 5000);
  EXPECT_EQ(lit_below_band, 0);
  // compose_cover scales the 600x900 card one to one, so what is drawn is what
  // the tile shows
  const sparxie::Rgba cover = sparxie::compose_cover(card, {});
  const std::size_t middle = static_cast<std::size_t>(card.h / 2 * card.w + card.w / 2) * 4;
  EXPECT_EQ(cover.px[middle], card.px[middle]);
  // a finished cover keeps its size when it already is 600x900, and scales otherwise
  EXPECT_EQ(sparxie::fit_cover(card).w, 600);
  sparxie::Rgba small;
  small.w = 300;
  small.h = 450;
  small.px.assign(300u * 450u * 4u, 200);
  EXPECT_EQ(sparxie::fit_cover(small).w, 600);
  EXPECT_EQ(sparxie::fit_cover(small).h, 900);
  EXPECT_EQ(sparxie::fit_cover({}).w, 0);
}
