/**
 * @file tests/unit/test_sunshinesvc_log.cpp
 * @brief Tests for the service log sink selection used by sunshinesvc.
 */
#include <tools/sunshinesvc_log.h>

#include <gtest/gtest.h>

#include <atomic>
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

  // Each test runs against its own directory so the shared sunshine.log
  // location is never touched, cleanup only removes files the test created,
  // and parallel test runs cannot collide.
  class IsolatedLogDir {
  public:
    IsolatedLogDir() {
      static std::atomic<unsigned> counter { 0 };
      path_ = temp_dir() + L"sunshine-log-test-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(counter.fetch_add(1)) + L"\\";
      CreateDirectoryW(path_.c_str(), NULL);
    }

    ~IsolatedLogDir() {
      // Files created inside are closed and deleted by the tests; failing
      // to remove a leftover only orphans a uniquely named directory.
      RemoveDirectoryW(path_.c_str());
    }

    const std::wstring &
    path() const {
      return path_;
    }

  private:
    std::wstring path_;
  };

}  // namespace

// The primary name must fit a MAX_PATH buffer including the suffix and its
// NUL: the boundary sits one below capacity - suffix length.
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

TEST(ServiceLogSinks, PrimaryRefusesOversizedDirectory) {
  EXPECT_EQ(sunshinesvc::open_primary_log_handle_in(std::wstring(300, L'a')), INVALID_HANDLE_VALUE);
}

// Reproduces the lock from #1119: a handle opened without write sharing (as
// written by builds before the fix) must block the primary sink and be
// rescued by the per-instance fallback instead of failing service start.
TEST(ServiceLogSinks, FallbackEngagesWhenPrimaryHeldByOldStyleWriter) {
  IsolatedLogDir dir;
  SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  const auto primary = dir.path() + L"sunshine.log";

  const auto blocker = CreateFileW(primary.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    &inheritable,
    CREATE_ALWAYS,
    0,
    NULL);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);

  EXPECT_EQ(sunshinesvc::open_primary_log_handle_in(dir.path()), INVALID_HANDLE_VALUE);

  const auto fallback = sunshinesvc::open_fallback_log_handle_in(dir.path());
  ASSERT_NE(fallback, INVALID_HANDLE_VALUE);

  const auto fallback_name = sunshinesvc::fallback_log_name(dir.path(), GetCurrentProcessId());
  EXPECT_NE(GetFileAttributesW(fallback_name.c_str()), INVALID_FILE_ATTRIBUTES);

  DWORD written = 0;
  EXPECT_TRUE(WriteFile(fallback, "x", 1, &written, NULL));
  EXPECT_EQ(written, 1u);

  CloseHandle(fallback);
  CloseHandle(blocker);
  DeleteFileW(fallback_name.c_str());
  DeleteFileW(primary.c_str());
}

// A fix-era core shares write on the primary log. The primary open must
// still refuse — CREATE_ALWAYS must not truncate a log another writer owns —
// and the fallback keeps the service startable.
TEST(ServiceLogSinks, FallbackEngagesWhenPrimaryHeldByWriteSharingWriter) {
  IsolatedLogDir dir;
  SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
  const auto primary = dir.path() + L"sunshine.log";

  const auto blocker = CreateFileW(primary.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    &inheritable,
    CREATE_ALWAYS,
    0,
    NULL);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
  DWORD written = 0;
  ASSERT_TRUE(WriteFile(blocker, "sentinel", 8, &written, NULL));
  ASSERT_EQ(written, 8u);

  EXPECT_EQ(sunshinesvc::open_primary_log_handle_in(dir.path()), INVALID_HANDLE_VALUE);

  // The blocker's content survives the refused primary attempt untouched.
  const auto reread = CreateFileW(primary.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL,
    OPEN_EXISTING,
    0,
    NULL);
  ASSERT_NE(reread, INVALID_HANDLE_VALUE);
  char buffer[8] = {};
  ASSERT_TRUE(ReadFile(reread, buffer, sizeof(buffer), &written, NULL));
  EXPECT_EQ(written, 8u);
  EXPECT_EQ(std::string(buffer, sizeof(buffer)), "sentinel");
  CloseHandle(reread);

  const auto fallback = sunshinesvc::open_fallback_log_handle_in(dir.path());
  ASSERT_NE(fallback, INVALID_HANDLE_VALUE);
  CloseHandle(fallback);

  CloseHandle(blocker);
  DeleteFileW(sunshinesvc::fallback_log_name(dir.path(), GetCurrentProcessId()).c_str());
  DeleteFileW(primary.c_str());
}

TEST(ServiceLogSinks, PrimaryOpensUnlocked) {
  IsolatedLogDir dir;

  const auto handle = sunshinesvc::open_primary_log_handle_in(dir.path());
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  CloseHandle(handle);
  DeleteFileW((dir.path() + L"sunshine.log").c_str());
}
