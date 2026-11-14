/**
 * @file tests/unit/test_client_family.cpp
 * @brief Evidence-only client family identification.
 */

#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/client_family.h"

#ifndef CLIENT_FAMILY_FIXTURE
  #define CLIENT_FAMILY_FIXTURE "tests/fixtures/client_families.json"
#endif

namespace {
  using json = nlohmann::json;

  client_family::observation_t
  make_observation(const json &fixture, std::int64_t now = 1762243200) {
    client_family::observation_t observation;
    observation.now = now;

    const auto query = fixture.value("query", json::object());
    for (const auto &[name, value] : query.items()) {
      client_family::add_query(observation, name, value.get<std::string>());
    }
    const auto headers = fixture.value("headers", json::object());
    for (const auto &[name, value] : headers.items()) {
      client_family::add_header(observation, name, value.get<std::string>());
    }
    const auto sdp = fixture.value("sdp", json::object());
    for (const auto &[name, value] : sdp.items()) {
      client_family::add_sdp(observation, name, value.get<std::string>());
    }
    const auto signal = fixture.value("signal", json::object());
    for (const auto &[name, value] : signal.items()) {
      observation.add("input." + name, value.get<std::string>());
    }
    return observation;
  }

  std::string
  expected_string(const json &value) {
    return value.is_null() ? std::string {} : value.get<std::string>();
  }
}  // namespace

TEST(ClientFamilyTest, DocumentedFixturesMatchExpectedFamily) {
  std::ifstream input { CLIENT_FAMILY_FIXTURE };
  ASSERT_TRUE(input.is_open()) << CLIENT_FAMILY_FIXTURE;
  const auto root = json::parse(input);
  ASSERT_EQ(root.value("schema_version", 0), 1);

  const auto &fixtures = root.at("fixtures");
  std::set<std::string> ids;
  std::set<client_family::family_e> families;
  for (const auto &fixture : fixtures) {
    const auto id = fixture.at("id").get<std::string>();
    SCOPED_TRACE(id);
    EXPECT_TRUE(ids.insert(id).second) << "duplicate fixture id";

    const auto match = client_family::identify(make_observation(fixture));
    const auto &expected = fixture.at("expect");
    EXPECT_EQ(std::string { client_family::to_string(match.family) }, expected.at("family").get<std::string>());
    families.insert(match.family);

    const auto expected_official = expected.at("official");
    const auto actual_official = client_family::official(match.family);
    if (expected_official.is_null()) {
      EXPECT_FALSE(actual_official.has_value());
    }
    else {
      ASSERT_TRUE(actual_official.has_value());
      EXPECT_EQ(*actual_official, expected_official.get<bool>());
    }

    EXPECT_EQ(match.os, expected_string(expected.at("os")));
    EXPECT_EQ(match.version, expected_string(expected.at("version")));
  }

  EXPECT_EQ(ids.size(), fixtures.size());
  EXPECT_TRUE(families.contains(client_family::family_e::unknown));
  EXPECT_TRUE(families.contains(client_family::family_e::moonlight_vplus_android));
  EXPECT_TRUE(families.contains(client_family::family_e::moonlight_vplus_desktop));
  EXPECT_TRUE(families.contains(client_family::family_e::moonlight_macos_enhanced));
  EXPECT_TRUE(families.contains(client_family::family_e::voidlink));
  EXPECT_TRUE(families.contains(client_family::family_e::moonlight_upstream));
}

TEST(ClientFamilyTest, UnknownEvidenceDoesNotProduceAVerdict) {
  client_family::observation_t observation;
  observation.now = 1762243200;
  client_family::add_query(observation, "uniqueid", "0123456789abcdef");
  client_family::add_query(observation, "clientname", "Living room");

  const auto match = client_family::identify(observation);
  EXPECT_EQ(match.family, client_family::family_e::unknown);
  EXPECT_FALSE(match.matched());
  EXPECT_FALSE(client_family::official(match.family).has_value());

  const auto record = client_family::merge({}, observation);
  EXPECT_EQ(record.family, client_family::family_e::unknown);
  EXPECT_EQ(record.last_seen, observation.now);
  EXPECT_TRUE(record.version.empty());
  EXPECT_TRUE(record.os.empty());
}

TEST(ClientFamilyTest, UpgradeUpdatesVersionWithoutChangingFamily) {
  client_family::observation_t first;
  first.now = 1762243200;
  client_family::add_header(first, "User-Agent", "MoonlightEnhanced/1.3.8 CFNetwork/1498.1.5 Darwin/23.5.0");

  auto record = client_family::merge({}, first);
  EXPECT_EQ(record.family, client_family::family_e::moonlight_macos_enhanced);
  EXPECT_EQ(record.version, "1.3.8");
  EXPECT_EQ(record.os, "macos");

  client_family::observation_t second;
  second.now = 1762329600;
  client_family::add_header(second, "User-Agent", "MoonlightEnhanced/1.3.9 CFNetwork/1498.1.5 Darwin/23.5.0");
  record = client_family::merge(record, second);

  EXPECT_EQ(record.family, client_family::family_e::moonlight_macos_enhanced);
  EXPECT_EQ(record.version, "1.3.9");
  EXPECT_EQ(record.os, "macos");
  EXPECT_EQ(record.last_seen, second.now);
}

