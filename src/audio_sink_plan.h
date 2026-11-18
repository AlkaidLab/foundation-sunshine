/**
 * @file src/audio_sink_plan.h
 * @brief The decisions the audio capture makes about the virtual output device, kept apart from any audio API so they are tested alone.
 *
 * While a client streams without playing on the host, the host switches the Windows default to a virtual device and
 * captures it. Three questions come up and are answered here:
 *  - which device that is (the configured virtual sink, the captured device when it is a virtual one, or Steam Streaming Speakers);
 *  - which channel layout the device is asked for when it cannot take the one the client wants (VB-CABLE has no 7.1.4,
 *    and refuses some bit depths), so the stream falls back to a narrower layout instead of ending up without sound;
 *  - in which order the wave formats of a device are tried (the bit depth of the current default is a preference, not a rule).
 */
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace audio_sink_plan {
  /// The device the host switches the Windows default to while a stream runs without host playback.
  enum class virtual_target_e {
    steam_streaming,  ///< Steam Streaming Speakers: nothing is configured
    virtual_sink,  ///< the `virtual_sink` setting
    audio_sink,  ///< the `audio_sink` setting, because it names a virtual device (VB-CABLE, Steam, other VB-Audio devices)
  };

  /**
   * @brief Which device carries the stream when the host is muted.
   * @param virtual_sink the `virtual_sink` setting ("" when unset)
   * @param audio_sink the `audio_sink` setting ("" when unset)
   * @param audio_sink_is_virtual the device `audio_sink` points at is a known virtual device
   *
   * A person who chose VB-CABLE as the device to capture expects the sound of the game to reach it. The game only plays to the
   * Windows default, so the default has to be switched to that device, exactly as it is for `virtual_sink`. A virtual device has
   * no speakers: the stream runs on it whether or not the client asks to play the audio on the host (sink_t::null_is_selected_device).
   */
  virtual_target_e
  choose_virtual_target(const std::string &virtual_sink, const std::string &audio_sink, bool audio_sink_is_virtual);

  /// The channel counts a virtual device is tried with for a stream of `stream_channels` channels, widest first, always ending with 2.
  std::vector<int>
  layout_ladder(int stream_channels);

  /**
   * @brief The widest layout of the ladder that `supported` accepts.
   * @return the channel count; 2 when none is accepted (the last resort is tried as before, and fails as before)
   */
  int
  pick_layout(int stream_channels, const std::function<bool(int)> &supported);

  /**
   * @brief The order in which the wave formats of a virtual device are tried.
   * @param valid_bits the valid bits per sample of each wave format, in the preference order of the list
   * @param wanted_bits the valid bits of the current default device (0: no preference)
   * @return indexes into the list: the formats with the wanted bit depth first, then the others, the nearest depth first and
   *         the list order within one depth. A device that offers no format of the wanted depth (VB-CABLE has no 32-bit
   *         format) is then still tried with the others instead of being refused.
   */
  std::vector<std::size_t>
  format_try_order(const std::vector<int> &valid_bits, int wanted_bits);
}  // namespace audio_sink_plan
