#include "src/hdr/calibration.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <vector>

namespace {
  void
  put(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (24 - 8 * index));
  }

  std::vector<std::uint8_t>
  profile() {
    std::vector<std::uint8_t> bytes(212);
    put(bytes, 0, static_cast<std::uint32_t>(bytes.size()));
    bytes[8] = 4;
    put(bytes, 12, 0x6D6E7472);
    put(bytes, 16, 0x52474220);
    put(bytes, 20, 0x58595A20);
    put(bytes, 36, 0x61637370);
    put(bytes, 128, 2);
    put(bytes, 132, 0x4D484332);
    put(bytes, 136, 156);
    put(bytes, 140, 36);
    put(bytes, 144, 0x6C756D69);
    put(bytes, 148, 192);
    put(bytes, 152, 20);
    put(bytes, 156, 0x4D484332);
    put(bytes, 168, 66);
    put(bytes, 172, 1600 * 65536);
    put(bytes, 192, 0x58595A20);
    put(bytes, 204, 1200 * 65536);
    return bytes;
  }

  hdr::calibration_binding_t
  binding() {
    return { "S-1-5-21-user", "vdd-A", { "A.icc", std::string(64, 'a'), { true, 1600, 0.001f, 1200 } } };
  }
}  // namespace

TEST(HdrCalibration, ReadsV2AndV4MhcLuminance) {
  for (const auto version : { 2, 4 }) {
    auto bytes = profile();
    bytes[8] = version;
    const auto result = hdr::parse_calibration_luminance(bytes);
    ASSERT_TRUE(result);
    EXPECT_FLOAT_EQ(result->max_nits, 1600);
    EXPECT_FLOAT_EQ(result->max_full_frame_nits, 1200);
    EXPECT_NEAR(result->min_nits, 0.001, 1.0 / 65536);
  }
}

TEST(HdrCalibration, RejectsReadsOutsideTheDeclaredTag) {
  auto bytes = profile();
  put(bytes, 140, 4);  // Valid luminance bytes still exist elsewhere in the file.
  EXPECT_FALSE(hdr::parse_calibration_luminance(bytes));
  bytes = profile();
  put(bytes, 152, 12);
  EXPECT_FALSE(hdr::parse_calibration_luminance(bytes));
}

TEST(HdrCalibration, RejectsInvalidTagTypesAndRanges) {
  for (const auto offset : { 156u, 192u }) {
    auto bytes = profile();
    put(bytes, offset, 0x42414421);
    EXPECT_FALSE(hdr::parse_calibration_luminance(bytes));
  }
  auto bytes = profile();
  put(bytes, 204, 1800 * 65536);
  EXPECT_FALSE(hdr::parse_calibration_luminance(bytes));
  bytes = profile();
  put(bytes, 168, 0xFFFF0000);  // Negative minimum luminance.
  EXPECT_FALSE(hdr::parse_calibration_luminance(bytes));
}

TEST(HdrCalibration, RejectsMalformedHeadersAndOffsets) {
  for (const auto [offset, value] : {
         std::pair { 0u, 999u }, { 12u, 0u }, { 16u, 0u }, { 36u, 0u },
         { 128u, 0xFFFFFFFFu }, { 136u, 0xFFFFFFFCu }, { 136u, 128u }, { 140u, 999u },
         { 164u, 4097u }, { 176u, 32u }, { 180u, 40u } }) {
    auto bytes = profile();
    put(bytes, offset, value);
    EXPECT_FALSE(hdr::parse_calibration_luminance(bytes)) << offset;
  }
}

