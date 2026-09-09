#include <gtest/gtest.h>

#include <filesystem>

#include "src/platform/windows/hdr_enhanced/nvidia_rtx_video/adapter_loader.h"

namespace {
  TEST(TrueHdrAdapterLoader, RejectsRelativeAndMissingPaths) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::adapter_loader_t loader;
    EXPECT_FALSE(loader.load("fake_truehdr_adapter.dll", std::filesystem::path(FAKE_TRUEHDR_RUNTIME_PATH)));
    EXPECT_EQ(loader.error(), "adapter_path_not_absolute");

    EXPECT_FALSE(loader.load(std::filesystem::temp_directory_path() / "missing_truehdr_adapter.dll", std::filesystem::path(FAKE_TRUEHDR_RUNTIME_PATH)));
    EXPECT_EQ(loader.error().find("adapter_load_failed:"), 0u);
  }

  TEST(TrueHdrAdapterLoader, LoadsCompleteVersionedApi) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::adapter_loader_t loader;
    ASSERT_TRUE(loader.load(std::filesystem::path(FAKE_TRUEHDR_ADAPTER_PATH), std::filesystem::path(FAKE_TRUEHDR_RUNTIME_PATH))) << loader.error();
    ASSERT_TRUE(loader.api());
    EXPECT_EQ(loader.api()->abi_version, FOUNDATION_TRUEHDR_ADAPTER_ABI_VERSION);
    EXPECT_TRUE(loader.api()->create);
    EXPECT_TRUE(loader.api()->process);
  }

  TEST(TrueHdrAdapterLoader, RejectsAbiMismatch) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::adapter_loader_t loader;
    EXPECT_FALSE(loader.load(std::filesystem::path(FAKE_TRUEHDR_BAD_ADAPTER_PATH), std::filesystem::path(FAKE_TRUEHDR_RUNTIME_PATH)));
    EXPECT_EQ(loader.error(), "adapter_abi_mismatch");
  }

  TEST(TrueHdrAdapterLoader, MissingRuntimeDoesNotLoadTheAdapter) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::adapter_loader_t loader;
    EXPECT_FALSE(loader.load(
      std::filesystem::path(FAKE_TRUEHDR_ADAPTER_PATH),
      std::filesystem::temp_directory_path() / "missing_nvngx_truehdr.dll"));
    EXPECT_EQ(loader.error(), "runtime_open_failed");
  }
}  // namespace
