#include <cstring>

#include <gtest/gtest.h>

#include "src/pen_barrel_roll.h"

TEST(PenBarrelRollProbe, DefaultsToAzimuthWhileRollIsConstant) {
  input::pen_wire::roll_probe_t probe;
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(probe.select(120, i), i);
  }
}

TEST(PenBarrelRollProbe, VariationEnablesIndependentRollForTheSession) {
  input::pen_wire::roll_probe_t probe;
  EXPECT_EQ(probe.select(359, 90), 90);
  EXPECT_EQ(probe.select(0, 180), 0);
  EXPECT_EQ(probe.select(0, 270), 0);
  EXPECT_EQ(probe.select(359, 270), 359);
  EXPECT_EQ(probe.select(input::pen_wire::unknown, 90), 90);
  EXPECT_EQ(probe.select(359, 270), 359);
}

TEST(PenBarrelRollProbe, UnknownValuesDoNotEnableRollAndSessionsStartFresh) {
  input::pen_wire::roll_probe_t probe;
  EXPECT_EQ(probe.select(input::pen_wire::unknown, 90), 90);
  EXPECT_EQ(probe.select(120, 90), 90);
  EXPECT_EQ(probe.select(input::pen_wire::unknown, 180), 180);
  EXPECT_EQ(probe.select(120, 180), 180);
  EXPECT_EQ(probe.select(121, 180), 121);
  input::pen_wire::roll_probe_t next_session;
  EXPECT_EQ(next_session.select(121, 180), 180);
}
TEST(PenBarrelRollProbe, DisconnectOrLegacyPacketResetsActivatedProbe) {
  input::pen_wire::roll_probe_t probe;
  EXPECT_EQ(probe.select(120, 90), 90);
  EXPECT_EQ(probe.select(121, 90), 121);
  probe.reset();
  EXPECT_EQ(probe.select(121, 180), 180);
  EXPECT_EQ(probe.select(121, 270), 270);
  EXPECT_EQ(probe.select(122, 270), 122);
}

TEST(PenBarrelRoll, MatchesExtendedWireLayout) {
  EXPECT_EQ(sizeof(input::pen_wire::packet_t), 40);
  EXPECT_EQ(offsetof(input::pen_wire::packet_t, pen), 0);
  EXPECT_EQ(offsetof(input::pen_wire::packet_t, barrelRoll), 36);
  EXPECT_EQ(offsetof(input::pen_wire::packet_t, reserved), 38);
}

TEST(PenBarrelRoll, ReadsLittleEndianTwistAndPreservesLegacyPose) {
  input::pen_wire::packet_t packet {};
  packet.pen.tilt = 45;
  packet.pen.rotation = 123;
  const std::uint8_t encoded[] = {0x0e, 0x01}; // 270 degrees
  std::memcpy(&packet.barrelRoll, encoded, sizeof(encoded));
  EXPECT_EQ(input::pen_wire::decode_roll(packet.barrelRoll), 270);
  EXPECT_EQ(packet.pen.tilt, 45);
  EXPECT_EQ(packet.pen.rotation, 123);
}

TEST(PenBarrelRoll, PreservesUnknownInsteadOfWrappingIt) {
  EXPECT_EQ(input::pen_wire::decode_roll(0xffff), input::pen_wire::unknown);
}

TEST(PenBarrelRoll, NormalizesDegrees) {
  const std::uint8_t encoded[] = {0xd1, 0x02}; // 721 degrees
  std::uint16_t value;
  std::memcpy(&value, encoded, sizeof(value));
  EXPECT_EQ(input::pen_wire::decode_roll(value), 1);
  EXPECT_EQ(input::pen_wire::decode_roll(0), 0);
}

TEST(PenBarrelRoll, RejectsTruncatedAndIncorrectlySizedPackets) {
  const std::uint8_t encoded[] = {0, 0, 0, 36};
  std::uint32_t size;
  std::memcpy(&size, encoded, sizeof(size));
  EXPECT_TRUE(input::pen_wire::valid_size(40, size));
  for (std::size_t available : {0, 8, 36, 38, 39, 41}) {
    EXPECT_FALSE(input::pen_wire::valid_size(available, size));
  }
  EXPECT_FALSE(input::pen_wire::valid_size(40, 0));
}
