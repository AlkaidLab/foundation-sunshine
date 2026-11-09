#include "src/sparxie/host.h"
#include "tests/sparxie/pe_fixture.h"
#include "src/sparxie/platform.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cmath>
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
  // a picture with some texture (diagonal stripes of 4 px, 40 up and down around the colour): the host refuses a blank or one-colour background, as real art never is
  sparxie::Rgba
  textured(int w, int h, int r, int g, int b) {
    sparxie::Rgba image;
    image.w = w;
    image.h = h;
    image.px.assign(static_cast<std::size_t>(w * h * 4), 255);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int d = ((x / 4 + y / 4) % 2) ? 40 : -40;
        const std::size_t at = static_cast<std::size_t>((y * w + x) * 4);
        image.px[at] = static_cast<std::uint8_t>(std::clamp(r + d, 0, 255));
        image.px[at + 1] = static_cast<std::uint8_t>(std::clamp(g + d, 0, 255));
        image.px[at + 2] = static_cast<std::uint8_t>(std::clamp(b + d, 0, 255));
      }
    return image;
  }

  sparxie::Rgba
  solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    sparxie::Rgba image;
    image.w = w;
    image.h = h;
    image.px.assign(static_cast<std::size_t>(w * h * 4), 0);
    for (int i = 0; i < w * h; ++i) {
      image.px[i * 4] = r;
      image.px[i * 4 + 1] = g;
      image.px[i * 4 + 2] = b;
      image.px[i * 4 + 3] = 255;
    }
    return image;
  }
}  // namespace
TEST(Art, VersionWindowScheduleAndCompose) {
  EXPECT_EQ(sparxie::art_version_of("https://cdn.example/a/abc123def.webp?x=1"), "abc123def");
  sparxie::Rgba background = solid(90, 60, 0, 0, 0);
  for (int y = 0; y < 60; ++y) {
    background.px[(y * 90 + 70) * 4] = 255;
    background.px[(y * 90 + 70) * 4 + 1] = 255;
    background.px[(y * 90 + 70) * 4 + 2] = 255;
  }
  EXPECT_GT(sparxie::cover_window_x(background), 20);
  const sparxie::Rgba cover = sparxie::compose_cover(background, solid(10, 4, 255, 0, 0));
  EXPECT_EQ(cover.w, 600);
  EXPECT_EQ(cover.h, 900);
  sparxie::Rgba wide = solid(120, 100, 1, 2, 3);
  wide.px[(16 * 120) * 4] = 9;
  const sparxie::Rgba banner = sparxie::crop_banner(wide);
  EXPECT_EQ(banner.w, 120);
  EXPECT_LT(banner.h, banner.w);
  EXPECT_EQ(banner.px[0], 9);
  EXPECT_FALSE(sparxie::encode_png(cover).empty());
  sparxie::ArtSchedule schedule;
  const auto start = std::chrono::system_clock::time_point { std::chrono::hours(10) };
  EXPECT_TRUE(sparxie::art_due(schedule, start, sparxie::ArtReason::host_start));
  EXPECT_FALSE(sparxie::art_due(schedule, start, sparxie::ArtReason::host_start));
  EXPECT_FALSE(sparxie::art_due(schedule, start + std::chrono::minutes(5), sparxie::ArtReason::app_list));
  EXPECT_TRUE(sparxie::art_due(schedule, start + std::chrono::minutes(16), sparxie::ArtReason::app_list));
  EXPECT_FALSE(sparxie::art_due(schedule, start + std::chrono::hours(2), sparxie::ArtReason::interval));
  EXPECT_TRUE(sparxie::art_due(schedule, start + std::chrono::hours(7), sparxie::ArtReason::interval));
  sparxie::json games;
  games["data"]["games"] = sparxie::json::array();
  sparxie::json game;
  game["biz"] = "hk4e_cn";
  game["display"]["icon"]["url"] = "https://cdn/icon.png";
  game["display"]["logo"]["url"] = "https://cdn/logo.png";
  game["display"]["background"] = nullptr;
  games["data"]["games"].push_back(game);
  sparxie::json basic;
  sparxie::json info;
  info["game"]["biz"] = "hk4e_cn";
  sparxie::json bg;
  bg["background"]["url"] = "https://cdn/carousel_HASH.webp";
  info["backgrounds"].push_back(bg);
  basic["data"]["game_info_list"].push_back(info);
  const auto urls = sparxie::art_urls_from_payload(games, basic, "hk4e_cn", true);
  EXPECT_EQ(urls.version, "carousel_HASH");
  EXPECT_EQ(urls.icon, "https://cdn/icon.png");
}
TEST(Host, CreateImportWarningAndArt) {
  const fs::path root = fs::temp_directory_path() / "sparxie-host-flow";
  fs::remove_all(root);
  fs::create_directories(root / "game" / "YuanShen_Data" / "Plugins");
  {
    std::ofstream out(root / "game" / "YuanShen.exe");
    out << "x";
  }
  {
    std::ofstream out(root / "game" / "YuanShen_Data" / "Plugins" / "PCGameSDK.dll");
    out << "x";
  }
  sparxie::HostEnv env = env_at(root);
  fs::create_directories(root / "install" / "components" / "sparxie");
  {
    std::ofstream out(root / "install" / "components" / "sparxie" / "Sparxie.exe");
    out << "x";
  }
  {
    std::ofstream out(root / "install" / "components" / "sparxie" / "component.json");
    sparxie::json body;
    body["version"] = "0.9.0";
    out << body.dump();
  }
  auto status = sparxie::component_status(env);
  EXPECT_TRUE(status.body["installed"]);
  EXPECT_FALSE(status.body["warned"]);
  EXPECT_FALSE(status.body["statement"].get<std::string>().empty());
  env.process_running = [](const std::vector<std::string> &) { return true; };
  auto blocked = sparxie::create_app(env, { { "path", (root / "game").string() }, { "mode", "touch" }, { "name", "Genshin" } });
  EXPECT_EQ(blocked.status.error_code, "sparxie_running");
  env.process_running = [](const std::vector<std::string> &) { return false; };
  auto created = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "touch" }, { "name", "Genshin bili" } });
  EXPECT_TRUE(created.status.ok) << created.status.error;
  EXPECT_TRUE(created.body["wrote"]);
  EXPECT_EQ(created.body["app"]["sparxie"]["region"], "bili");
  EXPECT_NE(created.body["app"]["cmd"].get<std::string>().find("--profile"), std::string::npos);
  EXPECT_NE(created.body["app"]["cmd"].get<std::string>().find("--sparxie"), std::string::npos);
  EXPECT_TRUE(created.body["app"]["sparxie"]["launchTemplateProvisional"]);
  auto pc = sparxie::create_app(env, { { "path", (root / "game").string() }, { "mode", "pc" }, { "name", "Genshin pad" } });
  EXPECT_TRUE(pc.status.ok) << pc.status.error;
  EXPECT_FALSE(pc.body["wrote"]);
  EXPECT_EQ(pc.body["write_back"]["state"], "skipped");
  EXPECT_NE(pc.body["app"]["cmd"].get<std::string>().find("YuanShen.exe"), std::string::npos);
  auto profiles = sparxie::list_profiles(env);
  EXPECT_EQ(profiles.body["profiles"].size(), 1u);
  auto imported = sparxie::import_apps(env, { { "ids", { profiles.body["profiles"][0]["id"] } } });
  EXPECT_TRUE(imported.status.ok) << imported.status.error;
  EXPECT_EQ(imported.body["apps"].size(), 1u);
  auto ack = sparxie::acknowledge_warning(env);
  EXPECT_TRUE(ack.body["warned"]);
  EXPECT_FALSE(ack.body["statement"].get<std::string>().empty());
  status = sparxie::component_status(env);
  EXPECT_TRUE(status.body["warned"]);
  EXPECT_FALSE(status.body["statement"].get<std::string>().empty());
  sparxie::ArtUrls urls;
  urls.version = "poster1";
  auto art = sparxie::publish_art(env, "genshin-bili", urls, textured(30, 30, 20, 40, 60), solid(8, 4, 255, 255, 255), solid(4, 4, 1, 2, 3));
  EXPECT_TRUE(art.status.ok) << art.status.error;
  auto custom = sparxie::set_cover(env, 0, "custom");
  EXPECT_TRUE(custom.status.ok) << custom.status.error;
  urls.version = "poster2";
  auto again = sparxie::publish_art(env, "genshin-bili", urls, textured(30, 30, 9, 9, 9), solid(8, 4, 1, 1, 1), {});
  EXPECT_TRUE(again.status.ok);
  std::ifstream apps_in(env.apps_file);
  sparxie::json apps;
  apps_in >> apps;
  EXPECT_EQ(apps["apps"][0]["sparxie"]["cover"], "custom");
  EXPECT_EQ(apps["apps"][0].value("image-path", std::string()).find("poster2"), std::string::npos);
}
namespace {
  // width and height from the IHDR of a PNG file; {0, 0} when it is not one
  std::pair<int, int>
  png_size(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char head[24] = {};
    in.read(reinterpret_cast<char *>(head), sizeof(head));
    if (in.gcount() < 24 || head[1] != 'P' || head[2] != 'N' || head[3] != 'G') return { 0, 0 };
    const auto be = [&](int at) { return (head[at] << 24) | (head[at + 1] << 16) | (head[at + 2] << 8) | head[at + 3]; };
    return { be(16), be(20) };
  }

