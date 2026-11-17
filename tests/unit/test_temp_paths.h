/**
 * @file tests/unit/test_temp_paths.h
 * @brief Temporary directories for tests that run in more than one executable at the same time.
 *
 * Most unit-test sources are compiled into the aggregate `test_sunshine` and also into a dedicated executable, and `ctest -j`
 * runs both at once. A fixed name collides, and so does a name from `std::random_device`: with MinGW's libstdc++ it can
 * produce the same sequence in every process. A name made of the process id, the clock and a counter cannot.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #include <process.h>
#else
  #include <unistd.h>
#endif

namespace test_support {
  inline unsigned long
  process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
  }

  /** A path in the system temp directory that no other process and no other call of this process uses; not created. */
  inline std::filesystem::path
  unique_temp_path(std::string_view prefix) {
    static std::atomic<unsigned> counter { 0 };
    std::error_code ec;
    auto root = std::filesystem::temp_directory_path(ec);
    if (ec) {
      root = std::filesystem::path("/scratch");
    }
    return root / (std::string(prefix) + "-" + std::to_string(process_id()) + "-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
  }

  /** Removes a tree without ever throwing: on Windows a file that something still holds open fails the removal, and a
   *  throwing cleanup in a destructor or at the end of a test ends the process in `terminate`. Retries a few times. */
  inline void
  remove_tree_quiet(const std::filesystem::path &path) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
      if (!ec) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
    }
  }

  /** A fresh directory that is removed (quietly) when the object goes away. */
  class ScopedTempDir {
  public:
    explicit ScopedTempDir(std::string_view prefix):
        path_(unique_temp_path(prefix)) {
      std::error_code ec;
      std::filesystem::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &
    operator=(const ScopedTempDir &) = delete;

    ~ScopedTempDir() {
      remove_tree_quiet(path_);
    }

    const std::filesystem::path &
    path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };
}  // namespace test_support
