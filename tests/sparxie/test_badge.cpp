#include "src/sparxie/art.h"
#include "src/sparxie/host.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <tuple>
#include <gtest/gtest.h>
namespace fs = std::filesystem;
namespace {
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

  sparxie::Rgba
  solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    sparxie::Rgba image;
    image.w = w;
    image.h = h;
    image.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    for (int i = 0; i < w * h; ++i) {
      image.px[static_cast<std::size_t>(i) * 4] = r;
      image.px[static_cast<std::size_t>(i) * 4 + 1] = g;
      image.px[static_cast<std::size_t>(i) * 4 + 2] = b;
      image.px[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return image;
  }

  // real art is never one colour (the host refuses a blank background): a gradient with a check pattern
  sparxie::Rgba
  textured(int w, int h) {
    sparxie::Rgba image = solid(w, h, 0, 0, 0);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
        image.px[at] = static_cast<std::uint8_t>(40 + x * 180 / w);
        image.px[at + 1] = static_cast<std::uint8_t>(40 + y * 180 / h);
        image.px[at + 2] = static_cast<std::uint8_t>(((x / 16 + y / 16) % 2) * 120 + 40);
      }
    return image;
  }

  // a logo on a transparent canvas (half of the columns of the block transparent, so the plate rule does not take it for a logo on a plate)
  sparxie::Rgba
  block_logo(int bw, int bh) {
    sparxie::Rgba logo;
    logo.w = 416;
    logo.h = 234;
    logo.px.assign(static_cast<std::size_t>(logo.w * logo.h * 4), 0);
    for (int y = 40; y < 40 + bh; ++y)
      for (int x = 30; x < 30 + bw; ++x)
        for (int c = 0; c < 4; ++c) logo.px[static_cast<std::size_t>((y * logo.w + x) * 4 + c)] = c == 3 ? ((x - 30) / 6 % 2 == 0 ? 255 : 0) : 255;
    return logo;
  }

  void
  write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  sparxie::Rgba
  read_image(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
  }

  sparxie::json
  read_apps(const fs::path &file) {
    std::ifstream in(file);
    sparxie::json body;
    in >> body;
    return body;
  }

  // one touch (or pc) game app of the genshin-cn install, as the host stores it; `extra` is merged into the sparxie block
  sparxie::json
  app_of(const std::string &mode, const std::string &cover, const sparxie::json &extra = sparxie::json::object()) {
    sparxie::json app;
    app["name"] = "Genshin";
    app["cmd"] = "\"x\"";
    app["sparxie"] = { { "install", "genshin-cn" }, { "game", "genshin" }, { "region", "cn" }, { "mode", mode }, { "cover", cover }, { "profile", mode == "touch" ? "genshin-cn-1" : "" }, { "exe", "D:/g/YuanShen.exe" }, { "dir", "D:/g" } };
    for (auto it = extra.begin(); it != extra.end(); ++it) app["sparxie"][it.key()] = it.value();
    return app;
  }

  void
  put_apps(const sparxie::HostEnv &env, const std::vector<sparxie::json> &apps) {
    sparxie::json body;
    body["apps"] = apps;
    fs::create_directories(env.apps_file.parent_path());
    std::ofstream(env.apps_file) << body.dump();
  }

  void
  publish(sparxie::HostEnv &env, const std::string &version = "v1") {
    sparxie::ArtUrls urls;
    urls.version = version;
    ASSERT_TRUE(sparxie::publish_art(env, "genshin-cn", urls, textured(2560, 1440), block_logo(225, 80), {}).status.ok);
  }

  fs::path
  plain_cover(const sparxie::HostEnv &env, const std::string &version = "v1") {
    return env.covers_dir / ("sparxie-genshin-cn-" + version + ".png");
  }
  fs::path
  badged_cover(const sparxie::HostEnv &env, const std::string &version = "v1") {
    return env.covers_dir / ("sparxie-genshin-cn-" + version + "-badge" + std::to_string(sparxie::kBadgeRevision) + ".png");
  }
  fs::path
  poster(const sparxie::HostEnv &env, const char *name) {
    return env.host_data / "posters" / "genshin-cn" / name;
  }