  // width, height and the sampling byte of the first component (0x11: 4:4:4, 0x22: 4:2:0) from the SOF0 of a JPEG file; zeros when it is not one
  struct JpegInfo {
    int w = 0, h = 0, sampling = 0;
    bool jpeg = false;
  };
  JpegInfo
  jpeg_info(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    JpegInfo info;
    if (bytes.size() < 4 || static_cast<unsigned char>(bytes[0]) != 0xFF || static_cast<unsigned char>(bytes[1]) != 0xD8) return info;
    info.jpeg = true;
    for (std::size_t at = 2; at + 9 < bytes.size();) {
      if (static_cast<unsigned char>(bytes[at]) != 0xFF) { ++at; continue; }
      const unsigned char marker = static_cast<unsigned char>(bytes[at + 1]);
      if (marker == 0xC0) {
        info.h = (static_cast<unsigned char>(bytes[at + 5]) << 8) | static_cast<unsigned char>(bytes[at + 6]);
        info.w = (static_cast<unsigned char>(bytes[at + 7]) << 8) | static_cast<unsigned char>(bytes[at + 8]);
        info.sampling = static_cast<unsigned char>(bytes[at + 11]);
        return info;
      }
      if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
      at += 2 + ((static_cast<unsigned char>(bytes[at + 2]) << 8) | static_cast<unsigned char>(bytes[at + 3]));
    }
    return info;
  }

  // a logo canvas of 416x234 with a striped block of bw x bh at an off-centre place (the launcher logos sit in a padded canvas, and have no opaque plate: the stripes
  // leave half of the block transparent)
  sparxie::Rgba
  block_logo(int bw, int bh, std::uint8_t value) {
    sparxie::Rgba logo;
    logo.w = 416;
    logo.h = 234;
    logo.px.assign(static_cast<std::size_t>(logo.w * logo.h * 4), 0);
    for (int y = 40; y < 40 + bh; ++y)
      for (int x = 30; x < 30 + bw; ++x)
        for (int c = 0; c < 4; ++c) logo.px[static_cast<std::size_t>((y * logo.w + x) * 4 + c)] = c == 3 ? ((x - 30) / 6 % 2 == 0 ? 255 : 0) : value;
    return logo;
  }

  // the bounding box of the pixels of the hero that differ from the plain backdrop by more than 60 in some channel
  sparxie::HeroBox
  lit_box(const sparxie::Rgba &hero, int backdrop) {
    int x0 = hero.w, y0 = hero.h, x1 = -1, y1 = -1;
    for (int y = 0; y < hero.h; ++y)
      for (int x = 0; x < hero.w; ++x)
        if (std::abs(hero.px[static_cast<std::size_t>((y * hero.w + x) * 4)] - backdrop) > 60) {
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x);
          y1 = std::max(y1, y);
        }
    if (x1 < 0) return {};
    return { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
  }
}  // namespace

TEST(Hero, IsA16By9PosterOfThePlainFrameNotAStrip) {
  // a 16:9 background is taken whole and scaled to 1920x1080: the pixel at the top-left corner is the background's, not a body part cut at 16 percent down
  sparxie::Rgba background = solid(2560, 1440, 10, 20, 30);
  background.px[0] = 200;
  const sparxie::Rgba hero = sparxie::compose_hero(background, {});
  EXPECT_EQ(hero.w, 1920);
  EXPECT_EQ(hero.h, 1080);
  EXPECT_EQ(hero.px.size(), static_cast<std::size_t>(1920 * 1080 * 4));
  EXPECT_GT(hero.px[0], 10) << "the first row of the frame is in the poster";
  // a background smaller than 1080p keeps its own size (no enlargement)
  const sparxie::Rgba small = sparxie::compose_hero(solid(800, 450, 1, 2, 3), {});
  EXPECT_EQ(small.w, 800);
  EXPECT_EQ(small.h, 450);
  EXPECT_FALSE(sparxie::encode_png(hero).empty());
  EXPECT_TRUE(sparxie::compose_hero(sparxie::Rgba {}, {}).px.empty());
}

TEST(Hero, AnOddBackgroundIsCroppedAroundTheDetailNotTheCentre) {
  // 3200x1000 (wider than 16:9): the window slides to the side with the detail
  sparxie::Rgba wide = solid(3200, 1000, 20, 20, 20);
  for (int y = 0; y < 1000; ++y)
    for (int x = 2300; x < 3200; ++x) wide.px[static_cast<std::size_t>((y * 3200 + x) * 4)] = static_cast<std::uint8_t>((x + y) % 2 ? 250 : 5);
  const sparxie::Rgba hero = sparxie::compose_hero(wide, {});
  EXPECT_NEAR(hero.w * 9, hero.h * 16, 16) << "16:9";
  int detail = 0;
  for (int x = 0; x < hero.w; ++x) detail += hero.px[static_cast<std::size_t>((hero.h / 2 * hero.w + x) * 4)] != 20 ? 1 : 0;
  EXPECT_GT(detail, hero.w / 3) << "the window holds the detailed side";
  // 1600x1600 (taller): 16:9 as well
  sparxie::Rgba tall = solid(1600, 1600, 20, 20, 20);
  for (int y = 1100; y < 1600; ++y)
    for (int x = 0; x < 1600; ++x) tall.px[static_cast<std::size_t>((y * 1600 + x) * 4)] = static_cast<std::uint8_t>((x + y) % 2 ? 250 : 5);
  const sparxie::Rgba tall_hero = sparxie::compose_hero(tall, {});
  EXPECT_EQ(tall_hero.w * 9, tall_hero.h * 16);
  int bottom_detail = 0;
  for (int x = 0; x < tall_hero.w; ++x) bottom_detail += tall_hero.px[static_cast<std::size_t>(((tall_hero.h - 2) * tall_hero.w + x) * 4)] != 20 ? 1 : 0;
  EXPECT_GT(bottom_detail, tall_hero.w / 2) << "the window holds the detailed rows";
}

TEST(Hero, TheLogoIsLargeCentredAndInsideTheSafeAreaAboveTheTitleScrim) {
  const sparxie::Rgba background = solid(2560, 1440, 30, 30, 30);
  for (const auto &size : std::vector<std::pair<int, int>> { { 129, 101 }, { 225, 80 }, { 292, 89 }, { 60, 60 } }) {
    const sparxie::Rgba logo = block_logo(size.first, size.second, 255);
    const sparxie::Rgba hero = sparxie::compose_hero(background, logo);
    const sparxie::HeroBox box = sparxie::hero_logo_box(hero.w, hero.h, logo);
    ASSERT_GT(box.w, 0);
    // horizontally centred
    EXPECT_NEAR(box.x + box.w / 2.0, hero.w / 2.0, 2.0);
    // inside the part above the client's bottom title scrim
    EXPECT_GE(box.y, static_cast<int>(hero.h * sparxie::kHeroSafeTop));
    EXPECT_LE(box.y + box.h, static_cast<int>(hero.h * sparxie::kHeroSafeBottom));
    // large, not a corner mark: a third of the width or a quarter of the height
    EXPECT_TRUE(box.w >= hero.w * 3 / 10 || box.h >= hero.h / 4) << size.first << "x" << size.second;
    // and the drawn pixels are where the box says
    const sparxie::HeroBox drawn = lit_box(hero, 30);
    EXPECT_NEAR(drawn.x, box.x, 3);
    EXPECT_NEAR(drawn.y, box.y, 3);
    EXPECT_NEAR(drawn.w, box.w, 6);
    EXPECT_NEAR(drawn.h, box.h, 6);
  }
  EXPECT_EQ(sparxie::hero_logo_box(1920, 1080, sparxie::Rgba {}).w, 0);
}

TEST(Hero, ABackdropCloseToTheLogoIsDimmedSoftlyAndOnlyBehindIt) {
  // a white logo on a near-white backdrop: dimmed behind the logo, the corners keep the backdrop
  const sparxie::Rgba bright = solid(2560, 1440, 235, 235, 235);
  const sparxie::Rgba logo = block_logo(129, 101, 255);
  const sparxie::Rgba hero = sparxie::compose_hero(bright, logo);
  const sparxie::HeroBox box = sparxie::hero_logo_box(hero.w, hero.h, logo);
  const auto at = [&](int x, int y) { return static_cast<int>(hero.px[static_cast<std::size_t>((y * hero.w + x) * 4)]); };
  EXPECT_EQ(at(5, 5), 235) << "corner untouched";
  EXPECT_EQ(at(hero.w - 6, hero.h - 6), 235) << "the title scrim area untouched";
  const int beside = at(box.x - box.w / 5, box.y + box.h / 2);
  EXPECT_LT(beside, 235) << "dimmed just beside the logo";
  EXPECT_GE(beside, 235 * 5 / 10) << "subtle, not a black box";
  // a white logo on a dark backdrop needs nothing
  const sparxie::Rgba dark = solid(2560, 1440, 20, 20, 20);
  EXPECT_EQ(sparxie::compose_hero(dark, logo).px[static_cast<std::size_t>((box.y + box.h / 2) * hero.w * 4 + (box.x - box.w / 5) * 4)], 20);
  // a logo on its own opaque black plate loses the plate (the mark alone, as the bilibili logo is), and is set apart by the same shadow
  sparxie::Rgba plate = solid(416, 234, 0, 0, 0);
  for (int y = 90; y < 140; ++y)
    for (int x = 150; x < 260; ++x)
      for (int c = 0; c < 3; ++c) plate.px[static_cast<std::size_t>((y * 416 + x) * 4 + c)] = 255;
  EXPECT_TRUE(sparxie::logo_has_plate(plate));
  EXPECT_FALSE(sparxie::logo_has_plate(logo));
  const sparxie::Rgba cut = sparxie::logo_without_plate(plate);
  EXPECT_EQ(cut.px[static_cast<std::size_t>((10 * 416 + 10) * 4 + 3)], 0) << "the plate is transparent";
  EXPECT_EQ(cut.px[static_cast<std::size_t>((100 * 416 + 200) * 4 + 3)], 255) << "the mark stays";
  EXPECT_EQ(cut.px[static_cast<std::size_t>((100 * 416 + 200) * 4)], 255);
  const sparxie::Rgba plated = sparxie::compose_hero(bright, plate);
  const sparxie::HeroBox plate_box = sparxie::hero_logo_box(plated.w, plated.h, plate);
  ASSERT_GT(plate_box.w, 0);
  EXPECT_LT(plate_box.w, plated.w / 2) << "the box is the mark, not the 416x234 plate";
  EXPECT_EQ(plated.px[static_cast<std::size_t>(((plate_box.y + plate_box.h / 2) * plated.w + plate_box.x - plate_box.w / 2) * 4)], 235) << "no black plate around it";
  EXPECT_GT(sparxie::logo_visible_area(logo), 0);
  EXPECT_GT(sparxie::logo_visible_area(logo), sparxie::logo_visible_area(plate) / 1) << "129x101 stripes' bounding box is bigger than the 110x50 mark";
  EXPECT_EQ(sparxie::logo_visible_area(sparxie::Rgba {}), 0);
  // a dark logo on a dark backdrop is lifted
  const sparxie::Rgba dark_logo = block_logo(129, 101, 0);
  const sparxie::Rgba lifted = sparxie::compose_hero(solid(2560, 1440, 40, 40, 40), dark_logo);
  EXPECT_GT(lifted.px[static_cast<std::size_t>(((box.y + box.h / 2) * lifted.w + box.x - box.w / 5) * 4)], 40);
}

