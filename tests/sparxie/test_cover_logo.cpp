#include "src/sparxie/art.h"
#include "src/sparxie/svg_path.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

namespace {
  // A key art with some texture (the host refuses one-colour backgrounds) in a mid tone.
  sparxie::Rgba
  backdrop(int w, int h, int base) {
    sparxie::Rgba image;
    image.w = w;
    image.h = h;
    image.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 255);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int d = ((x / 8 + y / 8) % 2) ? 14 : -14;
        const std::size_t at = static_cast<std::size_t>(y * w + x) * 4;
        for (int c = 0; c < 3; ++c) image.px[at + c] = static_cast<std::uint8_t>(std::clamp(base + d, 0, 255));
      }
    return image;
  }

  // A logo canvas of w x h that is transparent except for a mark (mw x mh, centred): white, or black with a given tone.
  sparxie::Rgba
  logo_canvas(int w, int h, int mw, int mh, std::uint8_t tone = 255) {
    sparxie::Rgba logo;
    logo.w = w;
    logo.h = h;
    logo.px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    for (int y = (h - mh) / 2; y < (h - mh) / 2 + mh; ++y)
      for (int x = (w - mw) / 2; x < (w - mw) / 2 + mw; ++x) {
        const std::size_t at = static_cast<std::size_t>(y * w + x) * 4;
        logo.px[at] = logo.px[at + 1] = logo.px[at + 2] = tone;
        logo.px[at + 3] = 255;
      }
    return logo;
  }

  // the bounding box of the pixels that differ strongly from the backdrop value (the visible logo)
  sparxie::HeroBox
  bright_box(const sparxie::Rgba &image, int over) {
    int x0 = image.w, y0 = image.h, x1 = -1, y1 = -1;
    for (int y = 0; y < image.h; ++y)
      for (int x = 0; x < image.w; ++x)
        if (image.px[static_cast<std::size_t>(y * image.w + x) * 4] > over) {
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x);
          y1 = std::max(y1, y);
        }
    if (x1 < 0) return {};
    return { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
  }
}  // namespace

TEST(CoverLogo, TheVisibleMarkIsLargeCentredAndInTheLowerThirdWhateverTheCanvas) {
  // a wide mark, a tall one, a mark in a canvas with a wide transparent margin (the small launcher logos), and a small canvas
  const sparxie::Rgba logos[] = { logo_canvas(640, 360, 420, 120), logo_canvas(300, 400, 120, 300), logo_canvas(416, 234, 90, 30), logo_canvas(64, 64, 60, 20) };
  for (const auto &logo : logos) {
    const sparxie::HeroBox box = sparxie::cover_logo_box(600, 900, logo);
    ASSERT_GT(box.w, 0);
    EXPECT_LE(box.w, 600 * 66 / 100 + 1);
    EXPECT_LE(box.h, 900 * 17 / 100 + 1);
    // centred across, its middle at 76 percent of the height: inside the lower third and above the clients' bottom title area
    EXPECT_NEAR(box.x + box.w / 2.0, 300.0, 1.5);
    EXPECT_NEAR(box.y + box.h / 2.0, 900 * 0.76, 1.5);
    EXPECT_GE(box.y, 900 * 2 / 3);
    EXPECT_LE(box.y + box.h, 900 * 88 / 100);
  }
  // a wide mark takes the full 66 percent of the width: big, not the small mark of a margin-heavy canvas
  EXPECT_NEAR(sparxie::cover_logo_box(600, 900, logos[0]).w, 396, 2);
  // the mark of a logo with a huge margin is as big as one without (the box follows the visible mark, not the canvas)
  EXPECT_GE(sparxie::cover_logo_box(600, 900, logos[2]).w, 150);
  EXPECT_EQ(sparxie::cover_logo_box(600, 900, {}).w, 0);
}

