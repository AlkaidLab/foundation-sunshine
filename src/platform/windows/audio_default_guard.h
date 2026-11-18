/**
 * @file src/platform/windows/audio_default_guard.h
 * @brief Keep the user's default audio endpoints when a virtual USB audio device arrives.
 *
 * Windows may promote a newly published USB audio endpoint to the default
 * device of its flow.  A virtual device of this product must never change the
 * defaults, so its owner records them before the attach and puts back any
 * default that moved onto the device afterwards.  The endpoint is recognised
 * by the hardware id in its instance id; a default the user changed to some
 * other device meanwhile is left alone.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <mmdeviceapi.h>

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace platf::audio {

  struct default_endpoint {
    EDataFlow flow { eRender };
    ERole role { eConsole };
    std::wstring id;
  };

  /** The six defaults (render and capture, three roles each) that exist now. */
  std::optional<std::vector<default_endpoint>>
  capture_default_endpoints();

  /**
   * Set a recorded default back when it is now one of our devices: an endpoint
   * is ours when its instance id, its friendly name or the friendly name of its
   * adapter contains one of `markers` (for example the product string; the
   * instance id of an endpoint is a software-device path and carries no USB
   * ids).  Returns true if any default was changed.
   */
  /** The ids of every active render and capture endpoint now. */
  std::set<std::wstring>
  capture_active_endpoint_ids();

  /** `existing`, when given, lists endpoints that were there before the lease: they are never taken back. */
  bool
  restore_default_endpoints(const std::vector<default_endpoint> &original, std::initializer_list<std::wstring_view> markers,
    const std::set<std::wstring> *existing = nullptr);

  /**
   * Restore after the endpoints of a new device have been published.  The
   * class driver publishes them after the attach has completed, so the check
   * repeats at the given delays.
   */
  void
  restore_default_endpoints_after_attach(const std::vector<default_endpoint> &original, std::initializer_list<std::wstring_view> markers,
    std::initializer_list<std::chrono::milliseconds> delays = { std::chrono::milliseconds(0), std::chrono::milliseconds(250), std::chrono::milliseconds(1000), std::chrono::milliseconds(2500), std::chrono::milliseconds(5000) },
    const std::set<std::wstring> *existing = nullptr);

  /** What the setup of one endpoint of the virtual pad found and did (see configure_pad_endpoints). */
  struct pad_endpoint_state_t {
    std::wstring id;
    std::wstring name;
    EDataFlow flow { eRender };
    bool first_time { false };  ///< this endpoint id was not seen before
    unsigned long speakers_before { 0 }, full_range_before { 0 }, speakers_after { 0 }, full_range_after { 0 };
    bool format_before_ok { false }, format_after_ok { false };
    float volume_before { -1.0f }, volume_after { -1.0f };
    bool mute_before { false }, mute_after { false };
    bool volume_written { false };
    bool configured_now { false };  ///< something was written this pass
    bool verified { false };  ///< every wanted value reads back
    std::string warning;
  };

  /**
   * Make the Windows endpoints of the virtual pad usable without a trip to the Sound control panel: for the render
   * endpoint the quadraphonic speaker mask (front left and right, surround left and right: 0x33) as both the
   * physical and the full-range configuration and the four-channel 48 kHz 16-bit device format, which is what
   * "Configure speakers" writes; for both render and capture the volume at 100 percent and unmuted, but only the
   * first time an endpoint id appears (a stable device keeps what the user set, and a zero or muted known device is
   * reported as a warning, not changed). Every value is read back. Endpoints are recognised by `marker` in the
   * adapter or friendly name. Safe to call repeatedly.
   */
  std::vector<pad_endpoint_state_t>
  configure_pad_endpoints(std::wstring_view marker, const std::filesystem::path &known_file);

  /** Names of endpoints that match `marker` and are not active now (left over from earlier attaches). */
  std::vector<std::wstring>
  list_stale_pad_endpoints(std::wstring_view marker);

  /**
   * Remove the phantom top-level USB devices (not present now) with `hardware_marker` in their instance id whose id
   * does not contain `keep_marker` (the stable serial prefix): the instances an earlier build created before the
   * device had a serial. Returns the instance ids removed.
   */
  std::vector<std::wstring>
  prune_phantom_pad_devices(std::wstring_view hardware_marker, std::wstring_view keep_marker);

}  // namespace platf::audio