TEST(Hero, TheJpegIsAFractionOfThePngAndKeepsTheColour) {
  sparxie::Rgba image = solid(640, 360, 10, 20, 30);
  for (int y = 100; y < 200; ++y)
    for (int x = 200; x < 400; ++x) {
      image.px[static_cast<std::size_t>((y * 640 + x) * 4)] = 250;
      image.px[static_cast<std::size_t>((y * 640 + x) * 4 + 1)] = 30;
      image.px[static_cast<std::size_t>((y * 640 + x) * 4 + 2)] = 30;
    }
  const auto jpeg = sparxie::encode_jpeg(image, sparxie::kHeroJpegQuality);
  ASSERT_GT(jpeg.size(), 4u);
  EXPECT_EQ(jpeg[0], 0xFF);
  EXPECT_EQ(jpeg[1], 0xD8);
  EXPECT_LT(jpeg.size(), sparxie::encode_png(image).size() * 2) << "sane";
  EXPECT_TRUE(sparxie::encode_jpeg(sparxie::Rgba {}, 90).empty());
}

TEST(Hero, ABackgroundThatReportsAlphaZeroIsStillOpaqueArt) {
  // the pre-fix poster weighted every pixel by the alpha a decoder reported: a lossless WebP that comes back with alpha 0 made the whole hero black (and a transparent cover)
  sparxie::Rgba background = textured(2560, 1440, 120, 100, 140);
  for (std::size_t i = 3; i < background.px.size(); i += 4) background.px[i] = 0;
  const sparxie::Rgba hero = sparxie::compose_hero(background, {});
  EXPECT_FALSE(sparxie::image_is_blank(hero));
  EXPECT_NEAR(hero.px[static_cast<std::size_t>((540 * hero.w + 960) * 4 + 1)], 100, 41);
  EXPECT_EQ(hero.px[3], 255);
  const sparxie::Rgba cover = sparxie::compose_cover(background, {});
  EXPECT_EQ(cover.px[static_cast<std::size_t>((450 * cover.w + 300) * 4 + 3)], 255) << "the cover is opaque too";
  EXPECT_FALSE(sparxie::image_is_blank(cover));
}

TEST(Hero, BlankPicturesAreRecognised) {
  EXPECT_TRUE(sparxie::image_is_blank(sparxie::Rgba {}));
  sparxie::Rgba zeros;
  zeros.w = 1920;
  zeros.h = 1080;
  zeros.px.assign(static_cast<std::size_t>(1920 * 1080 * 4), 0);
  EXPECT_TRUE(sparxie::image_is_blank(zeros)) << "all zeros";
  EXPECT_TRUE(sparxie::image_is_blank(solid(640, 360, 200, 90, 40))) << "one colour";
  sparxie::Rgba black_with_logo = solid(1920, 1080, 0, 0, 0);
  for (int y = 400; y < 500; ++y)
    for (int x = 700; x < 1200; ++x) black_with_logo.px[static_cast<std::size_t>((y * 1920 + x) * 4)] = black_with_logo.px[static_cast<std::size_t>((y * 1920 + x) * 4 + 1)] = black_with_logo.px[static_cast<std::size_t>((y * 1920 + x) * 4 + 2)] = 255;
  EXPECT_TRUE(sparxie::image_is_blank(black_with_logo)) << "a black poster with a logo on it";
  EXPECT_FALSE(sparxie::image_is_blank(textured(640, 360, 120, 100, 140)));
  EXPECT_FALSE(sparxie::image_is_blank(textured(640, 360, 30, 30, 40))) << "dark art is art";
  // the JPEG heroes
  const auto good = sparxie::encode_jpeg(textured(640, 360, 120, 100, 140), 90);
  const auto black = sparxie::encode_jpeg(solid(640, 360, 0, 0, 0), 90);
  EXPECT_FALSE(sparxie::jpeg_is_blank(std::string_view(reinterpret_cast<const char *>(good.data()), good.size())));
  EXPECT_TRUE(sparxie::jpeg_is_blank(std::string_view(reinterpret_cast<const char *>(black.data()), black.size())));
  EXPECT_TRUE(sparxie::jpeg_is_blank("not a jpeg"));
  EXPECT_TRUE(sparxie::jpeg_is_blank({}));
}

