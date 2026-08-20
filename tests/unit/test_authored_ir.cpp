#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
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
  sine_pcm(double frequency_hz, double amplitude, std::size_t first_frame = 0) {
    std::vector<std::int16_t> pcm(240 * 2);
    for (std::size_t frame = 0; frame < 240; ++frame) {
      const auto sample = static_cast<std::int16_t>(
        std::sin(static_cast<double>(first_frame + frame) * frequency_hz * 6.283185307 / 48000.0) * amplitude);
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

TEST(AuthoredDualSenseIr, LegacyFallbackLiftsLowLevelContentAboveMotorFloor) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};

  std::optional<haptics::legacy_rumble_t> latest;
  for (std::uint32_t chunk = 0; chunk <= 8; ++chunk) {
    const auto pcm = sine_pcm(120.0, 1400.0, chunk * 240U);
    const auto output = session.process(
      0, chunk == 0 ? 0x01 : 0, 240, chunk,
      static_cast<std::uint64_t>(chunk) * 5000U, pcm,
      start + std::chrono::milliseconds(chunk * 5U));
    if (output) latest = output;
  }

  ASSERT_TRUE(latest.has_value());
  // Client renderers typically keep only the high byte. Preserve enough
  // amplitude to clear real motor startup thresholds after that quantization.
  EXPECT_GE(latest->low_frequency, 0x1800u);
}

TEST(AuthoredDualSenseIr, LegacyFallbackHoldsShortPulseBeforeStop) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto pcm = sine_pcm(120.0, 24000.0);
  const auto first = session.process(0, 0x01, 240, 1, 0, pcm, start);
  ASSERT_TRUE(first.has_value());
  ASSERT_GT(first->low_frequency, 0);

  const std::vector<std::uint8_t> silence(240 * 4);
  std::optional<haptics::legacy_rumble_t> held;
  for (std::uint32_t chunk = 1; chunk <= 5; ++chunk) {
    const auto output = session.process(
      0, 0, 240, chunk, chunk * 5000, silence,
      start + std::chrono::milliseconds(chunk * 5));
    if (output) held = output;
  }
  ASSERT_TRUE(held.has_value());
  // The hold preserves a nonzero dispatch, while release smoothing may
  // legitimately update its level between packets.
  EXPECT_GT(held->low_frequency, 0);
  EXPECT_GT(held->high_frequency, 0);

  const auto stopped = session.poll(start + 125ms);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->low_frequency, 0);
  EXPECT_EQ(stopped->high_frequency, 0);
}

TEST(AuthoredDualSenseIr, LegacyFallbackClearsFloorAfterRelease) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto burst = sine_pcm(120.0, 32000.0);
  ASSERT_TRUE(session.process(0, 0x01, 240, 0, 0, burst, start).has_value());

  const std::vector<std::uint8_t> silence(240 * 4);
  std::optional<haptics::legacy_rumble_t> at_release_start;
  std::optional<haptics::legacy_rumble_t> at_hold_expiry;
  std::optional<haptics::legacy_rumble_t> latest;
  for (std::uint32_t chunk = 1; chunk <= 80; ++chunk) {
    const auto output = session.process(
      0, 0, 240, chunk, chunk * 5000, silence,
      start + std::chrono::milliseconds(chunk * 5));
    if (output) latest = output;
    if (chunk == 4) at_release_start = latest;
    if (chunk == 16) at_hold_expiry = output;
  }
  ASSERT_TRUE(at_release_start.has_value());
  EXPECT_GE(at_release_start->low_frequency, 0x07AEu);
  ASSERT_TRUE(at_hold_expiry.has_value());
  EXPECT_EQ(at_hold_expiry->low_frequency, 0u);
  EXPECT_EQ(at_hold_expiry->high_frequency, 0u);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->low_frequency, 0u);
  EXPECT_EQ(latest->high_frequency, 0u);
}

TEST(AuthoredDualSenseIr, LegacyFallbackKeepsReleaseTailForLongEffect) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto burst = sine_pcm(120.0, 32000.0);
  const std::vector<std::uint8_t> silence(240 * 4);

  // Keep the authored signal active beyond the short-pulse hold window.
  bool saw_output = false;
  for (std::uint32_t chunk = 0; chunk <= 20; ++chunk) {
    const auto output = session.process(
      0, chunk == 0 ? 0x01 : 0, 240, chunk, chunk * 5000,
      burst, start + std::chrono::milliseconds(chunk * 5));
    saw_output = saw_output || output.has_value();
  }
  ASSERT_TRUE(saw_output);

  std::optional<haptics::legacy_rumble_t> release_start;
  std::optional<haptics::legacy_rumble_t> latest;
  for (std::uint32_t chunk = 21; chunk <= 80; ++chunk) {
    const auto output = session.process(
      0, 0, 240, chunk, chunk * 5000, silence,
      start + std::chrono::milliseconds(chunk * 5));
    if (output) {
      if (!release_start) release_start = output;
      latest = output;
    }
  }

  ASSERT_TRUE(release_start.has_value());
  // A long effect releases through the configured tail instead of being hard
  // cut at the 80 ms short-pulse boundary.
  EXPECT_GT(release_start->low_frequency, 0u);
  EXPECT_GT(release_start->high_frequency, 0u);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->low_frequency, 0u);
  EXPECT_EQ(latest->high_frequency, 0u);
}