  // how many pixels of the two pictures differ, and the bounding box of those that do
  struct Delta {
    long long count = 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
  };
  Delta
  delta(const sparxie::Rgba &a, const sparxie::Rgba &b) {
    Delta d;
    EXPECT_EQ(a.w, b.w);
    EXPECT_EQ(a.h, b.h);
    for (int y = 0; y < a.h; ++y)
      for (int x = 0; x < a.w; ++x) {
        const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.w) + static_cast<std::size_t>(x)) * 4;
        if (a.px[at] == b.px[at] && a.px[at + 1] == b.px[at + 1] && a.px[at + 2] == b.px[at + 2]) continue;
        ++d.count;
        d.x0 = std::min(d.x0, x);
        d.y0 = std::min(d.y0, y);
        d.x1 = std::max(d.x1, x);
        d.y1 = std::max(d.y1, y);
      }
    return d;
  }
}  // namespace

TEST(Badge, TheMarkShipsInTheHostAndDecodes) {
  const sparxie::Rgba mark = sparxie::badge_mark();
  EXPECT_EQ(mark.w, 256);
  EXPECT_EQ(mark.h, 256);
  long long opaque = 0;
  for (std::size_t i = 3; i < mark.px.size(); i += 4) opaque += mark.px[i] > 200 ? 1 : 0;
  EXPECT_GT(opaque, 256 * 256 / 4) << "a mark, not an empty canvas";
}

TEST(Badge, PlateIsReadableOnAPhoneGridAndClearOfTheLogoAndTheTitleScrim) {
  // a 2:3 cover (110 px wide on a phone) and a 16:9 hero (180 px wide)
  const auto cover = sparxie::badge_box(600, 900);
  const auto hero = sparxie::badge_box(1920, 1080);
  EXPECT_GE(cover.size * 110.0 / 600, 24.0) << "at least 24 px on a 110 px wide portrait tile";
  EXPECT_GE(hero.size * 180.0 / 1920, 24.0) << "at least 24 px on a 180 px wide landscape tile";
  // top-right, inside the picture
  for (const auto &[w, h, box] : { std::tuple { 600, 900, cover }, std::tuple { 1920, 1080, hero } }) {
    EXPECT_LE(box.x + box.size, w);
    EXPECT_GE(box.y, 0);
    EXPECT_GT(box.x + box.size / 2, w * 3 / 4) << "right side";
    EXPECT_LT(box.y + box.size, h / 3) << "top third: above the bottom title scrim";
  }
  // the centred hero logo (the widest one the hero takes) and the badge do not meet
  const sparxie::HeroBox logo = sparxie::hero_logo_box(1920, 1080, block_logo(292, 89));
  ASSERT_GT(logo.w, 0);
  EXPECT_LT(logo.x + logo.w, hero.x);
  // and not too big to be a corner mark
  EXPECT_LE(hero.size, 1080 / 3);
  EXPECT_LE(cover.size, 600 / 3);
}

TEST(Badge, OnlyTheCornerChangesAndTheMarkIsDrawnInIt) {
  const sparxie::Rgba base = solid(1920, 1080, 40, 80, 120);
  const sparxie::Rgba badged = sparxie::with_badge(base, sparxie::badge_mark());
  const Delta d = delta(base, badged);
  const auto box = sparxie::badge_box(base.w, base.h);
  EXPECT_GT(d.count, 0);
  // the plate with its shadow: a little beyond the box, nowhere else
  const int reach = box.size / 3;
  EXPECT_GE(d.x0, box.x - reach);
  EXPECT_LE(d.x1, box.x + box.size + reach);
  EXPECT_GE(d.y0, box.y - reach);
  EXPECT_LE(d.y1, box.y + box.size + 2 * reach);
  // the mark is bright against the dark plate
  int bright = 0;
  for (int y = box.y; y < box.y + box.size; ++y)
    for (int x = box.x; x < box.x + box.size; ++x)
      if (badged.px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(badged.w) + static_cast<std::size_t>(x)) * 4] > 200) ++bright;
  EXPECT_GT(bright, box.size * box.size / 10);
  // an empty picture comes back as it is
  EXPECT_EQ(sparxie::with_badge({}, sparxie::badge_mark()).w, 0);
}