TEST(HdrCalibration, RequiresAnExactClientUuid) {
  const auto clients = nlohmann::json::array({ { { "uuid", "A" }, { "name", "B" }, { "hdrCalibration", binding() } } }).dump();
  const auto restored = hdr::client_calibration(clients, "A");
  ASSERT_TRUE(restored);
  const auto saved = binding();
  EXPECT_TRUE(hdr::calibration_matches(*restored, saved.user_sid, saved.vdd_id, saved.profile));
  EXPECT_FALSE(nlohmann::json(saved).contains("maxNits"));  // Brightness comes from the verified file.
  EXPECT_FALSE(hdr::client_calibration(clients, "B"));
  EXPECT_FALSE(hdr::client_calibration(clients, ""));
  EXPECT_FALSE(hdr::client_calibration("{", "A"));
}

TEST(HdrCalibration, RejectsAmbiguousOrMalformedBindings) {
  auto clients = nlohmann::json::array({ { { "uuid", "A" }, { "hdrCalibration", binding() } } });
  clients.push_back(clients.front());
  EXPECT_FALSE(hdr::client_calibration(clients.dump(), "A"));
  clients.erase(1);
  for (const auto &bad : {
         nlohmann::json { { "version", 2 } },
         nlohmann::json { { "profileName", "../A.icc" } },
         nlohmann::json { { "sha256", "changed" } } }) {
    clients[0]["hdrCalibration"] = binding();
    clients[0]["hdrCalibration"].update(bad);
    EXPECT_FALSE(hdr::client_calibration(clients.dump(), "A"));
  }
}

TEST(HdrCalibration, RejectsAnotherUserDisplayOrReplacedFile) {
  const auto saved = binding();
  EXPECT_TRUE(hdr::calibration_matches(saved, saved.user_sid, "vdd-A", saved.profile));
  EXPECT_FALSE(hdr::calibration_matches(saved, "another-user", "vdd-A", saved.profile));
  EXPECT_FALSE(hdr::calibration_matches(saved, saved.user_sid, "vdd-B", saved.profile));
  auto replaced = saved.profile;
  replaced.fingerprint = std::string(64, 'b');
  EXPECT_FALSE(hdr::calibration_matches(saved, saved.user_sid, "vdd-A", replaced));
  replaced = saved.profile;
  replaced.profile_name = "B.icc";
  EXPECT_FALSE(hdr::calibration_matches(saved, saved.user_sid, "vdd-A", replaced));
}

TEST(HdrCalibration, StaleSettingsPreserveBindingsButKeepTheUsersOtherEdits) {
  const auto stored = nlohmann::json::array({ { { "uuid", "A" }, { "name", "old" }, { "hdrCalibration", binding() } } });
  const auto incoming = nlohmann::json::array({ { { "uuid", "A" }, { "name", "renamed" }, { "hdrBrightnessMode", "manual" } }, nullptr, 7, "legacy" });
  const auto merged = nlohmann::json::parse(hdr::preserve_client_calibrations(incoming.dump(), stored.dump()));
  EXPECT_EQ(merged[0]["name"], "renamed");
  EXPECT_EQ(merged[0]["hdrBrightnessMode"], "manual");
  EXPECT_EQ(merged[0]["hdrCalibration"], stored[0]["hdrCalibration"]);
  for (std::size_t i = 1; i < incoming.size(); ++i) {
    EXPECT_EQ(merged[i], incoming[i]);
  }
}

TEST(HdrCalibration, SettingsCannotImportAnotherClientsCalibration) {
  const auto stored = nlohmann::json::array({ { { "uuid", "A" }, { "hdrCalibration", binding() } } });
  const auto incoming = nlohmann::json::array({ { { "uuid", "B" }, { "hdrCalibration", binding() } } });
  const auto merged = nlohmann::json::parse(hdr::preserve_client_calibrations(incoming.dump(), stored.dump()));
  EXPECT_FALSE(merged[0].contains("hdrCalibration"));
  auto ambiguous = stored;
  ambiguous.push_back(stored.front());
  const auto repaired = hdr::preserve_client_calibrations("[{\"uuid\":\"A\"}]", ambiguous.dump());
  EXPECT_FALSE(nlohmann::json::parse(repaired)[0].contains("hdrCalibration"));
}
