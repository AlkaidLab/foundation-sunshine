#include "src/generated_cover.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include "test_temp_paths.h"

TEST(GeneratedCover, AcceptsExactlyTheSixStylesAndFallsBackToTonal) {
  for (const auto style : { "tonal", "monogram", "icon", "editorial", "glass", "geometric" }) {
    EXPECT_TRUE(generated_cover::is_style_name(style));
  }
  EXPECT_FALSE(generated_cover::is_style_name("auto"));
  EXPECT_FALSE(generated_cover::is_style_name("unknown"));

  generated_cover::app_t app;
  app.cover_style = "auto";
  EXPECT_EQ(generated_cover::style_name(generated_cover::effective_style(app, "tonal")), "tonal");
  EXPECT_EQ(generated_cover::style_name(generated_cover::effective_style(app, "unknown")), "tonal");
  app.cover_style = "geometric";
  EXPECT_EQ(generated_cover::style_name(generated_cover::effective_style(app, "tonal")), "geometric");
}

TEST(GeneratedCover, HueAndMonogramFollowTheCoverDesign) {
  // FNV-1a over the UTF-8 bytes
  EXPECT_EQ(generated_cover::name_hash(""), 0x811c9dc5u);
  EXPECT_EQ(generated_cover::name_hash("a"), 0xe40c292cu);
  struct row_t {
    const char *name;
    std::uint32_t hash;
    const char *monogram;
  };
  const std::array<row_t, 10> rows { {
    { "桌面", 466552780u, "桌" },
    { "Steam Big Picture", 2466769215u, "SB" },
    { "原神", 2663344731u, "原" },
    { "崩坏：星穹铁道", 3626665237u, "崩" },
    { "Microsoft Edge", 2273132764u, "ME" },
    { "The Legend of Zelda: Breath of the Wild – Definitive Edition Collector's Bundle", 3882049785u, "TL" },
    { "ファイナルファンタジーXIV", 1642498772u, "フ" },
    { "🎮 Game Night 🔥", 1756931222u, "GN" },
    { "X", 3708558887u, "X" },
    { "Google Chrome", 3466005000u, "GC" },
  } };
  for (const auto &row : rows) {
    EXPECT_EQ(generated_cover::name_hash(row.name), row.hash) << row.name;
    EXPECT_EQ(generated_cover::monogram(row.name), row.monogram) << row.name;
  }
  EXPECT_EQ(generated_cover::monogram("🎮"), "🎮");
  EXPECT_EQ(generated_cover::monogram("..."), ".");
  EXPECT_EQ(generated_cover::monogram(""), "?");
  EXPECT_EQ(generated_cover::monogram("Éclair-Noir"), "ÉN");
  EXPECT_EQ(generated_cover::monogram("one"), "O");
}

TEST(GeneratedCover, TheKatakanaMiddleDotIsPunctuationAndNeverTheLetterOfACover) {
  EXPECT_EQ(generated_cover::monogram("\xE3\x83\xBB\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0"), "\xE3\x82\xB2") << "the middle dot, then the game";
  EXPECT_EQ(generated_cover::monogram("\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0"), "\xE3\x82\xB2");
  EXPECT_EQ(generated_cover::monogram("\xE3\x82\xA0\xE3\x82\xB2"), "\xE3\x82\xB2") << "the double hyphen too";
}

TEST(GeneratedCover, TheCyrillicYoIsUppercasedLikeTheOtherLettersOfAName) {
  EXPECT_EQ(generated_cover::monogram("\xD1\x91\xD0\xBB\xD0\xBA\xD0\xB8"), "\xD0\x81") << "ёлки";
  EXPECT_EQ(generated_cover::monogram("\xD0\x81\xD0\xBB\xD0\xBA\xD0\xB8"), "\xD0\x81") << "Ёлки";
  EXPECT_EQ(generated_cover::monogram("\xD0\xBC\xD0\xB8\xD1\x80 \xD1\x91\xD0\xB6"), "\xD0\x9C\xD0\x81") << "мир ёж";
}

