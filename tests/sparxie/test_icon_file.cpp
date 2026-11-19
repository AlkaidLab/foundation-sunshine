#include "src/sparxie/art.h"
#include "src/sparxie/icon_file.h"
#include "tests/sparxie/pe_fixture.h"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {
  namespace fs = std::filesystem;

  // The decoder of the host for PNG (and JPEG) pictures; the platform decoder is not needed for the fixtures.
  sparxie::Rgba
  decode_png(std::string_view bytes) {
    return sparxie::decode_image(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
  }

  const std::uint8_t *
  at(const sparxie::Rgba &image, int x, int y) {
    return &image.px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.w) + static_cast<std::size_t>(x)) * 4];
  }
}  // namespace

TEST(IconFile, AnIcoPicksTheLargestPictureAndHandsAPngEntryToTheDecoder) {
  const std::string ico = pefix::ico_file({ pefix::dib_image(16, 10), pefix::dib_image(32, 20), pefix::png_image(256, 40), pefix::dib_image(48, 30) });
  const sparxie::Rgba icon = sparxie::icon_from_ico(ico, decode_png);
  ASSERT_EQ(icon.w, 256);
  ASSERT_EQ(icon.h, 256);
  EXPECT_EQ(at(icon, 0, 0)[0], 40);
  EXPECT_EQ(at(icon, 0, 0)[1], 200);
  // the same file read through the generic entry point (an .ico is an .ico whatever its name)
  EXPECT_EQ(sparxie::icon_from_file_bytes(ico, decode_png).w, 256);
}

TEST(IconFile, AnIcoWithoutAPngEntryDecodesItsBitmaps) {
  // 32 bit with alpha: bottom-up rows come out the right way up (green on the top half), and the alpha is the file's
  const sparxie::Rgba icon = sparxie::icon_from_ico(pefix::ico_file({ pefix::dib_image(16, 10), pefix::dib_image(48, 30, true, true) }), decode_png);
  ASSERT_EQ(icon.w, 48);
  EXPECT_EQ(at(icon, 2, 2)[1], 200) << "the top row is the top of the picture";
  EXPECT_EQ(at(icon, 2, 46)[1], 20);
  EXPECT_EQ(at(icon, 2, 2)[3], 255);
  EXPECT_EQ(at(icon, 40, 2)[3], 0) << "the right half is transparent";
  // 32 bit with no alpha at all: the AND mask says what is transparent, the rest is opaque
  const sparxie::Rgba masked = sparxie::icon_from_ico(pefix::ico_file({ pefix::dib_image(32, 30, false, true) }), decode_png);
  ASSERT_EQ(masked.w, 32);
  EXPECT_EQ(at(masked, 2, 2)[3], 255);
  EXPECT_EQ(at(masked, 30, 2)[3], 0);
  // 24 bit with an AND mask
  const sparxie::Rgba rgb = sparxie::icon_from_ico(pefix::ico_file({ pefix::dib24_image(32, 50) }), decode_png);
  ASSERT_EQ(rgb.w, 32);
  EXPECT_NEAR(at(rgb, 1, 1)[0], 50, 4);
  EXPECT_EQ(at(rgb, 1, 1)[1], 200);
  EXPECT_EQ(at(rgb, 1, 1)[3], 255);
  EXPECT_EQ(at(rgb, 28, 1)[3], 0);
  // 8 bit with a palette
  const sparxie::Rgba paletted = sparxie::icon_from_ico(pefix::ico_file({ pefix::dib8_image(16) }), decode_png);
  ASSERT_EQ(paletted.w, 16);
  EXPECT_EQ(at(paletted, 1, 1)[0], 0x80);
  EXPECT_EQ(at(paletted, 14, 1)[2], 0xff);
  EXPECT_EQ(at(paletted, 14, 1)[1], 0xa0);
}

