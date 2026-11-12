/**
 * @file src/ds5/control_extension.h
 * @brief DualSense control extensions v1 (0x5515 host to client, 0x5516 client to host).
 *
 * Both messages are negotiated through a versioned SDP namespace and never through a controller
 * capability bit:
 *   client offer   a=x-ml-general.ds5Extensions:output=1,input-status=1
 *   host answer    a=x-ss-general.ds5Extensions:output=1,input-status=1
 * "output=1" means the peer accepts 0x5515, "input-status=1" means the peer accepts 0x5516. A
 * peer sends only the direction the other side declared, so a peer without the attribute never
 * sees either message.
 *
 * Bodies are little-endian and fixed size (16 and 12 bytes). A body that is not exactly the
 * documented form is rejected whole: wrong length, version other than 1, reserved bytes not zero,
 * unknown valid bits, a field outside its range, or a field that is not set in the valid mask but
 * is not zero.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ds5::control_extension {
  constexpr std::uint16_t type_output = 0x5515;
  constexpr std::uint16_t type_input_status = 0x5516;

  constexpr std::uint8_t version = 1;
  constexpr std::size_t output_body_bytes = 16;
  constexpr std::size_t input_status_body_bytes = 12;

  constexpr std::string_view client_attribute = "x-ml-general.ds5Extensions";
  constexpr std::string_view host_attribute = "x-ss-general.ds5Extensions";
  constexpr std::string_view offer = "output=1,input-status=1";
  /// What a client offers when it can play the pad's speaker channels (a physical four-channel endpoint).
  constexpr std::string_view offer_with_pad_audio = "output=1,input-status=1,pad-audio=1";
  /// The host's answer: every token it understands. A client that does not know a token ignores it.
  constexpr std::string_view host_answer = "output=1,input-status=1,pad-audio=1";

  constexpr std::uint32_t mask_output_v1 = 0x00000001u;
  constexpr std::uint32_t mask_input_status_v1 = 0x00000002u;
  /// The haptics PCM packet may carry four channels: speaker left and right, then haptic left and right.
  constexpr std::uint32_t mask_pad_audio_v1 = 0x00000004u;

  constexpr std::uint32_t out_player_leds = 0x00000001u;
  constexpr std::uint32_t out_mute_led = 0x00000002u;
  constexpr std::uint32_t out_speaker_volume = 0x00000004u;
  constexpr std::uint32_t out_headphone_volume = 0x00000008u;
  constexpr std::uint32_t out_mic_volume = 0x00000010u;
  constexpr std::uint32_t out_mic_mute = 0x00000020u;
  constexpr std::uint32_t out_audio_routing = 0x00000040u;
  constexpr std::uint32_t out_lightbar_brightness = 0x00000080u;
  constexpr std::uint32_t out_known = 0x000000ffu;

  constexpr std::uint32_t in_headset = 0x00000001u;
  constexpr std::uint32_t in_microphone = 0x00000002u;
  constexpr std::uint32_t in_mic_muted = 0x00000004u;
  constexpr std::uint32_t in_known = 0x00000007u;

  /** @brief Output fields; the device values are the DualSense output report bytes. */
  struct output_t {
    std::uint16_t controller {};
    std::uint32_t valid_mask {};
    std::uint8_t player_led_mask {};  ///< report byte 44, five bits
    std::uint8_t mute_led {};  ///< report byte 9, 0 or 1
    std::uint8_t speaker_volume {};  ///< report byte 6
    std::uint8_t headphone_volume {};  ///< report byte 5
    std::uint8_t mic_volume {};  ///< report byte 7
    std::uint8_t mic_mute {};  ///< report byte 10 bit 4, 0 or 1
    std::uint8_t audio_routing {};  ///< report byte 8
    std::uint8_t lightbar_brightness {};  ///< report byte 43

    friend bool operator==(const output_t &, const output_t &) = default;
  };

  struct input_status_t {
    std::uint16_t controller {};
    std::uint32_t valid_mask {};
    std::uint8_t headset_present {};
    std::uint8_t microphone_present {};
    std::uint8_t microphone_muted {};

    friend bool operator==(const input_status_t &, const input_status_t &) = default;
  };

  /**
   * @brief Parse an SDP attribute value of "name=version" tokens separated by ',' or ';'.
   * @return The mask_* bits of the tokens understood. Unknown names, versions other than 1 and
   *         malformed tokens never produce a bit.
   */
  constexpr std::uint32_t
  parse_sdp_value(std::string_view value) {
    std::uint32_t mask = 0;
    std::size_t offset = 0;
    while (offset < value.size()) {
      while (offset < value.size() &&
             (value[offset] == ',' || value[offset] == ';' || value[offset] == ' ' || value[offset] == '\t')) {
        ++offset;
      }
      const auto start = offset;
      while (offset < value.size() && value[offset] != ',' && value[offset] != ';') {
        ++offset;
      }
      auto end = offset;
      while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
      }
      const auto token = value.substr(start, end - start);
      const auto equals = token.find('=');
      if (equals == std::string_view::npos) {
        continue;
      }
      const auto name = token.substr(0, equals);
      const auto number = token.substr(equals + 1);
      if (number == "1") {
        if (name == "output") {
          mask |= mask_output_v1;
        }
        else if (name == "input-status") {
          mask |= mask_input_status_v1;
        }
        else if (name == "pad-audio") {
          mask |= mask_pad_audio_v1;
        }
      }
    }
    return mask;
  }

  /** @brief True when the client offered "output=1", so 0x5515 may be sent to it. */
  constexpr bool
  accepts_output(std::uint32_t negotiated_mask) {
    return (negotiated_mask & mask_output_v1) != 0;
  }

  /** @brief True when the client offered "pad-audio=1": the haptics PCM packet may then carry the speaker channels. */
  constexpr bool
  accepts_pad_audio(std::uint32_t negotiated_mask) {
    return (negotiated_mask & mask_pad_audio_v1) != 0;
  }

  /** @brief True when the client offered "input-status=1", so 0x5516 from it is accepted. */
  constexpr bool
  accepts_input_status(std::uint32_t negotiated_mask) {
    return (negotiated_mask & mask_input_status_v1) != 0;
  }

  namespace detail {
    constexpr std::uint16_t
    read16(const std::uint8_t *p) {
      return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }
    constexpr std::uint32_t
    read32(const std::uint8_t *p) {
      return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
             (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }
    constexpr void
    write16(std::uint8_t *p, std::uint16_t v) {
      p[0] = static_cast<std::uint8_t>(v & 0xff);
      p[1] = static_cast<std::uint8_t>(v >> 8);
    }
    constexpr void
    write32(std::uint8_t *p, std::uint32_t v) {
      p[0] = static_cast<std::uint8_t>(v & 0xff);
      p[1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
      p[2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
      p[3] = static_cast<std::uint8_t>(v >> 24);
    }
  }  // namespace detail

  /** @brief True when every field is in range and no field is set outside the valid mask. */
  constexpr bool
  valid(const output_t &o) {
    if ((o.valid_mask & ~out_known) != 0) return false;
    if (o.player_led_mask > 0x1f || o.mute_led > 1 || o.mic_mute > 1) return false;
    return true;
  }

  /** @brief Encode the 16-byte body of 0x5515. Fields outside the valid mask are written as zero. */
  constexpr std::optional<std::array<std::uint8_t, output_body_bytes>>
  encode(const output_t &o) {
    if (!valid(o)) return std::nullopt;
    std::array<std::uint8_t, output_body_bytes> body {};
    body[0] = version;
    detail::write16(body.data() + 2, o.controller);
    detail::write32(body.data() + 4, o.valid_mask);
    body[8] = (o.valid_mask & out_player_leds) ? o.player_led_mask : 0;
    body[9] = (o.valid_mask & out_mute_led) ? o.mute_led : 0;
    body[10] = (o.valid_mask & out_speaker_volume) ? o.speaker_volume : 0;
    body[11] = (o.valid_mask & out_headphone_volume) ? o.headphone_volume : 0;
    body[12] = (o.valid_mask & out_mic_volume) ? o.mic_volume : 0;
    body[13] = (o.valid_mask & out_mic_mute) ? o.mic_mute : 0;
    body[14] = (o.valid_mask & out_audio_routing) ? o.audio_routing : 0;
    body[15] = (o.valid_mask & out_lightbar_brightness) ? o.lightbar_brightness : 0;
    return body;
  }

  /** @brief Decode the body of 0x5515; the whole body is rejected on any violation. */
  constexpr std::optional<output_t>
  decode_output(const std::uint8_t *body, std::size_t length) {
    if (body == nullptr || length != output_body_bytes || body[0] != version || body[1] != 0) {
      return std::nullopt;
    }
    output_t o;
    o.controller = detail::read16(body + 2);
    o.valid_mask = detail::read32(body + 4);
    o.player_led_mask = body[8];
    o.mute_led = body[9];
    o.speaker_volume = body[10];
    o.headphone_volume = body[11];
    o.mic_volume = body[12];
    o.mic_mute = body[13];
    o.audio_routing = body[14];
    o.lightbar_brightness = body[15];
    if (!(o.valid_mask & out_player_leds) && o.player_led_mask != 0) return std::nullopt;
    if (!(o.valid_mask & out_mute_led) && o.mute_led != 0) return std::nullopt;
    if (!(o.valid_mask & out_speaker_volume) && o.speaker_volume != 0) return std::nullopt;
    if (!(o.valid_mask & out_headphone_volume) && o.headphone_volume != 0) return std::nullopt;
    if (!(o.valid_mask & out_mic_volume) && o.mic_volume != 0) return std::nullopt;
    if (!(o.valid_mask & out_mic_mute) && o.mic_mute != 0) return std::nullopt;
    if (!(o.valid_mask & out_audio_routing) && o.audio_routing != 0) return std::nullopt;
    if (!(o.valid_mask & out_lightbar_brightness) && o.lightbar_brightness != 0) return std::nullopt;
    if (!valid(o)) return std::nullopt;
    return o;
  }

  constexpr bool
  valid(const input_status_t &s) {
    if ((s.valid_mask & ~in_known) != 0) return false;
    return s.headset_present <= 1 && s.microphone_present <= 1 && s.microphone_muted <= 1;
  }

  /** @brief Encode the 12-byte body of 0x5516. */
  constexpr std::optional<std::array<std::uint8_t, input_status_body_bytes>>
  encode(const input_status_t &s) {
    if (!valid(s)) return std::nullopt;
    std::array<std::uint8_t, input_status_body_bytes> body {};
    body[0] = version;
    detail::write16(body.data() + 2, s.controller);
    detail::write32(body.data() + 4, s.valid_mask);
    body[8] = (s.valid_mask & in_headset) ? s.headset_present : 0;
    body[9] = (s.valid_mask & in_microphone) ? s.microphone_present : 0;
    body[10] = (s.valid_mask & in_mic_muted) ? s.microphone_muted : 0;
    return body;
  }

  /** @brief Decode the body of 0x5516; the whole body is rejected on any violation. */
  constexpr std::optional<input_status_t>
  decode_input_status(const std::uint8_t *body, std::size_t length) {
    if (body == nullptr || length != input_status_body_bytes || body[0] != version || body[1] != 0 ||
        body[11] != 0) {
      return std::nullopt;
    }
    input_status_t s;
    s.controller = detail::read16(body + 2);
    s.valid_mask = detail::read32(body + 4);
    s.headset_present = body[8];
    s.microphone_present = body[9];
    s.microphone_muted = body[10];
    if (!(s.valid_mask & in_headset) && s.headset_present != 0) return std::nullopt;
    if (!(s.valid_mask & in_microphone) && s.microphone_present != 0) return std::nullopt;
    if (!(s.valid_mask & in_mic_muted) && s.microphone_muted != 0) return std::nullopt;
    if (!valid(s)) return std::nullopt;
    return s;
  }

  /**
   * @brief Keep only the fields of @p current that differ from what was already sent, and record
   *        them in @p sent.
   * @param sent Values already sent for the controller; updated with every field returned.
   * @param current The fields a new output report sets.
   * @return The changed fields with the others cleared (an empty valid mask when nothing changed).
   *
   * Games rewrite the whole output report with the same enable flags on every frame; only changes
   * need to cross the network.
   */
  inline output_t
  take_changes(output_t &sent, const output_t &current) {
    output_t delta;
    delta.controller = current.controller;
    sent.controller = current.controller;
    const auto take = [&](std::uint32_t bit, std::uint8_t output_t::*field) {
      if (!(current.valid_mask & bit)) return;
      if ((sent.valid_mask & bit) && sent.*field == current.*field) return;
      delta.valid_mask |= bit;
      delta.*field = current.*field;
      sent.valid_mask |= bit;
      sent.*field = current.*field;
    };
    take(out_player_leds, &output_t::player_led_mask);
    take(out_mute_led, &output_t::mute_led);
    take(out_speaker_volume, &output_t::speaker_volume);
    take(out_headphone_volume, &output_t::headphone_volume);
    take(out_mic_volume, &output_t::mic_volume);
    take(out_mic_mute, &output_t::mic_mute);
    take(out_audio_routing, &output_t::audio_routing);
    take(out_lightbar_brightness, &output_t::lightbar_brightness);
    return delta;
  }

  /**
   * @brief Keep a pad's player LEDs from being blanked by a report that never asked for it.
   *
   * A game that fills every enable bit of a default report sets the player-LED bit with a zero
   * mask. That is not a request for dark LEDs: the pad keeps the number it was given when it
   * connected. A zero mask passes only after the game itself sent a non-zero one, as an explicit
   * clear. @p sent holds what was already forwarded for this pad.
   */
  inline void
  drop_unset_player_leds(output_t &current, const output_t &sent) {
    if ((current.valid_mask & out_player_leds) == 0 || current.player_led_mask != 0) return;
    const bool game_set_leds = (sent.valid_mask & out_player_leds) != 0 && sent.player_led_mask != 0;
    if (game_set_leds) return;
    current.valid_mask &= ~out_player_leds;
    current.player_led_mask = 0;
  }

  /** @brief Fold the fields @p source sets into @p target: the newest value of each field wins. */
  inline void
  merge_fields(output_t &target, const output_t &source) {
    target.controller = source.controller;
    const auto take = [&](std::uint32_t bit, std::uint8_t output_t::*field) {
      if (!(source.valid_mask & bit)) return;
      target.valid_mask |= bit;
      target.*field = source.*field;
    };
    take(out_player_leds, &output_t::player_led_mask);
    take(out_mute_led, &output_t::mute_led);
    take(out_speaker_volume, &output_t::speaker_volume);
    take(out_headphone_volume, &output_t::headphone_volume);
    take(out_mic_volume, &output_t::mic_volume);
    take(out_mic_mute, &output_t::mic_mute);
    take(out_audio_routing, &output_t::audio_routing);
    take(out_lightbar_brightness, &output_t::lightbar_brightness);
  }

  /** @brief Output report valid flags (Linux hid-playstation DS_OUTPUT_VALID_FLAG0/1/2). */
  namespace report_flags {
    constexpr std::uint8_t flag0_headphone_volume = 0x10;
    constexpr std::uint8_t flag0_speaker_volume = 0x20;
    constexpr std::uint8_t flag0_mic_volume = 0x40;
    constexpr std::uint8_t flag0_audio_control = 0x80;
    constexpr std::uint8_t flag1_mute_led = 0x01;
    constexpr std::uint8_t flag1_mic_mute = 0x02;
    constexpr std::uint8_t flag1_player_leds = 0x10;
    constexpr std::uint8_t flag2_light_brightness = 0x01;
  }  // namespace report_flags

  /**
   * @brief Project a 48-byte DualSense output report body (report id stripped: byte 0 is the first
   *        valid flag) onto the extension fields the report sets.
   * @param body The output report starting at valid_flag0 (47 bytes).
   * @param controller The wire controller number.
   * @return The fields set by the report. The valid mask is empty when the report sets none.
   *
   * The report offsets are the ones of the USB output report with the report id removed: headset
   * volume 4, speaker volume 5, microphone volume 6, audio routing 7, mute LED 8, power save and
   * microphone mute 9, lightbar brightness 42, player LEDs 43.
   */
  inline output_t
  from_output_report(const std::uint8_t *body, std::uint16_t controller) {
    using namespace report_flags;
    output_t o;
    o.controller = controller;
    const auto flag0 = body[0];
    const auto flag1 = body[1];
    const auto flag2 = body[38];
    if (flag1 & flag1_player_leds) {
      o.valid_mask |= out_player_leds;
      o.player_led_mask = body[43] & 0x1f;
    }
    if (flag1 & flag1_mute_led) {
      o.valid_mask |= out_mute_led;
      o.mute_led = body[8] ? 1 : 0;
    }
    if (flag0 & flag0_speaker_volume) {
      o.valid_mask |= out_speaker_volume;
      o.speaker_volume = body[5];
    }
    if (flag0 & flag0_headphone_volume) {
      o.valid_mask |= out_headphone_volume;
      o.headphone_volume = body[4];
    }
    if (flag0 & flag0_mic_volume) {
      o.valid_mask |= out_mic_volume;
      o.mic_volume = body[6];
    }
    if (flag1 & flag1_mic_mute) {
      o.valid_mask |= out_mic_mute;
      o.mic_mute = (body[9] & 0x10) ? 1 : 0;
    }
    if (flag0 & flag0_audio_control) {
      o.valid_mask |= out_audio_routing;
      o.audio_routing = body[7];
    }
    if (flag2 & flag2_light_brightness) {
      o.valid_mask |= out_lightbar_brightness;
      o.lightbar_brightness = body[42];
    }
    return o;
  }
}  // namespace ds5::control_extension
