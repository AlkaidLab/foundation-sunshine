/**
 * @file src/platform/windows/ds5/ds5_attach_policy.h
 * @brief How many times a native DualSense USB/IP attach is tried, and whether it waits.
 *
 * The attach runs while a controller is being allocated, so a wait here also holds
 * the input thread.  A structural failure (no host-side backend, no usbip.exe, no
 * driver, a rejected request, or a stale-port listing that cannot run) cannot clear
 * by itself: it is reported to the caller at once and the caller falls back to
 * ViGEm.  A failure that a synchronous stale-port sweep can clear gets one more
 * attempt.  The sweep runs the detach to completion, so no sleep is needed between
 * the attempts; the previous 1.5 s, 3 s and 4.5 s backoff kept the input thread
 * blocked for nine seconds when nothing could succeed.
 *
 * Source: usbip-win2-attach-cli-v0.9.8.1.cpp and usbip-win2-cli-v0.9.8.1.cpp
 * (attach opens the VHCI device before it contacts the server; a missing driver
 * or helper fails before any transfer and reports a process exit), plus the
 * statuses of usbip_host_controller in src/remote_usb/remote_usb_host_controller.h.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "src/remote_usb/remote_usb_host_controller.h"

namespace platf::ds5::attach_policy {
  /** One initial attempt and at most one retry after a useful stale-port sweep. */
  constexpr unsigned kMaxAttempts = 2;

  struct decision_t {
    bool retry { false };
    std::chrono::milliseconds delay { 0 };
  };

  constexpr std::string_view
  name(remote_usb::usbip_host_status status) {
    using remote_usb::usbip_host_status;
    switch (status) {
      case usbip_host_status::ok:
        return "ok";
      case usbip_host_status::invalid_argument:
        return "invalid_argument";
      case usbip_host_status::busy:
        return "busy";
      case usbip_host_status::stopped:
        return "stopped";
      case usbip_host_status::launch_failed:
        return "launch_failed";
      case usbip_host_status::attach_failed:
        return "attach_failed";
      case usbip_host_status::detach_failed:
        return "detach_failed";
      case usbip_host_status::timed_out:
        return "timed_out";
      case usbip_host_status::cancelled:
        return "cancelled";
      case usbip_host_status::unsupported:
        return "unsupported";
      case usbip_host_status::sweep_failed:
        return "sweep_failed";
    }
    return "unknown";
  }

  /** The stale-port sweep can clear an attach failure only when it could list the imports. */
  constexpr bool
  sweep_can_clear(remote_usb::usbip_host_status status) {
    using remote_usb::usbip_host_status;
    return status == usbip_host_status::ok || status == usbip_host_status::detach_failed;
  }

  /**
   * @param attach the result of the attempt that just failed
   * @param sweep the stale-port sweep performed before that attempt
   * @param attempts_done attempts already made, including the failed one
   *
   * `busy`, `timed_out` and `attach_failed` are the failures a sweep can clear.
   * Everything else is structural.  No path returns a non-zero delay: the sweep
   * already waits for its detach, and the input thread must not sleep here.
   */
  constexpr decision_t
  next(remote_usb::usbip_host_status attach, remote_usb::usbip_host_status sweep, unsigned attempts_done) {
    using remote_usb::usbip_host_status;
    if (attempts_done >= kMaxAttempts) return {};
    switch (attach) {
      case usbip_host_status::busy:
      case usbip_host_status::timed_out:
      case usbip_host_status::attach_failed:
        if (sweep_can_clear(sweep)) return { true, std::chrono::milliseconds(0) };
        return {};
      default:
        return {};
    }
  }
}  // namespace platf::ds5::attach_policy
