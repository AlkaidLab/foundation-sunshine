/**
 * @file src/system_info_cache.cpp
 * @brief 60-second cache around the host inventory probe.
 */
#include "system_info.h"

#include <chrono>
#include <ctime>
#include <mutex>

namespace system_info {
  namespace {
    std::string
    format_utc_iso(std::int64_t epoch_seconds) {
      const auto value = static_cast<std::time_t>(epoch_seconds);
      std::tm broken_down {};
#ifdef _WIN32
      gmtime_s(&broken_down, &value);
#else
      gmtime_r(&value, &broken_down);
#endif
      char buffer[32];
      std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &broken_down);
      return buffer;
    }

    std::int64_t
    now_epoch_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
    }
  }  // namespace

  snapshot_t
  sample(std::string_view configured_adapter, bool refresh) {
    static std::mutex mutex;
    static snapshot_t cached;
    static std::string cached_key;
    static std::int64_t cached_at = 0;

    std::lock_guard lock { mutex };
    const auto now = now_epoch_seconds();
    const std::string key { configured_adapter };
    if (!refresh && cached_at != 0 && key == cached_key && now - cached_at < CACHE_TTL_SECONDS) {
      return cached;
    }
    auto result = collect(configured_adapter);
    result.checked_at = format_utc_iso(now);
    cached = result;
    cached_key = key;
    cached_at = now;
    return result;
  }
}  // namespace system_info
