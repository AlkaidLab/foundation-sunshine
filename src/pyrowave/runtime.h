/**
 * @file src/pyrowave/runtime.h
 * @brief Runtime gates for the experimental PyroWave server path.
 */
#pragma once

#include <cstdint>

namespace pyrowave {
  /** Return true when a wire format is the experimental PyroWave format. */
  [[nodiscard]] bool
  is_experimental_video_format(std::uint32_t video_format) noexcept;

  /**
   * Return whether this Sunshine build has a complete server implementation.
   * The protocol contract may exist before the encoder and GPU handoff are
   * ready; those stages must not be selected by an RTSP request prematurely.
   */
  [[nodiscard]] bool
  is_server_video_format_available(std::uint32_t video_format) noexcept;

  /**
   * Return whether the statically linked server implementation passes the
   * process-level early gate (API version, default Vulkan device and basic
   * interop support). The result is cached for the process lifetime so SDP
   * generation does not repeatedly initialize the upstream runtime. This is
   * not a promise that the active capture display, format, shader or fence can
   * be used; those resources must be validated when a session is created.
   */
  [[nodiscard]] bool
  is_server_runtime_available() noexcept;

  /**
   * Start the runtime probe on its own thread at host start, so the first RTSP DESCRIBE does not pay for creating the
   * Vulkan device on the RTSP thread (a slow DESCRIBE holds every other RTSP request behind it). Returns at once.
   */
  void
  prewarm_server_runtime() noexcept;

}  // namespace pyrowave
