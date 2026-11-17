#include "src/streaming/dynamic_param_protocol.h"

#include <gtest/gtest.h>

TEST(DynamicParamProtocol, EncodesHostBitrateCallbackAsLittleEndianWords) {
  const auto payload = streaming::dynamic_param_protocol::encode_bitrate_update(400000);
  EXPECT_EQ(payload, (std::array<std::uint8_t, 8> { 2, 0, 0, 0, 0x80, 0x1A, 0x06, 0 }));

  std::int32_t bitrate = 0;
  EXPECT_TRUE(streaming::dynamic_param_protocol::decode_bitrate_update(
    std::string_view { reinterpret_cast<const char *>(payload.data()), payload.size() }, bitrate));
  EXPECT_EQ(bitrate, 400000);
}

TEST(DynamicParamProtocol, RejectsWrongTypeLengthAndRange) {
  std::int32_t bitrate = 0;
  const auto valid = streaming::dynamic_param_protocol::encode_bitrate_update(1000);
  std::string payload(reinterpret_cast<const char *>(valid.data()), valid.size());
  payload[0] = 3;
  EXPECT_FALSE(streaming::dynamic_param_protocol::decode_bitrate_update(payload, bitrate));
  payload = std::string(7, '\0');
  EXPECT_FALSE(streaming::dynamic_param_protocol::decode_bitrate_update(payload, bitrate));
  const auto invalid = streaming::dynamic_param_protocol::encode_bitrate_update(800001);
  EXPECT_FALSE(streaming::dynamic_param_protocol::decode_bitrate_update(
    std::string_view { reinterpret_cast<const char *>(invalid.data()), invalid.size() }, bitrate));
}

TEST(DynamicParamProtocol, BitrateCallbackUsesTheRegistryBitNotUpstreamResolution) {
  // 0x4000 is the next free fork bit after endpoint refresh (0x8000); 0x400 is
  // the upstream resolution-change request and is never used for bitrate here.
  EXPECT_EQ(streaming::dynamic_param_protocol::bitrate_callback_feature, 0x4000u);
  EXPECT_NE(streaming::dynamic_param_protocol::bitrate_callback_feature, 0x400u);
}
