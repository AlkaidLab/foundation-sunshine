/**
 * @file tests/unit/platform/windows/test_nvenc_dynamic_factory.cpp
 * @brief Tests for NVENC driver discovery without requiring an NVIDIA GPU.
 */

#ifdef _WIN32

  #include "src/nvenc/win/nvenc_dynamic_factory.h"

  #include <bit>
  #include <cstdint>
  #include <string_view>

  #include <gtest/gtest.h>

namespace {

  std::uint32_t reported_version;
  std::uint32_t reported_status;

  std::uint32_t WINAPI
  get_fake_max_supported_version(std::uint32_t *version) {
    *version = reported_version;
    return reported_status;
  }

  nvenc::shared_dll
  make_fake_dll() {
    constexpr std::uintptr_t fake_handle_value = 1U;
    return {
      reinterpret_cast<HMODULE>(fake_handle_value),
      [](HMODULE) {
      },
    };
  }

  nvenc::nvenc_runtime_api
  make_fake_runtime_api() {
    return {
      []() {
        return make_fake_dll();
      },
      [](HMODULE dll, const char *symbol) {
        EXPECT_EQ(dll, make_fake_dll().get());
        EXPECT_EQ(std::string_view { symbol }, "NvEncodeAPIGetMaxSupportedVersion");
        return std::bit_cast<FARPROC>(&get_fake_max_supported_version);
      },
    };
  }

  TEST(NvencDynamicFactoryTest, HandlesMissingDriverLoader) {
    EXPECT_FALSE(nvenc::nvenc_dynamic_factory::get({}));
  }

  TEST(NvencDynamicFactoryTest, HandlesMissingSymbolResolver) {
    const nvenc::nvenc_runtime_api runtime_api {
      []() {
        return make_fake_dll();
      },
      {},
    };

    EXPECT_FALSE(nvenc::nvenc_dynamic_factory::get(runtime_api));
  }

  TEST(NvencDynamicFactoryTest, HandlesUnavailableDriver) {
    bool resolved_symbol = false;
    const nvenc::nvenc_runtime_api runtime_api {
      []() {
        return nvenc::shared_dll {};
      },
      [&resolved_symbol](HMODULE, const char *) {
        resolved_symbol = true;
        return FARPROC {};
      },
    };

    EXPECT_FALSE(nvenc::nvenc_dynamic_factory::get(runtime_api));
    EXPECT_FALSE(resolved_symbol);
  }

  TEST(NvencDynamicFactoryTest, HandlesMissingVersionQuery) {
    const nvenc::nvenc_runtime_api runtime_api {
      []() {
        return make_fake_dll();
      },
      [](HMODULE, const char *) {
        return FARPROC {};
      },
    };

    EXPECT_FALSE(nvenc::nvenc_dynamic_factory::get(runtime_api));
  }

  TEST(NvencDynamicFactoryTest, HandlesFailedVersionQuery) {
    reported_version = 13U << 4U;
    reported_status = 1U;
    EXPECT_FALSE(nvenc::nvenc_dynamic_factory::get(make_fake_runtime_api()));
  }

  TEST(NvencDynamicFactoryTest, RejectsUnsupportedDriver) {
    reported_version = 10U << 4U;
    reported_status = 0U;
    EXPECT_FALSE(nvenc::nvenc_dynamic_factory::get(make_fake_runtime_api()));
  }

  TEST(NvencDynamicFactoryTest, SelectsSupportedSdkImplementations) {
    reported_status = 0U;
    for (const auto expected : nvenc::supported_nvenc_sdk_versions) {
      const auto version = nvenc::nvenc_sdk_version_number(expected);
      reported_version = ((version / 100U) << 4U) | (version % 100U);
      const auto factory = nvenc::nvenc_dynamic_factory::get(make_fake_runtime_api());
      ASSERT_TRUE(factory);
      EXPECT_EQ(factory->sdk_version(), expected);
    }

    reported_version = 14U << 4U;
    const auto newest_factory = nvenc::nvenc_dynamic_factory::get(make_fake_runtime_api());
    ASSERT_TRUE(newest_factory);
    EXPECT_EQ(newest_factory->sdk_version(), nvenc::supported_nvenc_sdk_versions.front());
  }

}  // namespace

#endif
