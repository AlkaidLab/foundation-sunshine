/**
 * @file tests/unit/test_default_apps.cpp
 * @brief The Big Screen entry is offered once to an apps.json that is older than it (src/default_apps.h).
 */
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include <boost/property_tree/json_parser.hpp>

#include "src/default_apps.h"

#include <gtest/gtest.h>

// The focused suite links the file migration path only; the host's log sinks
// are not under test. This is the same one-line logger set tools/test_beast_http.cpp uses.
#include "src/logging_severity.h"

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;

namespace pt = boost::property_tree;

namespace {
  pt::ptree
  parse(const std::string &json) {
    pt::ptree tree;
    std::istringstream in { json };
    pt::read_json(in, tree);
    return tree;
  }

  std::string
  write(const pt::ptree &tree) {
    std::ostringstream out;
    pt::write_json(out, tree);
    return out.str();
  }

  std::vector<std::string>
  names(const pt::ptree &tree) {
    std::vector<std::string> out;
    for (const auto &[_, app] : tree.get_child("apps")) {
      out.push_back(app.get<std::string>("name"));
    }
    return out;
  }

  // an apps.json of an install from before the entry
  const char *const old_file = R"({"env":{},"apps":[{"name":"Desktop","image-path":"desktop"},{"name":"Steam Big Picture","cmd":"steam://open/bigpicture"}]})";

  std::string
  read_text(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
  }

  const pt::ptree *
  app_named(const pt::ptree &tree, const std::string &name) {
    for (const auto &[_, app] : tree.get_child("apps")) {
      if (app.get<std::string>("name") == name) return &app;
    }
    return nullptr;
  }
}  // namespace

TEST(DefaultApps, ExistingFileGetsBigScreenOnceAtTheEnd) {
  auto tree = parse(old_file);
  EXPECT_TRUE(default_apps::offer_big_screen(tree, false));
  EXPECT_EQ(names(tree), (std::vector<std::string> { "Desktop", "Steam Big Picture", "Big Screen" }));
  EXPECT_TRUE(default_apps::was_offered(tree, default_apps::big_screen_id));
  // the second start finds the record and changes nothing
  EXPECT_FALSE(default_apps::offer_big_screen(tree, false));
  EXPECT_EQ(names(tree).size(), 3u);
}

TEST(DefaultApps, ADeletedEntryNeverReturns) {
  auto tree = parse(old_file);
  ASSERT_TRUE(default_apps::offer_big_screen(tree, false));
  // the person saves the list through the Web UI without the entry; the record is a key of the file that the Web UI keeps
  auto saved = parse(write(tree));
  auto &apps = saved.get_child("apps");
  pt::ptree kept;
  for (const auto &[_, app] : apps) {
    if (app.get<std::string>("name") != "Big Screen") {
      kept.push_back({ "", app });
    }
  }
  saved.erase("apps");
  saved.push_back({ "apps", kept });
  EXPECT_EQ(names(saved).size(), 2u);
  EXPECT_FALSE(default_apps::offer_big_screen(saved, false));
  EXPECT_EQ(names(saved).size(), 2u);
}

TEST(DefaultApps, TheBundledFileOfANewInstallIsNotOfferedAgain) {
  // a new install: the bundled file has the entry and the record
  auto tree = parse(old_file);
  ASSERT_TRUE(default_apps::offer_big_screen(tree, true));
  auto fresh = parse(write(tree));
  EXPECT_FALSE(default_apps::offer_big_screen(fresh, true));
  EXPECT_EQ(names(fresh).size(), 3u);
}

TEST(DefaultApps, ACopyThePersonMadeIsKeptAndNotDuplicated) {
  auto tree = parse(R"({"env":{},"apps":[{"name":"My couch UI","detached":[".\\assets\\gui\\sunshine-gui.exe --desktop"]}]})");
  EXPECT_TRUE(default_apps::offer_big_screen(tree, false));
  EXPECT_EQ(names(tree), (std::vector<std::string> { "My couch UI" }));
  EXPECT_TRUE(default_apps::was_offered(tree, default_apps::big_screen_id));
  EXPECT_FALSE(default_apps::offer_big_screen(tree, false));
}

TEST(DefaultApps, NameFollowsTheLocale) {
  EXPECT_TRUE(default_apps::locale_is_zh("zh"));
  EXPECT_TRUE(default_apps::locale_is_zh("zh_TW"));
  EXPECT_TRUE(default_apps::locale_is_zh("zh-Hans"));
  EXPECT_FALSE(default_apps::locale_is_zh("en"));
  EXPECT_FALSE(default_apps::locale_is_zh(""));
  auto tree = parse(old_file);
  ASSERT_TRUE(default_apps::offer_big_screen(tree, true));
  EXPECT_EQ(names(tree).back(), "\xE5\xA4\xA7\xE5\xB1\x8F");
}

