/**
 * @file src/platform/windows/virtual_device_host/microphone_client.h
 * @brief Product-facing USB/IP virtual microphone client.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace platf::virtual_device_host {
  struct microphone_status_t {
    bool component_available = false;
    bool online = false;
    bool device_created = false;
    bool host_streaming = false;
    std::uint32_t generation = 0;
    std::uint32_t buffered_bytes = 0;
    std::uint32_t underruns = 0;
    std::uint32_t dropped_frames = 0;
    std::uint32_t submit_errors = 0;
    std::int32_t last_error = 0;
    std::string state = "absent";
    std::string error_code;
  };

  /** Last published status, safe to query from the Web UI thread. */
  microphone_status_t microphone_status();

  class microphone_client_t {
  public:
    microphone_client_t();
    ~microphone_client_t();

    microphone_client_t(const microphone_client_t &) = delete;
    microphone_client_t &operator=(const microphone_client_t &) = delete;

    /** Launch the host and create its persistent virtual microphone. */
    bool start();

    /** Queue mono 48 kHz signed 16-bit PCM without blocking on the pipe. */
    int write_pcm(const std::int16_t *samples, std::size_t frame_count);

    /** End the current remote stream while preserving the Windows endpoint. */
    bool flush();

    bool online() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };

  /** Process-wide device retained across Moonlight microphone sessions. */
  microphone_client_t &persistent_microphone_client();
}  // namespace platf::virtual_device_host
