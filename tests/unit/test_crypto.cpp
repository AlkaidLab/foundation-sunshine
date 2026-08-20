/**
 * @file tests/unit/test_crypto.cpp
 * @brief Tests for cryptography helpers.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <gtest/gtest.h>

#include "src/crypto.h"
#include "src/config.h"
#include "src/file_handler.h"
#include "src/httpcommon.h"
#include "src/nvhttp.h"
#include "src/nvhttp/pairing.h"

namespace {
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  std::string
  without_trailing_line_endings(std::string pem) {
    while (!pem.empty() && (pem.back() == '\n' || pem.back() == '\r')) {
      pem.pop_back();
    }
    return pem;
  }

  std::string
  with_crlf_line_endings(const std::string_view pem) {
    std::string result;
    result.reserve(pem.size() + 32);
    for (const char ch : pem) {
      if (ch == '\n') {
        result.push_back('\r');
      }
      result.push_back(ch);
    }
    return result;
  }

  class PairingStateGuard {
  public:
    PairingStateGuard(
      const std::string &cert,
      const std::string &uuid,
      const fs::path &directory = fs::temp_directory_path(),
      bool remove_directory = false):
        state_file_(directory / ("sunshine_pairing_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json")),
        previous_state_file_(config::nvhttp.file_state),
        previous_unique_id_(http::unique_id),
        remove_directory_(remove_directory) {
      fs::create_directories(state_file_.parent_path());

      pt::ptree device;
      device.put("name", "test-client");
      device.put("cert", cert);
      device.put("uuid", uuid);

      pt::ptree state;
      state.put("root.uniqueid", "test-host");
      pt::ptree devices;
      devices.push_back({ "", device });
      state.add_child("root.named_devices", devices);
      std::ofstream state_stream { state_file_ };
      if (!state_stream.is_open()) {
        throw std::runtime_error("unable to open pairing state fixture");
      }
      pt::write_json(state_stream, state);
      state_stream.flush();
      state_stream.close();
      if (!state_stream) {
        throw std::runtime_error("unable to write pairing state fixture");
      }

      config::nvhttp.file_state = file_handler::path_to_utf8(state_file_);
      nvhttp::pairing::load_state();
    }

    ~PairingStateGuard() {
      nvhttp::erase_all_clients();
      config::nvhttp.file_state = previous_state_file_;
      http::unique_id = previous_unique_id_;

      std::error_code ec;
      fs::remove(state_file_, ec);
      if (remove_directory_) {
        fs::remove(state_file_.parent_path(), ec);
      }
    }

    const fs::path &
    state_file() const noexcept {
      return state_file_;
    }

  private:
    fs::path state_file_;
    std::string previous_state_file_;
    std::string previous_unique_id_;
    bool remove_directory_;
  };
}  // namespace

TEST(CryptoX509, MatchesEquivalentPemFormatting) {
  const auto credentials = crypto::gen_creds("format-test-client", 2048);
  const auto certificate = crypto::x509(credentials.x509);
  ASSERT_TRUE(certificate);
  ASSERT_TRUE(credentials.x509.ends_with('\n'));

  EXPECT_TRUE(crypto::x509_matches_pem(certificate.get(), credentials.x509));

  const auto without_trailing_newline = without_trailing_line_endings(credentials.x509);
  EXPECT_TRUE(crypto::x509_matches_pem(certificate.get(), without_trailing_newline));

  const auto with_crlf = with_crlf_line_endings(credentials.x509);
  EXPECT_TRUE(crypto::x509_matches_pem(certificate.get(), with_crlf));
}

TEST(CryptoX509, RejectsInvalidOrDifferentCertificates) {
  const auto first_credentials = crypto::gen_creds("same-subject", 2048);
  const auto second_credentials = crypto::gen_creds("same-subject", 2048);
  const auto first_certificate = crypto::x509(first_credentials.x509);
  ASSERT_TRUE(first_certificate);

  EXPECT_FALSE(crypto::x509_matches_pem(first_certificate.get(), second_credentials.x509));
  EXPECT_FALSE(crypto::x509_matches_pem(first_certificate.get(), "not a certificate"));
  EXPECT_FALSE(crypto::x509_matches_pem(nullptr, first_credentials.x509));
}

TEST(NvhttpPairing, ResolvesUuidForVerifiedPeerCertificate) {
  const auto credentials = crypto::gen_creds("paired-client", 2048);
  auto certificate = crypto::x509(credentials.x509);
  ASSERT_TRUE(certificate);

  const std::string expected_uuid = "paired-client-uuid";
  const auto stored_cert = without_trailing_line_endings(with_crlf_line_endings(credentials.x509));
  PairingStateGuard state { stored_cert, expected_uuid };

  // This mirrors the X509 pointer passed from the TLS handshake callback.
  EXPECT_EQ(nvhttp::pairing::verify_client_certificate(certificate.get(), true), nullptr);
  EXPECT_EQ(nvhttp::pairing::client_uuid_for_cert(certificate.get()), expected_uuid);
}

TEST(NvhttpPairing, ReadsAndWritesStateFromUnicodePath) {
#ifdef _WIN32
  const auto credentials = crypto::gen_creds("unicode-path-client", 2048);
  auto certificate = crypto::x509(credentials.x509);
  ASSERT_TRUE(certificate);

  const auto directory = fs::temp_directory_path() /
                         (fs::path { L"sunshine_配对_路径_" } /
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string expected_uuid = "unicode-path-client-uuid";
  PairingStateGuard state { credentials.x509, expected_uuid, directory, true };

  EXPECT_EQ(nvhttp::pairing::client_uuid_for_cert(certificate.get()), expected_uuid);

  nvhttp::erase_all_clients();
  pt::ptree saved_state;
  std::ifstream state_stream { state.state_file() };
  ASSERT_TRUE(state_stream.is_open());
  ASSERT_NO_THROW(pt::read_json(state_stream, saved_state));
  EXPECT_EQ(saved_state.get<std::string>("root.uniqueid"), "test-host");
  EXPECT_TRUE(saved_state.get_child("root.named_devices").empty());
#else
  GTEST_SKIP() << "Unicode native-path behavior is Windows-specific";
#endif
}