TEST(GeneratedCover, CacheKeyIncludesEveryDeterministicInput) {
  const auto portrait = generated_cover::cache_hash("tonal", generated_cover::orientation_e::portrait, "桌面", "C:/game.exe", 42);
  EXPECT_EQ(portrait, generated_cover::cache_hash("tonal", generated_cover::orientation_e::portrait, "桌面", "C:/game.exe", 42));
  EXPECT_NE(portrait, generated_cover::cache_hash("tonal", generated_cover::orientation_e::landscape, "桌面", "C:/game.exe", 42));
  EXPECT_NE(portrait, generated_cover::cache_hash("monogram", generated_cover::orientation_e::portrait, "桌面", "C:/game.exe", 42));
  EXPECT_NE(portrait, generated_cover::cache_hash("tonal", generated_cover::orientation_e::portrait, "桌面2", "C:/game.exe", 42));
  EXPECT_NE(portrait, generated_cover::cache_hash("tonal", generated_cover::orientation_e::portrait, "桌面", "C:/other.exe", 42));
  EXPECT_NE(portrait, generated_cover::cache_hash("tonal", generated_cover::orientation_e::portrait, "桌面", "C:/game.exe", 43));
}

namespace {
  // the art version of an application that shows a generated cover carries the cover only where the host draws it
#ifdef _WIN32
  constexpr bool kHostDraws = true;
#else
  constexpr bool kHostDraws = false;
#endif

  // the host's assets folder, a picture of the person's, and a picture of the person's that merely is called box.png
  struct placeholder_fixture_t {
    std::filesystem::path root = test_support::unique_temp_path("generated-cover-selection");
    std::filesystem::path placeholder = root / "assets" / "box.png";
    std::filesystem::path poster = root / "real.png";
    std::filesystem::path own_box = root / "mine" / "box.png";

    placeholder_fixture_t() {
      std::filesystem::create_directories(root / "assets");
      std::filesystem::create_directories(root / "mine");
      for (const auto &file : { placeholder, poster, own_box }) std::ofstream(file) << "test";
    }

    ~placeholder_fixture_t() { test_support::remove_tree_quiet(root); }

    // the application as the host hands it over: what apps.json says and the file its poster lookup found
    generated_cover::app_t
    app(std::string name, std::string image_path, std::string poster_path, bool xbox = false) const {
      generated_cover::app_t result;
      result.id = name;
      result.name = std::move(name);
      result.image_path = std::move(image_path);
      result.poster_path = std::move(poster_path);
      result.placeholder_path = placeholder.string();
      result.xbox_app = xbox;
      return result;
    }
  };
}  // namespace

TEST(GeneratedCover, APosterWhoseNameIsNotAsciiIsTheCoverWhateverTheCodePageOfTheHostIs) {
  const placeholder_fixture_t fixture;
  const auto utf8 = [](const std::filesystem::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
  };
  const auto poster = fixture.root / std::filesystem::path(u8"\u539f\u795e.png");
  const auto hero = fixture.root / std::filesystem::path(u8"\u539f\u795e-hero.png");
  std::ofstream(poster) << "a poster";
  std::ofstream(hero) << "a landscape picture";
  ASSERT_TRUE(std::filesystem::exists(poster)) << "the fixture file is where the native path says";
  auto app = fixture.app("Genshin", utf8(poster), utf8(poster));
  app.hero_path = utf8(hero);
  const auto cover = generated_cover::describe(app, "tonal", fixture.root);
  EXPECT_EQ(cover.source, generated_cover::source_e::poster) << "the person's own poster is not replaced by a generated cover";
  EXPECT_EQ(cover.portrait.path, poster);
  EXPECT_EQ(cover.landscape.path, hero);
  EXPECT_TRUE(generated_cover::art_fact(app, "tonal").empty());
  EXPECT_FALSE(generated_cover::shows_placeholder(app));
  // the same path is also the placeholder's: an application that has no landscape picture shows the poster as its landscape cover
  app.hero_path.clear();
  EXPECT_EQ(generated_cover::describe(app, "tonal", fixture.root).landscape.path, poster);
}