TEST(DefaultApps, TheEntryOpensThePanelDetachedAndClosesItWhenTheStreamEnds) {
  const auto app = default_apps::big_screen_app(false);
  // a desktop stream: no command of its own, so the stream is the host's screen with the big screen on it
  EXPECT_FALSE(app.get_optional<std::string>("cmd"));
  const auto &detached = app.get_child("detached");
  ASSERT_EQ(detached.size(), 1u);
  EXPECT_EQ(detached.begin()->second.get_value<std::string>(), std::string { default_apps::big_screen_open_cmd });
  const auto &prep = app.get_child("prep-cmd").begin()->second;
  EXPECT_EQ(prep.get<std::string>("do"), "");
  EXPECT_EQ(prep.get<std::string>("undo"), std::string { default_apps::big_screen_close_cmd });
  EXPECT_TRUE(default_apps::opens_big_screen(app));
}

TEST(DefaultApps, AFileThatIsNotAnAppsFileIsLeftAlone) {
  auto tree = parse(R"({"env":{}})");
  EXPECT_FALSE(default_apps::offer_big_screen(tree, false));
  EXPECT_FALSE(default_apps::was_offered(tree, default_apps::big_screen_id));
}

TEST(DefaultApps, AReleasedAppsFileKeepsEveryEntryWhenTheOfferRuns) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "config_upgrade";
  for (const char *era : { "2026.1031", "2026.1112" }) {
    SCOPED_TRACE(era);
    const auto source = root / era / "apps.json";
    const auto copy = std::filesystem::temp_directory_path() / ("sunshine-apps-upgrade-" + std::string(era) + ".json");
    std::error_code ignored;
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing, ignored);
    ASSERT_FALSE(ignored) << source;

    const auto before = parse(read_text(source));
    default_apps::migrate_file(copy.string(), "en");
    const auto after = parse(read_text(copy));

    EXPECT_EQ(write(after.get_child("env")), write(before.get_child("env")));
    EXPECT_EQ(after.get_child("apps").size(), before.get_child("apps").size() + 1u);
    for (const auto &[_, before_app] : before.get_child("apps")) {
      const auto name = before_app.get<std::string>("name");
      const auto *after_app = app_named(after, name);
      ASSERT_NE(after_app, nullptr) << name;
      for (const char *field : { "cmd", "image-path", "output", "working-dir", "elevated", "auto-detach",
             "wait-all", "exit-timeout", "mouse-mode", "gamepad", "exclude-global-prep-cmd",
             "prep-cmd", "menu-cmd", "detached", "rtx-hdr" }) {
        const auto expected = before_app.get_child_optional(field);
        if (!expected) continue;
        const auto actual = after_app->get_child_optional(field);
        ASSERT_TRUE(actual) << name << " lost " << field;
        if (expected->empty()) {
          EXPECT_EQ(actual->get_value<std::string>(), expected->get_value<std::string>()) << name << " changed " << field;
        }
        else {
          EXPECT_EQ(write(*actual), write(*expected)) << name << " changed " << field;
        }
      }
    }
    ASSERT_NE(app_named(after, "Big Screen"), nullptr);

    // the record keeps the offer to one start
    default_apps::migrate_file(copy.string(), "en");
    EXPECT_EQ(parse(read_text(copy)).get_child("apps").size(), before.get_child("apps").size() + 1u);
    std::filesystem::remove(copy, ignored);
  }
}

// ---- names of the default entries follow the language (src/default_app_names.h, default_apps.h) ----

namespace {
  // the four default entries as the bundled file ships them, with the commands of the Desktop entry
  const char *const bundled_en = R"({"apps":[
    {"name":"Desktop","default-id":"desktop","image-path":"desktop","menu-cmd":[{"id":"kcENAT5r9P","name":"Touch Keyboard","cmd":"a"},{"id":"rjeOKHmcdL","name":"Desktop Pet","cmd":"b"}]},
    {"name":"Big Screen","default-id":"big-screen","image-path":"bigscreen.png","detached":[".\\assets\\gui\\sunshine-gui.exe --desktop"]},
    {"name":"Steam Big Picture","default-id":"steam","cmd":"steam://open/bigpicture"},
    {"name":"Xbox Game","default-id":"xbox","cmd":"cmd /c \"start xbox:\""}]})";

  std::string
  menu_name(const pt::ptree &tree, int index) {
    const auto &menu = tree.get_child("apps").begin()->second.get_child("menu-cmd");
    auto it = menu.begin();
    std::advance(it, index);
    return it->second.get<std::string>("name");
  }
}  // namespace

