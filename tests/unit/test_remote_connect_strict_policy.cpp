#include <gtest/gtest.h>

#include "src/remote_connect/strict_policy.h"

namespace {
  const char key_a[] = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  const char key_a_upper[] = "0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20";
  const char key_b[] = "ff02030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
}  // namespace

TEST(RemoteConnectStrictPolicy, EmptyInputIsValidWithoutKeys) {
  for (const char *raw : {"", "   ", "\t\r\n;"}) {
    const auto parsed = remote_connect::strict_policy::parse_client_keys(raw);
    EXPECT_TRUE(parsed.valid) << raw;
    EXPECT_TRUE(parsed.keys.empty()) << raw;
    EXPECT_TRUE(parsed.error.empty()) << raw;
  }
}

TEST(RemoteConnectStrictPolicy, SingleKeyIsNormalizedToLowercase) {
  const auto parsed = remote_connect::strict_policy::parse_client_keys(key_a_upper);
  ASSERT_TRUE(parsed.valid);
  ASSERT_EQ(parsed.keys.size(), 1u);
  EXPECT_EQ(parsed.keys[0], key_a);
}

TEST(RemoteConnectStrictPolicy, MultipleKeysSplitOnSemicolonsAndTolerateWhitespace) {
  std::string raw = std::string(" ") + key_a_upper + " ;\t" + key_b + ";";
  const auto parsed = remote_connect::strict_policy::parse_client_keys(raw);
  ASSERT_TRUE(parsed.valid);
  ASSERT_EQ(parsed.keys.size(), 2u);
  EXPECT_EQ(parsed.keys[0], key_a);
  EXPECT_EQ(parsed.keys[1], key_b);
}

TEST(RemoteConnectStrictPolicy, InvalidEntriesFailTheWholeList) {
  for (const auto &raw : {
         std::string("tooshort"),
         std::string(64, 'z') + ";",                // non-hex character
         std::string(63, 'a') + ";",                // one character short
         std::string(key_a) + ";" + key_a,         // duplicate entry
       }) {
    const auto parsed = remote_connect::strict_policy::parse_client_keys(raw);
    EXPECT_FALSE(parsed.valid) << raw;
    EXPECT_FALSE(parsed.error.empty()) << raw;
    EXPECT_TRUE(parsed.keys.empty()) << raw;
  }
}

TEST(RemoteConnectStrictPolicy, AllZeroKeyIsRejectedBeforeItCanReachTheCore) {
  // The core rejects an all-zero key by failing instance creation.
  for (const auto &raw : {
         std::string(64, '0'),
         key_a + std::string(";") + std::string(64, '0'),
       }) {
    const auto parsed = remote_connect::strict_policy::parse_client_keys(raw);
    EXPECT_FALSE(parsed.valid) << raw;
    EXPECT_NE(parsed.error.find("all zero"), std::string::npos) << raw;
    // A valid key earlier in the list is not kept from an invalid result.
    EXPECT_TRUE(parsed.keys.empty()) << raw;
  }
}

TEST(RemoteConnectStrictPolicy, EmptyKeyListRendersNothing) {
  EXPECT_EQ(remote_connect::strict_policy::render_table({}), "");
}

TEST(RemoteConnectStrictPolicy, RenderedTableIsTheExactExperimentalSchema) {
  const auto table = remote_connect::strict_policy::render_table({key_a, key_b});
  EXPECT_EQ(
    table,
    "[experimental_strict_data]\n"
    "advertise_v1 = true\n"
    "require_v1 = true\n"
    "expected_peer_keys = [\"0:" + std::string(key_a) + "\", \"0:" + key_b + "\"]\n"
  );
}