TEST(GeneratedCover, EveryWayAnAppEndsOnThePlaceholderGetsAGeneratedCover) {
  const placeholder_fixture_t fixture;
  const auto box = fixture.placeholder.string();
  const std::array<generated_cover::app_t, 11> placeholders { {
    fixture.app("no path", "", box),
    fixture.app("blank path", "  ", box),
    fixture.app("bare name", "box.png", box),
    fixture.app("capitals", "BOX.PNG", box),
    fixture.app("older relative", "./assets/box.png", box),
    fixture.app("older windows relative", ".\\assets\\box.png", box),
    fixture.app("quoted", "\"box.png\"", box),
    fixture.app("absolute", box, box),
    fixture.app("file is gone", (fixture.root / "gone" / "missing.png").string(), box),
    fixture.app("lookup found nothing", (fixture.root / "gone" / "missing.png").string(), ""),
    fixture.app("not an image the host can use", (fixture.root / "notes.txt").string(), box),
  } };
  for (const auto &app : placeholders) {
    EXPECT_TRUE(generated_cover::shows_placeholder(app)) << app.name;
    EXPECT_EQ(generated_cover::describe(app, "tonal", fixture.root).source, generated_cover::source_e::generated) << app.name;
    EXPECT_EQ(!generated_cover::art_fact(app, "tonal").empty(), kHostDraws) << app.name;
  }
}

TEST(GeneratedCover, ARealPosterIsNeverReplacedNotEvenOneCalledBoxPng) {
  const placeholder_fixture_t fixture;
  const std::array<generated_cover::app_t, 3> posters { {
    fixture.app("poster", fixture.poster.string(), fixture.poster.string()),
    fixture.app("relative poster", "real.png", fixture.poster.string()),
    // the person's own picture is called box.png too: it is not the host's file
    fixture.app("own box", fixture.own_box.string(), fixture.own_box.string()),
  } };
  for (const auto &app : posters) {
    EXPECT_FALSE(generated_cover::shows_placeholder(app)) << app.name;
    EXPECT_EQ(generated_cover::describe(app, "tonal", fixture.root).source, generated_cover::source_e::poster) << app.name;
    EXPECT_TRUE(generated_cover::art_fact(app, "tonal").empty()) << app.name << ": a poster adds nothing to the art version";
  }
}

TEST(GeneratedCover, TheXboxAppKeepsItsXboxArtAndAXboxAppWithAPosterKeepsThePoster) {
  const placeholder_fixture_t fixture;
  const auto box = fixture.placeholder.string();
  for (const auto *image : { "box.png", "", "./assets/box.png" }) {
    const auto app = fixture.app("Xbox Game", image, box, true);
    EXPECT_FALSE(generated_cover::shows_placeholder(app)) << image;
    EXPECT_EQ(generated_cover::describe(app, "tonal", fixture.root).source, generated_cover::source_e::xbox) << image;
    EXPECT_TRUE(generated_cover::art_fact(app, "tonal").empty()) << image << ": the Xbox art is not a generated cover";
  }
  const auto own = fixture.app("Xbox Game", fixture.poster.string(), fixture.poster.string(), true);
  EXPECT_EQ(generated_cover::describe(own, "tonal", fixture.root).source, generated_cover::source_e::poster);
  // the same picture on an app that is not the Xbox app is a placeholder
  EXPECT_EQ(generated_cover::describe(fixture.app("Xbox Game", "box.png", box), "tonal", fixture.root).source, generated_cover::source_e::generated);
}

TEST(GeneratedCover, AnOwnLandscapePictureStaysTheLandscapeCoverOfAnAppOnThePlaceholder) {
  const placeholder_fixture_t fixture;
  const auto hero = fixture.root / "hero.png";
  std::ofstream(hero) << "a landscape picture of the person";
  auto game = fixture.app("Hero Only", "", fixture.placeholder.string());
  game.hero_path = hero.string();
  const auto cover = generated_cover::describe(game, "tonal", fixture.root);
  EXPECT_EQ(cover.source, generated_cover::source_e::generated) << "the portrait cover replaces the placeholder";
  EXPECT_EQ(cover.landscape.path, hero) << "the landscape picture of the person is not replaced";
  auto without = game;
  without.hero_path.clear();
  if (kHostDraws) {
    EXPECT_NE(generated_cover::art_fact(game, "tonal"), generated_cover::art_fact(without, "tonal")) << "the landscape cover is not part of the version";
  }
}