TEST(DefaultAppNames, ChineseToEnglishToChineseAndTheCommandsOfDesktop) {
  auto tree = parse(bundled_en);
  ASSERT_TRUE(default_apps::retitle(tree, "zh"));
  EXPECT_EQ(names(tree), (std::vector<std::string> { "桌面", "大屏", "Steam Big Picture 模式", "Xbox 游戏" }));
  EXPECT_EQ(menu_name(tree, 0), "触摸键盘");
  EXPECT_EQ(menu_name(tree, 1), "桌宠");
  ASSERT_TRUE(default_apps::retitle(tree, "en"));
  EXPECT_EQ(names(tree), (std::vector<std::string> { "Desktop", "Big Screen", "Steam Big Picture", "Xbox Game" }));
  EXPECT_EQ(menu_name(tree, 0), "Touch Keyboard");
  ASSERT_TRUE(default_apps::retitle(tree, "zh"));
  EXPECT_EQ(names(tree)[0], "桌面");
  EXPECT_FALSE(default_apps::retitle(tree, "zh")) << "already in that language: nothing to write";
}

TEST(DefaultAppNames, ARenamedEntryKeepsTheName) {
  auto tree = parse(bundled_en);
  // the person renamed Xbox Game and the Touch Keyboard command, and wrote an English name into Desktop that is none of its names
  for (auto &[_, app] : tree.get_child("apps")) {
    if (app.get<std::string>("default-id") == "xbox") app.put("name", "My Xbox");
    if (app.get<std::string>("default-id") == "desktop") {
      app.put("name", "Work PC");
      for (auto &[__, command] : app.get_child("menu-cmd")) {
        if (command.get<std::string>("id") == "kcENAT5r9P") command.put("name", "Keys");
      }
    }
  }
  for (const char *locale : { "zh", "ja", "en", "zh_TW", "de" }) {
    default_apps::retitle(tree, locale);
    const auto shown = names(tree);
    EXPECT_EQ(shown[0], "Work PC") << locale;
    EXPECT_EQ(shown[3], "My Xbox") << locale;
    EXPECT_EQ(menu_name(tree, 0), "Keys") << locale;
  }
  // an entry the person did not rename still follows
  EXPECT_EQ(names(tree)[1], "Großbild");
  EXPECT_EQ(menu_name(tree, 1), "Desktop-Begleiter");
}

TEST(DefaultAppNames, ALanguageAddedLaterAndTheFallbacks) {
  EXPECT_EQ(default_apps::names::name_in("desktop", "ja"), "デスクトップ");
  EXPECT_EQ(default_apps::names::name_in("big-screen", "zh_TW"), "大螢幕");
  EXPECT_EQ(default_apps::names::name_in("big-screen", "zh-Hant"), "大螢幕");
  EXPECT_EQ(default_apps::names::name_in("big-screen", "zh-Hans"), "大屏");
  EXPECT_EQ(default_apps::names::name_in("desktop", "zh_CN"), "桌面");
  EXPECT_EQ(default_apps::names::name_in("desktop", "pt_PT"), "Ambiente de trabalho");
  EXPECT_EQ(default_apps::names::name_in("desktop", "pt_BR"), "Área de trabalho");
  EXPECT_EQ(default_apps::names::name_in("desktop", "de_AT"), "Desktop");
  EXPECT_EQ(default_apps::names::name_in("xbox", "en_US"), "Xbox Game");
  EXPECT_EQ(default_apps::names::name_in("xbox", "en_GB"), "Xbox Game");
  EXPECT_EQ(default_apps::names::name_in("xbox", "xx"), "Xbox Game") << "unknown language: English";
  EXPECT_EQ(default_apps::names::name_in("xbox", ""), "Xbox Game");
  EXPECT_EQ(default_apps::names::name_in("nothing", "en"), "");
  // Japanese after Chinese after English in one file
  auto tree = parse(bundled_en);
  default_apps::retitle(tree, "zh");
  default_apps::retitle(tree, "ja");
  EXPECT_EQ(names(tree), (std::vector<std::string> { "デスクトップ", "ビッグスクリーン", "Steam Big Picture モード", "Xbox ゲーム" }));
}

TEST(DefaultAppNames, EveryLanguageHasEveryNameAndTheNamesDoNotCollide) {
  for (const auto &entry : default_apps::names::table) {
    for (std::size_t column = 0; column < default_apps::names::locales.size(); ++column) {
      EXPECT_FALSE(entry.text[column].empty()) << entry.id << ' ' << default_apps::names::locales[column];
      EXPECT_EQ(default_apps::names::column_of(default_apps::names::locales[column]), column) << default_apps::names::locales[column];
    }
  }
  // one name must not belong to two entries (the entry would be told from its name)
  for (const auto &a : default_apps::names::table) {
    for (const auto &b : default_apps::names::table) {
      if (a.id == b.id) continue;
      for (const auto text : a.text) EXPECT_FALSE(default_apps::names::is_known(b.id, text)) << a.id << " / " << b.id << " / " << text;
    }
  }
}