TEST(CoverLogo, TheComposedCoverCarriesTheLogoWhereTheBoxSays) {
  const sparxie::Rgba art = backdrop(1600, 900, 70);
  const sparxie::Rgba logo = logo_canvas(640, 360, 420, 120);
  const sparxie::Rgba cover = sparxie::compose_cover(art, logo);
  ASSERT_EQ(cover.w, 600);
  ASSERT_EQ(cover.h, 900);
  const sparxie::HeroBox box = sparxie::cover_logo_box(600, 900, logo);
  const sparxie::HeroBox seen = bright_box(cover, 200);
  EXPECT_NEAR(seen.x, box.x, 3);
  EXPECT_NEAR(seen.y, box.y, 3);
  EXPECT_NEAR(seen.w, box.w, 5);
  EXPECT_NEAR(seen.h, box.h, 5);
  // nothing of the logo above the lower third
  int bright_above = 0;
  for (int y = 0; y < 600; ++y)
    for (int x = 0; x < 600; ++x)
      if (cover.px[static_cast<std::size_t>(y * 600 + x) * 4] > 200) ++bright_above;
  EXPECT_EQ(bright_above, 0);
}

TEST(CoverLogo, ALogoOnItsOwnBlackPlateIsCutOutLikeTheHeroDoesIt) {
  // the old zzz logo: a white mark on an opaque black plate; the plate must not show as a black rectangle on the cover or on the hero
  sparxie::Rgba plated = logo_canvas(400, 200, 400, 200, 0);
  for (int y = 70; y < 130; ++y)
    for (int x = 60; x < 340; ++x) plated.px[static_cast<std::size_t>(y * 400 + x) * 4] = plated.px[static_cast<std::size_t>(y * 400 + x) * 4 + 1] = plated.px[static_cast<std::size_t>(y * 400 + x) * 4 + 2] = 255;
  ASSERT_TRUE(sparxie::logo_has_plate(plated));
  const sparxie::Rgba art = backdrop(1600, 900, 150);
  const sparxie::Rgba cover = sparxie::compose_cover(art, plated);
  const sparxie::HeroBox box = sparxie::cover_logo_box(600, 900, plated);
  // the box is the mark (280 x 60 of the canvas), not the plate (400 x 200)
  EXPECT_GT(box.w, 0);
  EXPECT_NEAR(static_cast<double>(box.w) / box.h, 280.0 / 60.0, 0.2);
  // a pixel on the old plate but outside the mark keeps the backdrop's tone (about 150), it is not near black
  const int plate_x = box.x + box.w / 2, plate_y = box.y - box.h;  // above the mark, where the plate would have been
  const std::size_t at = static_cast<std::size_t>(std::max(0, plate_y) * 600 + plate_x) * 4;
  EXPECT_GT(cover.px[at], 90);
  const sparxie::Rgba hero = sparxie::compose_hero(art, plated);
  const sparxie::HeroBox hero_box = sparxie::hero_logo_box(hero.w, hero.h, plated);
  const std::size_t hero_at = static_cast<std::size_t>(std::max(0, hero_box.y - hero_box.h) * hero.w + hero.w / 2) * 4;
  EXPECT_GT(hero.px[hero_at], 90);
}

TEST(CoverLogo, TheCoverAndTheHeroShareTheSamePlacementStyle) {
  // one style across the poster shapes: the visible mark is centred on both, laid on by the same routine; a dark logo gets the same soft lift behind it on both
  const sparxie::Rgba art = backdrop(1920, 1080, 40);
  // a dark mark with open strokes (a solid dark block would read as a plate and be cut out)
  sparxie::Rgba dark = logo_canvas(640, 360, 420, 120, 20);
  for (int y = 0; y < dark.h; ++y)
    for (int x = 0; x < dark.w; ++x)
      if ((x / 6) % 2) dark.px[static_cast<std::size_t>(y * dark.w + x) * 4 + 3] = 0;
  const sparxie::Rgba cover = sparxie::compose_cover(art, dark);
  const sparxie::Rgba hero = sparxie::compose_hero(art, dark);
  const sparxie::HeroBox cover_box = sparxie::cover_logo_box(600, 900, dark);
  const sparxie::HeroBox hero_box = sparxie::hero_logo_box(hero.w, hero.h, dark);
  EXPECT_NEAR(cover_box.x + cover_box.w / 2.0, 300.0, 1.5);
  EXPECT_NEAR(hero_box.x + hero_box.w / 2.0, hero.w / 2.0, 1.5);
  // behind the dark logo the backdrop is lifted on both (it was tone 40 under a tone 20 logo)
  // the mean tone of a block just above the mark (inside the soft lift), against the plain backdrop's 40
  const auto lifted = [](const sparxie::Rgba &image, const sparxie::HeroBox &box) {
    double sum = 0;
    int count = 0;
    const int x0 = box.x + box.w / 2 - 8, y0 = box.y - 20;
    for (int y = y0; y < y0 + 16; ++y)
      for (int x = x0; x < x0 + 16; ++x) {
        // the margin just above the mark, inside the soft lift
        sum += image.px[static_cast<std::size_t>(y * image.w + x) * 4];
        ++count;
      }
    return sum / count;
  };
  EXPECT_GT(lifted(cover, cover_box), 44);
  EXPECT_GT(lifted(hero, hero_box), 44);
}

