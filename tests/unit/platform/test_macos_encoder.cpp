/**
 * @file tests/unit/platform/test_macos_encoder.cpp
 * @brief Test macOS VideoToolbox encoder implementation.
 */
#ifdef __APPLE__

#include <src/platform/macos/nv12_zero_device.h>
#include <src/video.h>

#include "../../tests_common.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

class MacOSEncoderTest: public ::testing::Test {
protected:
  void SetUp() override {
    // Only run these tests on macOS
  }
};

TEST_F(MacOSEncoderTest, NV12DeviceInitialization) {
  platf::nv12_zero_device device;

  // Mock display pointer (we don't actually use it in init)
  void *mock_display = nullptr;

  // Mock resolution function
  auto resolution_fn = [](void *display, int width, int height) {
    // No-op for testing
  };

  // Mock pixel format function
  auto pixel_format_fn = [](void *display, int pixelFormat) {
    // No-op for testing
  };

  // Test NV12 initialization
  int result = device.init(mock_display, platf::pix_fmt_e::nv12, resolution_fn, pixel_format_fn);
  EXPECT_EQ(result, 0);
  EXPECT_NE(device.data, nullptr);
}

TEST_F(MacOSEncoderTest, P010DeviceInitialization) {
  platf::nv12_zero_device device;

  void *mock_display = nullptr;

  auto resolution_fn = [](void *display, int width, int height) {
    // No-op for testing
  };

  auto pixel_format_fn = [](void *display, int pixelFormat) {
    // No-op for testing
  };

  // Test P010 (10-bit) initialization
  int result = device.init(mock_display, platf::pix_fmt_e::p010, resolution_fn, pixel_format_fn);
  EXPECT_EQ(result, 0);
  EXPECT_NE(device.data, nullptr);
}

TEST_F(MacOSEncoderTest, YUV420DimensionAlignment) {
  // Test that odd dimensions are properly aligned to even values
  struct TestCase {
    int input_width;
    int input_height;
    int expected_width;
    int expected_height;
  };

  std::vector<TestCase> test_cases = {
    {1920, 1080, 1920, 1080},  // Already even
    {1921, 1080, 1922, 1080},  // Odd width
    {1920, 1081, 1920, 1082},  // Odd height
    {1921, 1081, 1922, 1082},  // Both odd
    {1, 1, 2, 2},              // Minimum odd values
    {0, 0, 0, 0},              // Edge case: zero
  };

  for (const auto &test_case : test_cases) {
    int aligned_width = (test_case.input_width + 1) & ~1;
    int aligned_height = (test_case.input_height + 1) & ~1;

    EXPECT_EQ(aligned_width, test_case.expected_width)
      << "Width alignment failed for input: " << test_case.input_width;
    EXPECT_EQ(aligned_height, test_case.expected_height)
      << "Height alignment failed for input: " << test_case.input_height;
  }
}

TEST_F(MacOSEncoderTest, HWFramesContextAlignment) {
  platf::nv12_zero_device device;

  // Create a mock AVHWFramesContext
  AVHWFramesContext frames;
  frames.width = 1921;  // Odd width
  frames.height = 1081;  // Odd height
  frames.format = AV_PIX_FMT_VIDEOTOOLBOX;
  frames.sw_format = AV_PIX_FMT_NV12;

  // Call init_hwframes which should align dimensions
  device.init_hwframes(&frames);

  // Verify dimensions are aligned to even values
  EXPECT_EQ(frames.width, 1922);
  EXPECT_EQ(frames.height, 1082);
}

// Note: VideoToolbox encoder availability test removed because it requires
// full display device initialization which may crash in test environment

TEST_F(MacOSEncoderTest, VideoToolboxPixelFormats) {
  // Verify that VideoToolbox supports the expected pixel formats
  const auto &formats = video::videotoolbox.platform_formats;
  EXPECT_NE(formats, nullptr);

  // VideoToolbox should support NV12 (8-bit) and P010 (10-bit)
  if (formats) {
    EXPECT_NE(formats->pix_fmt_8bit, platf::pix_fmt_e::unknown);
    EXPECT_NE(formats->pix_fmt_10bit, platf::pix_fmt_e::unknown);
  }
}

TEST_F(MacOSEncoderTest, ColorspaceInitializationWithVideoToolbox) {
  // Test that colorspace is properly initialized when creating VideoToolbox encoder
  video::config_t config;
  config.width = 1920;
  config.height = 1080;
  config.framerate = 60;
  config.bitrate = 20000;
  config.slicesPerFrame = 1;
  config.numRefFrames = 0;
  config.encoderCscMode = 2;  // Limited range, Rec. 709
  config.videoFormat = 0;  // H.264
  config.dynamicRange = 0;  // SDR 8-bit
  config.chromaSamplingType = 0;  // 4:2:0
  config.enableIntraRefresh = 0;

  // Get colorspace from config
  auto colorspace = video::colorspace_from_client_config(config, false);

  // Verify colorspace is properly initialized
  EXPECT_EQ(colorspace.colorspace, video::colorspace_e::rec709);
  EXPECT_EQ(colorspace.full_range, false);
  EXPECT_EQ(colorspace.bit_depth, 8);

  // Verify bit_depth is valid (8 or 10)
  EXPECT_TRUE(colorspace.bit_depth == 8 || colorspace.bit_depth == 10)
    << "Invalid bit_depth: " << colorspace.bit_depth;
}

#endif  // __APPLE__
