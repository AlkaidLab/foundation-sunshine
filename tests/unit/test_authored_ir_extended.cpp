// SPDX-License-Identifier: GPL-3.0-only
#include "src/haptics/authored_ir_extended.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>
extern "C" {
#include "third-party/moonlight-common-c/src/Ds5HapticsIrStream.h"
}

namespace {
  using frame_t = LI_DS5_HAPTICS_IR_EXTENDED_FRAME;
  void
  capture_extended(const frame_t *frame, void *context) { *static_cast<frame_t *>(context) = *frame; }
  frame_t
  parsed(const std::optional<std::vector<std::uint8_t>> &wire) {
    frame_t frame {};
    EXPECT_TRUE(wire.has_value());
    if (wire) EXPECT_TRUE(processDs5HapticsIrExtendedStreamPacket(wire->data(), static_cast<int>(wire->size()), capture_extended, &frame));
    return frame;
  }
  std::vector<std::uint8_t>
  signal(double seconds, double carrier = 0, double amplitude = 0) {
    const auto frames = static_cast<unsigned>(std::lround(seconds * 48000));
    std::vector<std::uint8_t> pcm(frames * 4);
    for (unsigned i = 0; i < frames; ++i) {
      const double t = i / 48000.0;
      const double ramp = std::clamp(std::min(t / .05, (seconds - t) / .05), 0.0, 1.0);
      const auto sample = static_cast<std::int16_t>(std::lround(amplitude * ramp * std::sin(t * carrier * 6.283185307179586) * 32767));
      for (unsigned lane = 0; lane < 2; ++lane) {
        pcm[i * 4 + lane * 2] = static_cast<std::uint8_t>(sample);
        pcm[i * 4 + lane * 2 + 1] = static_cast<std::uint8_t>(sample >> 8);
      }
    }
    return pcm;
  }
  void
  add_pulse(std::vector<std::uint8_t> &pcm, double at, double amplitude = .12) {
    const auto start = static_cast<unsigned>(std::lround(at * 48000));
    for (unsigned i = 0; i < 2400 && (start + i) * 4 < pcm.size(); ++i) {
      const double t = i / 48000.0;
      const double value = amplitude * (1 - std::exp(-t / .0007)) * std::exp(-t / .006) * std::sin(t * 270 * 6.283185307179586);
      for (unsigned lane = 0; lane < 2; ++lane) {
        const auto p = (start + i) * 4 + lane * 2;
        const auto original = static_cast<std::int16_t>(std::uint16_t(pcm[p]) | (std::uint16_t(pcm[p + 1]) << 8));
        const auto sample = static_cast<std::int16_t>(original + std::lround(value * 32767));
        pcm[p] = static_cast<std::uint8_t>(sample);
        pcm[p + 1] = static_cast<std::uint8_t>(sample >> 8);
      }
    }
  }
}  // namespace

TEST(AuthoredIrExtended, InvalidInputDoesNotAdvanceTheStream) {
  haptics::authored_ir_extended_session_t session;
  const auto silence = signal(.005);
  EXPECT_FALSE(session.process(0, 1, 241, 0, 0, silence));
  EXPECT_FALSE(session.process(0, 1, 240, 0, 0, std::span(silence).first(12)));
  EXPECT_FALSE(session.process(0, 0x80, 240, 0, 0, silence));
  EXPECT_FALSE(session.process(0, 0, 0, 0, 0, {}));
  EXPECT_EQ(parsed(session.process(0, 1, 240, 0, 0, silence)).base.sourceSequenceNumber, 0);
}

TEST(AuthoredIrExtended, EncodesEnhancementAndPreservesTheLegacyV2Base) {
  for (const unsigned chunk : { 240u, 137u }) {
    haptics::authored_ir_session_t legacy;
    haptics::authored_ir_extended_session_t enhanced;
    auto pcm = signal(.35, 85, .12);
    add_pulse(pcm, .1);
    std::uint32_t sequence = 0;
    bool saw_body = false, saw_event = false;
    for (unsigned start = 0; start < pcm.size() / 4; start += chunk) {
      const auto count = static_cast<std::uint16_t>(std::min<unsigned>(chunk, pcm.size() / 4 - start));
      const auto input = std::span(pcm).subspan(start * 4, count * 4);
      const auto time = std::uint64_t(start) * 1000000 / 48000;
      const auto base = legacy.process(2, start ? 0 : 1, count, sequence, time, input);
      const auto packet = enhanced.process(2, start ? 0 : 1, count, sequence++, time, input);
      ASSERT_EQ(base.has_value(), packet.has_value());
      if (base) {
        ASSERT_TRUE(std::equal(base->begin(), base->end(), packet->begin()));
        const auto frame = parsed(packet);
        EXPECT_EQ(frame.base.controllerNumber, 2);
        saw_body |= frame.lanes[0].continuousLow > .01f;
        saw_event |= frame.eventCount > 0;
      }
    }
    const auto end = parsed(enhanced.process(2, 2, 0, sequence, pcm.size() / 4 * 1000000ULL / 48000, {}));
    EXPECT_NE(end.base.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END, 0);
    EXPECT_EQ(end.eventCount, 0);
    EXPECT_EQ(end.lanes[0].continuousLow, 0);
    EXPECT_EQ(end.lanes[1].continuousHigh, 0);
    EXPECT_TRUE(saw_body);
    EXPECT_TRUE(saw_event);
  }
}

TEST(AuthoredIrExtended, SmoothAttacksKeepTheirBodyWithoutBecomingPointEvents) {
  for (const auto carrier : { 80., 250., 300. }) {
    haptics::authored_ir_extended_session_t session;
    const auto pcm = signal(.35, carrier, .12);
    bool saw_body = false, saw_base_attack = false;
    for (unsigned start = 0; start < pcm.size() / 4; start += 240) {
      const auto frame = parsed(session.process(0, start ? 0 : 1, 240, start / 240,
        std::uint64_t(start) * 1000000 / 48000, std::span(pcm).subspan(start * 4, 960)));
      EXPECT_EQ(frame.eventCount, 0);
      saw_body |= std::max(frame.lanes[0].continuousLow, frame.lanes[0].continuousHigh) > .01f;
      saw_base_attack |= frame.base.lanes[0].transientStrength > 0;
    }
    EXPECT_TRUE(saw_body);
    EXPECT_TRUE(saw_base_attack);
  }
}