TEST(AuthoredDualSenseIr, LegacyFallbackDiscontinuityDoesNotReuseHold) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto burst = sine_pcm(120.0, 32000.0);
  ASSERT_TRUE(session.process(0, 0x01, 240, 0, 0, burst, start).has_value());

  const std::vector<std::uint8_t> silence(240 * 4);
  const auto restarted = session.process(
    0, 0x05, 240, 1, 5000, silence, start + 5ms);
  ASSERT_TRUE(restarted.has_value());
  EXPECT_EQ(restarted->low_frequency, 0u);
  EXPECT_EQ(restarted->high_frequency, 0u);
}

TEST(AuthoredDualSenseIr, LegacyFallbackSeparatesTactileBandsAndRejectsHiss) {
  using namespace std::chrono_literals;
  const auto measure = [](double frequency_hz, std::uint32_t chunks) {
    haptics::legacy_rumble_session_t session;
    std::optional<haptics::legacy_rumble_t> latest;
    const auto start = std::chrono::steady_clock::time_point {1s};
    for (std::uint32_t chunk = 0; chunk <= chunks; ++chunk) {
      const auto pcm = sine_pcm(frequency_hz, 24000.0, chunk * 240U);
      const auto output = session.process(
        0, chunk == 0 ? 0x01 : 0, 240, chunk,
        static_cast<std::uint64_t>(chunk) * 5000U, pcm,
        start + std::chrono::milliseconds(chunk * 5U));
      if (output) latest = output;
    }
    return latest;
  };

  const auto body = measure(120.0, 20);
  const auto texture = measure(300.0, 20);
  const auto hiss = measure(1000.0, 20);
  ASSERT_TRUE(body.has_value());
  ASSERT_TRUE(texture.has_value());
  ASSERT_TRUE(hiss.has_value());
  EXPECT_GT(body->low_frequency, texture->low_frequency);
  EXPECT_GT(texture->high_frequency, body->high_frequency);
  // Out-of-band hiss must reach exactly zero rather than merely a small value,
  // otherwise the noise gate has latched open on the onset transient.
  EXPECT_EQ(hiss->low_frequency, 0u);
  EXPECT_EQ(hiss->high_frequency, 0u);

  // Sustained residual stays gated no matter how long it runs.
  const auto long_hiss = measure(1000.0, 400);
  ASSERT_TRUE(long_hiss.has_value());
  EXPECT_EQ(long_hiss->low_frequency, 0u);
  EXPECT_EQ(long_hiss->high_frequency, 0u);
}

TEST(AuthoredDualSenseIr, LegacyFallbackGateReleasesAfterLoudOnset) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};

  // A loud in-band burst opens both gates.
  const auto burst = sine_pcm(120.0, 32000.0);
  const auto opened = session.process(3, 0x01, 240, 0, 0, burst, start);
  ASSERT_TRUE(opened.has_value());
  EXPECT_GT(opened->low_frequency, 0);
  EXPECT_GT(opened->high_frequency, 0);

  // Hysteresis must not keep them open on a level parked between the open and
  // close thresholds; the hold budget has to expire and mute the motors.
  std::optional<haptics::legacy_rumble_t> latest;
  std::optional<haptics::legacy_rumble_t> at_240ms;
  for (std::uint32_t chunk = 1; chunk <= 60; ++chunk) {
    const auto pcm = sine_pcm(1000.0, 24000.0, chunk * 240U);
    const auto output = session.process(
      3, 0, 240, chunk, static_cast<std::uint64_t>(chunk) * 5000U, pcm,
      start + std::chrono::milliseconds(chunk * 5U));
    if (output) latest = output;
    if (chunk == 48) at_240ms = latest;
  }
  // Both motors reach zero by 200 ms: a 60 ms hold budget plus the low motor's
  // 40 ms release tail. Checking at 240 ms bounds the budget instead of only
  // proving that the gate eventually closes.
  ASSERT_TRUE(at_240ms.has_value());
  EXPECT_EQ(at_240ms->low_frequency, 0u);
  EXPECT_EQ(at_240ms->high_frequency, 0u);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->low_frequency, 0u);
  EXPECT_EQ(latest->high_frequency, 0u);
}

TEST(AuthoredDualSenseIr, LegacyFallbackWatchdogReleasesMotors) {
  using namespace std::chrono_literals;
  haptics::legacy_rumble_session_t session;
  ASSERT_TRUE(session.ready());
  const auto start = std::chrono::steady_clock::time_point {1s};
  const auto pcm = sine_pcm(300.0, 24000.0);

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
