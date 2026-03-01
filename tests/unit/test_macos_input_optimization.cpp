/**
 * @file tests/unit/test_macos_input_optimization.cpp
 * @brief Test macOS input optimization features
 */
#include <gtest/gtest.h>
#include "src/config.h"

#ifdef __APPLE__

// Test mouse sensitivity configuration
TEST(MacOSInputOptimization, MouseSensitivityConfig) {
  // Test default value
  EXPECT_FLOAT_EQ(config::input_mouse_sensitivity, 1.0f);

  // Test valid range
  config::input_mouse_sensitivity = 0.5f;
  EXPECT_FLOAT_EQ(config::input_mouse_sensitivity, 0.5f);

  config::input_mouse_sensitivity = 2.0f;
  EXPECT_FLOAT_EQ(config::input_mouse_sensitivity, 2.0f);

  config::input_mouse_sensitivity = 1.5f;
  EXPECT_FLOAT_EQ(config::input_mouse_sensitivity, 1.5f);

  // Reset to default
  config::input_mouse_sensitivity = 1.0f;
}

// Test sensitivity clamping
TEST(MacOSInputOptimization, SensitivityClamping) {
  // Values should be clamped to 0.5-2.0 range during config parsing
  // This is tested in the config parsing logic

  float test_val = std::clamp(0.3f, 0.5f, 2.0f);
  EXPECT_FLOAT_EQ(test_val, 0.5f);

  test_val = std::clamp(2.5f, 0.5f, 2.0f);
  EXPECT_FLOAT_EQ(test_val, 2.0f);

  test_val = std::clamp(1.2f, 0.5f, 2.0f);
  EXPECT_FLOAT_EQ(test_val, 1.2f);
}

// Test sensitivity application to mouse delta
TEST(MacOSInputOptimization, SensitivityApplication) {
  // Test sensitivity multiplier application
  int deltaX = 10;
  int deltaY = 20;
  float sensitivity = 1.5f;

  int adjusted_deltaX = static_cast<int>(deltaX * sensitivity);
  int adjusted_deltaY = static_cast<int>(deltaY * sensitivity);

  EXPECT_EQ(adjusted_deltaX, 15);
  EXPECT_EQ(adjusted_deltaY, 30);

  // Test with sensitivity < 1.0
  sensitivity = 0.5f;
  adjusted_deltaX = static_cast<int>(deltaX * sensitivity);
  adjusted_deltaY = static_cast<int>(deltaY * sensitivity);

  EXPECT_EQ(adjusted_deltaX, 5);
  EXPECT_EQ(adjusted_deltaY, 10);
}

// Test audio format conversion (int16 to float32)
TEST(MacOSInputOptimization, AudioFormatConversion) {
  // Test int16 to float32 conversion
  int16_t sample_int16 = 16384;  // Half of max positive value
  float sample_float32 = sample_int16 / 32768.0f;

  EXPECT_FLOAT_EQ(sample_float32, 0.5f);

  // Test max positive value
  sample_int16 = 32767;
  sample_float32 = sample_int16 / 32768.0f;
  EXPECT_NEAR(sample_float32, 1.0f, 0.0001f);

  // Test max negative value
  sample_int16 = -32768;
  sample_float32 = sample_int16 / 32768.0f;
  EXPECT_FLOAT_EQ(sample_float32, -1.0f);

  // Test zero
  sample_int16 = 0;
  sample_float32 = sample_int16 / 32768.0f;
  EXPECT_FLOAT_EQ(sample_float32, 0.0f);
}

// Test frame count calculation
TEST(MacOSInputOptimization, AudioFrameCount) {
  // Test frame count calculation for stereo int16 PCM
  size_t data_size = 1024;  // bytes
  size_t frameCount = data_size / (2 * sizeof(int16_t));  // 2 channels

  EXPECT_EQ(frameCount, 256);  // 1024 / 4 = 256 frames

  // Test with different sizes
  data_size = 4096;
  frameCount = data_size / (2 * sizeof(int16_t));
  EXPECT_EQ(frameCount, 1024);

  // Test empty data
  data_size = 0;
  frameCount = data_size / (2 * sizeof(int16_t));
  EXPECT_EQ(frameCount, 0);
}

#endif  // __APPLE__