TEST(Badge, TheDecoderReadsWhatTheHostWritesAndNothingElse) {
  const sparxie::Rgba image = solid(64, 48, 200, 10, 10);
  const auto png = sparxie::encode_png(image);
  const auto jpg = sparxie::encode_jpeg(image, 90);
  EXPECT_EQ(sparxie::decode_image(png.data(), png.size()).w, 64);
  EXPECT_EQ(sparxie::decode_image(jpg.data(), jpg.size()).h, 48);
  const std::uint8_t junk[] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0 };
  EXPECT_EQ(sparxie::decode_image(junk, sizeof(junk)).w, 0);
  EXPECT_EQ(sparxie::decode_image(nullptr, 0).w, 0);
}

TEST(BadgePosters, ATouchAppWithAnOfficialCoverGetsTheBadgedPostersByDefault) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-default";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, { app_of("touch", "official"), app_of("pc", "official"), app_of("touch", "official", { { "badge", false } }) });
  publish(env);
  // both variants exist, under their own names
  EXPECT_TRUE(fs::exists(plain_cover(env)));
  EXPECT_TRUE(fs::exists(badged_cover(env)));
  EXPECT_TRUE(fs::exists(poster(env, "hero.jpg")));
  EXPECT_TRUE(fs::exists(poster(env, "hero-badge.jpg")));
  EXPECT_NE(fs::file_size(plain_cover(env)), fs::file_size(badged_cover(env)));
  const auto apps = read_apps(env.apps_file)["apps"];
  EXPECT_EQ(apps[0]["image-path"], badged_cover(env).string()) << "touch: badge on without being asked";
  EXPECT_EQ(apps[0]["hero"], poster(env, "hero-badge.jpg").string());
  EXPECT_EQ(apps[1]["image-path"], plain_cover(env).string()) << "an app that launches the game directly has no badge";
  EXPECT_EQ(apps[1]["hero"], poster(env, "hero.jpg").string());
  EXPECT_EQ(apps[2]["image-path"], plain_cover(env).string()) << "badge switched off";
  EXPECT_EQ(apps[2]["hero"], poster(env, "hero.jpg").string());
  // the badged file is the plain poster with the corner drawn in
  const auto d = delta(read_image(plain_cover(env)), read_image(badged_cover(env)));
  const auto box = sparxie::badge_box(600, 900);
  EXPECT_GT(d.count, 0);
  EXPECT_GE(d.x0, box.x - box.size / 3);
  EXPECT_LT(d.y1, 900 / 3);
}

TEST(BadgePosters, NothingIsRewrittenWhenNothingChanged) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-stable";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, { app_of("touch", "official") });
  publish(env);
  const auto before_hero = fs::last_write_time(poster(env, "hero-badge.jpg"));
  const auto before_cover = fs::last_write_time(badged_cover(env));
  const std::string apps_before = read_apps(env.apps_file).dump();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  publish(env);
  EXPECT_EQ(fs::last_write_time(poster(env, "hero-badge.jpg")), before_hero);
  EXPECT_EQ(fs::last_write_time(badged_cover(env)), before_cover);
  EXPECT_EQ(read_apps(env.apps_file).dump(), apps_before);
}

