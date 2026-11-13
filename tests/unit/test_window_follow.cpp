/**
 * @file tests/unit/test_window_follow.cpp
 * @brief Regression tests for window-follow capture geometry.
 */
#include <src/input.h>
#include <src/video.h>
#include <src/video_touch_port.h>

#include "../tests_common.h"

#ifdef _WIN32
  #include <src/platform/windows/display.h>
#endif

namespace {
  video::config_t
  stream_config(int width, int height) {
    video::config_t config {};
    config.width = width;
    config.height = height;
    return config;
  }
}  // namespace

TEST(WindowFollowTouchPort, NormalDisplayMappingIsUnchanged) {
  const auto config = stream_config(1280, 720);
  const auto port = video::make_touch_port_for_rect(
    config.width, config.height, 1920, 1080, 0, 0, 1920, 1080);

  EXPECT_EQ(port.offset_x, 0);
  EXPECT_EQ(port.offset_y, 0);
  EXPECT_EQ(port.width, 1280);
  EXPECT_EQ(port.height, 720);
  EXPECT_EQ(port.env_width, 1920);
  EXPECT_EQ(port.env_height, 1080);
  EXPECT_EQ(port.display_width, 1920);
  EXPECT_EQ(port.display_height, 1080);
  EXPECT_FLOAT_EQ(port.client_offsetX, 0.0f);
  EXPECT_FLOAT_EQ(port.client_offsetY, 0.0f);
  EXPECT_FLOAT_EQ(port.scalar_inv, 1.5f);
}

TEST(WindowFollowTouchPort, UsesWindowVirtualScreenRectangle) {
  // Window on a second monitor: the absolute pointer must be offset by the
  // window's virtual-screen position, not the monitor opened by DXGI.
  const auto config = stream_config(1280, 720);
  const auto port = video::make_touch_port_for_rect(
    config.width, config.height, 960, 540, 1920, 0, 3840, 1080);

  EXPECT_EQ(port.offset_x, 1920);
  EXPECT_EQ(port.offset_y, 0);
  EXPECT_EQ(port.display_width, 960);
  EXPECT_EQ(port.display_height, 540);
  EXPECT_EQ(port.env_width, 3840);
  EXPECT_EQ(port.env_height, 1080);
  EXPECT_FLOAT_EQ(port.client_offsetX, 0.0f);
  EXPECT_FLOAT_EQ(port.client_offsetY, 0.0f);
  EXPECT_FLOAT_EQ(port.scalar_inv, 0.75f);
}

TEST(WindowFollowTouchPort, KeepsLetterboxOffsetsForWindowAspect) {
  const auto config = stream_config(1920, 1080);
  const auto port = video::make_touch_port_for_rect(
    config.width, config.height, 800, 800, 100, 200, 2560, 1440);

  EXPECT_EQ(port.offset_x, 100);
  EXPECT_EQ(port.offset_y, 200);
  EXPECT_EQ(port.display_width, 800);
  EXPECT_EQ(port.display_height, 800);
  EXPECT_FLOAT_EQ(port.client_offsetX, 420.0f);
  EXPECT_FLOAT_EQ(port.client_offsetY, 0.0f);
  EXPECT_NEAR(port.scalar_inv, 1.0f / 1.35f, 0.00001f);
}

TEST(WindowFollowTouchPort, PreservesPhysicalOffsetsForMultiMonitorAndDpi) {
  // Per-monitor-DPI aware windows report physical virtual-screen offsets. The
  // helper must pass them through without applying a second DPI scale.
  const auto config = stream_config(2560, 1440);
  const auto port = video::make_touch_port_for_rect(
    config.width, config.height, 1280, 720, 3840, -720, 5120, 2160);

  EXPECT_EQ(port.offset_x, 3840);
  EXPECT_EQ(port.offset_y, -720);
  EXPECT_EQ(port.display_width, 1280);
  EXPECT_EQ(port.display_height, 720);
  EXPECT_EQ(port.env_width, 5120);
  EXPECT_EQ(port.env_height, 2160);
  EXPECT_FLOAT_EQ(port.scalar_inv, 0.5f);
}

TEST(WindowFollowTouchPort, ChangeDetectionCoversEveryPublishedField) {
  const auto config = stream_config(1280, 720);
  const auto base = video::make_touch_port_for_rect(
    config.width, config.height, 1920, 1080, 0, 0, 1920, 1080);
  EXPECT_TRUE(video::touch_ports_equal(base, base));

  auto moved = base;
  moved.offset_x += 1;
  EXPECT_FALSE(video::touch_ports_equal(base, moved));

  auto resized = base;
  resized.display_height += 1;
  EXPECT_FALSE(video::touch_ports_equal(base, resized));

  auto environment_changed = base;
  environment_changed.env_width += 1;
  EXPECT_FALSE(video::touch_ports_equal(base, environment_changed));

  auto letterbox_changed = base;
  letterbox_changed.client_offsetY += 1.0f;
  EXPECT_FALSE(video::touch_ports_equal(base, letterbox_changed));

  auto scale_changed = base;
  scale_changed.scalar_inv += 0.25f;
  EXPECT_FALSE(video::touch_ports_equal(base, scale_changed));
}

#ifdef _WIN32
TEST(WgcWindowResizePolicy, RequiresFullReinitWhenEitherDimensionChanges) {
  EXPECT_FALSE(platf::dxgi::wgc_window_size_requires_reinit(800, 600, 800, 600));

  // Same width, taller window: the old buggy path kept the old image buffer and
  // copied the taller frame into it.
  EXPECT_TRUE(platf::dxgi::wgc_window_size_requires_reinit(800, 600, 800, 900));

  EXPECT_TRUE(platf::dxgi::wgc_window_size_requires_reinit(800, 600, 1024, 600));
  EXPECT_TRUE(platf::dxgi::wgc_window_size_requires_reinit(800, 600, 1024, 900));
}
#endif
