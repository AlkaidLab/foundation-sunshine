/**
 * @file tests/unit/test_cursor_render.cpp
 * @brief Test cursor render mode helpers and configuration parsing.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../tests_common.h"
#include "config.h"
#include "cursor_render.h"
#include "process.h"

using namespace std::literals;

struct CursorRenderConfigTest: testing::Test {
  std::string old_cursor_render_mode;

  void
  SetUp() override {
    old_cursor_render_mode = config::input.cursor_render_mode;
    config::input.cursor_render_mode = "remote";
  }

  void
  TearDown() override {
    config::input.cursor_render_mode = old_cursor_render_mode;
  }
};

TEST(CursorRenderModeTests, ParsesModes) {
  EXPECT_EQ(cursor_render::mode_from_view("remote"), cursor_render::mode_e::remote);
  EXPECT_EQ(cursor_render::mode_from_view("client"), cursor_render::mode_e::client);
  EXPECT_EQ(cursor_render::mode_from_view("auto"), cursor_render::mode_e::automatic);
  EXPECT_EQ(cursor_render::mode_from_view("automatic"), cursor_render::mode_e::automatic);
  EXPECT_EQ(cursor_render::mode_from_view("invalid", cursor_render::mode_e::client), cursor_render::mode_e::client);

  EXPECT_EQ(cursor_render::app_mode_from_view("inherit"), cursor_render::app_mode_e::inherit);
  EXPECT_EQ(cursor_render::app_mode_from_view("remote"), cursor_render::app_mode_e::remote);
  EXPECT_EQ(cursor_render::app_mode_from_view("client"), cursor_render::app_mode_e::client);
  EXPECT_EQ(cursor_render::app_mode_from_view("auto"), cursor_render::app_mode_e::automatic);
}

TEST(CursorRenderModeTests, AppOverrideTakesPrecedence) {
  cursor_render::client_caps_t caps;
  caps.cursor_channel_v1 = true;

  auto result = cursor_render::resolve_effective_mode(
    cursor_render::mode_e::client,
    cursor_render::app_mode_e::remote,
    caps,
    true,
    false);

  EXPECT_EQ(result.mode, cursor_render::effective_mode_e::remote);
  EXPECT_EQ(result.reason, "requested remote");
}

TEST(CursorRenderModeTests, CapabilityMissingForcesRemote) {
  cursor_render::client_caps_t caps;

  auto result = cursor_render::resolve_effective_mode(
    cursor_render::mode_e::client,
    cursor_render::app_mode_e::inherit,
    caps,
    true,
    false);

  EXPECT_EQ(result.mode, cursor_render::effective_mode_e::remote);
  EXPECT_EQ(result.reason, "client does not support cursor channel v1");
}

TEST(CursorRenderModeTests, AutoFallsBackWhenBackendCannotProvideMetadata) {
  cursor_render::client_caps_t caps;
  caps.cursor_channel_v1 = true;

  auto result = cursor_render::resolve_effective_mode(
    cursor_render::mode_e::automatic,
    cursor_render::app_mode_e::inherit,
    caps,
    false,
    false);

  EXPECT_EQ(result.mode, cursor_render::effective_mode_e::remote);
  EXPECT_EQ(result.reason, "auto fallback: capture backend lacks cursor metadata");
}

TEST(CursorRenderModeTests, ClientSelectedWhenCapabilityAndBackendSupportIt) {
  cursor_render::client_caps_t caps;
  caps.cursor_channel_v1 = true;
  caps.cursor_native_supported = true;

  auto result = cursor_render::resolve_effective_mode(
    cursor_render::mode_e::automatic,
    cursor_render::app_mode_e::inherit,
    caps,
    true,
    false);

  EXPECT_EQ(result.mode, cursor_render::effective_mode_e::client);
  EXPECT_EQ(result.reason, "auto selected client");
}

TEST(CursorRenderModeTests, ShapeIdIsStableAndChangesWithPixels) {
  std::vector<std::uint8_t> pixels { 0, 1, 2, 3, 4, 5, 6, 7 };

  auto shape_a = cursor_render::shape_id(cursor_render::shape_format_e::bgra32_straight, 1, 2, 4, 0, 0, pixels);
  auto shape_b = cursor_render::shape_id(cursor_render::shape_format_e::bgra32_straight, 1, 2, 4, 0, 0, pixels);
  pixels[0] = 9;
  auto shape_c = cursor_render::shape_id(cursor_render::shape_format_e::bgra32_straight, 1, 2, 4, 0, 0, pixels);

  EXPECT_EQ(shape_a, shape_b);
  EXPECT_NE(shape_a, shape_c);
}

TEST(CursorRenderModeTests, SequenceComparisonDropsOldStates) {
  EXPECT_TRUE(cursor_render::seq_newer(2, 1));
  EXPECT_FALSE(cursor_render::seq_newer(1, 2));
  EXPECT_TRUE(cursor_render::seq_newer(0, 0xffffffffu));
  EXPECT_FALSE(cursor_render::seq_newer(0xffffffffu, 0));
}

TEST(CursorRenderModeTests, ClientMouseOnlyUpdateSkipsVideoFrame) {
  EXPECT_TRUE(cursor_render::should_skip_video_frame(cursor_render::effective_mode_e::client, true, false, false));
  EXPECT_FALSE(cursor_render::should_skip_video_frame(cursor_render::effective_mode_e::client, true, true, false));
  EXPECT_FALSE(cursor_render::should_skip_video_frame(cursor_render::effective_mode_e::remote, true, false, false));
  EXPECT_FALSE(cursor_render::should_skip_video_frame(cursor_render::effective_mode_e::client, true, false, true));
}

TEST_F(CursorRenderConfigTest, ParsesGlobalConfig) {
  std::unordered_map<std::string, std::string> vars;
  vars["cursor_render_mode"] = "client";

  config::apply_config(std::move(vars));

  EXPECT_EQ(config::input.cursor_render_mode, "client");
}

TEST(CursorRenderProcessTests, ParsesAppOverride) {
  const auto file_path = std::filesystem::temp_directory_path() / "sunshine_cursor_render_mode_apps.json";
  {
    std::ofstream out(file_path);
    out << R"json({
      "env": {},
      "apps": [
        {
          "name": "Cursor Test",
          "cmd": "echo test",
          "cursor-render-mode": "client"
        }
      ]
    })json";
  }

  auto parsed = proc::parse(file_path.string());
  std::filesystem::remove(file_path);

  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->get_apps().size(), 1);
  EXPECT_EQ(parsed->get_apps()[0].cursor_render_mode, "client");
}
