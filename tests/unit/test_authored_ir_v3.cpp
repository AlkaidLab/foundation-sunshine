// SPDX-License-Identifier: GPL-3.0-only
#include "src/haptics/authored_ir_v3.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <map>
#include <vector>
extern "C" {
#include "third-party/moonlight-common-c/src/Ds5HapticsIrStream.h"
}

namespace {
  using frame_t = LI_DS5_HAPTICS_IR_FRAME_V3;
  using event_t = LI_DS5_HAPTICS_IR_EVENT_V3;
  void
  capture_v3(const frame_t *frame, void *context) { *static_cast<frame_t *>(context) = *frame; }
  frame_t
  parsed(const std::optional<std::vector<std::uint8_t>> &wire) {
    frame_t frame {};
    EXPECT_TRUE(wire.has_value());
    if (wire) EXPECT_TRUE(processDs5HapticsIrV3StreamPacket(wire->data(), static_cast<int>(wire->size()), capture_v3, &frame));
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
  struct playback_t {
    std::map<std::uint32_t, event_t> events;
    double body = 0;
    unsigned body_frames = 0;
  };
  playback_t
  analyze(const std::vector<std::uint8_t> &pcm, unsigned chunk) {
    haptics::authored_ir_v3_session_t session;
    playback_t result;
    std::uint32_t sequence = 0;
    for (unsigned start = 0; start < pcm.size() / 4; start += chunk) {
      const auto count = std::min<unsigned>(chunk, static_cast<unsigned>(pcm.size() / 4) - start);
      const auto frame = parsed(session.process(2, start ? 0 : 1, static_cast<std::uint16_t>(count), sequence++,
        std::uint64_t(start) * 1000000 / 48000, std::span(pcm).subspan(start * 4, count * 4)));
      EXPECT_EQ(frame.controllerNumber, 2);
      for (unsigned i = 0; i < frame.eventCount; ++i) {
        const auto &event = frame.events[i];
        const auto [it, inserted] = result.events.emplace(event.eventId, event);
        if (!inserted) {
          EXPECT_EQ(it->second.sourceTimeUs, event.sourceTimeUs);
          EXPECT_EQ(it->second.leftStrength, event.leftStrength);
          EXPECT_EQ(it->second.rightStrength, event.rightStrength);
        }
      }
      if (frame.windowEndUs >= 200000 && frame.windowEndUs <= 800000) {
        result.body += std::max(frame.lanes[0].continuousLow, frame.lanes[0].continuousHigh);
        ++result.body_frames;
      }
    }
    const auto end = parsed(session.process(2, 2, 0, sequence, pcm.size() / 4 * 1000000ULL / 48000, {}));
    EXPECT_NE(end.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END, 0);
    EXPECT_EQ(end.eventCount, 0);
    EXPECT_EQ(end.lanes[0].continuousLow, 0);
    EXPECT_EQ(end.lanes[1].continuousHigh, 0);
    return result;
  }
}  // namespace

TEST(AuthoredIrV3, LifecycleEpochsSequenceWrapAndGaps) {
  haptics::authored_ir_v3_session_t session;
  const auto silence = signal(.005);
  auto first = parsed(session.process(0, 1, 240, UINT32_MAX - 1, 1000000, silence));
  EXPECT_NE(first.flags & LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY, 0);
  auto second = parsed(session.process(0, 0, 240, UINT32_MAX, 1005000, silence));
  EXPECT_EQ(first.epoch, second.epoch);
  auto wrapped = parsed(session.process(0, 0, 240, 0, 1010000, silence));
  EXPECT_EQ(first.epoch, wrapped.epoch);
  EXPECT_FALSE(session.process(0, 0, 240, 0, 1010000, silence));
  EXPECT_FALSE(session.process(0, 0, 240, UINT32_MAX, 1005000, silence));
  auto gap = parsed(session.process(0, 0, 240, 3, 1025000, silence));
  EXPECT_NE(first.epoch, gap.epoch);
  EXPECT_NE(gap.flags & LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY, 0);
  auto end = parsed(session.process(0, 2, 0, 4, 1030000, {}));
  EXPECT_EQ(gap.epoch, end.epoch);
  EXPECT_NE(end.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END, 0);
  EXPECT_FALSE(session.process(0, 0, 240, 3, 1025000, silence));
  auto restarted = parsed(session.process(0, 1, 240, 0, 0, silence));
  EXPECT_NE(gap.epoch, restarted.epoch);
}

TEST(AuthoredIrV3, InvalidInputDoesNotAdvanceTheStream) {
  haptics::authored_ir_v3_session_t session;
  const auto silence = signal(.005);
  EXPECT_FALSE(session.process(0, 1, 241, 0, 0, silence));
  EXPECT_FALSE(session.process(0, 1, 240, 0, 0, std::span(silence).first(12)));
  EXPECT_FALSE(session.process(0, 0x80, 240, 0, 0, silence));
  EXPECT_FALSE(session.process(0, 0, 0, 0, 0, {}));
  EXPECT_EQ(parsed(session.process(0, 1, 240, 0, 0, silence)).frameSequence, 0);
}

TEST(AuthoredIrV3, SteadyCarriersRemainContinuousWithoutFalseOnsetTrains) {
  for (const auto frequency : { 85.0, 250.0, 300.0 }) {
    SCOPED_TRACE(frequency);
    const auto result = analyze(signal(1, frequency, .12), 240);
    EXPECT_GT(result.body / result.body_frames, .01);
    for (const auto &[id, event] : result.events) EXPECT_LT(event.sourceTimeUs, 200000u);
  }
}

TEST(AuthoredIrV3, DenseSourcePulsesRetainTheirIndividualOnsetsWithoutDuplicates) {
  for (const unsigned density : { 25u, 50u, 60u, 80u }) {
    SCOPED_TRACE(density);
    auto pcm = signal(2.8);
    for (unsigned i = 0; i < density * 2; ++i) add_pulse(pcm, .4 + i / double(density));
    const auto result = analyze(pcm, 240);
    ASSERT_EQ(result.events.size(), density * 2);
    unsigned index = 0;
    for (const auto &[id, event] : result.events) {
      const double source = .4 + index++ / double(density);
      EXPECT_GE(event.sourceTimeUs / 1e6, source - .005);
      EXPECT_LE(event.sourceTimeUs / 1e6, source + .020);
    }
  }
}

TEST(AuthoredIrV3, EventIdentityIsIndependentOfTransportFragmentation) {
  auto pcm = signal(.4);
  for (unsigned i = 0; i < 10; ++i) add_pulse(pcm, .05 + i * .02);
  const auto a = analyze(pcm, 240);
  const auto b = analyze(pcm, 137);
  ASSERT_EQ(a.events.size(), 10);
  ASSERT_EQ(a.events.size(), b.events.size());
  for (const auto &[id, event] : a.events) {
    const auto found = b.events.find(id);
    ASSERT_NE(found, b.events.end());
    EXPECT_EQ(event.sourceTimeUs, found->second.sourceTimeUs);
    EXPECT_FLOAT_EQ(event.leftStrength, found->second.leftStrength);
    EXPECT_FLOAT_EQ(event.rightStrength, found->second.rightStrength);
  }
}
