#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "src/remote_connect/strict_policy.h"

namespace {
  const std::string key_a = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  const std::string key_b = "ff02030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
}  // namespace

TEST(RemoteConnectStrictPolicy, KeyHexIsSixtyFourLowercaseCharacters) {
  std::array<std::uint8_t, 32> key {};
  key[0] = 0x00;
  key[1] = 0x0a;
  key[2] = 0xab;
  key[31] = 0xfe;

  const auto hex = remote_connect::strict_policy::key_hex(key);

  ASSERT_EQ(hex.size(), 64u);
  EXPECT_EQ(hex.substr(0, 6), "000aab");
  EXPECT_EQ(hex.substr(62, 2), "fe");
  EXPECT_EQ(hex.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(RemoteConnectStrictPolicy, EveryKeyHasItsOwnHex) {
  std::array<std::uint8_t, 32> key {};
  const auto zero = remote_connect::strict_policy::key_hex(key);
  key[31] = 0x01;
  EXPECT_NE(remote_connect::strict_policy::key_hex(key), zero);
}

TEST(RemoteConnectStrictPolicy, EmptyKeyListRendersNothing) {
  const auto table = remote_connect::strict_policy::render_table({});
  ASSERT_TRUE(table.has_value());
  EXPECT_EQ(*table, "");
}

TEST(RemoteConnectStrictPolicy, RenderedTableIsTheExactExperimentalSchema) {
  const auto table = remote_connect::strict_policy::render_table({key_a, key_b});

  ASSERT_TRUE(table.has_value());
  EXPECT_EQ(
    *table,
    "[experimental_strict_data]\n"
    "advertise_v1 = true\n"
    "require_v1 = true\n"
    "expected_peer_keys = [\"0:" + key_a + "\", \"0:" + key_b + "\"]\n"
  );
}

// The table is written into a configuration file the core parses. A key that
// is not exactly 64 lowercase hex characters would either change what the
// table says or break it, so the whole table is refused rather than written.
TEST(RemoteConnectStrictPolicy, AnyMalformedKeyMeansNoTable) {
  const std::vector<std::string> malformed {
    std::string(),
    std::string("tooshort"),
    std::string(63, 'a'),
    std::string(65, 'a'),
    std::string(64, 'g'),
    std::string(64, 'A'),
    "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef",
    key_a + "\"",
    "\"" + key_a.substr(0, 63),
    "\\" + key_a.substr(0, 63),
    "\n" + key_a.substr(0, 63),
    key_a.substr(0, 63) + " ",
  };
  for (const auto &bad : malformed) {
    EXPECT_FALSE(remote_connect::strict_policy::render_table({bad}).has_value()) << bad;
    // One bad entry spoils the table even when the others are fine.
    EXPECT_FALSE(remote_connect::strict_policy::render_table({key_a, bad, key_b}).has_value()) << bad;
  }
}
