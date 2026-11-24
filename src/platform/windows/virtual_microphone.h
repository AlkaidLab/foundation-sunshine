/**
 * @file src/platform/windows/virtual_microphone.h
 * @brief The host's own virtual microphone: a USB device that Windows sees as a capture endpoint.
 *
 * One session owns the device model (virtual_usb::uac1_microphone), a USB/IP
 * transport that presents it to Windows, and the guard that keeps the user's
 * default audio endpoints.  The device exists from start() to stop(); at most
 * one session exists per host.  Nothing here depends on the DualSense.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "src/virtual_usb/uac1_microphone.h"

namespace platf::audio {

  struct virtual_microphone_status {
    bool component_available { false };
    bool online { false };
    bool device_created { false };
    bool host_streaming { false };
    std::uint32_t generation { 0 };
    std::uint32_t buffered_bytes { 0 };
    std::uint32_t underruns { 0 };
    std::uint32_t dropped_frames { 0 };
    std::uint32_t submit_errors { 0 };
    std::int32_t last_error { 0 };
    /** absent, creating, enumerating, idle, host_capturing, remote_active, destroying, device_faulted. */
    std::string state { "absent" };
    std::string error_code;
  };

  class virtual_microphone_t {
  public:
    virtual_microphone_t();
    ~virtual_microphone_t();

    virtual_microphone_t(const virtual_microphone_t &) = delete;
    virtual_microphone_t &
    operator=(const virtual_microphone_t &) = delete;

    /**
     * Create the device and wait until Windows has attached it.  Returns an
     * empty string on success, otherwise one of MIC_USBIP_COMPONENT_UNAVAILABLE,
     * MIC_USBIP_DEVICE_BUSY, MIC_USBIP_START_TIMEOUT or MIC_USBIP_CREATE_FAILED.
     */
    std::string
    start();

    /** Remove the device and release everything.  Safe to repeat. */
    void
    stop();

    bool
    running() const noexcept;

    /**
     * Queue mono 48 kHz signed 16-bit PCM without blocking.  Returns the bytes
     * stored, 0 when the frames were dropped because the queue is full and -1
     * when no device exists.
     */
    int
    write_pcm(const std::int16_t *samples, std::size_t frame_count) noexcept;

    /** Drop queued audio, for example when the remote stream restarts. */
    void
    flush() noexcept;

    /** True once Windows has opened the capture stream (an application records). */
    bool
    host_capturing() const noexcept;

    virtual_microphone_status
    status() const;

    /** The model's own counters (fill, ratio, trims), for diagnostics and the verification tools. */
    virtual_usb::microphone_stats
    model_stats() const;

    /** Snapshot of the running session, or of the last failure when none runs. */
    static virtual_microphone_status
    current_status();

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
  };

}  // namespace platf::audio
