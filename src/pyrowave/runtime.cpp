/**
 * @file src/pyrowave/runtime.cpp
 * @brief Runtime gates for experimental video formats.
 */
#include "runtime.h"

#include "src/logging.h"
#include "src/utility.h"

extern "C" {
#include "third-party/moonlight-common-c/src/PyrowaveProtocol.h"
}

#ifdef _WIN32
  #include <vulkan/vulkan.h>
  #include <pyrowave.h>
#endif

#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

using namespace std::string_view_literals;

namespace pyrowave {
  namespace {
    template<typename callback_t>
    void
    safe_probe_log(callback_t &&callback) noexcept {
      try {
        callback();
      }
      catch (...) {
        // A diagnostic failure must never turn an optional capability probe
        // into a process failure.
      }
    }
  }

  bool
  is_experimental_video_format(std::uint32_t video_format) noexcept {
    return video_format == LI_PYROWAVE_VIDEO_FORMAT;
  }

  bool
  is_server_video_format_available(std::uint32_t video_format) noexcept {
#ifdef _WIN32
    return is_experimental_video_format(video_format);
#else
    return !is_experimental_video_format(video_format);
#endif
  }

  bool
  is_server_runtime_available() noexcept {
#ifdef _WIN32
    static std::once_flag probe_once;
    static bool available = false;
    std::call_once(probe_once, [] {
      // The first call creates a Vulkan device. It used to happen inside the first RTSP DESCRIBE, on the one thread that
      // serves every RTSP request; say how long it took so a slow connect can be told from a slow client.
      const auto probe_begin = std::chrono::steady_clock::now();
      const auto log_duration = util::fail_guard([probe_begin] {
        safe_probe_log([&] {
          BOOST_LOG(info) << "PyroWave runtime probe took "sv
                          << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - probe_begin).count()
                          << " ms"sv;
        });
      });
      std::uint32_t major = 0;
      std::uint32_t minor = 0;
      std::uint32_t patch = 0;
      pyrowave_get_api_version(&major, &minor, &patch);
      if (major != PYROWAVE_API_VERSION_MAJOR ||
          minor != PYROWAVE_API_VERSION_MINOR ||
          patch != PYROWAVE_API_VERSION_PATCH) {
        safe_probe_log([&] {
          BOOST_LOG(warning) << "PyroWave runtime probe failed: API version "
                             << major << '.' << minor << '.' << patch
                             << " (expected " << PYROWAVE_API_VERSION_MAJOR << '.'
                             << PYROWAVE_API_VERSION_MINOR << '.' << PYROWAVE_API_VERSION_PATCH << ')';
        });
        return;
      }
      pyrowave_device device = nullptr;
      const auto create_result = pyrowave_create_default_device(&device);
      if (create_result != PYROWAVE_SUCCESS || device == nullptr) {
        safe_probe_log([&] {
          BOOST_LOG(warning) << "PyroWave runtime probe failed: default device creation returned "
                             << static_cast<int>(create_result)
                             << (device == nullptr ? " with a null device"sv : ""sv);
        });
        if (device != nullptr) {
          pyrowave_device_destroy(device);
        }
        return;
      }

      const auto interop_supported = pyrowave_device_confirm_interop_support(device);
      if (interop_supported) {
        pyrowave_device_destroy(device);
        available = true;
      }
      else {
        safe_probe_log([] {
          BOOST_LOG(warning) << "PyroWave runtime probe failed: default device does not support required interop";
        });
        pyrowave_device_destroy(device);
      }
    });
    return available;
#else
    return false;
#endif
  }

  void
  prewarm_server_runtime() noexcept {
#ifdef _WIN32
    try {
      // Detached on purpose: the probe is cached for the process lifetime, and a DESCRIBE that arrives while it runs
      // waits on the same std::call_once, so nothing is ever probed twice.
      std::thread([] {
        (void) is_server_runtime_available();
      }).detach();
    }
    catch (...) {
      // No thread: the probe simply runs on the first DESCRIBE as before.
    }
#endif
  }
}  // namespace pyrowave