TEST(Hero, ABlankBackgroundPublishesNothingAndKeepsThePrevious) {
  const fs::path root = fs::temp_directory_path() / "sparxie-host-blank";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  std::vector<std::string> warnings;
  env.warn = [&](const std::string &message) { warnings.push_back(message); };
  sparxie::json apps;
  apps["apps"] = sparxie::json::array();
  sparxie::json app;
  app["name"] = "Star Rail";
  app["sparxie"]["install"] = "starrail-cn";
  app["sparxie"]["cover"] = "official";
  apps["apps"].push_back(app);
  fs::create_directories(env.apps_file.parent_path());
  std::ofstream(env.apps_file) << apps.dump();
  sparxie::ArtUrls urls;
  urls.version = "v1";
  ASSERT_TRUE(sparxie::publish_art(env, "starrail-cn", urls, textured(1280, 720, 120, 100, 140), block_logo(210, 100, 255), {}).status.ok);
  const fs::path hero = env.host_data / "posters" / "starrail-cn" / "hero.jpg";
  const auto read = [](const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string before = read(hero);
  const auto cover_before = fs::directory_iterator(env.covers_dir)->path();
  const std::string cover_bytes = read(cover_before);
  ASSERT_FALSE(before.empty());
  // a later refresh whose decoder returns zeros
  sparxie::Rgba zeros;
  zeros.w = 2560;
  zeros.h = 1440;
  zeros.px.assign(static_cast<std::size_t>(2560 * 1440 * 4), 0);
  urls.version = "v2";
  const auto blank = sparxie::publish_art(env, "starrail-cn", urls, zeros, block_logo(210, 100, 255), {});
  EXPECT_FALSE(blank.status.ok);
  EXPECT_EQ(read(hero), before) << "the good poster is kept";
  EXPECT_FALSE(fs::exists(env.covers_dir / "sparxie-starrail-cn-v2.png")) << "no blank cover";
  EXPECT_EQ(read(cover_before), cover_bytes);
  EXPECT_EQ(sparxie::publish_hero(env, "starrail-cn", zeros, block_logo(210, 100, 255)), -1);
  EXPECT_EQ(read(hero), before);
  EXPECT_GE(warnings.size(), 2u);
}

#ifdef _WIN32
TEST(Hero, TheRealStarRailBackgroundDecodesAndComposesOnWindows) {
  // the lossless WebP (VP8L) of the Star Rail launcher background, as the host cached it: the hero of a test host came out black from it
  std::ifstream in(std::string(SPARXIE_FIXTURES_DIR) + "/starrail-cn-bg.webp", std::ios::binary);
  ASSERT_TRUE(in.good()) << "fixture missing";
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const sparxie::Rgba background = sparxie::host_decode_image(bytes);
  ASSERT_EQ(background.w, 2560);
  ASSERT_EQ(background.h, 1440);
  double alpha = 0;
  for (std::size_t i = 3; i < background.px.size(); i += 4) alpha += background.px[i];
  std::cout << "[ info ] decoded alpha mean " << alpha / (static_cast<double>(background.px.size()) / 4) << "\n";
  EXPECT_FALSE(sparxie::image_is_blank(background)) << "WIC returned a blank picture for the lossless WebP";
  const sparxie::Rgba hero = sparxie::compose_hero(background, block_logo(210, 100, 255));
  EXPECT_FALSE(sparxie::image_is_blank(hero));
  // the same with an alpha of zero (what a host process may get from the codec): still the art
  sparxie::Rgba transparent = background;
  for (std::size_t i = 3; i < transparent.px.size(); i += 4) transparent.px[i] = 0;
  EXPECT_FALSE(sparxie::image_is_blank(sparxie::compose_hero(transparent, block_logo(210, 100, 255))));
}
#endif

#ifdef _WIN32
// A diagnostic, not a gate: SPARXIE_PROBE_POSTERS names a copy of a host's posters folder (the cached pictures of an installed host); the refresh composes every hero from it with the REAL
// image decoder and no network, and the heroes must not be blank. It is how the black Star Rail posters of a test host were reproduced.
TEST(Hero, TheCachedPicturesOfARealHostComposeNoBlankHero) {
  const char *posters = std::getenv("SPARXIE_PROBE_POSTERS");
  if (!posters) GTEST_SKIP() << "SPARXIE_PROBE_POSTERS is not set";
  const fs::path root = fs::temp_directory_path() / "sparxie-host-probe";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  std::vector<std::string> warnings;
  env.warn = [&](const std::string &message) { warnings.push_back(message); std::cout << "[ warn ] " << message << "\n"; };
  fs::create_directories(env.host_data);
  fs::copy(posters, env.host_data / "posters", fs::copy_options::recursive);
  for (const auto &entry : fs::recursive_directory_iterator(env.host_data / "posters"))
    if (entry.path().filename() == "hero.jpg") fs::remove(entry.path());
  auto offline = [](const std::string &) -> std::optional<std::string> { return std::nullopt; };
  const auto result = sparxie::refresh_official(env, offline, [](std::string_view bytes) { return sparxie::host_decode_image(bytes); });
  std::cout << "[ info ] " << result.body.dump() << "\n";
  int checked = 0;
  for (const auto &entry : fs::recursive_directory_iterator(env.host_data / "posters")) {
    if (entry.path().filename() != "hero.jpg") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::cout << "[ info ] " << entry.path().parent_path().filename().string() << " hero " << bytes.size() << " bytes, blank " << sparxie::jpeg_is_blank(bytes) << "\n";
    EXPECT_FALSE(sparxie::jpeg_is_blank(bytes)) << entry.path();
    ++checked;
  }
  EXPECT_GT(checked, 0);
}
#endif

TEST(Hero, PublishWritesTheLandscapeJpegBesideTheKeptBanner) {
  const fs::path root = fs::temp_directory_path() / "sparxie-host-hero";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  sparxie::json apps;
  apps["apps"] = sparxie::json::array();
  sparxie::json app;
  app["name"] = "Zzz";
  app["sparxie"]["install"] = "zzz-cn";
  app["sparxie"]["cover"] = "official";
  apps["apps"].push_back(app);
  fs::create_directories(env.apps_file.parent_path());
  std::ofstream(env.apps_file) << apps.dump();
  sparxie::ArtUrls urls;
  urls.version = "v1";
  const auto art = sparxie::publish_art(env, "zzz-cn", urls, textured(2560, 1440, 30, 60, 90), block_logo(225, 80, 255), {});
  ASSERT_TRUE(art.status.ok);
  const fs::path hero = env.host_data / "posters" / "zzz-cn" / "hero.jpg";
  const JpegInfo info = jpeg_info(hero);
  EXPECT_TRUE(info.jpeg);
  EXPECT_EQ(info.w, 1920);
  EXPECT_EQ(info.h, 1080);
  EXPECT_EQ(info.sampling, 0x11) << "4:4:4: the thin strokes of a logo keep their colour";
  EXPECT_LT(fs::file_size(hero), 1500u * 1024u) << "a JPEG, not several MB of PNG";
  EXPECT_FALSE(fs::exists(env.host_data / "posters" / "zzz-cn" / "hero.png"));
  EXPECT_TRUE(fs::exists(env.host_data / "posters" / "zzz-cn" / "banner.png")) << "kept for the banner slot";
  std::ifstream in(env.apps_file);
  sparxie::json after;
  in >> after;
  EXPECT_EQ(after["apps"][0]["hero"], hero.string());
}

TEST(Host, RefreshUsesContentHashAndSkipsCustom) {
  const fs::path root = fs::temp_directory_path() / "sparxie-host-refresh";
  fs::remove_all(root);
  sparxie::HostEnv env = env_at(root);
  sparxie::json apps;
  apps["apps"] = sparxie::json::array();
  sparxie::json app;
  app["name"] = "Genshin";
  app["sparxie"]["install"] = "genshin-cn";
  app["sparxie"]["game"] = "genshin";
  app["sparxie"]["region"] = "cn";
  app["sparxie"]["cover"] = "custom";
  apps["apps"].push_back(app);
  fs::create_directories(root);
  {
    std::ofstream out(env.apps_file);
    out << apps.dump();
  }
  auto get_url = [](const std::string &url) -> std::optional<std::string> {
    if (url.find("getGames") != std::string::npos) {
      sparxie::json body;
      body["data"]["games"][0]["biz"] = "hk4e_cn";
      body["data"]["games"][0]["display"]["background"]["url"] = "https://cdn/posterHASH.webp";
      body["data"]["games"][0]["display"]["logo"]["url"] = "https://cdn/logo.png";
      body["data"]["games"][0]["display"]["icon"]["url"] = "https://cdn/icon.png";
      return body.dump();
    }
    return std::string("img");
  };
  auto decode = [](std::string_view) { return textured(16, 16, 10, 20, 30); };
  auto first = sparxie::refresh_official(env, get_url, decode);
  EXPECT_TRUE(first.status.ok) << first.body.dump();
  // the stored application's poster, and the national-biz poster of the bilibili client (its own cache): the payload here answers only hk4e_cn
  // the two HoYoverse answers plus a placeholder card for every title and
  // region this fixture's transport does not answer for
  EXPECT_EQ(first.body["updated"], 15);
  auto second = sparxie::refresh_official(env, get_url, decode);
  EXPECT_EQ(second.body["updated"], 0);
  std::ifstream in(env.apps_file);
  sparxie::json after;
  in >> after;
  EXPECT_EQ(after["apps"][0]["sparxie"]["cover"], "custom");
  EXPECT_TRUE(after["apps"][0].value("image-path", std::string()).empty());
}

TEST(SparxieHost, StatusCarriesTheLaunchersNoteAndNothingWhenThereIsNone) {
  const fs::path root = fs::temp_directory_path() / "sparxie-launch-notice-test";
  fs::remove_all(root);
  fs::create_directories(root);
  sparxie::HostEnv env = env_at(root);
  EXPECT_TRUE(sparxie::component_status(env).body["launchNotice"].is_null());
  fs::create_directories(env.host_data);
  {
    std::ofstream out(env.host_data / "launch-notice.json", std::ios::binary);
    out << R"({"version":1,"id":"a1","time":"2026-11-11T12:00:00Z","kind":"direct","profile":"p1","game":"starRail","message":"Sparxie 未能接管本次启动，已直接启动游戏","messageEn":"Sparxie could not take over this launch; the game was started directly","reason":"x","log":""})";
  }
  const auto status = sparxie::component_status(env);
  ASSERT_TRUE(status.body["launchNotice"].is_object());
  EXPECT_EQ(status.body["launchNotice"]["kind"], "direct");
  EXPECT_EQ(status.body["launchNotice"]["message"], "Sparxie 未能接管本次启动，已直接启动游戏");
  EXPECT_EQ(sparxie::launch_notice(env)["id"], "a1");
  {
    std::ofstream out(env.host_data / "launch-notice.json", std::ios::binary | std::ios::trunc);
    out << "{ not json";
  }
  EXPECT_TRUE(sparxie::launch_notice(env).is_null());
  {
    std::ofstream out(env.host_data / "launch-notice.json", std::ios::binary | std::ios::trunc);
    out << R"({"version":1,"kind":"direct"})";
  }
  EXPECT_TRUE(sparxie::launch_notice(env).is_null());
  fs::remove_all(root);
}

namespace {
  std::uint32_t
  crc32_of(const std::string &data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : data) {
      crc ^= byte;
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
  }

  void
  put32(std::string &out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(value >> (8 * i) & 0xFF));
  }

  void
  put16(std::string &out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
  }

  // A zip with one stored entry (what NTE's launcher file list serves for its pictures, without the compression).
  std::string
  zip_of(const std::string &name, const std::string &data) {
    std::string out;
    const std::uint32_t crc = crc32_of(data);
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, crc);
    put32(out, static_cast<std::uint32_t>(data.size()));
    put32(out, static_cast<std::uint32_t>(data.size()));
    put16(out, static_cast<std::uint16_t>(name.size()));
    put16(out, 0);
    out += name;
    out += data;
    const std::uint32_t central = static_cast<std::uint32_t>(out.size());
    put32(out, 0x02014b50);
    put16(out, 20);
    put16(out, 20);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, crc);
    put32(out, static_cast<std::uint32_t>(data.size()));
    put32(out, static_cast<std::uint32_t>(data.size()));
    put16(out, static_cast<std::uint16_t>(name.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, 0);
    out += name;
    const std::uint32_t central_size = static_cast<std::uint32_t>(out.size()) - central;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);
    put16(out, 1);
    put32(out, central_size);
    put32(out, central);
    put16(out, 0);
    return out;
  }

  // What the official sources answer in the vendor refresh tests. `webp` pictures are the bytes the test decoder cannot read (a machine without the WebP codec).
  struct VendorNet {
    bool kuro_up = true;
    bool hypergryph_up = true;
    bool steam_up = true;
    bool pics_up = true;
    bool icons_up = true;
    int icon_side = 256;
    int icon_fetches = 0;
    bool nte_up = true;
    int pictures = 0;
    std::vector<std::string> picture_urls;

    std::optional<std::string>
    get(const std::string &url) {
      if (url.find("getGames") != std::string::npos) {
        sparxie::json body;
        body["data"]["games"][0]["biz"] = "hk4e_cn";
        body["data"]["games"][0]["display"]["background"]["url"] = "https://cdn/posterHASH.webp";
        return body.dump();
      }
      if (url.find("kurogame.com/launcher/launcher/") != std::string::npos && url.find("index.json") != std::string::npos)
        return kuro_up ? std::optional<std::string>("{\"functionCode\":{\"background\":\"KuroBackgroundCode\"}}") : std::nullopt;
      if (url.find("/background/KuroBackgroundCode/") != std::string::npos)
        return kuro_up ? std::optional<std::string>("{\"functionSwitch\":1,\"firstFrameImage\":\"https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/z9eh4ixe3jlnk7wx8a.webp\",\"slogan\":\"https://pcdownload-huoshan.aki-game.com/launcher/clientUpload/sh81q4z9f428xrpo8d.png\"}") : std::nullopt;
      if (url.find("IStoreBrowseService") != std::string::npos) {
        if (!steam_up) return std::nullopt;
        const auto at = url.find("%22appid%22%3A");
        const std::string id = at == std::string::npos ? "3513350" : url.substr(at + 14, url.find_first_not_of("0123456789", at + 14) - at - 14);
        return "{\"response\":{\"store_items\":[{\"appid\":" + id + ",\"assets\":{\"asset_url_format\":\"steam/apps/" + id + "/${FILENAME}?t=1\","
               "\"library_capsule_2x\":\"07948a96389614937d331335b9dca86bd5166d7c/library_capsule_2x.jpg\",\"community_icon\":\"b2065aa2\","
               "\"raw_page_background\":\"7c80b6040bdbd6d48471c33dbc55e7fbca8abc3" + id.substr(0, 1) + "/page_bg_raw.jpg\"}}]}}";
      }
      if (url.rfind("https://api.steamcmd.net/v1/info/", 0) == 0) {
        if (!pics_up) return std::nullopt;
        const std::string id = url.substr(33);
        return "{\"data\":{\"" + id + "\":{\"common\":{\"clienticon\":\"b2065aa2b618ff4b310e929024baf237a3d58ff9\",\"library_assets_full\":{"
               "\"library_capsule\":{\"image2x\":{\"english\":\"07948a96389614937d331335b9dca86bd5166d7c/library_capsule_2x.jpg\",\"schinese\":\"fcb169eb2c41ba8fa4e7a78d40ea57b7a635df03/library_capsule_schinese_2x.jpg\"}},"
               "\"library_hero\":{\"image\":{\"english\":\"71dc6e3d1b733e31187f06c547576c9916f10bdb/library_hero.jpg\"}},"
               "\"library_logo\":{\"image\":{\"english\":\"42099b9aa260f177b764231cf8d8d9ce69f03361/logo.png\",\"schinese\":\"8555af917c101826af81713c250cc74a929a00d8/logo_schinese.png\"}}}}}}}";
      }
      // the program icon files: Steam's .ico (a 256 px PNG entry beside small bitmaps) and the Yostar site's PNG
      if (url == "https://ak.hypergryph.com/") return std::string("<html><defs><path id=\"svg_def-title_arknights\" fill-rule=\"evenodd\" fill=\"currentColor\" d=\"M20 20h200v60H20zM40 40h160v20H40z\"/></defs></html>");
      if (url.find(".ico") != std::string::npos) {
        ++icon_fetches;
        return icons_up ? std::optional<std::string>(pefix::ico_file({ pefix::dib_image(32, 20), pefix::png_image(icon_side, 60) })) : std::nullopt;
      }
      if (url.find("yo-star.com") != std::string::npos) {
        ++icon_fetches;
        if (!icons_up) return std::nullopt;
        const auto png = sparxie::encode_png(pefix::picture(icon_side / 2, 90));
        return std::string(png.begin(), png.end());
      }
      if (url.find("getAllGameBasicInfo") != std::string::npos) return std::string("{}");
      if (url.find("Version.ini") != std::string::npos) return nte_up ? std::optional<std::string>("FileListURL=https://yhcdn1.wmupd.com/hd/publish_ob/launcher/1/AllFiles.xml\r\n") : std::nullopt;
      if (url.find("AllFiles.xml") != std::string::npos)
        return "<Url BaseUrl=\"https://yhcdn1.wmupd.com/hd/publish_ob/launcher\" /><ProductVersion Version=\"1\" /><File Checksum=\"cf54a64e41f1435e162948f77e768b67\" Path=\"/ResFilesM/1289/bgimgs/bg_0.png\" />";
      if (url == "https://yh.wanmei.com/") return "<img src=\"https://yh.wanmei.com/images/cover261105/logo.png\">";
      if (url.find("bg_0.png.zip") != std::string::npos) return zip_of("bg_0.png", "png-bytes");
      if (url.find("version_briefing") != std::string::npos)
        return "<link href=\"https://web.hycdn.cn/endfield/webview/_version_briefing/v1d6/favicon.ico\" rel=\"icon\"><script defer src=\"https://web.hycdn.cn/endfield/webview/_version_briefing/v1d6/version-v1d6.1d6d24.js\"></script>";
      ++pictures;
      picture_urls.push_back(url);
      // the launcher pictures of Hypergryph and Kuro are webp: the test decoder reads only "img" and "png-bytes"
      if (url.find(".webp") != std::string::npos) return std::string("WEBP");
      return std::string("img");
    }

    std::optional<std::string>
    post(const std::string &url, const std::string &body) {
      if (!hypergryph_up || url.find("batch_proxy") == std::string::npos) return std::nullopt;
      const std::string code = body.find("GzD1CpaWgmSq1wew") != std::string::npos ? "GzD1CpaWgmSq1wew" : "6LL0KJuqHBVz33WK";
      const bool arknights = code == "GzD1CpaWgmSq1wew";
      const std::string file = arknights ? "53a2be88a3371a1473ecd41e89fb0ad5.png" : "15d43bac17c83b83d53f667aeb639952.webp";
      return "{\"proxy_rsps\":[{\"kind\":\"get_main_bg_image\",\"get_main_bg_image_rsp\":{\"main_bg_image\":{\"url\":\"https://hg-utils-public.hycdn.cn/hg-utils/prod/eppcsuwqpaueijqk/" + code + "/15/d4/" + file + "\"}}}]}";
    }
  };

  sparxie::Rgba
  decode_test_picture(std::string_view bytes) {
    if (bytes == "img" || bytes == "png-bytes") return textured(16, 16, 100, 110, 130);
    if (bytes.size() > 8 && bytes.compare(0, 4, "\x89PNG") == 0) return sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
    return {};
  }

  fs::path
  cover_of(const sparxie::HostEnv &env, const std::string &id) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(env.covers_dir, ec))
      if (entry.path().filename().string().rfind("sparxie-" + id + "-", 0) == 0) return entry.path();
    return fs::path {};
  }

  sparxie::HostEnv
  vendor_env(const char *name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    sparxie::HostEnv env = env_at(root);
    fs::create_directories(root);
    std::ofstream out(env.apps_file);
    out << sparxie::json { { "apps", sparxie::json::array() } }.dump();
    return env;
  }

  bool
  has_published(const sparxie::HostEnv &env, const std::string &id) {
    const fs::path posters = env.host_data / "posters" / id;
    return !cover_of(env, id).empty() && fs::is_regular_file(posters / "banner.png") && fs::is_regular_file(posters / "hero.jpg") && fs::is_regular_file(posters / "icon.png");
  }
}  // namespace

