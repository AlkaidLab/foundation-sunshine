#include <gtest/gtest.h>

#include <filesystem>

#include "src/platform/windows/hdr_enhanced/nvidia_rtx_video/bridge_loader.h"

namespace {
  TEST(TrueHdrBridgeLoader, RejectsRelativeAndMissingPaths) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::bridge_loader_t loader;
    EXPECT_FALSE(loader.load("fake_truehdr_bridge.dll"));
    EXPECT_EQ(loader.error(), "bridge_path_not_absolute");

    EXPECT_FALSE(loader.load(std::filesystem::temp_directory_path() / "missing_truehdr_bridge.dll"));
    EXPECT_EQ(loader.error().find("bridge_load_failed:"), 0u);
  }

  TEST(TrueHdrBridgeLoader, LoadsCompleteVersionedApi) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::bridge_loader_t loader;
    ASSERT_TRUE(loader.load(std::filesystem::path(FAKE_TRUEHDR_BRIDGE_PATH))) << loader.error();
    ASSERT_TRUE(loader.api());
    EXPECT_EQ(loader.api()->abi_version, FOUNDATION_TRUEHDR_BRIDGE_ABI_VERSION);
    EXPECT_TRUE(loader.api()->create);
    EXPECT_TRUE(loader.api()->process);
  }

  TEST(TrueHdrBridgeLoader, RejectsAbiMismatch) {
    platf::dxgi::hdr_enhanced::nvidia_rtx_video::bridge_loader_t loader;
    EXPECT_FALSE(loader.load(std::filesystem::path(FAKE_TRUEHDR_BAD_BRIDGE_PATH)));
    EXPECT_EQ(loader.error(), "bridge_abi_mismatch");
  }
}  // namespace
