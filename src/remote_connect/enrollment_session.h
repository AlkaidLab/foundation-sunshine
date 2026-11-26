#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "enrollment_proof.h"
#include "enrollment_token.h"

namespace remote_connect {
  struct enrollment_invitation_context_t {
    enrollment_bytes32_t host_certificate_sha256 {};
    enrollment_bytes16_t bootstrap_id {};
    enrollment_ipv4_t host_ipv4 {};
    enrollment_ipv4_t client_ipv4 {};
    std::uint16_t enrollment_port = 0;
    std::string discovery_peer;
  };

  // Isolated in-memory primitive. No endpoint or credential activation uses
  // it. The future TLS transport MUST assign connection IDs locally, never
  // reuse them across connections, and never trust a network-provided ID. The
  // caller must independently authenticate the host/admin, validate URI
  // semantics and keep invitation tokens secret. Deadlines are checked at
  // each operation; future integration must poll or schedule teardown so an
  // idle challenge's temporary key is promptly erased after expiration.
  class enrollment_session_t {
  public:
    using clock_t = enrollment_token_store_t::clock_t;
    using clock_source_t = enrollment_token_store_t::clock_source_t;
    using connection_id_t = std::uint64_t;

    struct invitation_t {
      std::uint64_t generation;
      enrollment_bytes32_t token;
    };

    struct challenge_t {
      enrollment_bytes32_t challenge;
      enrollment_bytes32_t host_ephemeral_public;
    };

    enum class state_t {
      inactive,
      awaiting_connection,
      awaiting_proof,
      awaiting_confirmation,
      candidate_confirmed_not_authorized,
    };

    enrollment_session_t();
    // For deterministic tests only; production callers use the default
    // steady clock and must never accept a clock from the network.
    explicit enrollment_session_t(clock_source_t clock_source);
    ~enrollment_session_t();
    enrollment_session_t(const enrollment_session_t &) = delete;
    enrollment_session_t &operator=(const enrollment_session_t &) = delete;

    // At most one invitation is live. Issuing a replacement invalidates the
    // previous one, including on failure. The caller owns the returned token.
    // The generation is unique only within this object's lifetime; a future
    // network API needs a separate unpredictable process/session epoch so an
    // old request cannot match a new invitation after a process restart.
    std::optional<invitation_t>
    issue(const enrollment_invitation_context_t &context, std::chrono::seconds ttl);

    // ID must identify one actual, live TLS connection, provided by the trusted
    // transport, not by the request body. Tokens are checked without consuming.
    std::optional<challenge_t>
    challenge(std::uint64_t generation, const enrollment_bytes32_t &token, connection_id_t connection);

    // A correctly bound attempt burns its challenge before proof verification.
    // On success the token is consumed EARLY and the comparison code is only
    // awaiting an owner decision: no device or credential is authorized.
    std::optional<enrollment_confirmation_t>
    prove(std::uint64_t generation, const enrollment_bytes32_t &token, connection_id_t connection,
          const enrollment_bytes32_t &client_public, const enrollment_bytes32_t &client_certificate_sha256,
          const enrollment_bytes32_t &proof);

    // What an owner-confirmed candidate consists of. It is handed over once,
    // by take_confirmed_candidate(), and cannot be retrieved again.
    struct confirmed_candidate_t {
      enrollment_bytes32_t client_public;
      enrollment_bytes32_t client_certificate_sha256;
    };

    // Called only after a trusted host UI has asked the user to compare codes.
    // True means "candidate confirmed", NOT that it is persisted or authorized.
    bool
    confirm_candidate(std::uint64_t generation, const enrollment_confirmation_t &compared_code);

    // Hands the confirmed candidate to the caller and ends the invitation.
    // Succeeds only for a generation that is still unexpired and in the
    // candidate_confirmed_not_authorized state, so a stale invitation, an
    // unconfirmed candidate and a second call all fail. After it returns the
    // candidate cannot be cancelled: cancel() reports false from here on, and
    // the caller owns committing or discarding it.
    std::optional<confirmed_candidate_t>
    take_confirmed_candidate(std::uint64_t generation);

    // A stale generation/connection cannot affect a newer invitation. Even a
    // confirmed candidate is destroyed; there is no commit/activation API yet.
    bool
    cancel(std::uint64_t generation);
    void
    connection_closed(connection_id_t connection);
    state_t
    state(std::uint64_t generation);

  private:
    bool
    current_locked(std::uint64_t generation);
    void
    clear_challenge_locked();
    void
    clear_locked();
    void
    reject_attempt_locked();

    std::mutex mutex_;
    clock_source_t clock_source_;
    enrollment_token_store_t token_store_; // never exposed; outer lock is always held
    enrollment_invitation_context_t context_ {};
    std::uint64_t last_generation_ = 0;
    std::uint64_t generation_ = 0;
    clock_t::time_point expires_at_ {};
    clock_t::time_point challenge_expires_at_ {};
    state_t state_ = state_t::inactive;
    connection_id_t connection_ = 0;
    unsigned int attempts_ = 0;
    enrollment_bytes32_t host_private_ {};
    enrollment_bytes32_t host_public_ {};
    enrollment_bytes32_t challenge_ {};
    enrollment_confirmation_t confirmation_ {};
    // Keep the exact proof-bound identity private to this state machine; no
    // current API can retrieve or activate it.
    enrollment_bytes32_t candidate_public_ {};
    enrollment_bytes32_t candidate_certificate_sha256_ {};
  };
}  // namespace remote_connect