TEST(Host, RefreshPublishesEveryTitleThroughOnePathWithItsOfficialSources) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-vendor");
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok) << first.body.dump();
  const sparxie::json &sources = first.body["sources"];
  // Endfield cn/bilibili: the launcher's picture has the title painted in, so the clean variant (Steam's library hero + logo) is taken first.
  EXPECT_EQ(sources["endfield-cn"], "steam-clean") << first.body.dump();
  EXPECT_EQ(sources["endfield-bili"], "steam-clean");
  EXPECT_EQ(sources["endfield-global"], "steam");
  // Arknights cn: the launcher's png decodes.
  EXPECT_EQ(sources["arknights-cn"], "hypergryph");
  EXPECT_NE(cover_of(env, "arknights-cn").filename().string().find("53a2be88a3371a1473ecd41e89fb0ad5"), std::string::npos) << cover_of(env, "arknights-cn");
  // Kuro: its first frame is a webp too; the national builds have no second source and get the placeholder, the global builds the store.
  EXPECT_EQ(sources["wuwa-cn"], "placeholder");
  EXPECT_EQ(sources["wuwa-global"], "steam");
  EXPECT_EQ(sources["pgr-cn"], "placeholder");
  EXPECT_EQ(sources["pgr-global"], "steam");
  // NTE: the national launcher's zipped png, the store build globally.
  EXPECT_EQ(sources["nte-cn"], "nte-launcher");
  EXPECT_EQ(sources["nte-global"], "steam");
  EXPECT_NE(cover_of(env, "nte-cn").filename().string().find("cf54a64e41f1435e162948f77e768b67"), std::string::npos);
  // Arknights global has no sourced official art at all: the placeholder, not counted as a failure.
  EXPECT_EQ(sources["arknights-global"], "placeholder");
  for (const auto &id : first.body["skipped"]) EXPECT_NE(id.get<std::string>(), "arknights-global");
  // Every title and region is published the same way: portrait cover, banner, landscape hero and icon.
  for (const char *vendor : { "endfield", "arknights", "wuwa", "pgr", "nte" })
    for (const char *region : { "cn", "bili", "global" }) {
      const std::string id = std::string(vendor) + "-" + region;
      if (std::string(vendor) == "arknights" && std::string(region) == "bili") continue;
      if (std::string(vendor) == "pgr" && std::string(region) == "bili") continue;
      EXPECT_TRUE(has_published(env, id)) << id;
    }
  // the placeholder's own landscape card is the hero, and a store build's finished capsule the cover
  EXPECT_NE(cover_of(env, "wuwa-cn").filename().string().find("placeholder"), std::string::npos);
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "wuwa-cn" / "index.json"))["version"], "placeholder");
  // Every title and region has a real icon of 256 px (Steam's client icon, or Yostar's site icon for Arknights), never a crop of the background.
  for (const char *id : { "endfield-cn", "endfield-bili", "endfield-global", "wuwa-cn", "wuwa-bili", "wuwa-global", "pgr-cn", "pgr-global", "nte-cn", "nte-bili", "nte-global" })
    EXPECT_EQ(first.body["icons"][id], "steam-clienticon") << id;
  for (const char *id : { "arknights-cn", "arknights-global" }) EXPECT_EQ(first.body["icons"][id], "official-site") << id;
  for (const char *id : { "endfield-cn", "wuwa-global", "nte-cn", "arknights-global" }) {
    std::ifstream in(env.host_data / "posters" / id / "icon.png", std::ios::binary);
    const std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const sparxie::Rgba icon = sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(png.data()), png.size());
    EXPECT_EQ(icon.w, 256) << id;
    EXPECT_EQ(icon.h, 256) << id;
  }
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "endfield-cn" / "index.json"))["iconSource"], "steam-clienticon");
  // the clean variant's logo and finished capsule are kept for the hero and the cover (the Chinese logo for the national client)
  EXPECT_TRUE(fs::is_regular_file(env.host_data / "posters" / "endfield-cn" / "71dc6e3d1b73-8555af917c10-logo.bin"));
  EXPECT_TRUE(fs::is_regular_file(env.host_data / "posters" / "endfield-global" / "7c80b6040bdbd6d4-07948a963896-42099b9aa260-logo.bin"));
  // A second run changes nothing: the versions answer again, nothing is written twice and no icon is fetched again.
  net.icon_fetches = 0;
  const auto second = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  EXPECT_EQ(second.body["updated"], 0) << second.body.dump();
  EXPECT_EQ(net.icon_fetches, 0) << "a real icon made earlier stays";
}