TEST(BadgePosters, TheSwitchPointsTheAppAtAnotherFileEachWay) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-toggle";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, { app_of("touch", "official") });
  publish(env);
  const auto off = sparxie::update_app(env, 0, { { "badge", false } });
  ASSERT_TRUE(off.status.ok) << off.status.error;
  auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["sparxie"]["badge"], false);
  EXPECT_EQ(app["image-path"], plain_cover(env).string());
  EXPECT_EQ(app["hero"], poster(env, "hero.jpg").string());
  const auto on = sparxie::update_app(env, 0, { { "badge", true } });
  ASSERT_TRUE(on.status.ok) << on.status.error;
  app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["sparxie"]["badge"], true);
  EXPECT_EQ(app["image-path"], badged_cover(env).string());
  EXPECT_EQ(app["hero"], poster(env, "hero-badge.jpg").string());
  // an app that no longer starts through Sparxie has no badge, whatever the setting says
  const auto direct = sparxie::update_app(env, 0, { { "mode", "pc" } });
  ASSERT_TRUE(direct.status.ok) << direct.status.error;
  app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], plain_cover(env).string());
  EXPECT_EQ(app["hero"], poster(env, "hero.jpg").string());
}

TEST(BadgePosters, AnAppOfTheEarlierBuildsGetsTheBadgeAtHostStartAndAMissingBadgedCoverIsMadeFromThePlainOne) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-migrate";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  // posters made before the badge: only the plain cover and hero exist
  fs::create_directories(env.covers_dir);
  write_file(plain_cover(env), sparxie::encode_png(textured(600, 900)));
  write_file(poster(env, "hero.jpg"), sparxie::encode_jpeg(textured(1920, 1080), 90));
  put_apps(env, { app_of("touch", "official") });
  {
    auto apps = read_apps(env.apps_file);
    apps["apps"][0]["image-path"] = plain_cover(env).string();
    apps["apps"][0]["hero"] = poster(env, "hero.jpg").string();
    std::ofstream(env.apps_file) << apps.dump();
  }
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_TRUE(fs::exists(badged_cover(env))) << "made from the plain cover";
  EXPECT_EQ(app["image-path"], badged_cover(env).string());
  EXPECT_EQ(app["hero"], poster(env, "hero.jpg").string()) << "no badged hero yet: the plain one stays until the next refresh composes it";
  // the refresh composes the hero from the cached pictures; the hero of an app that has it points at it
  ASSERT_TRUE(sparxie::publish_hero(env, "genshin-cn", textured(2560, 1440), block_logo(225, 80)) >= 0);
  app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["hero"], poster(env, "hero-badge.jpg").string());
  // and a second start changes nothing
  EXPECT_EQ(sparxie::attach_official_covers(env), 0);
}

