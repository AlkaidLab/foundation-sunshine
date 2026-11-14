/**
 * @file src/telemetry/hooks.h
 * @brief Header-only hooks the per-frame code calls: a null pointer until telemetry::start() installs the real functions.
 *
 * The streaming code (perf_recorder) is also built into small test binaries that must not link the sender; it only includes this header.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace telemetry::hooks {

  struct table_t {
    std::atomic<void (*)(std::uint32_t)> frame_encoded { nullptr };
    std::atomic<void (*)(std::uint32_t, double)> encode_ms { nullptr };
    std::atomic<void (*)(std::uint32_t, double)> capture_ms { nullptr };
  };

  inline table_t &
  table() {
    static table_t instance;
    return instance;
  }

  inline void
  frame_encoded(std::uint32_t session_id) noexcept {
    if (const auto fn = table().frame_encoded.load(std::memory_order_relaxed)) fn(session_id);
  }
  inline void
  encode_ms(std::uint32_t session_id, double ms) noexcept {
    if (const auto fn = table().encode_ms.load(std::memory_order_relaxed)) fn(session_id, ms);
  }
  inline void
  capture_ms(std::uint32_t session_id, double ms) noexcept {
    if (const auto fn = table().capture_ms.load(std::memory_order_relaxed)) fn(session_id, ms);
  }

}  // namespace telemetry::hooks