TEST(GeneratedCover, TheArtFactChangesWithWhatTheClientWouldSee) {
  const placeholder_fixture_t fixture;
  const auto box = fixture.placeholder.string();
  const auto game = fixture.app("Old Game", "box.png", box);
  const auto fact = generated_cover::art_fact(game, "tonal");
  if (!kHostDraws) {
    EXPECT_TRUE(fact.empty()) << "no cover is drawn here: the art version is what it was";
    EXPECT_TRUE(generated_cover::art_fact(game, "glass").empty());
    return;
  }
  ASSERT_FALSE(fact.empty());
  EXPECT_EQ(fact, generated_cover::art_fact(game, "tonal")) << "stable while nothing changes";
  EXPECT_NE(fact, generated_cover::art_fact(game, "glass")) << "the global style";
  auto own_style = game;
  own_style.cover_style = "monogram";
  EXPECT_NE(fact, generated_cover::art_fact(own_style, "tonal")) << "the style of the app";
  EXPECT_NE(fact, generated_cover::art_fact(fixture.app("Other Game", "box.png", box), "tonal")) << "the name";
  // the file that carries the cover is named by the same key
  EXPECT_NE(fact.find("cover:"), std::string::npos);
  EXPECT_EQ(fact, generated_cover::art_fact(fixture.app("Old Game", "", box), "tonal")) << "every way to the placeholder gives the same cover";
}

#ifdef _WIN32

  #define STBI_ONLY_PNG
  #define STBI_ONLY_JPEG
  #define STB_IMAGE_STATIC
  #define STB_IMAGE_IMPLEMENTATION
  #include "src/stb_image.h"

TEST(GeneratedCover, TheCleanUpOnlyRemovesTheFilesTheGeneratorWroteNeverAPictureOfThePerson) {
  const auto root = test_support::unique_temp_path("generated-cover-prune");
  const auto covers = root / "covers";
  std::filesystem::create_directories(covers);
  const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 40);
  const auto key = std::string(64, 'a');
  const auto make_old = [&](const std::string &name) {
    std::ofstream(covers / name) << "picture";
    std::filesystem::last_write_time(covers / name, old_time);
  };
  // a picture of the person's in the folder the covers are read from, with a name that looks like one of ours, and an old file that is one of ours
  for (const auto *name : { "gen_family.png", "gen_vacation.jpg", "poster.png" }) make_old(name);
  make_old("gen_" + std::string(63, 'a') + ".png");  // one digit short
  make_old("gen_" + std::string(64, 'B') + ".png");  // not the digits the generator writes (a name that differs from the key by more than the case: the file system may not tell the cases apart)
  make_old("gen_" + key + ".png");
  generated_cover::app_t app;
  app.id = "prune";
  app.name = "Any Name";
  ASSERT_FALSE(generated_cover::preview(app, generated_cover::style_e::tonal, generated_cover::orientation_e::portrait, root).path.empty());
  for (const auto *name : { "gen_family.png", "gen_vacation.jpg", "poster.png" }) EXPECT_TRUE(std::filesystem::exists(covers / name)) << name;
  EXPECT_TRUE(std::filesystem::exists(covers / ("gen_" + std::string(63, 'a') + ".png")));
  EXPECT_TRUE(std::filesystem::exists(covers / ("gen_" + std::string(64, 'B') + ".png")));
  EXPECT_FALSE(std::filesystem::exists(covers / ("gen_" + key + ".png"))) << "an old file of the generator is cleaned up";
  test_support::remove_tree_quiet(root);
}

