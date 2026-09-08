#include <gtest/gtest.h>

#include <filesystem>

#include "src/platform/windows/rtx_hdr/backend_loader.h"

namespace {
  TEST(TrueHdrBackendLoader, RejectsRelativeAndMissingPaths) {
    platf::dxgi::rtx_hdr::backend_loader_t loader;
    EXPECT_FALSE(loader.load("fake_truehdr_backend.dll"));
    EXPECT_EQ(loader.error(), "backend_path_not_absolute");

    EXPECT_FALSE(loader.load(std::filesystem::temp_directory_path() / "missing_truehdr_backend.dll"));
    EXPECT_EQ(loader.error().find("backend_load_failed:"), 0u);
  }

  TEST(TrueHdrBackendLoader, LoadsCompleteVersionedApi) {
    platf::dxgi::rtx_hdr::backend_loader_t loader;
    ASSERT_TRUE(loader.load(std::filesystem::path(FAKE_TRUEHDR_BACKEND_PATH))) << loader.error();
    ASSERT_TRUE(loader.api());
    EXPECT_EQ(loader.api()->abi_version, FOUNDATION_TRUEHDR_ABI_VERSION);
    EXPECT_TRUE(loader.api()->create);
    EXPECT_TRUE(loader.api()->process);
  }

  TEST(TrueHdrBackendLoader, RejectsAbiMismatch) {
    platf::dxgi::rtx_hdr::backend_loader_t loader;
    EXPECT_FALSE(loader.load(std::filesystem::path(FAKE_TRUEHDR_BAD_BACKEND_PATH)));
    EXPECT_EQ(loader.error(), "backend_abi_mismatch");
  }
}  // namespace