TEST(ClientFamilyTest, WeakerObservationDoesNotDowngradeKnownFamily) {
  client_family::observation_t identified;
  identified.now = 1762243200;
  client_family::add_header(identified, "User-Agent", "MoonlightVPlus/1.2.3 (Linux; Android 14; Pixel 8)");

  auto record = client_family::merge({}, identified);
  ASSERT_EQ(record.family, client_family::family_e::moonlight_vplus_android);

  client_family::observation_t bare;
  bare.now = 1762329600;
  client_family::add_query(bare, "clientname", "Living room");
  record = client_family::merge(record, bare);

  EXPECT_EQ(record.family, client_family::family_e::moonlight_vplus_android);
  EXPECT_EQ(record.version, "1.2.3");
  EXPECT_EQ(record.os, "android");
  EXPECT_EQ(record.last_seen, bare.now);
}

TEST(ClientFamilyTest, OsOnlyEvidenceDoesNotRewriteAKnownFamily) {
  client_family::observation_t identified;
  client_family::add_header(identified, "User-Agent", "MoonlightEnhanced/1.3.9 Darwin/23.5.0");
  auto record = client_family::merge({}, identified);
  ASSERT_EQ(record.family, client_family::family_e::moonlight_macos_enhanced);
  ASSERT_EQ(record.os, "macos");

  client_family::observation_t unrelated;
  unrelated.now = 1762329600;
  client_family::add_header(unrelated, "User-Agent", "curl/8.5.0 (x86_64-pc-linux-gnu)");
  record = client_family::merge(record, unrelated);

  EXPECT_EQ(record.family, client_family::family_e::moonlight_macos_enhanced);
  EXPECT_EQ(record.os, "macos");
  EXPECT_EQ(record.last_seen, unrelated.now);
}

TEST(ClientFamilyTest, OfficialTriStateIsNullOnlyForUnknown) {
  EXPECT_TRUE(client_family::record_json({}).at("official").is_null());

  client_family::record_t enhanced;
  enhanced.family = client_family::family_e::moonlight_macos_enhanced;
  EXPECT_EQ(client_family::record_json(enhanced).at("official").get<bool>(), true);

  client_family::record_t upstream;
  upstream.family = client_family::family_e::moonlight_upstream;
  EXPECT_EQ(client_family::record_json(upstream).at("official").get<bool>(), false);
}

TEST(ClientFamilyTest, RecordJsonCarriesEvidenceAndTimestamp) {
  client_family::observation_t observation;
  observation.now = 1762243200;
  client_family::add_header(observation, "User-Agent", "MoonlightEnhanced/1.3.9 CFNetwork/1498.1.5 Darwin/23.5.0");
  const auto record = client_family::merge({}, observation);
  const auto output = client_family::record_json(record);

  EXPECT_EQ(output.at("family"), "moonlight-macos-enhanced");
  EXPECT_EQ(output.at("official"), true);
  EXPECT_EQ(output.at("version"), "1.3.9");
  EXPECT_EQ(output.at("os"), "macos");
  EXPECT_EQ(output.at("last_seen"), observation.now);
  EXPECT_TRUE(output.at("family_source").get<std::string>().starts_with("moonlight-macos-enhanced-user-agent:"));
}

TEST(ClientFamilyTest, EvidenceSourceNamesTheWinningRule) {
  client_family::observation_t observation;
  client_family::add_header(observation, "User-Agent", "MoonlightEnhanced/1.3.9 Darwin/23.5.0");
  const auto match = client_family::identify(observation);
  EXPECT_TRUE(match.source.starts_with("moonlight-macos-enhanced-user-agent:"));
  EXPECT_NE(match.source.find("header.user-agent"), std::string::npos);
}

TEST(ClientFamilyTest, PenBarrelRollIdentifiesVoidLink) {
  client_family::observation_t observation;
  observation.add("input.penBarrelRoll", "1");
  const auto match = client_family::identify(observation);
  EXPECT_EQ(match.family, client_family::family_e::voidlink);
  EXPECT_EQ(match.os, "ios");
  ASSERT_TRUE(client_family::official(match.family).has_value());
  EXPECT_FALSE(*client_family::official(match.family));
}

TEST(ClientFamilyTest, PairingNamePrefersClientnameThenDevicename) {
  EXPECT_EQ(client_family::pairing_device_name("Living room", "macos-test-host.local"), "Living room");
  EXPECT_EQ(client_family::pairing_device_name("", "macos-test-host.local"), "macos-test-host.local");
  EXPECT_EQ(client_family::pairing_device_name("   ", "macos-test-host.local"), "macos-test-host.local");
}

TEST(ClientFamilyTest, PairingNameIgnoresTheSharedMoonlightPlaceholder) {
  // Moonlight's shared client code sends devicename=roth for every device; it names nothing.
  EXPECT_EQ(client_family::pairing_device_name("", "roth"), "");
  EXPECT_EQ(client_family::pairing_device_name("", "Roth"), "");
  EXPECT_EQ(client_family::pairing_device_name("", ""), "");
  EXPECT_EQ(client_family::pairing_device_name("My phone", "roth"), "My phone");
}

TEST(ClientFamilyTest, SessionNameKeepsAnExplicitNameAndOtherwiseUsesThePairedOne) {
  EXPECT_EQ(client_family::session_client_name("Sent by client", "Paired name"), "Sent by client");
  EXPECT_EQ(client_family::session_client_name("unknown", "Paired name"), "Paired name");
  EXPECT_EQ(client_family::session_client_name("UNKNOWN", "Paired name"), "Paired name");
  EXPECT_EQ(client_family::session_client_name("", "Paired name"), "Paired name");
  EXPECT_EQ(client_family::session_client_name("unknown", ""), "unknown");
  EXPECT_EQ(client_family::session_client_name("", ""), "unknown");
}
