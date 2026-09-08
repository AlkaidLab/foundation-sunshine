/**
 * @file tests/unit/test_color_profile.cpp
 * @brief Test per-client HDR color profile resolution.
 */
#include <src/display_device/color_profile.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
  using display_device::color_profile::profile_policy_e;
  using display_device::color_profile::resolve_client_hdr_profile;
}

TEST(ColorProfileResolver, ResolvesUuidBeforeLegacyName) {
  const auto result = resolve_client_hdr_profile(
    R"([
      {"uuid":"uuid-a","name":"Living Room","hdrProfile":"uuid.icc"},
      {"uuid":"uuid-b","name":"uuid-a","hdrProfile":"name.icm"}
    ])",
    "uuid-a",
    "uuid-a"
  );

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.policy, profile_policy_e::apply);
  ASSERT_TRUE(result.profile);
  EXPECT_EQ(*result.profile, "uuid.icc");
  EXPECT_FALSE(result.used_legacy_name);
}

TEST(ColorProfileResolver, DoesNotFallBackToNameWhenUuidDoesNotMatch) {
  const auto result = resolve_client_hdr_profile(
    R"([{"uuid":"configured-uuid","name":"Living Room","hdrProfile":"legacy.icc"}])",
    "different-uuid",
    "Living Room"
  );

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.policy, profile_policy_e::unspecified);
  EXPECT_FALSE(result.profile);
  EXPECT_FALSE(result.used_legacy_name);
}

TEST(ColorProfileResolver, UsesUniqueLegacyNameOnlyWithoutUuid) {
  const auto result = resolve_client_hdr_profile(
    R"([{"name":"Living Room","hdrProfile":"legacy.ICM"}])",
    "",
    "Living Room"
  );

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.policy, profile_policy_e::apply);
  ASSERT_TRUE(result.profile);
  EXPECT_EQ(*result.profile, "legacy.ICM");
  EXPECT_TRUE(result.used_legacy_name);
}

TEST(ColorProfileResolver, RejectsAmbiguousLegacyName) {
  const auto result = resolve_client_hdr_profile(
    R"([
      {"uuid":"uuid-a","name":"Duplicate","hdrProfile":"first.icc"},
      {"uuid":"uuid-b","name":"Duplicate","hdrProfile":"second.icc"}
    ])",
    "",
    "Duplicate"
  );

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("ambiguous"), std::string::npos);
  EXPECT_EQ(result.policy, profile_policy_e::unspecified);
  EXPECT_FALSE(result.profile);
  EXPECT_FALSE(result.used_legacy_name);
}

TEST(ColorProfileResolver, DistinguishesMissingAndExplicitlyEmptyProfile) {
  const auto missing = resolve_client_hdr_profile(
    R"([{"uuid":"missing"}])",
    "missing",
    ""
  );
  ASSERT_TRUE(missing) << missing.error;
  EXPECT_EQ(missing.policy, profile_policy_e::unspecified);
  EXPECT_FALSE(missing.profile);

  const auto empty = resolve_client_hdr_profile(
    R"([{"uuid":"empty","hdrProfile":""}])",
    "empty",
    ""
  );
  ASSERT_TRUE(empty) << empty.error;
  EXPECT_EQ(empty.policy, profile_policy_e::clear);
  EXPECT_FALSE(empty.profile);
}

TEST(ColorProfileResolver, AcceptsCaseInsensitiveIccAndIcmExtensions) {
  for (const auto *profile : { "display.icc", "display.IcC", "display.icm", "display.ICM" }) {
    const auto clients = std::string { R"([{"uuid":"client","hdrProfile":")" } + profile + R"("}])";
    const auto result = resolve_client_hdr_profile(clients, "client", "");
    ASSERT_TRUE(result) << profile << ": " << result.error;
    ASSERT_EQ(result.policy, profile_policy_e::apply) << profile;
    ASSERT_TRUE(result.profile);
    EXPECT_EQ(*result.profile, profile);
  }
}

TEST(ColorProfileResolver, RejectsPathsQuotesAndUnsupportedExtensions) {
  for (const auto *profile : {
         "../display.icc",
         R"(folder\display.icc)",
         R"(C:\display.icc)",
         R"(bad"name.icc)",
         "bad'name.icm",
         "display.txt",
         ".icc",
       }) {
    const auto clients = nlohmann::json::array({
      {
        {"uuid", "client"},
        {"hdrProfile", profile},
      },
    }).dump();
    const auto result = resolve_client_hdr_profile(clients, "client", "");
    EXPECT_FALSE(result) << profile;
    EXPECT_FALSE(result.error.empty()) << profile;
    EXPECT_EQ(result.policy, profile_policy_e::unspecified) << profile;
    EXPECT_FALSE(result.profile) << profile;
  }
}

TEST(ColorProfileResolver, ReportsMalformedJsonAndFieldsWithoutThrowing) {
  for (const auto *clients : {
         "{",
         R"({"uuid":"client"})",
         R"([42])",
         R"([{"uuid":42,"hdrProfile":"display.icc"}])",
         R"([{"uuid":"client","hdrProfile":null}])",
       }) {
    const auto result = resolve_client_hdr_profile(clients, "client", "");
    EXPECT_FALSE(result) << clients;
    EXPECT_FALSE(result.error.empty()) << clients;
  }
}

TEST(ColorProfileResolver, ToleratesMissingIdentityFields) {
  const auto result = resolve_client_hdr_profile(
    R"([{"hdrProfile":"orphan.icc"},{"uuid":"client","hdrProfile":"display.icc"}])",
    "client",
    ""
  );

  ASSERT_TRUE(result) << result.error;
  EXPECT_EQ(result.policy, profile_policy_e::apply);
  ASSERT_TRUE(result.profile);
  EXPECT_EQ(*result.profile, "display.icc");
}

TEST(ColorProfileResolver, RejectsDuplicateUuid) {
  const auto result = resolve_client_hdr_profile(
    R"([
      {"uuid":"duplicate","hdrProfile":"first.icc"},
      {"uuid":"duplicate","hdrProfile":"second.icc"}
    ])",
    "duplicate",
    ""
  );

  EXPECT_FALSE(result);
  EXPECT_NE(result.error.find("duplicate uuid"), std::string::npos);
}