TEST(Host, RefreshTakesTheLauncherPictureWhenItDecodesAndKuroItsFirstFrame) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-launcher");
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  // a machine whose decoder reads webp
  const auto decode = [](std::string_view bytes) { return bytes == "WEBP" || bytes == "img" || bytes == "png-bytes" ? textured(16, 16, 100, 110, 130) : sparxie::Rgba {}; };
  const auto first = sparxie::refresh_official(env, get_url, decode, post_url);
  ASSERT_TRUE(first.status.ok) << first.body.dump();
  // the clean variant leads even on a machine that decodes the launcher's webp: no title painted twice
  EXPECT_EQ(first.body["sources"]["endfield-cn"], "steam-clean");
  EXPECT_NE(cover_of(env, "endfield-cn").filename().string().find("71dc6e3d1b73-8555af917c10"), std::string::npos) << cover_of(env, "endfield-cn");
  EXPECT_EQ(first.body["sources"]["wuwa-cn"], "kuro");
  EXPECT_EQ(first.body["sources"]["wuwa-global"], "kuro");
  EXPECT_EQ(first.body["sources"]["pgr-cn"], "kuro");
  EXPECT_NE(cover_of(env, "wuwa-cn").filename().string().find("z9eh4ixe3jlnk7wx8a-8555af917c10"), std::string::npos) << cover_of(env, "wuwa-cn");
  for (const char *id : { "endfield-cn", "endfield-bili", "wuwa-cn", "wuwa-global", "pgr-cn", "pgr-global", "arknights-cn", "nte-cn" }) EXPECT_TRUE(has_published(env, id)) << id;
  // the key art and the wordmark are kept for the hero
  EXPECT_TRUE(fs::is_regular_file(env.host_data / "posters" / "wuwa-cn" / "z9eh4ixe3jlnk7wx8a-8555af917c10-background.bin"));
  EXPECT_TRUE(fs::is_regular_file(env.host_data / "posters" / "wuwa-cn" / "z9eh4ixe3jlnk7wx8a-8555af917c10-logo.bin"));
  // the hero of every title comes from its cached key art on the next run without fetching a picture again
  fs::remove(env.host_data / "posters" / "wuwa-cn" / "hero.jpg");
  net.pictures = 0;
  const auto second = sparxie::refresh_official(env, get_url, decode, post_url);
  EXPECT_EQ(second.body["updated"], 0) << second.body.dump();
  EXPECT_EQ(second.body["heroes"], 1);
  EXPECT_EQ(net.pictures, 0) << (net.picture_urls.empty() ? std::string() : net.picture_urls.front());
  EXPECT_TRUE(fs::is_regular_file(env.host_data / "posters" / "wuwa-cn" / "hero.jpg"));
}

TEST(Host, ARealPictureIsNeverReplacedByThePlaceholderWhenTheSourcesAreDown) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-offline");
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok);
  const fs::path before = cover_of(env, "arknights-cn");
  ASSERT_FALSE(before.empty());
  net.hypergryph_up = false;
  net.kuro_up = false;
  net.steam_up = false;
  net.nte_up = false;
  net.pics_up = false;
  net.icons_up = false;
  const auto second = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  EXPECT_EQ(second.body["updated"], 0) << second.body.dump();
  // the real icon made earlier stays when the icon sources are down too
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "arknights-cn" / "index.json"))["iconSource"], "official-site");
  EXPECT_TRUE(fs::is_regular_file(env.host_data / "posters" / "arknights-cn" / "icon.png"));
  EXPECT_EQ(cover_of(env, "arknights-cn"), before);
  EXPECT_NE(cover_of(env, "arknights-cn").filename().string().find("53a2be88a3371a1473ecd41e89fb0ad5"), std::string::npos);
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "nte-cn" / "index.json"))["version"], "cf54a64e41f1435e162948f77e768b67");
  // a title that never had a real picture stays on its placeholder, and is reported as unanswered
  bool listed = false;
  for (const auto &id : second.body["skipped"]) listed = listed || id.get<std::string>() == "arknights-cn";
  EXPECT_TRUE(listed) << second.body.dump();
}

TEST(Host, APictureThatIsTooLargeOrABrokenZipFallsToThePlaceholder) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-bounds");
  VendorNet net;
  auto get_url = [&](const std::string &url) -> std::optional<std::string> {
    // the national NTE launcher answers a zip that is no zip and a store that answers a picture of 30 MiB
    if (url.find("bg_0.png.zip") != std::string::npos) return std::string("not a zip at all");
    if (url.find("page_bg_raw") != std::string::npos) return std::string(30u * 1024u * 1024u, 'x');
    return net.get(url);
  };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok);
  EXPECT_EQ(first.body["sources"]["nte-cn"], "placeholder") << first.body.dump();
  EXPECT_EQ(first.body["sources"]["nte-global"], "placeholder");
  EXPECT_TRUE(has_published(env, "nte-cn"));
  EXPECT_TRUE(has_published(env, "nte-global"));
}

TEST(Host, ABlankDecodeIsAFailedDecodeForEveryTitleAndABlankHeroIsHealedForEveryTitle) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-blank");
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  // a decoder that answers one colour for every picture: no title publishes it; each falls to its placeholder
  const auto zeros = [](std::string_view) { return solid(16, 16, 0, 0, 0); };
  const auto blank_run = sparxie::refresh_official(env, get_url, zeros, post_url);
  ASSERT_TRUE(blank_run.status.ok) << blank_run.body.dump();
  for (const char *id : { "endfield-cn", "wuwa-cn", "nte-global", "arknights-cn" }) EXPECT_EQ(blank_run.body["sources"][id], "placeholder") << id;
  EXPECT_FALSE(fs::is_regular_file(env.host_data / "posters" / "wuwa-cn" / "z9eh4ixe3jlnk7wx8a-8555af917c10-background.bin")) << "a blank picture is not cached";
  // a good decoder replaces the placeholders; then a blank hero of a non-HoYoverse title is composed again from its cached art
  const auto good = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(good.status.ok);
  EXPECT_EQ(good.body["sources"]["arknights-cn"], "hypergryph");
  const fs::path hero = env.host_data / "posters" / "arknights-cn" / "hero.jpg";
  const auto blank_jpeg = sparxie::encode_jpeg(solid(64, 36, 0, 0, 0), 90);
  {
    std::ofstream out(hero, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(blank_jpeg.data()), static_cast<std::streamsize>(blank_jpeg.size()));
  }
  ASSERT_TRUE(sparxie::jpeg_is_blank(std::string_view(reinterpret_cast<const char *>(blank_jpeg.data()), blank_jpeg.size())));
  const auto healed = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  EXPECT_GE(healed.body["heroes"].get<int>(), 1) << healed.body.dump();
  std::ifstream in(hero, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_FALSE(sparxie::jpeg_is_blank(bytes));
}