TEST(BadgePosters, ACustomCoverIsUntouchedUntilThePersonTurnsTheBadgeOn) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-custom";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  const fs::path own = root / "mine" / "poster.png";
  const fs::path own_hero = root / "mine" / "hero.jpg";
  write_file(own, sparxie::encode_png(solid(400, 600, 90, 20, 20)));
  write_file(own_hero, sparxie::encode_jpeg(solid(1280, 720, 20, 90, 20), 90));
  auto custom = app_of("touch", "custom");
  custom["image-path"] = own.string();
  custom["hero"] = own_hero.string();
  put_apps(env, { custom });
  publish(env);
  // the refresh and the host start leave it alone
  EXPECT_EQ(sparxie::attach_official_covers(env), 0);
  auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], own.string());
  EXPECT_EQ(app["hero"], own_hero.string());
  EXPECT_FALSE(app["sparxie"].contains("badge"));
  EXPECT_FALSE(fs::exists(env.host_data / "posters" / "custom")) << "no copy is made for a cover that has no badge";
  // turned on: the app points at a badged copy, the block remembers the originals
  const auto on = sparxie::update_app(env, 0, { { "cover", "custom" }, { "badge", true }, { "image-path", own.string() }, { "hero", own_hero.string() } });
  ASSERT_TRUE(on.status.ok) << on.status.error;
  app = read_apps(env.apps_file)["apps"][0];
  const fs::path made = app["image-path"].get<std::string>();
  const fs::path made_hero = app["hero"].get<std::string>();
  EXPECT_NE(made, own);
  EXPECT_EQ(made.parent_path(), env.host_data / "posters" / "custom");
  EXPECT_NE(made.string().find("-badge"), std::string::npos);
  EXPECT_EQ(made_hero.extension(), ".jpg") << "a JPEG stays a JPEG";
  EXPECT_EQ(app["sparxie"]["customImage"], own.string());
  EXPECT_EQ(app["sparxie"]["customHero"], own_hero.string());
  const auto d = delta(read_image(own), read_image(made));
  EXPECT_GT(d.count, 0);
  EXPECT_GE(d.x0, sparxie::badge_box(400, 600).x - 400 / 8);
  EXPECT_LT(d.y1, 600 / 3);
  // the person's own file is never modified
  EXPECT_EQ(read_image(own).px[0], 90);
  // the editor sends back what it was given (the badged copy): the original stays the source and nothing is badged twice
  const auto again = sparxie::update_app(env, 0, { { "cover", "custom" }, { "image-path", made.string() }, { "hero", made_hero.string() } });
  ASSERT_TRUE(again.status.ok) << again.status.error;
  app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], made.string());
  EXPECT_EQ(app["sparxie"]["customImage"], own.string());
  // off again: the original is the poster, the block forgets it
  const auto off = sparxie::update_app(env, 0, { { "badge", false } });
  ASSERT_TRUE(off.status.ok) << off.status.error;
  app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], own.string());
  EXPECT_EQ(app["hero"], own_hero.string());
  EXPECT_FALSE(app["sparxie"].contains("customImage"));
}

TEST(BadgePosters, ACustomAddressOrAnUnreadableFileKeepsItsPicture) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-custom-url";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  auto custom = app_of("touch", "custom", { { "badge", true } });
  custom["image-path"] = "https://example.invalid/poster.png";
  custom["hero"] = (root / "missing.png").string();
  put_apps(env, { custom });
  EXPECT_EQ(sparxie::attach_official_covers(env), 0);
  const auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], "https://example.invalid/poster.png");
  EXPECT_EQ(app["hero"], (root / "missing.png").string());
}

TEST(BadgePosters, ACoverChangedBehindTheBadgeIsBadgedAgainAndTheOldCopyGoes) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-custom-changed";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  const fs::path own = root / "mine" / "poster.png";
  write_file(own, sparxie::encode_png(solid(400, 600, 90, 20, 20)));
  auto custom = app_of("touch", "custom", { { "badge", true } });
  custom["image-path"] = own.string();
  put_apps(env, { custom });
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  const fs::path first = read_apps(env.apps_file)["apps"][0]["image-path"].get<std::string>();
  ASSERT_TRUE(fs::exists(first));
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "the same source: the same copy";
  write_file(own, sparxie::encode_png(solid(500, 750, 20, 20, 90)));
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  const fs::path second = read_apps(env.apps_file)["apps"][0]["image-path"].get<std::string>();
  EXPECT_NE(first, second);
  EXPECT_FALSE(fs::exists(first)) << "one copy per source";
  EXPECT_EQ(read_image(second).w, 500);
}

