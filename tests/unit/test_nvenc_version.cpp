/**
 * @file tests/unit/test_nvenc_version.cpp
 * @brief Tests for runtime NVENC SDK version selection.
 */

#include "src/nvenc/nvenc_version.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

  TEST(NvencVersionTest, DecodesPackedDriverVersion) {
    EXPECT_EQ(nvenc::decode_nvenc_driver_version((11U << 4U) | 0U), 1100U);
    EXPECT_EQ(nvenc::decode_nvenc_driver_version((13U << 4U) | 1U), 1301U);
  }

  TEST(NvencVersionTest, SelectsNewestCompatibleSdk) {
    const auto &versions = nvenc::supported_nvenc_sdk_versions;
    ASSERT_FALSE(versions.empty());

    for (std::size_t index = 0; index < versions.size(); ++index) {
      const auto version_number = nvenc::nvenc_sdk_version_number(versions[index]);
      EXPECT_EQ(nvenc::select_nvenc_sdk_version(version_number), versions[index]);

      if (index + 1 < versions.size()) {
        EXPECT_EQ(nvenc::select_nvenc_sdk_version(version_number - 1), versions[index + 1]);
      }
    }

    const auto oldest_version = nvenc::nvenc_sdk_version_number(versions.back());
    EXPECT_EQ(nvenc::select_nvenc_sdk_version(oldest_version - 1), nvenc::nvenc_sdk_version::unsupported);
    EXPECT_EQ(nvenc::select_nvenc_sdk_version(9999U), versions.front());
  }

}  // namespace