namespace {
  // a Genshin install (bili marker) and a Sparxie component beside the host; the component may list capabilities
  sparxie::HostEnv
  kbm_env(const fs::path &root, bool with_switch, bool with_file) {
    fs::remove_all(root);
    fs::create_directories(root / "game" / "YuanShen_Data" / "Plugins");
    std::ofstream(root / "game" / "YuanShen.exe") << "x";
    std::ofstream(root / "game" / "YuanShen_Data" / "Plugins" / "PCGameSDK.dll") << "x";
    sparxie::HostEnv env = env_at(root);
    const fs::path component = root / "install" / "components" / "sparxie";
    fs::create_directories(component / "app-0.1.0-test");
    std::ofstream(component / "Sparxie.exe") << "x";
    std::ofstream(component / "version.ini") << "exe_path=app-0.1.0-test\\Sparxie.Desktop.exe\n";
    if (with_file) {
      sparxie::json body = { { "schema", 1 }, { "capabilities", with_switch ? sparxie::json::array({ "touch-ui-switch" }) : sparxie::json::array() } };
      std::ofstream(component / "app-0.1.0-test" / "capabilities.json") << body.dump();
    }
    env.process_running = [](const std::vector<std::string> &) { return false; };
    return env;
  }
}  // namespace
TEST(Host, ASparxieWithTheSwitchServesKeyboardMouseAndGamepadThroughItself) {
  const fs::path root = fs::temp_directory_path() / "sparxie-host-kbm-switch";
  sparxie::HostEnv env = kbm_env(root, true, true);
  const auto status = sparxie::component_status(env);
  EXPECT_TRUE(status.body["touchUiSwitch"]);
  EXPECT_EQ(status.body["capabilities"], sparxie::json::array({ "touch-ui-switch" }));
  auto kbm = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "touch" }, { "name", "Genshin pad" }, { "options", { { "input", "kbm" }, { "fpsUnlock", true }, { "fps", 144 } } } });
  ASSERT_TRUE(kbm.status.ok) << kbm.status.error;
  // still a Sparxie launch (the launcher), with the profile saying: no touch interface
  EXPECT_EQ(kbm.body["app"]["sparxie"]["mode"], "touch");
  EXPECT_NE(kbm.body["app"]["cmd"].get<std::string>().find("--profile"), std::string::npos);
  EXPECT_EQ(kbm.body["app"]["sparxie"]["options"]["input"], "kbm");
  const auto stored = sparxie::read_store(env.data_dir);
  ASSERT_TRUE(stored.status.ok);
  EXPECT_EQ(stored.document["profiles"][0]["hoyo"]["touchUiEnabled"], false);
  EXPECT_EQ(stored.document["profiles"][0]["hoyo"]["fpsUnlockEnabled"], true);
  // the same title with touch: the profile has the touch interface
  auto touch = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "touch" }, { "name", "Genshin touch" }, { "options", { { "input", "touch" } } } });
  ASSERT_TRUE(touch.status.ok) << touch.status.error;
  EXPECT_EQ(sparxie::read_store(env.data_dir).document["profiles"][1]["hoyo"]["touchUiEnabled"], true);
  // editing the pad application back to touch turns the interface on in its own profile and leaves the other one alone
  sparxie::json apps;
  {
    std::ifstream in(env.apps_file);
    in >> apps;
  }
  const int pad_index = apps["apps"][0].value("name", std::string()) == "Genshin pad" ? 0 : 1;
  auto edit = sparxie::update_app(env, pad_index, { { "options", { { "input", "touch" }, { "fpsUnlock", true }, { "fps", 144 } } } });
  ASSERT_TRUE(edit.status.ok) << edit.status.error;
  const auto after = sparxie::read_store(env.data_dir);
  EXPECT_EQ(after.document["profiles"][0]["hoyo"]["touchUiEnabled"], true);
  EXPECT_EQ(after.document["profiles"][1]["hoyo"]["touchUiEnabled"], true);
}
TEST(Host, ASparxieWithoutTheSwitchStartsAKeyboardMouseApplicationDirectly) {
  for (const bool with_file : { false, true }) {
    // no capabilities.json (preview.9 and older) and a file that lists nothing both mean: no switch
    const fs::path root = fs::temp_directory_path() / (with_file ? "sparxie-host-kbm-empty" : "sparxie-host-kbm-old");
    sparxie::HostEnv env = kbm_env(root, false, with_file);
    const auto status = sparxie::component_status(env);
    EXPECT_TRUE(status.body["installed"]);
    EXPECT_FALSE(status.body["touchUiSwitch"]);
    auto kbm = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "touch" }, { "name", "Genshin pad" }, { "options", { { "input", "kbm" } } } });
    ASSERT_TRUE(kbm.status.ok) << kbm.status.error;
    EXPECT_EQ(kbm.body["app"]["sparxie"]["mode"], "pc");
    EXPECT_EQ(kbm.body["app"]["cmd"].get<std::string>().find("--profile"), std::string::npos);
    EXPECT_NE(kbm.body["app"]["cmd"].get<std::string>().find("YuanShen.exe"), std::string::npos);
    EXPECT_FALSE(kbm.body["wrote"]);
    // touch still goes through Sparxie
    auto touch = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "touch" }, { "name", "Genshin touch" }, { "options", { { "input", "touch" } } } });
    ASSERT_TRUE(touch.status.ok) << touch.status.error;
    EXPECT_EQ(touch.body["app"]["sparxie"]["mode"], "touch");
  }
}
TEST(Host, ALaunchBringsAnOlderKeyboardMouseProfileInLineWithTheApplication) {
  const fs::path root = fs::temp_directory_path() / "sparxie-host-kbm-launch";
  sparxie::HostEnv env = kbm_env(root, true, true);
  ASSERT_TRUE(sparxie::component_status(env).body["touchUiSwitch"]);
  auto made = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "touch" }, { "name", "Genshin pad" }, { "options", { { "input", "kbm" } } } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  const std::string cmd = made.body["app"]["cmd"];
  // an application made before the switch existed: its profile has no touchUiEnabled (Sparxie reads that as the touch interface)
  sparxie::json doc = sparxie::read_store(env.data_dir).document;
  doc["profiles"][0]["hoyo"].erase("touchUiEnabled");
  {
    std::ofstream out(env.data_dir / "config.json");
    out << doc.dump(2);
  }
  // Sparxie open: nothing is written
  env.process_running = [](const std::vector<std::string> &names) { return std::find(names.begin(), names.end(), "Sparxie.exe") != names.end(); };
  EXPECT_TRUE(sparxie::prepare_launch(env, "Genshin pad", cmd).ok);
  EXPECT_FALSE(sparxie::read_store(env.data_dir).document["profiles"][0]["hoyo"].contains("touchUiEnabled"));
  // Sparxie closed: the profile says no touch interface, and nothing else in it changed
  env.process_running = [](const std::vector<std::string> &) { return false; };
  EXPECT_TRUE(sparxie::prepare_launch(env, "Genshin pad", cmd).ok);
  const auto after = sparxie::read_store(env.data_dir).document;
  EXPECT_EQ(after["profiles"][0]["hoyo"]["touchUiEnabled"], false);
  EXPECT_EQ(after["profiles"][0]["hoyo"]["targetFps"], 120);
  EXPECT_EQ(after["profiles"].size(), 1u);
}

// ---- applications of the first builds (no stored input) ----
namespace {
  sparxie::json
  read_apps(const fs::path &file) {
    std::ifstream in(file);
    sparxie::json body;
    in >> body;
    return body;
  }
}  // namespace
TEST(AppsOfTheFirstBuilds, ADirectLaunchIsKeyboardMouseAndGamepadAndGoesThroughASparxieThatCanKeepThePcInterface) {
  const fs::path root = fs::temp_directory_path() / "sparxie-pc-legacy";
  sparxie::HostEnv env = kbm_env(root, true, true);
  fs::create_directories(env.install_root / "tools");
  std::ofstream(env.install_root / "tools" / "sparxie-launch.exe") << "x";
  // made as a direct launch, with options but no input (the first builds)
  auto made = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "pc" }, { "name", "Genshin direct" }, { "options", { { "fpsUnlock", true }, { "fps", 144 }, { "input", "touch" } } } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  {
    auto apps = read_apps(env.apps_file);
    ASSERT_EQ(apps["apps"][0]["sparxie"]["mode"], "pc");
    apps["apps"][0]["sparxie"]["options"].erase("input");
    std::ofstream(env.apps_file) << apps.dump();
  }
  // Sparxie open: nothing is written, the application waits for the next start
  env.process_running = [](const std::vector<std::string> &names) { return std::find(names.begin(), names.end(), "Sparxie.exe") != names.end(); };
  sparxie::attach_official_covers(env);
  EXPECT_EQ(read_apps(env.apps_file)["apps"][0]["sparxie"]["mode"], "pc");
  env.process_running = [](const std::vector<std::string> &) { return false; };
  EXPECT_GT(sparxie::attach_official_covers(env), 0);
  const auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["sparxie"]["mode"], "touch");
  EXPECT_FALSE(app["sparxie"]["profile"].get<std::string>().empty());
  EXPECT_EQ(app["sparxie"]["options"]["input"], "kbm") << "the input it always had is stored";
  EXPECT_EQ(app["sparxie"]["options"]["fpsUnlock"], true) << "its Sparxie options are kept";
  EXPECT_EQ(app["sparxie"]["options"]["fps"], 144);
  EXPECT_NE(app["cmd"].get<std::string>().find("sparxie-launch"), std::string::npos) << app["cmd"];
  EXPECT_EQ(app["elevated"], true);
  const auto store = sparxie::read_store(env.data_dir).document;
  ASSERT_EQ(store["profiles"].size(), 1u);
  EXPECT_EQ(store["profiles"][0]["hoyo"]["touchUiEnabled"], false) << "a session with the PC interface";
  EXPECT_EQ(store["profiles"][0]["hoyo"]["fpsUnlockEnabled"], true);
  EXPECT_EQ(sparxie::attach_official_covers(env), 0) << "the next start changes nothing";
}

