#include <array>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/remote_connect/enrollment_proof.h"
#include "src/remote_connect/enrollment_proof_internal.h"

#ifndef ENROLLMENT_V1_VECTOR_PATH
  #define ENROLLMENT_V1_VECTOR_PATH "tests/fixtures/remote-connect/enrollment-v1.json"
#endif

namespace {
  using namespace remote_connect;

  template<std::size_t N>
  std::array<std::uint8_t, N>
  from_hex(const std::string &text) {
    if (text.size() != 2 * N) throw std::runtime_error("Wrong test vector length");
    const auto digit = [](char c) -> std::uint8_t {
      if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
      throw std::runtime_error("Invalid test vector hex");
    };
    std::array<std::uint8_t, N> result {};
    for (std::size_t i = 0; i < N; ++i) result[i] = static_cast<std::uint8_t>((digit(text[i * 2]) << 4) | digit(text[i * 2 + 1]));
    return result;
  }

  struct vector_t {
    enrollment_proof_context_t context;
    enrollment_bytes32_t token;
    enrollment_bytes32_t host_private;
    enrollment_bytes32_t proof;
    enrollment_confirmation_t confirmation;
  };

  vector_t
  load_vector() {
    std::ifstream file(ENROLLMENT_V1_VECTOR_PATH, std::ios::binary);
    if (!file) throw std::runtime_error("Public proof test vector is unavailable");
    const auto json = nlohmann::json::parse(file);
    vector_t v {};
    v.context.version = json.at("version").get<std::uint8_t>();
    v.context.host_certificate_sha256 = from_hex<32>(json.at("host_certificate_der_sha256"));
    v.context.bootstrap_id = from_hex<16>(json.at("bootstrap_id"));
    v.context.challenge = from_hex<32>(json.at("challenge"));
    v.context.host_ephemeral_public = from_hex<32>(json.at("host_x25519_public"));
    v.context.client_public = from_hex<32>(json.at("client_x25519_public"));
    v.context.client_certificate_sha256 = from_hex<32>(json.at("client_certificate_der_sha256"));
    // The independently generated public fixture uses these exact IPv4 values.
    v.context.host_ipv4 = {100, 64, 10, 1};
    v.context.client_ipv4 = {100, 64, 10, 2};
    if (json.at("host_ipv4") != "100.64.10.1" || json.at("client_ipv4") != "100.64.10.2") throw std::runtime_error("Unexpected fixture addresses");
    v.context.enrollment_port = json.at("enrollment_port").get<std::uint16_t>();
    v.context.discovery_peer = json.at("discovery_peer").get<std::string>();
    v.token = from_hex<32>(json.at("token"));
    v.host_private = from_hex<32>(json.at("host_x25519_private"));
    v.proof = from_hex<32>(json.at("proof"));
    v.confirmation = from_hex<8>(json.at("confirmation_sha256").get<std::string>().substr(0, 16));
    return v;
  }

  TEST(RemoteEnrollmentProof, MatchesSharedPublicNodePythonOpenSslVector) {
    const auto v = load_vector();
    const auto result = verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, v.confirmation);
  }

  TEST(RemoteEnrollmentProof, RejectsWrongProofOrPrivateKeyWithoutReturningConfirmation) {
    auto v = load_vector();
    v.proof[0] ^= 1;
    EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
    v = load_vector();
    v.host_private[0] ^= 8;
    EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
    v = load_vector();
    v.context.host_ephemeral_public[0] ^= 1;
    EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
    v = load_vector();
    v.host_private.fill(0);
    EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
  }

  TEST(RemoteEnrollmentProof, RejectsLowOrderPeerKeys) {
    auto v = load_vector();
    v.context.client_public.fill(0);
    EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
    v.context.client_public[0] = 1;
    EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
  }

  TEST(RemoteEnrollmentProof, RejectsEncodingsEquivalentToAllZeroPrivateInput) {
    // Public test values independently produced with Python and OpenSSL from
    // the RFC-vector context and an all-zero host private key. The proof is
    // otherwise correct for this predictable scalar and matching host public.
    auto v = load_vector();
    v.context.host_ephemeral_public = from_hex<32>("2fe57da347cd62431528daac5fbb290730fff684afc4cfc2ed90995f58cb3b74");
    v.proof = from_hex<32>("c3d51f058f475b3fd9ed387ebd7fa0e5e4660abb822ec0c9cc1235a32e251959");
    for (std::uint8_t low = 0; low < 8; ++low) {
      for (const std::uint8_t high : {0x00, 0x40, 0x80, 0xc0}) {
        v.host_private.fill(0);
        v.host_private[0] = low;
        v.host_private[31] = high;
        EXPECT_TRUE(detail::matches_clamped_zero_x25519_private(v.host_private));
        EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
      }
    }
    v.host_private.fill(0);
    v.host_private[0] = 8;
    EXPECT_FALSE(detail::matches_clamped_zero_x25519_private(v.host_private));
    v.host_private.fill(0);
    v.host_private[1] = 1;
    EXPECT_FALSE(detail::matches_clamped_zero_x25519_private(v.host_private));
    v.host_private.fill(0);
    v.host_private[30] = 1;
    EXPECT_FALSE(detail::matches_clamped_zero_x25519_private(v.host_private));
    v.host_private.fill(0);
    v.host_private[31] = 1;
    EXPECT_FALSE(detail::matches_clamped_zero_x25519_private(v.host_private));
    EXPECT_FALSE(detail::matches_clamped_zero_x25519_private(load_vector().host_private));
  }

  TEST(RemoteEnrollmentProof, ExplicitlyRejectsAllZeroDerivedSecret) {
    enrollment_bytes32_t shared {};
    EXPECT_FALSE(detail::nonzero_x25519_shared_secret(shared));
    shared[31] = 1;
    EXPECT_TRUE(detail::nonzero_x25519_shared_secret(shared));
  }

  TEST(RemoteEnrollmentProof, BindsEveryTranscriptField) {
    const auto reference = load_vector();
    auto check = [](const vector_t &v) {
      EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
    };
    auto v = reference; v.token[0] ^= 1; check(v);
    v = reference; v.context.host_certificate_sha256[0] ^= 1; check(v);
    v = reference; v.context.bootstrap_id[0] ^= 1; check(v);
    v = reference; v.context.challenge[0] ^= 1; check(v);
    v = reference; v.context.host_ephemeral_public[0] ^= 1; check(v);
    v = reference; v.context.client_public[0] ^= 1; check(v);
    v = reference; v.context.client_certificate_sha256[0] ^= 1; check(v);
    v = reference; v.context.host_ipv4[3] ^= 1; check(v);
    v = reference; v.context.client_ipv4[3] ^= 1; check(v);
    v = reference; ++v.context.enrollment_port; check(v);
    v = reference; v.context.discovery_peer += "0"; check(v);
  }

  TEST(RemoteEnrollmentProof, RejectsMalformedFraming) {
    const auto reference = load_vector();
    auto check = [](const vector_t &v) {
      EXPECT_FALSE(verify_enrollment_proof_v1(v.context, v.token, v.host_private, v.proof));
    };
    auto v = reference; v.context.version = 0; check(v);
    v = reference; v.context.enrollment_port = 0; check(v);
    v = reference; v.context.discovery_peer = "udp://"; check(v);
    v = reference; v.context.discovery_peer = "tcp://discovery.example:11010"; check(v);
    v = reference; v.context.discovery_peer += "\n"; check(v);
    v = reference; v.context.discovery_peer = "udp://" + std::string(507, 'a'); check(v);
  }
}  // namespace
