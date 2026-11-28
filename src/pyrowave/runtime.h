/**
 * @file src/pyrowave/runtime.h
 * @brief Runtime gates for the experimental PyroWave server path.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace pyrowave {

  /**
   * @brief The advertisement state of the one-shot runtime probe.
   *
   * The DESCRIBE answer may carry the PyroWave contract while the probe has not failed: the client's own
   * capability is not visible to the host until ANNOUNCE, so the advertisement stays optimistic and a probe
   * that fails turns it off for the rest of the process (the client then falls back to a legacy format on
   * its next connection).
   */
  class probe_state_t {
  public:
    /** May the SDP carry the PyroWave contract: the probe has not failed (never run, running, or passed). */
    [[nodiscard]] bool
    may_advertise() const noexcept;

    /** The one-shot probe ended: only a failure changes the state. */
    void
    record_probe_result(bool available) noexcept;

  private:
    std::atomic<bool> failed_ { false };
  };

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
   * Return whether the SDP may carry the PyroWave contract: the one-shot runtime probe has not failed.
   * Never blocks and never starts the probe; the DESCRIBE answer uses this, so the handshake of a client
   * that never selects PyroWave never touches Vulkan at all.
   */
  [[nodiscard]] bool
  may_advertise() noexcept;

  /**
   * Return whether the statically linked server implementation passes the
   * process-level early gate (API version, default Vulkan device and basic interop support), running the
   * gate once if needed. The gate is the alkaidlab_pyrowave_probe tool next to this executable, bounded in time
   * and terminated on a hang, so a crashing or stuck third-party Vulkan implicit layer can only cost
   * the PyroWave advertisement. The result is cached for the process lifetime. This is not a promise
   * that the active capture display, format, shader or fence can be used; those resources must be
   * validated when a session is created.
   */
  [[nodiscard]] bool
  is_server_runtime_available() noexcept;

  /**
   * A short reason for a probe exit code, for the log line that says the probe failed. Empty when the code
   * carries no fixed meaning (the caller then logs the number itself).
   */
  [[nodiscard]] std::string_view
  probe_exit_code_reason(std::uint32_t exit_code) noexcept;

}  // namespace pyrowave
