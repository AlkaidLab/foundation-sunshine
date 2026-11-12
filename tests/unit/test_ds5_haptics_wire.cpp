/**
 * @file tests/unit/test_ds5_haptics_wire.cpp
 * @brief The haptics PCM packet: two channels for every client, four only with the pad-audio token.
 */
#include "src/ds5/haptics_wire.h"

#include <gtest/gtest.h>

#include <array>

namespace {
  namespace wire = ds5::haptics_wire;
}

TEST(Ds5HapticsWire, TwoChannelPacketIsWhatAnOldClientParses) {
  const std::array<std::uint8_t, 8> haptics { 1, 2, 3, 4, 5, 6, 7, 8 };
  const auto body = wire::build_pcm_body(0x01, 3, 2, 0x11223344, 0x0102030405060708ull, haptics.data(), nullptr);
  ASSERT_EQ(body.size(), wire::header_bytes + 8);
  EXPECT_EQ(body[0], 1);
  EXPECT_EQ(body[1], 0x01);
  EXPECT_EQ(body[2], 28);
  EXPECT_EQ(body[4], 3);
  EXPECT_EQ(body[6], 2);
  EXPECT_EQ(body[8], 0x44);
  EXPECT_EQ(body[11], 0x11);
  EXPECT_EQ(body[12], 0x08);
  EXPECT_EQ(body[19], 0x01);
  EXPECT_EQ(body[20], 0x80);
  EXPECT_EQ(body[21], 0xbb);
  EXPECT_EQ(body[24], 2);
  EXPECT_EQ(body[25], 16);
  EXPECT_EQ(body[26], 0);
  EXPECT_EQ(body[27], 0);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(body[28 + i], haptics[i]);
}

TEST(Ds5HapticsWire, FourChannelPacketPutsTheSpeakerFirst) {
  const std::array<std::uint8_t, 8> haptics { 1, 2, 3, 4, 5, 6, 7, 8 };
  const std::array<std::uint8_t, 8> speaker { 0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24 };
  const auto body = wire::build_pcm_body(0, 0, 2, 5, 0, haptics.data(), speaker.data());
  ASSERT_EQ(body.size(), wire::header_bytes + 16);
  EXPECT_EQ(body[24], 4);
  const std::array<std::uint8_t, 16> expected { 0x11, 0x12, 0x13, 0x14, 1, 2, 3, 4, 0x21, 0x22, 0x23, 0x24, 5, 6, 7, 8 };
  for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(body[28 + i], expected[i]) << i;
}

TEST(Ds5HapticsWire, LargestPacketFitsTheSendBuffer) {
  std::array<std::uint8_t, 240 * 4> haptics {}, speaker {};
  const auto body = wire::build_pcm_body(0, 0, 240, 1, 0, haptics.data(), speaker.data());
  EXPECT_EQ(body.size(), wire::header_bytes + 240 * 8);
}
