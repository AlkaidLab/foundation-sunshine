/**
 * @file src/platform/gamepad_selection.h
 * @brief Session and arrival rules for client-declared virtual controllers.
 */
#pragma once

#include <string_view>

#include <cstdint>

namespace platf::gamepad_selection {
  inline constexpr int automatic = 1;
  inline constexpr int xbox360 = 2;
  inline constexpr int dualshock4 = 3;
  inline constexpr int dualsense = 4;
  // Protocol extension coordinated with the client arrival metadata. The
  // existing 0x200 capability means PCM output is available; 0x400 identifies
  // the physical DualSense independently of that optional endpoint. The
  // capability allocation follows ds5-moonlight-controller-capability-20261105.
  inline constexpr std::uint16_t dualsense_pcm_capability = 0x0200;
  inline constexpr std::uint16_t dualsense_arrival_capability = 0x0400;
  inline constexpr std::uint8_t playstation_arrival_type = 0x02;

  // Read-only virtual pad type names for API responses (see pad_facts_t).
  inline constexpr std::string_view pad_type_xbox360 = "x360";
  inline constexpr std::string_view pad_type_dualshock4 = "ds4";
  inline constexpr std::string_view pad_type_dualsense = "ds5";

  constexpr int
  mode_from_preference(std::string_view preference) {
    if (preference == "x360") return xbox360;
    if (preference == "ds4") return dualshock4;
    if (preference == "ds5") return dualsense;
    return automatic;
  }

  /**
   * The arrival metadata identifies the pad as a DualSense: a PlayStation pad
   * carrying the PCM output capability (0x0200) or the identity capability
   * (0x0400). A PlayStation pad without either may still be a DualSense; the
   * client just does not tell the host which pad it is.
   */
  constexpr bool
  identifies_dualsense(std::uint8_t type, std::uint16_t capabilities) {
    return type == playstation_arrival_type &&
           (capabilities & (dualsense_pcm_capability | dualsense_arrival_capability)) != 0;
  }

  /**
   * Select the virtual controller for an allocation.
   *
   * A client-declared preference is session state, so it remains available when
   * the first controller arrives after the stream starts. The DS5 PCM arrival
   * capability is the only late-arrival signal that identifies a PlayStation pad
   * more specifically than the legacy PS type. See
   * ds5-moonlight-controller-arrival-20261105, controller-arrival capability
   * section, for the arrival metadata contract.
   */
  constexpr int
  select(int host_mode, std::string_view client_preference, bool client_override,
    std::uint8_t arrival_type, std::uint16_t capabilities) {
    const bool is_playstation = arrival_type == playstation_arrival_type;
    const bool has_ds5_capability = identifies_dualsense(arrival_type, capabilities);
    if (client_override && !client_preference.empty()) {
      return mode_from_preference(client_preference);
    }
    if (client_override && client_preference.empty() && is_playstation && has_ds5_capability) {
      return dualsense;
    }
    return host_mode;
  }

  constexpr bool
  is_dualsense_arrival(std::uint8_t type, std::uint16_t capabilities) {
    return type == playstation_arrival_type &&
           (capabilities & dualsense_arrival_capability) != 0;
  }

  /** Read-only facts about one controller slot, for session API responses. */
  struct pad_facts_t {
    bool allocated = false;
    /// The arrival reported a PlayStation pad (playstation_arrival_type).
    bool playstation_arrival = false;
    /// The arrival identified the pad as a DualSense (identifies_dualsense).
    bool dualsense_identity = false;
    /// The platform's pad_type_* name of the allocated virtual pad; empty when unnamed.
    std::string_view virtual_type {};
  };

  /**
   * The session-level PlayStation controller facts: the first allocated pad
   * whose arrival declared a PlayStation pad. A pad without arrival metadata
   * never answers for the DS4/DualSense distinction, because the host cannot
   * know which physical pad the client holds. Type and capability bits per
   * moonlight-limelight-pinned-f38adbb7-20261106 (LI_CTYPE_PS, LI_CCAP_DS5_HAPTICS_PCM).
   */
  constexpr pad_facts_t
  summarize_pads(const pad_facts_t *pads, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      if (pads[i].allocated && pads[i].playstation_arrival) {
        return pads[i];
      }
    }
    return {};
  }

  constexpr bool
  advertise_ds5_capability(bool ds5_available, int selected_mode,
    std::string_view client_preference, bool client_override) {
    if (!ds5_available) return false;
    if (selected_mode == dualsense) return true;
    return client_override && client_preference.empty() && selected_mode == automatic;
  }
}  // namespace platf::gamepad_selection