TEST(BadgePosters, TheHostsDefaultForANewAppIsOnForTheOfficialCoverAndOffForACustomOne) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-create";
  fs::remove_all(root);
  fs::create_directories(root / "game" / "YuanShen_Data" / "Plugins");
  std::ofstream(root / "game" / "YuanShen.exe") << "x";
  std::ofstream(root / "game" / "YuanShen_Data" / "Plugins" / "PCGameSDK.dll") << "x";
  sparxie::HostEnv env = env_at(root);
  fs::create_directories(root / "install" / "components" / "sparxie");
  std::ofstream(root / "install" / "components" / "sparxie" / "Sparxie.exe") << "x";
  {
    sparxie::json body;
    body["version"] = "0.9.0";
    std::ofstream(root / "install" / "components" / "sparxie" / "component.json") << body.dump();
  }
  env.process_running = [](const std::vector<std::string> &) { return false; };
  const fs::path game = root / "game" / "YuanShen.exe";
  const auto official = sparxie::create_app(env, { { "path", game.string() }, { "mode", "touch" }, { "name", "A" } });
  ASSERT_TRUE(official.status.ok) << official.status.error;
  EXPECT_EQ(official.body["app"]["sparxie"]["badge"], true);
  const auto own = root / "mine.png";
  write_file(own, sparxie::encode_png(solid(100, 150, 1, 2, 3)));
  const auto custom = sparxie::create_app(env, { { "path", game.string() }, { "mode", "touch" }, { "name", "B" }, { "cover", "custom" }, { "image-path", own.string() } });
  ASSERT_TRUE(custom.status.ok) << custom.status.error;
  EXPECT_EQ(custom.body["app"]["sparxie"]["badge"], false);
  EXPECT_EQ(custom.body["app"]["image-path"], own.string()) << "the custom cover is the poster as given";
  const auto chosen = sparxie::create_app(env, { { "path", game.string() }, { "mode", "touch" }, { "name", "C" }, { "badge", false } });
  ASSERT_TRUE(chosen.status.ok) << chosen.status.error;
  EXPECT_EQ(chosen.body["app"]["sparxie"]["badge"], false);
  const auto pc = sparxie::create_app(env, { { "path", game.string() }, { "mode", "pc" }, { "name", "D" } });
  ASSERT_TRUE(pc.status.ok) << pc.status.error;
  EXPECT_FALSE(pc.body["app"]["sparxie"].contains("badge"));
  // what the file says is what a later read believes: a stringified flag (the generic writer) is read back as the boolean
  sparxie::json stringified = app_of("touch", "official", { { "badge", "false" } })["sparxie"];
  EXPECT_TRUE(sparxie::normalize_block(stringified));
  EXPECT_EQ(stringified["badge"], false);
}

TEST(BadgeBlank, ABlankBackgroundPublishesNoBadgedVariantEither) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-blank";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, { app_of("touch", "official") });
  std::vector<std::string> warnings;
  env.warn = [&](const std::string &text) { warnings.push_back(text); };
  sparxie::ArtUrls urls;
  urls.version = "v1";
  EXPECT_FALSE(sparxie::publish_art(env, "genshin-cn", urls, solid(2560, 1440, 0, 0, 0), block_logo(225, 80), {}).status.ok);
  EXPECT_FALSE(fs::exists(env.covers_dir));
  EXPECT_LT(sparxie::publish_hero(env, "genshin-cn", solid(2560, 1440, 0, 0, 0), block_logo(225, 80)), 0);
  EXPECT_FALSE(fs::exists(poster(env, "hero.jpg")));
  EXPECT_FALSE(fs::exists(poster(env, "hero-badge.jpg")));
  EXPECT_FALSE(warnings.empty());
}

