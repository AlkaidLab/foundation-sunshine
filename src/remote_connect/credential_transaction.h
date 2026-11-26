#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace remote_connect::credential_transaction {
  using transaction_id_t = std::array<std::uint8_t, 16>;
  using public_key_t = std::array<std::uint8_t, 32>;

  struct device_t {
    std::string id;
    public_key_t easytier_public_key {};
    public_key_t gamestream_certificate_sha256 {};
    bool operator==(const device_t &) const = default;
  };

  // Every non-stable phase is understood to have business access closed.
  // The reducer cannot enforce a gate, validate caller authentication, run a
  // core, persist a record, or acknowledge a device as authorized.
  enum class phase_t {
    stable,
    add_prepared,
    add_verified_closed,
    add_committed_closed,
    add_rollback_closed,
    revoke_pending,
    revoke_verified_closed,
    revoke_committed_closed,
    recovery_closed,
  };

  struct record_t {
    std::uint64_t revision = 0;
    phase_t phase = phase_t::stable;
    std::map<std::string, device_t> active;
    // A revoked ID, EasyTier key and GameStream certificate fingerprint must
    // never reappear. Re-enrollment currently requires fresh identities; this
    // reducer has no tombstone-retirement operation.
    std::map<std::string, device_t> tombstones;
    std::optional<device_t> candidate;
    std::optional<std::string> revoke_target;
    transaction_id_t transaction_id {};
    transaction_id_t last_transaction_id {};
  };

  enum class action_t {
    prepare_add,
    prepare_revoke,
    add_verified,
    commit_add,
    abort_add,
    revoke_verified,
    commit_revoke,
    finish_closed,
    recover_closed,
  };

  struct event_t {
    action_t action;
    std::uint64_t expected_revision;
    transaction_id_t transaction_id {};
    std::optional<device_t> candidate;
    std::string target_id;
  };

  // True when `record` is a well-formed record in a known phase: every device
  // has a valid id and two distinct non-zero keys, tombstones and active
  // devices never share an id, a key or a certificate, and the phase agrees
  // with the candidate and revoke target it carries. reduce() applies the same
  // check to whatever it is given, so a record read from storage should be
  // checked with this before anything is built on it.
  bool
  valid(const record_t &record);

  // Pure value transformation. The caller must atomically compare/persist
  // the returned revision before any corresponding side effect. The caller
  // must independently verify that a closed gate, core shutdown/readiness,
  // owner confirmation, public-key validity and client identity actually exist
  // before submitting the relevant event. Nonzero bytes alone do not prove a
  // valid or authenticated key. CAS, persistence and core interfaces are not provided.
  // This primitive only detects replay with a stale revision/current or last
  // transaction ID; network API replay protection and durable IDs are future
  // integration requirements.
  std::optional<record_t>
  reduce(const record_t &before, const event_t &event);
}  // namespace remote_connect::credential_transaction
