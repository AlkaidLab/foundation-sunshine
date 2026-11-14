/**
 * @file src/platform/windows/ds5/ds5_pad_audio.h
 * @brief Stable USB identity and capture mute for the virtual DualSense.
 *
 * Windows uses the USB serial as the instance id when iSerialNumber is set,
 * and derives one container id from that identity. A new serial is a new
 * device, so the audio class driver publishes another endpoint pair.
 *
 * The capture feature-unit mute is the USB Audio Class 1.0 mute control.
 * Windows exposes it as IAudioEndpointVolume on the capture endpoint named
 * with the product string. The pad mute button uses that endpoint, and the
 * indicator follows it. No other capture device is substituted.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace platf::ds5::pad_audio {

  std::wstring
  lease_serial(int global_index);

  void
  release_serial(int global_index);

  std::array<std::uint8_t, 6>
  mac_from_serial(const std::wstring &serial);

  std::optional<bool>
  capture_muted();

  std::optional<bool>
  toggle_capture_mute();

  std::optional<bool>
  set_capture_mute(bool muted);

}  // namespace platf::ds5::pad_audio
