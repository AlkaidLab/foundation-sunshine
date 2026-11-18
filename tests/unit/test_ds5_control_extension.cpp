/**
 * @file tests/unit/test_ds5_control_extension.cpp
 * @brief DualSense control extensions 0x5515 / 0x5516: wire form, negotiation and old-peer behaviour.
 */
#include "src/ds5/control_extension.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {
  namespace ext = ds5::control_extension;

  TEST(Ds5ControlExtensionSdp, OfferAndAnswerUseTheSameNamespace) {
    EXPECT_EQ(ext::parse_sdp_value(ext::offer), ext::mask_output_v1 | ext::mask_input_status_v1);
    EXPECT_EQ(ext::client_attribute, "x-ml-general.ds5Extensions");
    EXPECT_EQ(ext::host_attribute, "x-ss-general.ds5Extensions");
    // A client that can play the pad's speaker offers pad-audio; the host's answer names every token it knows.
    EXPECT_EQ(ext::parse_sdp_value(ext::offer_with_pad_audio), ext::mask_output_v1 | ext::mask_input_status_v1 | ext::mask_pad_audio_v1);
    EXPECT_EQ(ext::parse_sdp_value(ext::host_answer), ext::mask_output_v1 | ext::mask_input_status_v1 | ext::mask_pad_audio_v1);
    EXPECT_FALSE(ext::accepts_pad_audio(ext::parse_sdp_value(ext::offer)));
    EXPECT_TRUE(ext::accepts_pad_audio(ext::parse_sdp_value(ext::offer_with_pad_audio)));
    EXPECT_EQ(ext::parse_sdp_value("pad-audio=2"), 0u);
    EXPECT_EQ(ext::parse_sdp_value("output=1"), ext::mask_output_v1);
    EXPECT_EQ(ext::parse_sdp_value("input-status=1"), ext::mask_input_status_v1);
    EXPECT_EQ(ext::parse_sdp_value("output=1;input-status=1"), 3u);
    EXPECT_EQ(ext::parse_sdp_value("  output=1 ,  input-status=1  "), 3u);
  }

  TEST(Ds5ControlExtensionSdp, UnknownOrMalformedTokensNeverProduceABit) {
    for (const std::string_view value : { "", "output=2", "output=10", "output=", "output", "=1", "outputs=1",
           "output=1x", "Output=1", "input-status=0", ",,;;" }) {
      EXPECT_EQ(ext::parse_sdp_value(value), 0u) << value;
    }
    EXPECT_EQ(ext::parse_sdp_value("future=1,output=1"), ext::mask_output_v1);
    EXPECT_EQ(ext::parse_sdp_value("output=1,,;;input-status=0"), ext::mask_output_v1);
  }

  TEST(Ds5ControlExtensionNegotiation, OldPeerSeesNothing) {
    // A client that does not know the attribute leaves the mask empty: nothing is sent to it and
    // nothing it sends is accepted.
    constexpr std::uint32_t old_client = 0;
    EXPECT_FALSE(ext::accepts_output(old_client));
    EXPECT_FALSE(ext::accepts_input_status(old_client));
    // Each direction is gated on its own token.
    EXPECT_TRUE(ext::accepts_output(ext::mask_output_v1));
    EXPECT_FALSE(ext::accepts_input_status(ext::mask_output_v1));
    EXPECT_TRUE(ext::accepts_input_status(ext::mask_input_status_v1));
    EXPECT_FALSE(ext::accepts_output(ext::mask_input_status_v1));
  }

  TEST(Ds5ControlExtensionOutput, GoldenBytes) {
    // version 1, reserved 0, controller 0x0302, valid 0xa5, then the eight value bytes.
    constexpr std::array<std::uint8_t, 16> golden { 0x01, 0x00, 0x02, 0x03, 0xa5, 0x00, 0x00, 0x00,
      0x1b, 0x00, 0x7f, 0x00, 0x00, 0x01, 0x00, 0x02 };
    ext::output_t source;
    source.controller = 0x0302;
    source.valid_mask = 0xa5;
    source.player_led_mask = 0x1b;
    source.speaker_volume = 0x7f;
    source.mic_mute = 1;
    source.lightbar_brightness = 2;
    const auto encoded = ext::encode(source);
    ASSERT_TRUE(encoded.has_value());
    EXPECT_EQ(*encoded, golden);
    const auto decoded = ext::decode_output(golden.data(), golden.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, source);
  }

  TEST(Ds5ControlExtensionOutput, EveryValidBitRoundTripsAlone) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      ext::output_t in;
      in.controller = static_cast<std::uint16_t>(bit + 1);
      in.valid_mask = 1u << bit;
      in.player_led_mask = bit == 0 ? 0x15 : 0;
      in.mute_led = bit == 1 ? 1 : 0;
      in.speaker_volume = bit == 2 ? 0xff : 0;
      in.headphone_volume = bit == 3 ? 0x40 : 0;
      in.mic_volume = bit == 4 ? 0x33 : 0;
      in.mic_mute = bit == 5 ? 1 : 0;
      in.audio_routing = bit == 6 ? 0x30 : 0;
      in.lightbar_brightness = bit == 7 ? 1 : 0;
      const auto body = ext::encode(in);
      ASSERT_TRUE(body.has_value()) << bit;
      const auto out = ext::decode_output(body->data(), body->size());
      ASSERT_TRUE(out.has_value()) << bit;
      EXPECT_EQ(*out, in) << bit;
    }
  }

  TEST(Ds5ControlExtensionOutput, WholeBodyRejections) {
    std::array<std::uint8_t, 16> good { 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00,
      0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    ASSERT_TRUE(ext::decode_output(good.data(), good.size()).has_value());
    EXPECT_FALSE(ext::decode_output(good.data(), 15).has_value());
    EXPECT_FALSE(ext::decode_output(nullptr, 16).has_value());
    std::vector<std::uint8_t> longer(good.begin(), good.end());
    longer.push_back(0);
    EXPECT_FALSE(ext::decode_output(longer.data(), longer.size()).has_value());

    const auto mutate = [&](std::size_t index, std::uint8_t value) {
      auto bad = good;
      bad[index] = value;
      return ext::decode_output(bad.data(), bad.size()).has_value();
    };
    EXPECT_FALSE(mutate(0, 2));  // version
    EXPECT_FALSE(mutate(1, 1));  // reserved
    EXPECT_FALSE(mutate(5, 1));  // unknown valid bit 8
    EXPECT_FALSE(mutate(7, 0x80));  // unknown valid bit 31
    EXPECT_FALSE(mutate(8, 0x20));  // player mask wider than five bits
    EXPECT_FALSE(mutate(9, 2));  // mute LED above 1
    EXPECT_FALSE(mutate(4, 0x00));  // values present without valid bits
    // A volume of zero under a set bit is a legal setting.
    EXPECT_TRUE(mutate(4, 0x07));
    // A non-zero value under a clear bit is rejected whole, for every value byte.
    for (std::size_t index = 10; index < 16; ++index) {
      auto bad = good;
      bad[index] = 1;
      EXPECT_FALSE(ext::decode_output(bad.data(), bad.size()).has_value()) << index;
    }
    auto bad_mic_mute = good;
    bad_mic_mute[4] = 0x23;
    bad_mic_mute[13] = 2;
    EXPECT_FALSE(ext::decode_output(bad_mic_mute.data(), bad_mic_mute.size()).has_value());
  }

  TEST(Ds5ControlExtensionOutput, EncodeRefusesOutOfRangeFields) {
    ext::output_t o;
    o.valid_mask = ext::out_player_leds;
    o.player_led_mask = 0x20;
    EXPECT_FALSE(ext::encode(o).has_value());
    o = {};
    o.valid_mask = 0x100;
    EXPECT_FALSE(ext::encode(o).has_value());
    o = {};
    o.valid_mask = ext::out_mic_mute;
    o.mic_mute = 2;
    EXPECT_FALSE(ext::encode(o).has_value());
  }

  TEST(Ds5ControlExtensionOutput, FieldsOutsideTheMaskAreWrittenAsZero) {
    ext::output_t o;
    o.valid_mask = ext::out_speaker_volume;
    o.speaker_volume = 0x44;
    o.headphone_volume = 0x55;  // not valid: must not reach the wire
    const auto body = ext::encode(o);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ((*body)[10], 0x44);
    EXPECT_EQ((*body)[11], 0);
  }

  TEST(Ds5ControlExtensionInputStatus, GoldenAndRoundTrip) {
    constexpr std::array<std::uint8_t, 12> golden { 0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00,
      0x01, 0x01, 0x00, 0x00 };
    ext::input_status_t in;
    in.controller = 3;
    in.valid_mask = ext::in_known;
    in.headset_present = 1;
    in.microphone_present = 1;
    in.microphone_muted = 0;
    const auto body = ext::encode(in);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, golden);
    const auto out = ext::decode_input_status(golden.data(), golden.size());
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, in);

    for (unsigned bit = 0; bit < 3; ++bit) {
      ext::input_status_t one;
      one.controller = 1;
      one.valid_mask = 1u << bit;
      one.headset_present = bit == 0;
      one.microphone_present = bit == 1;
      one.microphone_muted = bit == 2;
      const auto single = ext::encode(one);
      ASSERT_TRUE(single.has_value());
      const auto back = ext::decode_input_status(single->data(), single->size());
      ASSERT_TRUE(back.has_value());
      EXPECT_EQ(*back, one);
    }
  }

  TEST(Ds5ControlExtensionInputStatus, WholeBodyRejections) {
    constexpr std::array<std::uint8_t, 12> good { 0x01, 0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00,
      0x01, 0x01, 0x00, 0x00 };
    EXPECT_FALSE(ext::decode_input_status(good.data(), 11).has_value());
    const auto mutate = [&](std::size_t index, std::uint8_t value) {
      auto bad = good;
      bad[index] = value;
      return ext::decode_input_status(bad.data(), bad.size()).has_value();
    };
    EXPECT_FALSE(mutate(0, 2));
    EXPECT_FALSE(mutate(1, 1));
    EXPECT_FALSE(mutate(11, 1));  // second reserved byte
    EXPECT_FALSE(mutate(4, 0x08));  // unknown valid bit
    EXPECT_FALSE(mutate(4, 0x06));  // headset set while its bit is clear
    EXPECT_FALSE(mutate(8, 2));  // value above 1
    ext::input_status_t unknown;
    unknown.valid_mask = 0x08;
    EXPECT_FALSE(ext::encode(unknown).has_value());
  }

  TEST(Ds5ControlExtensionReport, FlagsAndOffsetsFollowTheDeviceReport) {
    // 47-byte output report body (report id removed): flag0, flag1, ... flag2 at 38.
    std::array<std::uint8_t, 47> body {};
    body[0] = 0xf0;  // headset, speaker, microphone volume and audio control enabled
    body[1] = 0x13;  // mute LED, microphone mute, player LEDs
    body[38] = 0x01;  // lightbar brightness
    body[4] = 0x20;  // headset volume
    body[5] = 0x64;  // speaker volume
    body[6] = 0x80;  // microphone volume
    body[7] = 0x30;  // audio routing
    body[8] = 1;  // mute LED
    body[9] = 0x10;  // microphone mute
    body[42] = 2;  // lightbar brightness
    body[43] = 0x15;  // player LEDs
    const auto fields = ext::from_output_report(body.data(), 4);
    EXPECT_EQ(fields.controller, 4);
    EXPECT_EQ(fields.valid_mask, ext::out_known);
    EXPECT_EQ(fields.headphone_volume, 0x20);
    EXPECT_EQ(fields.speaker_volume, 0x64);
    EXPECT_EQ(fields.mic_volume, 0x80);
    EXPECT_EQ(fields.audio_routing, 0x30);
    EXPECT_EQ(fields.mute_led, 1);
    EXPECT_EQ(fields.mic_mute, 1);
    EXPECT_EQ(fields.lightbar_brightness, 2);
    EXPECT_EQ(fields.player_led_mask, 0x15);
    EXPECT_TRUE(ext::encode(fields).has_value());
  }

  TEST(Ds5ControlExtensionReport, DisabledFieldsAreNotCarried) {
    std::array<std::uint8_t, 47> body {};
    body[5] = 0x64;
    body[43] = 0x1f;
    body[8] = 1;
    EXPECT_EQ(ext::from_output_report(body.data(), 0).valid_mask, 0u);
    body[0] = 0x20;  // speaker volume only
    const auto fields = ext::from_output_report(body.data(), 0);
    EXPECT_EQ(fields.valid_mask, ext::out_speaker_volume);
    EXPECT_EQ(fields.speaker_volume, 0x64);
    EXPECT_EQ(fields.player_led_mask, 0);
    // The player LED field is five bits wide.
    body[1] = 0x10;
    body[43] = 0xff;
    EXPECT_EQ(ext::from_output_report(body.data(), 0).player_led_mask, 0x1f);
  }

  TEST(Ds5ControlExtensionReport, OnlyChangesAreSent) {
    ext::output_t sent;
    ext::output_t current;
    current.controller = 2;
    current.valid_mask = ext::out_speaker_volume | ext::out_player_leds;
    current.speaker_volume = 0x40;
    current.player_led_mask = 0x04;
    auto first = ext::take_changes(sent, current);
    EXPECT_EQ(first.valid_mask, current.valid_mask);
    // The same report again changes nothing.
    EXPECT_EQ(ext::take_changes(sent, current).valid_mask, 0u);
    // One field changes: only that field goes out, the other stays quiet.
    current.speaker_volume = 0x41;
    const auto second = ext::take_changes(sent, current);
    EXPECT_EQ(second.valid_mask, ext::out_speaker_volume);
    EXPECT_EQ(second.speaker_volume, 0x41);
    EXPECT_EQ(second.player_led_mask, 0);
    // A field that becomes enabled with the value it had before still has to be sent once.
    current.valid_mask = ext::out_mic_mute;
    current.mic_mute = 0;
    EXPECT_EQ(ext::take_changes(sent, current).valid_mask, ext::out_mic_mute);
    EXPECT_EQ(ext::take_changes(sent, current).valid_mask, 0u);
  }

  /* A game that fills every enable bit of a default report sets the player-LED bit with a zero mask.
   * That is not a request for dark LEDs: the number the pad got at connect stays. */
  TEST(Ds5ControlExtensionReport, AMaskTheGameNeverSetDoesNotBlankThePlayerLeds) {
    std::array<std::uint8_t, 47> body {};
    body[1] = 0xff;  // every flag1 enable bit, all values zero
    ext::output_t sent;
    auto current = ext::from_output_report(body.data(), 1);
    ASSERT_NE(current.valid_mask & ext::out_player_leds, 0u);
    ext::drop_unset_player_leds(current, sent);
    EXPECT_EQ(current.valid_mask & ext::out_player_leds, 0u);
    EXPECT_EQ(ext::take_changes(sent, current).valid_mask & ext::out_player_leds, 0u);
    // A mask the game really sets goes through and is remembered.
    body[43] = 0x04;
    current = ext::from_output_report(body.data(), 1);
    ext::drop_unset_player_leds(current, sent);
    EXPECT_NE(ext::take_changes(sent, current).valid_mask & ext::out_player_leds, 0u);
    // After that a zero is the game's own clear and passes.
    body[43] = 0x00;
    current = ext::from_output_report(body.data(), 1);
    ext::drop_unset_player_leds(current, sent);
    const auto clear = ext::take_changes(sent, current);
    EXPECT_NE(clear.valid_mask & ext::out_player_leds, 0u);
    EXPECT_EQ(clear.player_led_mask, 0);
    // The same clear again is not a change.
    current = ext::from_output_report(body.data(), 1);
    ext::drop_unset_player_leds(current, sent);
    EXPECT_EQ(ext::take_changes(sent, current).valid_mask & ext::out_player_leds, 0u);
  }
}  // namespace
