/**
 * @file tests/unit/test_crypto.cpp
 * @brief Tests for cryptography helpers.
 */

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "src/crypto.h"

namespace {
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
