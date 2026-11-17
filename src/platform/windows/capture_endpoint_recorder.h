/**
 * @file src/platform/windows/capture_endpoint_recorder.h
 * @brief Records the first channel of a WASAPI capture endpoint found by name.
 *
 * Used by the microphone self-test and by the verification tools: it opens the
 * endpoint in shared mode with its mix format (no format conversion that could
 * hide a clock error), reads with the device's own timestamps and reports
 * discontinuities as WASAPI flags them.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace platf::audio {

  class capture_endpoint_recorder {
  public:
    struct block {
      /** QPC time of the first frame in 100 ns units (IAudioCaptureClient::GetBuffer). */
      std::uint64_t qpc_100ns { 0 };
      /** QPC time at which the application received the block, in 100 ns units. */
      std::uint64_t read_qpc_100ns { 0 };
      /** Device position of the first frame, in frames. */
      std::uint64_t position { 0 };
      bool discontinuity { false };
      bool silent { false };
      std::vector<float> samples;
    };

    capture_endpoint_recorder();
    ~capture_endpoint_recorder();

    capture_endpoint_recorder(const capture_endpoint_recorder &) = delete;
    capture_endpoint_recorder &
    operator=(const capture_endpoint_recorder &) = delete;

    /** Wait up to `wait` for an active capture endpoint whose name contains `name_part`. */
    bool
    open(std::wstring_view name_part, std::chrono::milliseconds wait);
    void
    close();

    std::uint32_t
    sample_rate() const;

    /**
     * Read for `duration`; appends to `blocks`.  Returns false on a device error.
     * After Start(), capture first waits up to `first_packet_wait` for the first
     * packet; the measured window starts with that packet, so the ramp-up of a
     * freshly arrived device does not shorten the measurement.  Pass zero to
     * measure from Start() as before.
     */
    bool
    record(std::chrono::milliseconds duration, std::vector<block> &blocks,
      std::chrono::milliseconds first_packet_wait = std::chrono::milliseconds(2000));

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
  };

}  // namespace platf::audio
