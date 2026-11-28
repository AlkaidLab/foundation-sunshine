#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace remote_connect {
  using enrollment_bytes32_t = std::array<std::uint8_t, 32>;
  using enrollment_bytes16_t = std::array<std::uint8_t, 16>;
  using enrollment_ipv4_t = std::array<std::uint8_t, 4>;

  // Public, already-parsed transcript fields. Certificate hashes, addresses,
  // challenge lifetime and the discovery URI's host/port still need to be
  // authenticated and validated by the future protocol implementation.
  struct enrollment_proof_context_t {
    std::uint8_t version = 1;
    enrollment_bytes32_t host_certificate_sha256 {};
    enrollment_bytes16_t bootstrap_id {};
    enrollment_bytes32_t challenge {};
    enrollment_bytes32_t host_ephemeral_public {};
    enrollment_bytes32_t client_public {};
    enrollment_bytes32_t client_certificate_sha256 {};
    enrollment_ipv4_t host_ipv4 {};
    enrollment_ipv4_t client_ipv4 {};
    std::uint16_t enrollment_port = 0;
    std::string discovery_peer;
  };

  using enrollment_confirmation_t = std::array<std::uint8_t, 8>;

  // Verify the draft v1 proof, returning the comparison-code bytes only on
  // success. This is NOT an authorization decision: it does not validate TLS,
  // issue/consume tokens, enforce expiry, verify the GameStream client key,
  // perform human confirmation, or install credentials. The caller owns and
  // must securely dispose of host_private and token. This function stores
  // neither input persistently and clears intermediate key material.
  std::optional<enrollment_confirmation_t>
  verify_enrollment_proof_v1(
    const enrollment_proof_context_t &context,
    const enrollment_bytes32_t &token,
    const enrollment_bytes32_t &host_private,
    const enrollment_bytes32_t &proof
  );
}  // namespace remote_connect