TEST(GeneratedCover, GenericLaunchersNeverBecomeTheIconButTheDesktopMayUseExplorer) {
  const auto root = test_support::unique_temp_path("generated-cover-icons");
  std::filesystem::create_directories(root);
  const auto hash_of = [&root](const std::string &command, const std::string &image_path = "") {
    generated_cover::app_t app;
    app.id = "icon-test";
    app.name = "Same Name";
    app.command = command;
    app.image_path = image_path;
    const auto asset = generated_cover::preview(app, generated_cover::style_e::icon, generated_cover::orientation_e::portrait, root);
    EXPECT_FALSE(asset.path.empty()) << command;
    return asset.hash;
  };
  const auto none = hash_of("");
  for (const auto *command : { R"(cmd.exe /c start "" game)", R"(CMD /c game)", R"(powershell.exe -Command game)", R"(pwsh -File game.ps1)", R"(rundll32.exe shell32.dll,Control_RunDLL)",
         R"(C:\Windows\System32\notepad.exe)", R"(explorer.exe C:\Games)", "steam://open/bigpicture" }) {
    EXPECT_EQ(hash_of(command), none) << command;
  }
  EXPECT_NE(hash_of(R"(C:\Windows\System32\taskmgr.exe)"), none);
  // an address among the arguments of a program does not hide the program; a command that is only an address has none
  EXPECT_EQ(hash_of(R"("C:\Windows\System32\taskmgr.exe" steam://open/bigpicture)"), hash_of(R"(C:\Windows\System32\taskmgr.exe)"));
  EXPECT_NE(hash_of(R"(C:\Windows\System32\taskmgr.exe)"), hash_of(R"(C:\Windows\System32\mstsc.exe)"));
  EXPECT_NE(hash_of("", "desktop.png"), none);
  EXPECT_EQ(hash_of(R"(explorer.exe C:\Games)", "desktop.png"), hash_of("", "desktop.png"));
  test_support::remove_tree_quiet(root);
}

TEST(GeneratedCover, ConceptRoundNamesRenderBothOrientationsForEveryStyle) {
  const auto root = test_support::unique_temp_path("generated-cover-golden");
  std::filesystem::create_directories(root);
  const std::array names {
    "桌面",
    "Steam Big Picture",
    "原神",
    "崩坏：星穹铁道",
    "Microsoft Edge",
    "The Legend of Zelda: Breath of the Wild – Definitive Edition Collector's Bundle",
    "ファイナルファンタジーXIV",
    "🎮 Game Night 🔥",
    "X",
    "Google Chrome",
  };
  const std::array styles { "tonal", "monogram", "icon", "editorial", "glass", "geometric" };
  for (const auto name : names) {
    generated_cover::app_t app;
    app.id = name;
    app.name = name;
    app.command = R"(C:\Windows\System32\notepad.exe)";
    for (const auto style_name : styles) {
      const auto style = generated_cover::style_from_name(style_name);
      for (const auto orientation : { generated_cover::orientation_e::portrait, generated_cover::orientation_e::landscape }) {
        const auto asset = generated_cover::preview(app, style, orientation, root);
        ASSERT_FALSE(asset.path.empty()) << name << " " << style_name;
        ASSERT_TRUE(std::filesystem::is_regular_file(asset.path));
        std::ifstream input(asset.path, std::ios::binary | std::ios::ate);
        ASSERT_GT(input.tellg(), 1024);
        int width = 0;
        int height = 0;
        int channels = 0;
        const auto size = static_cast<int>(input.tellg());
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        input.read(reinterpret_cast<char *>(bytes.data()), size);
        auto *pixels = stbi_load_from_memory(bytes.data(), size, &width, &height, &channels, 4);
        ASSERT_NE(pixels, nullptr) << name << " " << style_name;
        EXPECT_EQ(width, orientation == generated_cover::orientation_e::portrait ? 600 : 1920);
        EXPECT_EQ(height, orientation == generated_cover::orientation_e::portrait ? 900 : 1080);
        stbi_image_free(pixels);
      }
    }
  }
  test_support::remove_tree_quiet(root);
}

#endif