TEST(BadgeBlank, TheSelfHealRemovesABlankBadgedHeroAndABlankBadgedCoverAndTheOtherWayRound) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-heal";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, { app_of("touch", "official") });
  publish(env);
  ASSERT_TRUE(fs::exists(badged_cover(env)));
  ASSERT_TRUE(fs::exists(poster(env, "hero-badge.jpg")));
  const auto good_cover = fs::file_size(badged_cover(env));
  // a black badged hero and a black badged cover (the badge in the corner does not make a picture of them)
  const sparxie::Rgba black_hero = sparxie::with_badge(solid(1920, 1080, 0, 0, 0), sparxie::badge_mark());
  ASSERT_TRUE(sparxie::image_is_blank(black_hero));
  write_file(poster(env, "hero-badge.jpg"), sparxie::encode_jpeg(black_hero, 91));
  write_file(badged_cover(env), sparxie::encode_png(sparxie::with_badge(solid(600, 900, 0, 0, 0), sparxie::badge_mark())));
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  // the blank badged cover is made again from the good plain one; the app's hero falls back to the plain hero until the refresh composes the badged one
  EXPECT_EQ(fs::file_size(badged_cover(env)), good_cover);
  EXPECT_FALSE(fs::exists(poster(env, "hero-badge.jpg")));
  auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], badged_cover(env).string());
  EXPECT_EQ(app["hero"], poster(env, "hero.jpg").string());
  // the refresh (cached background and logo on disk) composes it again
  const fs::path cache = env.host_data / "posters" / "genshin-cn";
  write_file(cache / "v1-background.bin", sparxie::encode_jpeg(textured(2560, 1440), 95));
  auto decode = [](std::string_view bytes) { return sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()); };
  const auto none = [](const std::string &) -> std::optional<std::string> { return std::nullopt; };
  sparxie::refresh_official(env, none, decode);
  EXPECT_TRUE(fs::exists(poster(env, "hero-badge.jpg"))) << "composed again from the cached pictures";
  app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["hero"], poster(env, "hero-badge.jpg").string());
  // a blank plain hero takes its badged sibling with it
  write_file(poster(env, "hero.jpg"), sparxie::encode_jpeg(solid(1920, 1080, 0, 0, 0), 91));
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  EXPECT_FALSE(fs::exists(poster(env, "hero.jpg")));
  EXPECT_FALSE(fs::exists(poster(env, "hero-badge.jpg")));
}

// ---- 2026-11-12: the badge follows the input, migrated copies of the official covers, applications of the first builds ----
namespace {
  sparxie::json
  options_of(const std::string &input) {
    return { { "input", input } };
  }
}  // namespace

TEST(BadgeInput, OnlyAnApplicationWithTheTouchInputCarriesTheBadge) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-input";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, {
    app_of("touch", "official", { { "options", options_of("touch") } }),
    app_of("touch", "official", { { "options", options_of("kbm") }, { "badge", true } }),
    app_of("touch", "official", { { "options", options_of("auto") }, { "badge", true } }),  // a stored automatic input (removed): the host migrates it at start; until then it is not the touch input
    app_of("touch", "official"),
    app_of("touch", "official", { { "options", options_of("touch") }, { "badge", false } }),
  });
  publish(env);
  const auto apps = read_apps(env.apps_file)["apps"];
  EXPECT_EQ(apps[0]["image-path"], badged_cover(env).string()) << "touch input: the badge is on by default";
  EXPECT_EQ(apps[1]["image-path"], plain_cover(env).string()) << "keyboard, mouse and gamepad through Sparxie's PC session: the plain poster, whatever `badge` says";
  EXPECT_EQ(apps[1]["hero"], poster(env, "hero.jpg").string());
  EXPECT_EQ(apps[2]["image-path"], plain_cover(env).string()) << "a stored automatic input is not the touch input: the plain poster until it is migrated";
  EXPECT_EQ(apps[3]["image-path"], badged_cover(env).string()) << "no stored input is the touch input";
  EXPECT_EQ(apps[4]["image-path"], plain_cover(env).string()) << "the person's own off wins over the default";
}

