/**
 * @file tests/unit/test_client_fingerprint.cpp
 * @brief Tests for conservative client fingerprint detection.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <gtest/gtest.h>

#include <src/client_fingerprint.h>
#include <src/crypto.h>
#include <src/logging.h>

#ifdef CLIENT_FINGERPRINT_STANDALONE_TEST
boost::log::sources::severity_logger<int> verbose(0);
boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> info(2);
boost::log::sources::severity_logger<int> warning(3);
boost::log::sources::severity_logger<int> error(4);
boost::log::sources::severity_logger<int> fatal(5);
#endif

namespace {
  using args_t = std::multimap<std::string, std::string>;

  args_t
  suspected_fork_args() {
    return {
      {"virtualDisplay", "2"},
      {"virtualDisplayMode", "2400x1080x120"},
      {"devicenickname", "Example-Manufacturer-Example-Model"},
      {"ppi", "393"},
      {"screen_resolution", "2400x1080"},
      {"timeToTerminateApp", "-1"},
      {"UIScale", "200"},
    };
  }

  std::string
  base64(std::string_view value) {
    std::string encoded(4 * ((value.size() + 2) / 3), '\0');
    const auto size = EVP_EncodeBlock(
      reinterpret_cast<unsigned char *>(encoded.data()),
      reinterpret_cast<const unsigned char *>(value.data()),
      static_cast<int>(value.size())
    );
    encoded.resize(static_cast<std::size_t>(size));
    return encoded;
  }

  const crypto::creds_t &
  signing_credentials() {
    static const auto credentials = crypto::gen_creds("client-fingerprint-test", 2048);
    return credentials;
  }

  std::string
  rule_payload(std::uint64_t revision) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()
    ).count();
    return nlohmann::json {
      {"schema_version", 1},
      {"revision", revision},
      {"issued_at", now - 60},
      {"expires_at", now + 24 * 60 * 60},
      {"rules", nlohmann::json::array({
                  {
                    {"id", "dynamic-test-client"},
                    {"enabled", true},
                    {"confidence", "high"},
                    {"action", "warn"},
                    {"message_key", client_fingerprint::suspicious_client_code},
                    {"all", nlohmann::json::array({
                              {
                                {"query", "clientversion"},
                                {"op", "prefix"},
                                {"value", "unpublished-fork/"},
                              },
                            })},
                  },
                })},
    }
      .dump();
  }

  std::string
  signed_envelope(std::uint64_t revision) {
    const auto payload = rule_payload(revision);
    const auto private_key = crypto::pkey(signing_credentials().pkey);
    const auto signature = crypto::sign256(private_key, payload);
    const std::string_view signature_view {
      reinterpret_cast<const char *>(signature.data()),
      signature.size(),
    };
    return nlohmann::json {
      {"payload", base64(payload)},
      {"signature", base64(signature_view)},
    }
      .dump();
  }
}  // namespace

TEST(ClientFingerprintTest, MatchesCompleteDistinctiveParameterBundle) {
  EXPECT_TRUE(client_fingerprint::is_highly_suspected_unknown_client(suspected_fork_args()));
}

TEST(ClientFingerprintTest, DoesNotMatchOrdinaryMoonlightLaunch) {
  const args_t args {
    {"appid", "1"},
    {"clientname", "Android"},
    {"mode", "1920x1080x60"},
    {"uniqueid", "0123456789abcdef"},
  };

  EXPECT_FALSE(client_fingerprint::is_highly_suspected_unknown_client(args));
}

TEST(ClientFingerprintTest, RequiresEveryDistinctiveParameter) {
  static constexpr const char *required_parameters[] {
    "virtualDisplay",
    "virtualDisplayMode",
    "devicenickname",
    "ppi",
    "screen_resolution",
    "timeToTerminateApp",
    "UIScale",
  };

  for (const auto *parameter : required_parameters) {
    auto args = suspected_fork_args();
    args.erase(parameter);
    EXPECT_FALSE(client_fingerprint::is_highly_suspected_unknown_client(args))
      << "Removing " << parameter << " must prevent a high-confidence match";
  }
}

TEST(ClientFingerprintTest, RejectsDisabledOrMismatchedSignatureValues) {
  auto args = suspected_fork_args();
  args.find("virtualDisplay")->second = "0";
  EXPECT_FALSE(client_fingerprint::is_highly_suspected_unknown_client(args));

  args = suspected_fork_args();
  args.find("timeToTerminateApp")->second = "300";
  EXPECT_FALSE(client_fingerprint::is_highly_suspected_unknown_client(args));

  args = suspected_fork_args();
  args.find("UIScale")->second = "150";
  EXPECT_FALSE(client_fingerprint::is_highly_suspected_unknown_client(args));
}

TEST(ClientFingerprintTest, VerifiesSignedRuleEnvelope) {
  const auto parsed = client_fingerprint::parse_signed_rules(
    signed_envelope(7),
    signing_credentials().x509
  );
  ASSERT_TRUE(parsed) << parsed.error;
  EXPECT_EQ(parsed.rules->revision, 7);
  ASSERT_EQ(parsed.rules->rules.size(), 1);
  EXPECT_EQ(parsed.rules->rules.front().id, "dynamic-test-client");

  auto tampered = nlohmann::json::parse(signed_envelope(8));
  auto payload = tampered.at("payload").get<std::string>();
  payload[payload.size() / 2] = payload[payload.size() / 2] == 'A' ? 'B' : 'A';
  tampered["payload"] = payload;
  const auto rejected = client_fingerprint::parse_signed_rules(
    tampered.dump(),
    signing_credentials().x509
  );
  EXPECT_FALSE(rejected);
  EXPECT_NE(rejected.error.find("signature"), std::string::npos);
}

TEST(ClientFingerprintTest, InstallsNewRevisionAndRejectsRollback) {
  const auto test_directory =
    std::filesystem::temp_directory_path() /
    ("sunshine-client-fingerprint-" + crypto::rand_alphabet(
                                        12,
                                        "abcdefghijklmnopqrstuvwxyz0123456789"
                                      ));
  ASSERT_TRUE(std::filesystem::create_directories(test_directory));
  const auto certificate_file = test_directory / "rules.pem";
  const auto cache_file = test_directory / "rules.json";
  {
    std::ofstream certificate {certificate_file, std::ios::binary};
    ASSERT_TRUE(certificate);
    certificate << signing_credentials().x509;
  }

  auto guard = client_fingerprint::init({
    .remote_rules_enabled = true,
    .signing_certificate = certificate_file,
    .cache_file = cache_file,
  });
  ASSERT_TRUE(guard);

  const auto installed = client_fingerprint::install_signed_rules(signed_envelope(10));
  ASSERT_TRUE(installed) << installed.error;
  EXPECT_TRUE(installed.installed);
  EXPECT_EQ(installed.revision, 10);
  EXPECT_TRUE(std::filesystem::exists(cache_file));

  const args_t dynamic_args {
    {"clientversion", "unpublished-fork/1.0"},
  };
  const auto matched = client_fingerprint::match_client(dynamic_args);
  EXPECT_TRUE(matched.suspicious);
  EXPECT_EQ(matched.rule_id, "dynamic-test-client");
  EXPECT_EQ(matched.revision, 10);
  EXPECT_EQ(matched.source, "remote");

  const auto unchanged = client_fingerprint::install_signed_rules(signed_envelope(10));
  EXPECT_TRUE(unchanged);
  EXPECT_TRUE(unchanged.unchanged);

  const auto rollback = client_fingerprint::install_signed_rules(signed_envelope(9));
  EXPECT_FALSE(rollback);
  EXPECT_NE(rollback.error.find("roll back"), std::string::npos);

  guard.reset();
  std::error_code ec;
  std::filesystem::remove_all(test_directory, ec);
}