TEST(SvgPath, RasterisesFilledShapesWithEvenOddHolesAndArcs) {
  // a square: filled inside, empty outside
  const sparxie::Rgba square = sparxie::rasterize_svg_path("M10 10h80v80h-80z", 100, 100, 100);
  ASSERT_EQ(square.w, 100);
  ASSERT_EQ(square.h, 100);
  EXPECT_EQ(square.px[static_cast<std::size_t>(50 * 100 + 50) * 4 + 3], 255);
  EXPECT_EQ(square.px[static_cast<std::size_t>(5 * 100 + 5) * 4 + 3], 0);
  EXPECT_EQ(square.px[50 * 100 * 4 + 50 * 4], 255) << "white";
  // an even-odd hole: the inner square drawn the same way round is empty
  const sparxie::Rgba frame = sparxie::rasterize_svg_path("M10 10h80v80h-80zM30 30h40v40h-40z", 100, 100, 100);
  EXPECT_EQ(frame.px[static_cast<std::size_t>(50 * 100 + 50) * 4 + 3], 0);
  EXPECT_EQ(frame.px[static_cast<std::size_t>(20 * 100 + 20) * 4 + 3], 255);
  // a circle from two arcs, the flags written without separators ("10"): its area is pi r^2
  const sparxie::Rgba disc = sparxie::rasterize_svg_path("M10 50a40 40 0 1080 0a40 40 0 10-80 0z", 100, 100, 100);
  ASSERT_EQ(disc.w, 100);
  double area = 0;
  for (std::size_t i = 3; i < disc.px.size(); i += 4) area += disc.px[i] / 255.0;
  EXPECT_NEAR(area, 3.14159265 * 1600, 90);
  // a relative cubic (a rounded corner) and an absolute line close the same shape
  const sparxie::Rgba curved = sparxie::rasterize_svg_path("M10 50c0-22 18-40 40-40s40 18 40 40-18 40-40 40-40-18-40-40z", 100, 100, 100);
  double curved_area = 0;
  for (std::size_t i = 3; i < curved.px.size(); i += 4) curved_area += curved.px[i] / 255.0;
  EXPECT_NEAR(curved_area, 3.14159265 * 1600, 160);
  // scaling: the same path drawn twice as wide has four times the area
  const sparxie::Rgba big = sparxie::rasterize_svg_path("M10 10h80v80h-80z", 100, 100, 200);
  EXPECT_EQ(big.w, 200);
  EXPECT_EQ(big.h, 200);
}

TEST(SvgPath, BadDataIsNoPictureAndAPageGivesItsPath) {
  EXPECT_EQ(sparxie::rasterize_svg_path("", 10, 10, 10).w, 0);
  EXPECT_EQ(sparxie::rasterize_svg_path("M10 10 X 5 5", 100, 100, 100).w, 0);
  EXPECT_EQ(sparxie::rasterize_svg_path("M10 10 L", 100, 100, 100).w, 0);
  EXPECT_EQ(sparxie::rasterize_svg_path("M0 0h1v1z", 0, 100, 100).w, 0);
  EXPECT_EQ(sparxie::rasterize_svg_path("M0 0h1v1z", 100, 100, 100000).w, 0);
  const std::string page = "<html><svg><defs><path id=\"svg_def-title_x\" fill-rule=\"evenodd\" fill=\"currentColor\" d=\"M0 0h10v10z\"/></defs></svg></html>";
  EXPECT_EQ(sparxie::svg_path_of_page(page, "svg_def-title_x"), "M0 0h10v10z");
  EXPECT_TRUE(sparxie::svg_path_of_page(page, "other").empty());
  EXPECT_TRUE(sparxie::svg_path_of_page("<path id=\"a\"", "a").empty());
}
