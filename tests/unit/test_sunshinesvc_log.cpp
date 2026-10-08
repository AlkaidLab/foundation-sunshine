/**
 * @file tests/unit/test_sunshinesvc_log.cpp
 * @brief Tests for the service log sink selection used by sunshinesvc.
 */
#include <tools/sunshinesvc_log.h>

#include <gtest/gtest.h>

#include <string>

namespace {

  // L"sunshine.log" holds 12 characters.
  constexpr std::size_t kSuffixLength = 12;

  std::wstring
  temp_dir() {
    wchar_t buffer[MAX_PATH];
    const auto length = GetTempPathW(_countof(buffer), buffer);
    return std::wstring(buffer, length);
  }

}  // namespace

// wcscat_s requires strlen(temp) + strlen(suffix) + 1 <= capacity, so the
// boundary sits one below capacity - suffix length.
TEST(PrimaryLogPathFits, ReservesSuffixAndNulRoom) {
  constexpr std::size_t capacity = MAX_PATH;

  EXPECT_FALSE(sunshinesvc::primary_log_path_fits(0, capacity));
  EXPECT_FALSE(sunshinesvc::primary_log_path_fits(1, 1 + kSuffixLength));
  EXPECT_TRUE(sunshinesvc::primary_log_path_fits(1, 1 + kSuffixLength + 1));
  EXPECT_TRUE(sunshinesvc::primary_log_path_fits(capacity - kSuffixLength - 1, capacity));
  EXPECT_FALSE(sunshinesvc::primary_log_path_fits(capacity - kSuffixLength, capacity));
  EXPECT_FALSE(sunshinesvc::primary_log_path_fits(capacity - 1, capacity));
}

TEST(FallbackTempPathUsable, RequiresPositiveFittingLength) {
  EXPECT_FALSE(sunshinesvc::fallback_temp_path_usable(0, MAX_PATH));
  EXPECT_TRUE(sunshinesvc::fallback_temp_path_usable(MAX_PATH - 1, MAX_PATH));
  EXPECT_FALSE(sunshinesvc::fallback_temp_path_usable(MAX_PATH, MAX_PATH));
}

TEST(FallbackLogName, EmbedsProcessIdAfterPrefix) {
  EXPECT_EQ(sunshinesvc::fallback_log_name(L"C:\\Temp\\", 1234), L"C:\\Temp\\sunshine-1234.log");
  EXPECT_EQ(sunshinesvc::fallback_log_name(L"", 0), L"sunshine-0.log");
}

// Reproduces the lock from #1119: a handle opened without write sharing (as
// written by builds before the sharing fix) must block the primary sink and
// be rescued by the per-instance fallback instead of failing service start.
TEST(ServiceLogSinks, FallbackEngagesWhenPrimaryIsLockedByOldStyleHandle) {
  const auto primary = temp_dir() + L"sunshine.log";
  SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

  const auto blocker = CreateFileW(primary.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    &inheritable,
    CREATE_ALWAYS,
    0,
    NULL);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);

  // Sanity: the old-style handle really blocks a write-shared reopen.
  const auto blocked = CreateFileW(primary.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    &inheritable,
    OPEN_EXISTING,
    0,
    NULL);
  EXPECT_EQ(blocked, INVALID_HANDLE_VALUE);
  if (blocked != INVALID_HANDLE_VALUE) {
    CloseHandle(blocked);
  }

  EXPECT_EQ(sunshinesvc::open_primary_log_handle(), INVALID_HANDLE_VALUE);

  const auto fallback = sunshinesvc::open_fallback_log_handle();
  ASSERT_NE(fallback, INVALID_HANDLE_VALUE);

  const auto fallback_name = sunshinesvc::fallback_log_name(temp_dir(), GetCurrentProcessId());
  EXPECT_NE(GetFileAttributesW(fallback_name.c_str()), INVALID_FILE_ATTRIBUTES);

  DWORD written = 0;
  EXPECT_TRUE(WriteFile(fallback, "x", 1, &written, NULL));
  EXPECT_EQ(written, 1u);

  CloseHandle(fallback);
  CloseHandle(blocker);
  DeleteFileW(fallback_name.c_str());
  DeleteFileW(primary.c_str());
}

TEST(ServiceLogSinks, PrimaryOpensUnlocked) {
  const auto handle = sunshinesvc::open_primary_log_handle();
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  CloseHandle(handle);
  DeleteFileW((temp_dir() + L"sunshine.log").c_str());
}
