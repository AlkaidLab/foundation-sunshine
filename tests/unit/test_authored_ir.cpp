#include <cmath>
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
