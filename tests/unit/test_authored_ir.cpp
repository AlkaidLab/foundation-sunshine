#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "src/haptics/authored_ir.h"

extern "C" {
#include "third-party/moonlight-common-c/src/Ds5HapticsIrStream.h"
}

namespace {
  struct parsed_frame_t {
    bool called = false;
    LI_DS5_HAPTICS_IR_FRAME_V2 frame {};
  };

  void
  capture_frame(const LI_DS5_HAPTICS_IR_FRAME_V2 *frame, void *context) {
    auto &parsed = *static_cast<parsed_frame_t *>(context);
    parsed.called = true;
    parsed.frame = *frame;
  }

  std::vector<std::uint8_t>
  sine_pcm(double frequency_hz, double amplitude) {
    std::vector<std::int16_t> pcm(240 * 2);
    for (std::size_t frame = 0; frame < 240; ++frame) {
      const auto sample = static_cast<std::int16_t>(
        std::sin(static_cast<double>(frame) * frequency_hz * 6.283185307 / 48000.0) * amplitude);
      pcm[frame * 2] = sample;
      pcm[frame * 2 + 1] = sample;
    }
    std::vector<std::uint8_t> bytes(pcm.size() * sizeof(std::int16_t));
    std::memcpy(bytes.data(), pcm.data(), bytes.size());
    return bytes;
  }
}

TEST(AuthoredDualSenseIr, SunshineOutputParsesInMoonlightCommon) {
  haptics::authored_ir_session_t session;
  ASSERT_TRUE(session.ready());

  std::vector<std::int16_t> pcm(240 * 2);
  for (std::size_t frame = 0; frame < 240; ++frame) {
    const auto phase = static_cast<double>(frame) / 240.0;
    pcm[frame * 2] = static_cast<std::int16_t>(std::sin(phase * 6.283185307) * 18000.0);
    pcm[frame * 2 + 1] = frame < 24 ? 24000 : 0;
  }
  std::vector<std::uint8_t> bytes(pcm.size() * sizeof(std::int16_t));
  std::memcpy(bytes.data(), pcm.data(), bytes.size());

  const auto wire = session.process(3, 0x01 | 0x04, 240, 0x11223344,
                                    UINT64_C(1234567), bytes);
  ASSERT_TRUE(wire.has_value());
  ASSERT_EQ(wire->size(), DS5_HAPTICS_IR_STREAM_WIRE_SIZE);

  parsed_frame_t parsed;
  ASSERT_TRUE(processDs5HapticsIrStreamPacket(
    wire->data(), static_cast<int>(wire->size()), capture_frame, &parsed));
  ASSERT_TRUE(parsed.called);
  EXPECT_EQ(parsed.frame.controllerNumber, 3);
  EXPECT_EQ(parsed.frame.sourceSequenceNumber, UINT32_C(0x11223344));
  EXPECT_EQ(parsed.frame.sourceFrameCount, 240);
  EXPECT_NE(parsed.frame.flags & LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY, 0);
  EXPECT_GT(parsed.frame.lanes[0].rmsAmplitude, 0.0f);
  EXPECT_GT(parsed.frame.lanes[1].peakAmplitude, 0.0f);
  EXPECT_GE(parsed.frame.laneCorrelation, -1.0f);
  EXPECT_LE(parsed.frame.laneCorrelation, 1.0f);
}

TEST(AuthoredDualSenseIr, RejectsInvalidPcmSize) {
  haptics::authored_ir_session_t session;
  ASSERT_TRUE(session.ready());
  const std::array<std::uint8_t, 4> undersized_pcm {};
  EXPECT_FALSE(session.process(0, 0, 240, 1, 0, undersized_pcm).has_value());
}

TEST(AuthoredDualSenseIr, EmitsStreamEndFrame) {
  haptics::authored_ir_session_t session;
  ASSERT_TRUE(session.ready());
  const std::span<const std::uint8_t> empty_pcm;
  const auto wire = session.process(0, 0x02, 0, 9, 9000, empty_pcm);
  ASSERT_TRUE(wire.has_value());

  parsed_frame_t parsed;
  ASSERT_TRUE(processDs5HapticsIrStreamPacket(
    wire->data(), static_cast<int>(wire->size()), capture_frame, &parsed));
  EXPECT_NE(parsed.frame.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END, 0);
  EXPECT_NE(parsed.frame.flags & LI_DS5_HAPTICS_IR_FLAG_SILENT, 0);
}

TEST(AuthoredDualSenseIr, LegacyFallbackProducesAndStopsRumble) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto pcm = sine_pcm(100.0, 24000.0);

  const auto first = session.process(2, 0x01, 240, 1, 0, pcm, start);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->controller_id, 2);
  EXPECT_GT(first->low_frequency, 0);

  // Source PCM arrives every 5 ms, but legacy rumble is deliberately limited
  // to 50 Hz to avoid flooding clients and platform vibration APIs.
  EXPECT_FALSE(session.process(2, 0, 240, 2, 5000, pcm, start + 5ms).has_value());

  // Once the 20 ms emit period has elapsed a louder chunk must come through,
  // proving the suppression above was the rate limit.
  const auto louder = sine_pcm(100.0, 32000.0);
  const auto updated = session.process(2, 0, 240, 3, 25000, louder, start + 25ms);
  ASSERT_TRUE(updated.has_value());
  EXPECT_GT(updated->low_frequency, first->low_frequency);

  const std::span<const std::uint8_t> empty_pcm;
  const auto stopped = session.process(2, 0x02, 0, 4, 30000, empty_pcm, start + 30ms);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->low_frequency, 0);
  EXPECT_EQ(stopped->high_frequency, 0);

  const std::vector<std::uint8_t> silence(240 * 4);
  const auto restarted_silent = session.process(2, 0x01, 240, 5, 35000, silence, start + 35ms);
  ASSERT_TRUE(restarted_silent.has_value());
  EXPECT_EQ(restarted_silent->low_frequency, 0);
  EXPECT_EQ(restarted_silent->high_frequency, 0);

  // Past the emit period again, held silence stays quiet instead of
  // re-sending zero rumble at 50 Hz.
  EXPECT_FALSE(session.process(2, 0, 240, 6, 60000, silence, start + 60ms).has_value());
}

TEST(AuthoredDualSenseIr, LegacyFallbackWatchdogReleasesMotors) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto pcm = sine_pcm(1000.0, 24000.0);

  const auto first = session.process(1, 0x01, 240, 1, 0, pcm, start);
  ASSERT_TRUE(first.has_value());
  EXPECT_GT(first->high_frequency, 0);
  EXPECT_FALSE(session.poll(start + 99ms).has_value());

  const auto stopped = session.poll(start + 100ms);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->controller_id, 1);
  EXPECT_EQ(stopped->low_frequency, 0);
  EXPECT_EQ(stopped->high_frequency, 0);
  EXPECT_FALSE(session.poll(start + 200ms).has_value());
}