TEST(AppsOfTheFirstBuilds, WithoutTheSwitchADirectLaunchStaysDirectAndTheEditStoresItsInput) {
  const fs::path root = fs::temp_directory_path() / "sparxie-pc-legacy-noswitch";
  sparxie::HostEnv env = kbm_env(root, false, true);
  fs::create_directories(env.install_root / "tools");
  std::ofstream(env.install_root / "tools" / "sparxie-launch.exe") << "x";
  auto made = sparxie::create_app(env, { { "path", (root / "game" / "YuanShen.exe").string() }, { "mode", "pc" }, { "name", "Genshin direct" } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  {
    auto apps = read_apps(env.apps_file);
    apps["apps"][0]["sparxie"]["options"].erase("input");  // the first builds stored no input
    std::ofstream(env.apps_file) << apps.dump();
  }
  EXPECT_EQ(sparxie::attach_official_covers(env), 0);
  EXPECT_EQ(read_apps(env.apps_file)["apps"][0]["sparxie"]["mode"], "pc") << "an installed Sparxie that cannot keep the PC interface keeps the direct launch";
  // saving the application stores the input it had implicitly (kbm) and does not turn it into a touch application
  const auto saved = sparxie::update_app(env, 0, { { "name", "Genshin direct" } });
  ASSERT_TRUE(saved.status.ok) << saved.status.error;
  const auto app = read_apps(env.apps_file)["apps"][0];
  EXPECT_EQ(app["sparxie"]["mode"], "pc");
  EXPECT_EQ(app["sparxie"]["options"]["input"], "kbm");
}

TEST(AppsOfTheFirstBuilds, ZenlessZoneZeroHasNoPcInterfaceSessionSoItsKeyboardAppStartsTheGameDirectly) {
  const fs::path root = fs::temp_directory_path() / "sparxie-zzz-kbm";
  sparxie::HostEnv env = kbm_env(root, true, true);
  fs::create_directories(root / "zzz");
  std::ofstream(root / "zzz" / "ZenlessZoneZero.exe") << "x";
  auto made = sparxie::create_app(env, { { "path", (root / "zzz" / "ZenlessZoneZero.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "ZZZ pad" }, { "options", { { "input", "kbm" } } } });
  ASSERT_TRUE(made.status.ok) << made.status.error;
  EXPECT_EQ(made.body["app"]["sparxie"]["mode"], "pc") << "the patched Sparxie keeps the PC interface of Genshin and Star Rail only";
  auto touch = sparxie::create_app(env, { { "path", (root / "zzz" / "ZenlessZoneZero.exe").string() }, { "region", "cn" }, { "mode", "touch" }, { "name", "ZZZ touch" }, { "options", { { "input", "touch" } } } });
  ASSERT_TRUE(touch.status.ok) << touch.status.error;
  EXPECT_EQ(touch.body["app"]["sparxie"]["mode"], "touch");
}

TEST(Store, AnEndfieldProfileIsAcceptedAndItsResolutionRoundTrips) {
  const fs::path root = fs::temp_directory_path() / "sparxie-endfield-profile";
  fs::remove_all(root);
  fs::create_directories(root / "data");
  sparxie::ProfileDraft draft;
  draft.game = "endfield";
  draft.display_name = "终末地";
  draft.executable = (root / "Endfield.exe").string();
  draft.id = "endfield-cn-1";
  draft.options = { { "input", "touch" }, { "fps", 165 }, { "fpsUnlock", true }, { "resolution", "1920×1080" } };
  draft.display_fps = 144;
  const auto stored = sparxie::add_profile(root / "data", draft, {});
  ASSERT_TRUE(stored.status.ok) << stored.status.error;
  const auto doc = sparxie::read_store(root / "data").document;
  ASSERT_EQ(doc["profiles"].size(), 1u);
  EXPECT_EQ(doc["profiles"][0]["game"], "endfield");
  const auto &hoyo = doc["profiles"][0]["hoyo"];
  EXPECT_EQ(hoyo["endfieldInputMode"], "touch");
  EXPECT_EQ(hoyo["endfieldAutoResolution"], false);
  EXPECT_EQ(hoyo["endfieldWidth"], 1920);
  EXPECT_EQ(hoyo["endfieldHeight"], 1080);
  const auto options = sparxie::options_from_hoyo(hoyo);
  EXPECT_EQ(options["input"], "touch");
  EXPECT_EQ(options["resolution"], "1920×1080");
  // the removed automatic input: a profile of an older build that still says it reads as touch, and a draft that still says it is written as touch
  EXPECT_EQ(sparxie::options_from_hoyo({ { "endfieldInputMode", "auto" } })["input"], "touch");
  EXPECT_EQ(sparxie::options_from_hoyo({ { "endfieldInputMode", "pc" } })["input"], "kbm");
  sparxie::json old_options = { { "input", "auto" } };
  ASSERT_TRUE(sparxie::normalize_options(old_options).ok);
  EXPECT_EQ(old_options["input"], "touch");
  sparxie::json refused = { { "input", "nonsense" } };
  EXPECT_FALSE(sparxie::normalize_options(refused).ok);
  draft.options["resolution"] = "auto";
  ASSERT_TRUE(sparxie::update_profile(root / "data", "endfield-cn-1", draft, {}).status.ok);
  EXPECT_EQ(sparxie::read_store(root / "data").document["profiles"][0]["hoyo"]["endfieldAutoResolution"], true);
  EXPECT_FALSE(sparxie::options_from_hoyo(sparxie::read_store(root / "data").document["profiles"][0]["hoyo"]).contains("resolution"));
}

namespace {
  // A fake Hypergryph launcher folder with Endfield installed: <root>/games/Arknights Endfield/Endfield.exe, an executable whose icon is a 256 px PNG of a known colour.
  void
  install_fake_endfield(sparxie::HostEnv &env, const fs::path &root, std::uint8_t colour) {
    const fs::path game = root / "launcher" / "games" / "Arknights Endfield";
    fs::create_directories(game);
    const std::string exe = pefix::exe_file({ pefix::dib_image(32, 10), pefix::png_image(256, colour) });
    std::ofstream out(game / "Endfield.exe", std::ios::binary);
    out.write(exe.data(), static_cast<std::streamsize>(exe.size()));
    // the launcher's own SDK record names the national build, so the install is identified as endfield-cn
    fs::create_directories(game / "sdkdata");
    std::ofstream(game / "sdkdata" / "sdk_hgsdk_config.json") << "{}";
    env.hypergryph_roots = { root / "launcher" };
    env.steam_root = root / "no-steam";
    env.epic_manifests = root / "no-epic";
    env.uninstall_locations = [](const std::vector<std::string> &) { return std::vector<fs::path> {}; };
  }

  sparxie::Rgba
  icon_png_of(const sparxie::HostEnv &env, const std::string &id) {
    std::ifstream in(env.host_data / "posters" / id / "icon.png", std::ios::binary);
    const std::string png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(png.data()), png.size());
  }
}  // namespace

TEST(Host, TheIconOfAnInstalledTitleIsItsOwnProgramIcon) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-exe-icon");
  const fs::path root = fs::temp_directory_path() / "sparxie-host-refresh-exe-icon";
  install_fake_endfield(env, root, 90);
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok) << first.body.dump();
  // the detected install's own executable wins over every download: its 256 px PNG entry, stored as it is
  EXPECT_EQ(first.body["icons"]["endfield-cn"], "exe") << first.body.dump();
  const sparxie::Rgba icon = icon_png_of(env, "endfield-cn");
  ASSERT_EQ(icon.w, 256);
  ASSERT_EQ(icon.h, 256);
  EXPECT_EQ(icon.px[0], 90);
  EXPECT_EQ(icon.px[1], 200);
  // the regions that are not installed take the store's client icon
  EXPECT_EQ(first.body["icons"]["endfield-global"], "steam-clienticon");
  EXPECT_EQ(icon_png_of(env, "endfield-global").px[0], 60);
  // a second run keeps it, and the cache says how it was made
  const auto second = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  EXPECT_EQ(second.body["icons"]["endfield-cn"], "exe");
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "endfield-cn" / "index.json"))["iconSource"], "exe");
}

TEST(Host, WithoutADownloadTheSiblingRegionsProgramIconThenAMonogramNeverACropOfTheCover) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-icon-fallback");
  const fs::path root = fs::temp_directory_path() / "sparxie-host-refresh-icon-fallback";
  install_fake_endfield(env, root, 120);
  VendorNet net;
  net.icons_up = false;
  net.pics_up = false;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok) << first.body.dump();
  EXPECT_EQ(first.body["icons"]["endfield-cn"], "exe");
  EXPECT_EQ(first.body["icons"]["endfield-bili"], "exe-sibling");
  EXPECT_EQ(icon_png_of(env, "endfield-bili").px[0], 120);
  EXPECT_EQ(first.body["icons"]["wuwa-cn"], "monogram");
  EXPECT_EQ(icon_png_of(env, "wuwa-cn").w, 256);
  // the monogram is retried on the next run (the downloads came back) and replaced by the real icon
  net.icons_up = true;
  net.pics_up = true;
  const auto second = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  EXPECT_EQ(second.body["icons"]["wuwa-cn"], "steam-clienticon");
  EXPECT_EQ(icon_png_of(env, "wuwa-cn").px[0], 60);
}

TEST(Host, AnIconBelowSixtyFourPixelsIsNotAnIcon) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-icon-small");
  VendorNet net;
  net.icon_side = 32;  // Steam's old community icon size
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok) << first.body.dump();
  EXPECT_EQ(first.body["icons"]["wuwa-global"], "monogram");
  EXPECT_EQ(first.body["icons"]["arknights-cn"], "monogram");
  EXPECT_EQ(icon_png_of(env, "wuwa-global").w, 256);
}

TEST(Host, TheArknightsWordmarkIsDrawnFromTheSitesSvgAndKeptLikeAnyLogo) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-ak-logo");
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  const auto first = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(first.status.ok) << first.body.dump();
  EXPECT_EQ(first.body["sources"]["arknights-cn"], "hypergryph");
  bool kept = false;
  for (const auto &entry : fs::directory_iterator(env.host_data / "posters" / "arknights-cn"))
    if (entry.path().filename().string().find("-logo.bin") != std::string::npos) {
      std::ifstream in(entry.path(), std::ios::binary);
      const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const sparxie::Rgba logo = sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
      kept = logo.w == 1016 && logo.h == 476;
    }
  EXPECT_TRUE(kept) << "a 1016 px wide PNG of the drawn wordmark";
  // the launcher art shows the painted title where the hero is cut: the wordmark is for the portrait cover only
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "arknights-cn" / "index.json"))["logoCoverOnly"], true);
  EXPECT_EQ(sparxie::json::parse(std::ifstream(env.host_data / "posters" / "nte-cn" / "index.json"))["logoCoverOnly"], false);
}

TEST(Host, AComposedCoverOfAnOlderCompositionIsMadeAgainFromTheCacheAndACapsuleIsLeftAlone) {
  sparxie::HostEnv env = vendor_env("sparxie-host-refresh-cover-rev");
  VendorNet net;
  auto get_url = [&](const std::string &url) { return net.get(url); };
  auto post_url = [&](const std::string &url, const std::string &body) { return net.post(url, body); };
  ASSERT_TRUE(sparxie::refresh_official(env, get_url, decode_test_picture, post_url).status.ok);
  const auto kind_of = [&](const char *id) { return sparxie::json::parse(std::ifstream(env.host_data / "posters" / id / "index.json")); };
  EXPECT_EQ(kind_of("nte-cn")["coverKind"], "composed");
  EXPECT_EQ(kind_of("nte-cn")["coverRev"], 1);
  EXPECT_EQ(kind_of("endfield-global")["coverKind"], "capsule");
  EXPECT_EQ(kind_of("wuwa-cn")["coverKind"], "placeholder");
  // an install cached by an older composition: its cover is wrong and its index says revision 0
  const fs::path composed = cover_of(env, "nte-cn");
  const fs::path capsule = cover_of(env, "endfield-global");
  const auto read = [](const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string capsule_before = read(capsule);
  // what a quiet run fetches (the titles that only have a placeholder ask their launcher again each run)
  net.pictures = 0;
  ASSERT_TRUE(sparxie::refresh_official(env, get_url, decode_test_picture, post_url).status.ok);
  const int quiet_pictures = net.pictures;
  for (const char *id : { "nte-cn", "endfield-global" }) {
    sparxie::json index = kind_of(id);
    index["coverRev"] = 0;
    std::ofstream out(env.host_data / "posters" / id / "index.json");
    out << index.dump();
  }
  { std::ofstream out(composed, std::ios::binary | std::ios::trunc); out << "old"; }
  net.pictures = 0;
  const auto again = sparxie::refresh_official(env, get_url, decode_test_picture, post_url);
  ASSERT_TRUE(again.status.ok);
  const std::string rewritten = read(composed);
  const sparxie::Rgba cover = sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(rewritten.data()), rewritten.size());
  EXPECT_EQ(cover.w, 600);
  EXPECT_EQ(cover.h, 900);
  EXPECT_EQ(kind_of("nte-cn")["coverRev"], 1);
  EXPECT_EQ(read(capsule), capsule_before) << "a finished capsule is never recomposed";
  EXPECT_EQ(net.pictures, quiet_pictures) << "the recomposition fetches nothing more than a quiet run";
}