TEST(BadgeInput, ThePersonsOwnSwitchWinsAndAnInputChangeMovesThePoster) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-input-edit";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, { app_of("touch", "official", { { "options", options_of("touch") }, { "profile", "" } }) });
  publish(env);
  EXPECT_EQ(read_apps(env.apps_file)["apps"][0]["image-path"], badged_cover(env).string());
  const auto kbm = sparxie::update_app(env, 0, { { "options", options_of("kbm") } });
  ASSERT_TRUE(kbm.status.ok) << kbm.status.error;
  EXPECT_EQ(read_apps(env.apps_file)["apps"][0]["image-path"], plain_cover(env).string()) << "the input moved the poster to the plain one";
  const auto back = sparxie::update_app(env, 0, { { "options", options_of("touch") } });
  ASSERT_TRUE(back.status.ok) << back.status.error;
  EXPECT_EQ(read_apps(env.apps_file)["apps"][0]["image-path"], badged_cover(env).string());
  const auto off = sparxie::update_app(env, 0, { { "badge", false } });
  ASSERT_TRUE(off.status.ok) << off.status.error;
  EXPECT_EQ(read_apps(env.apps_file)["apps"][0]["image-path"], plain_cover(env).string());
}

TEST(BadgeInput, AnApplicationThatWasBadgedByDefaultLosesItAtHostStartWhenItsInputIsKeyboardAndMouse) {
  const fs::path root = fs::temp_directory_path() / "sparxie-badge-input-start";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  // a captured 星穹铁道: stored as a touch application with the input converted to kbm by an earlier save, and badged
  put_apps(env, { app_of("touch", "official", { { "options", options_of("kbm") }, { "badge", true } }) });
  publish(env);
  {
    auto apps = read_apps(env.apps_file);
    apps["apps"][0]["image-path"] = badged_cover(env).string();
    apps["apps"][0]["hero"] = poster(env, "hero-badge.jpg").string();
    std::ofstream(env.apps_file) << apps.dump();
  }
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  const auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["image-path"], plain_cover(env).string());
  EXPECT_EQ(app["hero"], poster(env, "hero.jpg").string());
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "a second start changes nothing";
}

TEST(MigratedCovers, ACopyOfAnOfficialCoverIsOfficialAgainAndARealCustomImageStaysCustom) {
  const fs::path root = fs::temp_directory_path() / "sparxie-migrated-cover";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  put_apps(env, {});
  publish(env);
  // the installer's migrate-images.ps1 copied the official cover (and, in another app, the badged one) to app_<hash>_<time>.png
  const fs::path copy_plain = env.covers_dir / "app_6de14b94_1794411249.png";
  const fs::path copy_badged = env.covers_dir / "app_ff8a9a3d_1794411249.png";
  fs::copy_file(plain_cover(env), copy_plain);
  fs::copy_file(badged_cover(env), copy_badged);
  const fs::path own = env.covers_dir / "app_deadbeef_1794411249.png";
  write_file(own, sparxie::encode_png(textured(600, 900)));
  sparxie::json touch_app = app_of("touch", "official", { { "options", options_of("touch") } });
  touch_app["image-path"] = "app_6de14b94_1794411249.png";  // a bare file name, as the installer wrote it
  sparxie::json kbm_app = app_of("touch", "official", { { "options", options_of("kbm") } });
  kbm_app["image-path"] = copy_badged.string();
  sparxie::json own_app = app_of("touch", "official", { { "options", options_of("touch") } });
  own_app["image-path"] = own.string();
  sparxie::json custom_app = app_of("touch", "custom", { { "options", options_of("touch") } });
  custom_app["image-path"] = copy_plain.string();
  put_apps(env, { touch_app, kbm_app, own_app, custom_app });
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  const auto apps = read_apps(env.apps_file)["apps"];
  EXPECT_EQ(apps[0]["image-path"], badged_cover(env).string()) << "a copy of the plain cover, touch input: the badged official cover";
  EXPECT_EQ(apps[1]["image-path"], plain_cover(env).string()) << "a copy of the badged cover, keyboard and mouse: the plain official cover";
  EXPECT_EQ(apps[2]["image-path"], own.string()) << "a picture of the person's own stays";
  EXPECT_EQ(apps[3]["image-path"], copy_plain.string()) << "a custom cover is never replaced, even by identical bytes";
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "a second start changes nothing";
}
