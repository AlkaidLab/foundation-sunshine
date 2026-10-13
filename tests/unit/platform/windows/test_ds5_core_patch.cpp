#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <gtest/gtest.h>
#include "src/platform/windows/ds5/core_patch.h"

namespace {
  // File contents stand in for digests to test activation, not SHA-256 itself.
  std::optional<std::string> contents(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), {});
  }
  void write(const std::filesystem::path &path, std::string_view value) {
    std::ofstream(path, std::ios::binary) << value;
  }
  class CorePatch: public testing::Test {
  protected:
    std::filesystem::path root, source, destination;
    std::string error;
    void SetUp() override {
      root = std::filesystem::temp_directory_path() /
             ("sunshine-core-patch-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
      std::filesystem::create_directories(root);
      source = root / "patched.dll";
      destination = root / "active.dll";
      write(source, "patched");
      write(destination, "original");
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    bool apply() { return platf::ds5::ensure_core_patch(source, destination, "original", "patched", contents, error); }
    std::size_t files() { return std::distance(std::filesystem::directory_iterator(root), {}); }
  };
  TEST_F(CorePatch, UpgradesLegacyAndIsIdempotent) {
    ASSERT_TRUE(apply()) << error;
    EXPECT_EQ(contents(destination), "patched");
    EXPECT_TRUE(apply()) << error;
    EXPECT_EQ(files(), 2);
  }
  TEST_F(CorePatch, RejectsUnknownCoreWithoutChangingIt) {
    write(destination, "foreign");
    EXPECT_FALSE(apply());
    EXPECT_EQ(contents(destination), "foreign");
    EXPECT_EQ(files(), 2);
  }
  TEST_F(CorePatch, RejectsCorruptPatchWithoutChangingLegacyCore) {
    write(source, "corrupt");
    EXPECT_FALSE(apply());
    EXPECT_EQ(contents(destination), "original");
    EXPECT_EQ(files(), 2);
  }
  TEST_F(CorePatch, SharingFailurePreservesLegacyCoreAndRemovesStaging) {
    const auto locked = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(apply());
    EXPECT_EQ(contents(destination), "original");
    EXPECT_EQ(files(), 2);
    CloseHandle(locked);
    EXPECT_TRUE(apply()) << error;
  }
  TEST_F(CorePatch, RacingReplacementIsPreserved) {
    const auto racing_hash = [this](const std::filesystem::path &path) {
      auto value = contents(path);
      if (path != source && path != destination) write(destination, "other-update");
      return value;
    };
    EXPECT_FALSE(platf::ds5::ensure_core_patch(source, destination, "original", "patched", racing_hash, error));
    EXPECT_EQ(contents(destination), "other-update");
    EXPECT_EQ(files(), 2);
  }
  TEST_F(CorePatch, CorrectlyPatchedCoreDoesNotRequireAnotherCopy) {
    write(destination, "patched");
    std::filesystem::remove(source);
    EXPECT_TRUE(apply()) << error;
    EXPECT_EQ(files(), 1);
  }
}  // namespace
#endif
