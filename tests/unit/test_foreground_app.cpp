/**
 * @file tests/unit/test_foreground_app.cpp
 * @brief Test the niri focused-window report parser (Linux-only source).
 *
 * The payloads mirror what `niri msg --json focused-window` prints; the parser
 * must survive field renames/types without losing the whole report, because a
 * niri release can change a field and the ABR consumer only needs pid/app_id.
 */
#include <src/platform/linux/foreground_app.h>

#include "../tests_common.h"

#ifndef _WIN32
namespace {
  using platf::foreground_app::info_t;

  info_t
  parse(const std::string &json_text) {
    info_t info;
    EXPECT_TRUE(platf::foreground_app::parse_niri_focused_window(json_text, info));
    return info;
  }
}  // namespace

TEST(ForegroundApp, ParsesNiriFocusedWindow) {
  const auto info = parse(
    R"({"id":12,"title":"Cyberpunk 2077","app_id":"steam_app_1091500","pid":4242,)"
    R"("workspace_id":1,"is_floating":false,"is_urgent":false})");

  EXPECT_EQ(info.window_title, "Cyberpunk 2077");
  EXPECT_EQ(info.exe_name, "steam_app_1091500");
  EXPECT_EQ(info.pid, 4242u);
}

TEST(ForegroundApp, NullFocusedWindowIsNotAReport) {
  info_t info;
  EXPECT_FALSE(platf::foreground_app::parse_niri_focused_window("null", info));
  EXPECT_FALSE(platf::foreground_app::parse_niri_focused_window("", info));
}

TEST(ForegroundApp, MissingFieldsOnlyCostThatField) {
  // No pid: the report is still usable, and the shared store keeps the previous
  // pid when the app class is unchanged.
  const auto info = parse(R"({"title":"Desktop","app_id":"plasmashell"})");
  EXPECT_EQ(info.exe_name, "plasmashell");
  EXPECT_EQ(info.pid, 0u);

  // Retyped pid and a renamed title: the app id must still come through.
  info_t partial;
  EXPECT_TRUE(platf::foreground_app::parse_niri_focused_window(
    R"({"title":null,"app_id":"firefox","pid":"4711"})", partial));
  EXPECT_EQ(partial.exe_name, "firefox");
  EXPECT_EQ(partial.pid, 0u);
  EXPECT_TRUE(partial.window_title.empty());
}

TEST(ForegroundApp, MalformedJsonIsIgnored) {
  info_t info;
  EXPECT_FALSE(platf::foreground_app::parse_niri_focused_window("{not json", info));
  EXPECT_FALSE(platf::foreground_app::parse_niri_focused_window("[1,2,3]", info));
  EXPECT_FALSE(platf::foreground_app::parse_niri_focused_window(R"({"id":1})", info))
    << "a window without title or app id carries nothing ABR can use";
}
#endif  // !_WIN32
