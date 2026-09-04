#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

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

  // Exercises locate_system_truehdr_runtime against an isolated directory
  // rather than the (possibly NVIDIA-equipped) real Program Files.
  class TrueHdrRuntimeLocatorTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      test_dir_ = std::filesystem::temp_directory_path() / "sunshine_truehdr_locator_test";
      std::filesystem::remove_all(test_dir_);
      std::filesystem::create_directories(test_dir_);
    }

    void
    TearDown() override {
      std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
  };

  TEST_F(TrueHdrRuntimeLocatorTest, FindsRuntimeBesideBackend) {
    // The locator matches on filename + regular-file only, so an empty file is
    // enough. Passing the full backend DLL path (not its directory) must find
    // the sibling runtime — this pins the caller/parameter contract.
    const auto runtime = test_dir_ / "nvngx_truehdr.dll";
    std::ofstream { runtime }.put('\0');
    const auto backend = test_dir_ / "foundation_truehdr_backend.dll";

    const auto hint = platf::dxgi::rtx_hdr::locate_system_truehdr_runtime(backend);
    const auto expected_u8 = runtime.u8string();
    const std::string expected { reinterpret_cast<const char *>(expected_u8.data()), expected_u8.size() };
    EXPECT_EQ(hint, expected);
  }

  TEST_F(TrueHdrRuntimeLocatorTest, EmptyResultWhenNoRuntimePresent) {
    // Redirect ProgramFiles so the NVIDIA-directory fallback cannot pick up a
    // runtime that happens to be installed on the test machine.
    struct program_files_redirect_t {
      char original_[256] {};
      bool had_original_ { false };
      explicit program_files_redirect_t(const std::string &dir) {
        const char *current = std::getenv("ProgramFiles");
        had_original_ = current != nullptr;
        if (had_original_) {
          std::strncpy(original_, current, sizeof(original_) - 1);
        }
        _putenv_s("ProgramFiles", dir.c_str());
      }
      ~program_files_redirect_t() {
        _putenv_s("ProgramFiles", had_original_ ? original_ : "");
      }
    } redirect { test_dir_.string() };

    const auto backend = test_dir_ / "foundation_truehdr_backend.dll";
    EXPECT_TRUE(platf::dxgi::rtx_hdr::locate_system_truehdr_runtime(backend).empty());
  }
}  // namespace