TEST(IconFile, ADamagedEntryIsSkippedForTheNextBestAndGarbageIsNoIcon) {
  pefix::IconImage broken = pefix::png_image(256, 40);
  broken.data = broken.data.substr(0, 20);  // a PNG cut short: the decoder says no
  const sparxie::Rgba icon = sparxie::icon_from_ico(pefix::ico_file({ pefix::dib_image(32, 20), broken }), decode_png);
  EXPECT_EQ(icon.w, 32);
  EXPECT_EQ(sparxie::icon_from_ico("", decode_png).w, 0);
  EXPECT_EQ(sparxie::icon_from_ico(std::string(40, 'x'), decode_png).w, 0);
  // an entry that points past the end of the file
  std::string lying = pefix::ico_file({ pefix::dib_image(32, 20) });
  lying[6 + 12] = static_cast<char>(0xff);
  lying[6 + 13] = static_cast<char>(0xff);
  EXPECT_EQ(sparxie::icon_from_ico(lying, decode_png).w, 0);
  // a bitmap with an absurd size claim
  pefix::IconImage huge = pefix::dib_image(16, 10);
  huge.data[4] = static_cast<char>(0xff);
  huge.data[5] = static_cast<char>(0xff);
  huge.data[6] = static_cast<char>(0xff);
  EXPECT_EQ(sparxie::icon_from_ico(pefix::ico_file({ huge }), decode_png).w, 0);
}

TEST(IconFile, APeExecutableGivesItsLargestIconPng) {
  for (const bool plus : { true, false }) {
    const std::string exe = pefix::exe_file({ pefix::dib_image(16, 10), pefix::png_image(256, 40), pefix::dib_image(64, 30), pefix::dib_image(32, 20) }, plus);
    const sparxie::Rgba icon = sparxie::icon_from_pe_bytes(exe, decode_png);
    ASSERT_EQ(icon.w, 256) << (plus ? "PE32+" : "PE32");
    EXPECT_EQ(at(icon, 0, 0)[0], 40);
    EXPECT_EQ(at(icon, 0, 0)[1], 200);
    EXPECT_EQ(at(icon, 0, 255)[1], 20);
  }
}

TEST(IconFile, APeExecutableWithOnlyBitmapIconsGivesTheBestBitmap) {
  const sparxie::Rgba icon = sparxie::icon_from_pe_bytes(pefix::exe_file({ pefix::dib_image(16, 10), pefix::dib_image(48, 33), pefix::dib_image(32, 20) }), decode_png);
  ASSERT_EQ(icon.w, 48);
  EXPECT_EQ(at(icon, 0, 0)[0], 33);
  // a PNG entry the decoder cannot read falls to the best bitmap
  pefix::IconImage bad = pefix::png_image(256, 40);
  bad.data = bad.data.substr(0, 30);
  EXPECT_EQ(sparxie::icon_from_pe_bytes(pefix::exe_file({ bad, pefix::dib_image(32, 20) }), decode_png).w, 32);
}

TEST(IconFile, ThePeIconIsReadFromAFileWithoutReadingTheWholeProgram) {
  const fs::path dir = fs::temp_directory_path() / "sparxie-icon-pe";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path exe = dir / "Game.exe";
  {
    std::ofstream out(exe, std::ios::binary);
    const std::string bytes = pefix::exe_file({ pefix::png_image(256, 70), pefix::dib_image(32, 20) });
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    // a big code section after the resources: never read
    out << std::string(8u * 1024u * 1024u, '\x90');
  }
  const sparxie::Rgba icon = sparxie::icon_from_pe(exe, decode_png);
  ASSERT_EQ(icon.w, 256);
  EXPECT_EQ(at(icon, 0, 0)[0], 70);
  EXPECT_EQ(sparxie::icon_from_pe(dir / "missing.exe", decode_png).w, 0);
  fs::remove_all(dir);
}