TEST(DefaultAppNames, TheIdOfAnApplicationDoesNotChangeWithTheLanguage) {
  auto tree = parse(bundled_en);
  const auto &apps = tree.get_child("apps");
  std::vector<std::string> before;
  for (const auto &[_, app] : apps) before.push_back(default_apps::id_name(app, app.get<std::string>("name")));
  default_apps::retitle(tree, "zh");
  std::vector<std::string> after;
  for (const auto &[_, app] : tree.get_child("apps")) after.push_back(default_apps::id_name(app, app.get<std::string>("name")));
  EXPECT_EQ(before, after);
  EXPECT_EQ(after[1], "Big Screen");
  // a renamed entry hashes its own name
  pt::ptree renamed;
  renamed.put("name", "My Xbox");
  renamed.put("default-id", "xbox");
  EXPECT_EQ(default_apps::id_name(renamed, "My Xbox"), "My Xbox");
}

TEST(DefaultAppNames, AnOldFileIsMarkedOnceAndOnlyWhereTheNameIsADefaultName) {
  auto tree = parse(R"({"apps":[{"name":"Desktop","image-path":"desktop"},{"name":"桌面","image-path":"desktop"},{"name":"Steam","cmd":"steam://open/bigpicture"},{"name":"Xbox 游戏","cmd":"cmd /c \"start xbox:\"","image-path":"box.png"},{"name":"Notepad","cmd":"notepad.exe"}]})");
  ASSERT_TRUE(default_apps::stamp_defaults(tree));
  std::vector<std::string> marks;
  for (const auto &[_, app] : tree.get_child("apps")) marks.push_back(app.get<std::string>("default-id", "-"));
  // "Steam" is the person's name: not marked, so it never follows; the rest of the defaults are
  EXPECT_EQ(marks, (std::vector<std::string> { "desktop", "desktop", "-", "xbox", "-" }));
  const auto &first = tree.get_child("apps").begin()->second;
  EXPECT_EQ(first.get<std::string>("hero"), "desktop-hero.png");
  EXPECT_FALSE(default_apps::stamp_defaults(tree)) << "marked once";
  // a marked entry whose hero the person cleared does not get it back
  for (auto &[_, app] : tree.get_child("apps")) app.erase("hero");
  EXPECT_FALSE(default_apps::stamp_defaults(tree));
  EXPECT_FALSE(tree.get_child("apps").begin()->second.get_optional<std::string>("hero"));
  // a Chinese install after the language changes to English
  default_apps::retitle(tree, "en");
  EXPECT_EQ(names(tree), (std::vector<std::string> { "Desktop", "Desktop", "Steam", "Xbox Game", "Notepad" }));
}

TEST(DefaultAppNames, TheBigScreenEntryIsMarkedAndShipsItsLandscapeCover) {
  const auto app = default_apps::big_screen_app(true);
  EXPECT_EQ(app.get<std::string>("default-id"), "big-screen");
  EXPECT_EQ(app.get<std::string>("hero"), "bigscreen-hero.png");
  EXPECT_EQ(app.get<std::string>("name"), "大屏");
  pt::ptree tree;
  tree.add_child("apps", pt::ptree {}).push_back({ "", app });
  default_apps::retitle(tree, "en");
  EXPECT_EQ(names(tree), (std::vector<std::string> { "Big Screen" }));
}

TEST(DefaultAppNames, TheStartShowsTheNamesInTheLanguageOfTheHost) {
  // a Chinese host whose language was set before the entries were marked: no later change of language renames them, the start does
  const auto file = std::filesystem::temp_directory_path() / "default-apps-start-test.json";
  {
    std::ofstream out { file };
    out << R"({"apps":[{"name":"Desktop","image-path":"desktop","default-id":"desktop"},{"name":"Steam Big Picture","cmd":"steam://open/bigpicture","default-id":"steam"},{"name":"My Xbox","default-id":"xbox"}]})";
  }
  default_apps::migrate_file(file.string(), "zh");
  pt::ptree tree;
  pt::read_json(file.string(), tree);
  const auto shown = names(tree);
  ASSERT_GE(shown.size(), 3u);
  EXPECT_EQ(shown[0], "桌面");
  EXPECT_EQ(shown[1], "Steam Big Picture 模式");
  EXPECT_EQ(shown[2], "My Xbox") << "a name the person wrote stays";
  std::filesystem::remove(file);
}
