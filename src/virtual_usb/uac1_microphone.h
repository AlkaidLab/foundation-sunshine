/**
 * @file src/virtual_usb/uac1_microphone.h
 * @brief A capture-only USB Audio Class 1.0 microphone as a virtual USB device.
 *
 * The device is mono, 48 kHz, 16-bit PCM, with one isochronous IN endpoint
 * (high speed, bInterval 4, one 96-byte packet per millisecond) and a feature
 * unit that offers mute and volume.  Windows binds its in-box USB Audio driver
 * to it and publishes a normal capture endpoint named after the product
 * string.
 *
 * Data path.  The producer (the thread that decodes the client microphone
 * stream) calls write_pcm(), which copies into a bounded single-producer
 * single-consumer ring and never blocks.  The bus clock (the transport asks
 * for 48 frames per packet slot) is the device clock.  The two clocks differ,
 * so the consumer steers its read position by the ring fill level: inside a
 * dead band it copies samples unchanged, outside it resamples by linear
 * interpolation with a ratio of at most +-0.5 percent.  A ring that runs dry
 * answers with silence and refills to a target level before it plays again;
 * a ring that fills beyond a high-water mark is trimmed back to the target.
 *
 * Sources: USB Audio Class 1.0 (Audio10.pdf) sections 4.3 to 4.6 for the
 * descriptors and 5.2.2 for the feature unit requests, Frmts10.pdf section 2
 * for the Type I PCM format, and USB 2.0 section 9.6 for the standard
 * descriptors.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "device.h"

namespace virtual_usb {

  /** Placeholder identity pending a vendor allocation; see the integration notes. */
  inline constexpr std::uint16_t kVirtualMicrophoneVendorId = 0x1209;
  inline constexpr std::uint16_t kVirtualMicrophoneProductId = 0xa1c0;
  inline constexpr char kVirtualMicrophoneProductString[] = "Sunshine Virtual Microphone";
  inline constexpr char kVirtualMicrophoneManufacturerString[] = "Foundation Sunshine";
  /** Stable across sessions so that Windows keeps one endpoint identity. */
  inline constexpr char kVirtualMicrophoneSerialString[] = "SUNSHINEVMIC0001";

  struct microphone_tuning {
    /** Ring capacity in frames; a power of two. */
    std::uint32_t ring_frames { 8192 };
    /** Fill level the servo steers to, and the level playback starts at. */
    std::uint32_t target_frames { 960 };
    /** Above this fill the consumer trims back to the target. */
    std::uint32_t high_water_frames { 2880 };
    /** No resampling while the fill is within this many frames of the target. */
    std::uint32_t dead_band_frames { 24 };
    /** Ratio change per frame of fill error outside the dead band. */
    double servo_gain { 1e-5 };
    /** Largest deviation of the resampling ratio from one. */
    double max_ratio_deviation { 0.005 };
    /** A write this recent counts as "the remote side is talking". */
    std::chrono::milliseconds active_window { 250 };
  };

  struct microphone_stats {
    std::uint64_t frames_written { 0 };
    /** Frames discarded on arrival because no application records (stale audio is never queued). */
    std::uint64_t frames_idle_discarded { 0 };
    /** Frames the producer could not store because the ring was full. */
    std::uint64_t frames_dropped { 0 };
    /** Frames the consumer discarded when the fill passed the high-water mark. */
    std::uint64_t frames_trimmed { 0 };
    std::uint64_t frames_served { 0 };
    /** Silence frames served while the remote side was talking. */
    std::uint64_t underrun_frames { 0 };
    /** Distinct underrun events. */
    std::uint32_t underruns { 0 };
    std::uint64_t packets_served { 0 };
    std::uint32_t buffered_frames { 0 };
    /** Resampling ratio of the last packet; exactly 1.0 inside the dead band. */
    double ratio { 1.0 };
    /** Smoothed fill level the servo steers, in frames. */
    double fill_average { 0.0 };
    /** Feature unit state set by the host (volume in 1/256 dB). */
    bool muted { false };
    std::int16_t volume { 0 };
    /** The host selected the configuration (the device is enumerated). */
    bool configured { false };
    /** The host opened the capture stream (an application records). */
    bool capturing { false };
    /** A write arrived within the active window. */
    bool remote_active { false };
  };

  class uac1_microphone final: public device {
  public:
    static constexpr std::uint32_t kSampleRate = 48000;
    static constexpr std::uint32_t kFramesPerPacket = 48;
    static constexpr std::uint32_t kPacketBytes = kFramesPerPacket * 2;
    static constexpr std::uint16_t kMaxPacketBytes = kPacketBytes + 2;
    static constexpr std::uint8_t kStreamingEndpoint = 1;
    static constexpr std::uint8_t kFeatureUnitId = 2;
    static constexpr std::int16_t kVolumeMin = -12800;  ///< -50 dB in 1/256 dB.
    static constexpr std::int16_t kVolumeMax = 0;
    static constexpr std::int16_t kVolumeStep = 256;

    explicit uac1_microphone(microphone_tuning tuning = {});
    ~uac1_microphone() override;

    uac1_microphone(const uac1_microphone &) = delete;
    uac1_microphone &
    operator=(const uac1_microphone &) = delete;

    /**
     * Queue mono 48 kHz signed 16-bit PCM.  Wait-free; call from one thread.
     * While Windows has not opened the capture stream nothing is queued (the frames are
     * counted in frames_idle_discarded), so a recording never starts with old audio.
     * Returns the number of frames stored (fewer than `frames` only when the
     * ring is full; the rest is counted in frames_dropped).
     */
    std::size_t
    write_pcm(const std::int16_t *samples, std::size_t frames) noexcept;

    /** Drop everything that is queued and start the next stream from the target fill. */
    void
    flush() noexcept;

    microphone_stats
    stats() const noexcept;

    // virtual_usb::device
    const device_identity &
    identity() const override { return identity_; }
    control_result
    control(const setup_packet &setup, std::span<const std::uint8_t> out_data) override;
    std::chrono::nanoseconds
    iso_packet_period(std::uint8_t endpoint) const override;
    bool
    iso_in(const iso_in_request &request, iso_in_reply &reply) override;
    void
    bus_reset() override;

    /** The descriptors, exposed so tests and tools can check them. */
    static std::vector<std::uint8_t>
    device_descriptor();
    static std::vector<std::uint8_t>
    configuration_descriptor(std::uint8_t descriptor_type = 2);
    static std::vector<std::uint8_t>
    string_descriptor(std::uint8_t index);

  private:
    struct state;

    void
    serve_packet(std::int16_t *out) noexcept;

    device_identity identity_;
    microphone_tuning tuning_;
    std::unique_ptr<state> state_;
  };

}  // namespace virtual_usb