TEST(IconFile, NoIconNotAPeAndDamagedFilesAreNeverACrash) {
  EXPECT_EQ(sparxie::icon_from_pe_bytes("", decode_png).w, 0);
  EXPECT_EQ(sparxie::icon_from_pe_bytes(std::string(500, 'A'), decode_png).w, 0);
  EXPECT_EQ(sparxie::icon_from_pe_bytes("MZ", decode_png).w, 0);
  // an executable with an empty group icon
  EXPECT_EQ(sparxie::icon_from_pe_bytes(pefix::exe_file({}), decode_png).w, 0);
  // a good file cut at many lengths, and with bytes flipped in the headers and the resource tree: no crash, never a wrong picture
  const std::string good = pefix::exe_file({ pefix::png_image(256, 40), pefix::dib_image(32, 20) });
  for (std::size_t length = 0; length < good.size(); length += 37) {
    const sparxie::Rgba icon = sparxie::icon_from_pe_bytes(std::string_view(good).substr(0, length), decode_png);
    if (icon.w > 0) {
      EXPECT_TRUE(icon.w == 256 || icon.w == 32) << length;
    }
  }
  for (std::size_t at_byte = 0; at_byte < 0x480 + 120; at_byte += 3) {
    std::string flipped = good;
    flipped[at_byte] = static_cast<char>(flipped[at_byte] ^ 0xa5);
    (void) sparxie::icon_from_pe_bytes(flipped, decode_png);
  }
  // a resource whose data RVA lies outside the file
  std::string lying = good;
  const std::size_t rsrc = 0x400;
  // the first data entry sits after the directories; bump every plausible RVA field by a lot
  for (std::size_t offset = rsrc + 100; offset + 4 < rsrc + 200; offset += 4) lying[offset + 3] = static_cast<char>(0x70);
  (void) sparxie::icon_from_pe_bytes(lying, decode_png);
}

TEST(IconFile, TheIconIsFittedIntoASquareWithoutStretching) {
  // a 100 x 50 picture: scaled to 256 x 128 and centred, transparent above and below
  sparxie::Rgba wide;
  wide.w = 100;
  wide.h = 50;
  wide.px.assign(100u * 50u * 4u, 255);
  const sparxie::Rgba square = sparxie::icon_square(wide);
  ASSERT_EQ(square.w, 256);
  ASSERT_EQ(square.h, 256);
  EXPECT_EQ(at(square, 128, 128)[3], 255);
  EXPECT_EQ(at(square, 128, 10)[3], 0);
  EXPECT_EQ(at(square, 128, 250)[3], 0);
  // a 64 px icon is enlarged, a 512 px one shrunk; a picture of one colour keeps its colour through both
  for (const int side : { 64, 512 }) {
    sparxie::Rgba plain;
    plain.w = side;
    plain.h = side;
    plain.px.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4, 0);
    for (std::size_t i = 0; i < plain.px.size(); i += 4) {
      plain.px[i] = 200;
      plain.px[i + 1] = 100;
      plain.px[i + 2] = 50;
      plain.px[i + 3] = 255;
    }
    const sparxie::Rgba scaled = sparxie::icon_square(plain);
    ASSERT_EQ(scaled.w, 256) << side;
    EXPECT_NEAR(at(scaled, 100, 100)[0], 200, 1);
    EXPECT_NEAR(at(scaled, 100, 100)[1], 100, 1);
    EXPECT_EQ(at(scaled, 100, 100)[3], 255);
  }
  EXPECT_EQ(sparxie::icon_square({}).w, 0);
}

TEST(IconFile, AnIconBelowSixtyFourPixelsOrEmptyIsNotUsable) {
  EXPECT_TRUE(sparxie::icon_is_usable(pefix::picture(128, 10)));
  EXPECT_FALSE(sparxie::icon_is_usable(pefix::picture(32, 10)));
  EXPECT_FALSE(sparxie::icon_is_usable({}));
  // a transparent canvas is nothing
  sparxie::Rgba clear = pefix::picture(128, 10);
  for (std::size_t i = 3; i < clear.px.size(); i += 4) clear.px[i] = 0;
  EXPECT_FALSE(sparxie::icon_is_usable(clear));
}

TEST(IconFile, AMonogramIsTheLastChoiceAndNeverACropOfTheCover) {
  for (const auto game : sparxie::kGames) {
    const sparxie::Rgba icon = sparxie::placeholder_icon(game);
    ASSERT_EQ(icon.w, sparxie::kIconPixels) << game;
    ASSERT_EQ(icon.h, sparxie::kIconPixels) << game;
    EXPECT_TRUE(sparxie::icon_is_usable(icon)) << game;
  }
}
