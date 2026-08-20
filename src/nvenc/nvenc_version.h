/**
 * @file src/nvenc/nvenc_version.h
 * @brief NVENC SDK version selection helpers.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nvenc {

  /**
   * @brief NVENC SDK implementations compiled into Sunshine.
   */
  enum class nvenc_sdk_version : std::uint32_t {
    unsupported = 0,
#define SUNSHINE_NVENC_SDK(version, name) sdk_##name = version,
#include "nvenc_sdk_versions.def"
#undef SUNSHINE_NVENC_SDK
  };

  /**
   * @brief NVENC SDK implementations in selection priority order.
   */
  inline constexpr std::array supported_nvenc_sdk_versions {
#define SUNSHINE_NVENC_SDK(version, name) nvenc_sdk_version::sdk_##name,
#include "nvenc_sdk_versions.def"
#undef SUNSHINE_NVENC_SDK
  };

  /**
   * @brief Convert an SDK version to major*100 + minor form.
   */
  constexpr std::uint32_t
  nvenc_sdk_version_number(nvenc_sdk_version version) {
    return static_cast<std::uint32_t>(version);
  }

  /**
   * @brief Decode the version returned by NvEncodeAPIGetMaxSupportedVersion().
   */
  constexpr std::uint32_t
  decode_nvenc_driver_version(std::uint32_t version) {
    return (version >> 4U) * 100U + (version & 0x0FU);
  }

  /**
   * @brief Select the newest compiled SDK supported by the installed driver.
   */
  constexpr nvenc_sdk_version
  select_nvenc_sdk_version(std::uint32_t max_version) {
    for (const auto version : supported_nvenc_sdk_versions) {
      if (max_version >= nvenc_sdk_version_number(version)) {
        return version;
      }
    }
    return nvenc_sdk_version::unsupported;
  }

  constexpr bool
  nvenc_sdk_versions_are_strictly_descending() {
    for (std::size_t index = 1; index < supported_nvenc_sdk_versions.size(); ++index) {
      if (nvenc_sdk_version_number(supported_nvenc_sdk_versions[index - 1]) <=
          nvenc_sdk_version_number(supported_nvenc_sdk_versions[index])) {
        return false;
      }
    }
    return true;
  }

  static_assert(nvenc_sdk_versions_are_strictly_descending(),
    "NVENC SDK compatibility tiers must be listed from newest to oldest");

}  // namespace nvenc
