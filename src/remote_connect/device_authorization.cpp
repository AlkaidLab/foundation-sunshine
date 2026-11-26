#include "device_authorization.h"

#include <algorithm>
#include <utility>

#include "strict_policy.h"

namespace remote_connect {
  namespace {
    using credential_transaction::action_t;
    using credential_transaction::device_t;
    using credential_transaction::phase_t;
    using credential_transaction::record_t;

    // The in-memory record is the revision that is on disk, or is unrecovered;
    // nothing may be built on it.
    constexpr char unavailable_error[] =
      "Remote device authorization is unavailable until the stored device record is restored; restart Sunshine";
    // The stored record is sound but an operation was left open.
    constexpr char unfinished_error[] =
      "The device record holds an unfinished operation; restart Sunshine to recover it";

    authorization_result_t
    failure(std::string error) {
      return { false, std::move(error) };
    }

    std::vector<std::string>
    active_keys(const record_t &record) {
      std::vector<std::string> keys;
      keys.reserve(record.active.size());
      for (const auto &[id, device] : record.active) {
        (void) id;
        keys.push_back(strict_policy::key_hex(device.easytier_public_key));
      }
      return keys;
    }

    bool
    valid_device_id(std::string_view id) {
      return !id.empty() && id.size() <= 64 &&
             std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
             });
    }
  }  // namespace

  device_authorizer_t::device_authorizer_t(enrollment_session_t &session, device_authorization_effects_t effects):
      session_(session), effects_(std::move(effects)) {}

  bool
  device_authorizer_t::start_transaction_locked() {
    if (!effects_.next_transaction_id) return false;
    const auto id = effects_.next_transaction_id();
    if (!id) return false;
    transaction_id_ = *id;
    return true;
  }

  bool
  device_authorizer_t::step_locked(action_t action, std::optional<device_t> candidate, std::string target_id) {
    credential_transaction::event_t event;
    event.action = action;
    event.expected_revision = record_.revision;
    event.transaction_id = transaction_id_;
    event.candidate = std::move(candidate);
    event.target_id = std::move(target_id);

    auto after = credential_transaction::reduce(record_, event);
    if (!after) return false;
    if (!effects_.persist || !effects_.persist(*after)) {
      usable_ = false;
      return false;
    }
    record_ = std::move(*after);
    return true;
  }

  bool
  device_authorizer_t::apply_locked(const std::vector<std::string> &keys) {
    return effects_.apply && effects_.apply(keys);
  }

  authorization_result_t
  device_authorizer_t::adopt(record_t record) {
    std::lock_guard lock(mutex_);
    usable_ = false;
    transaction_id_ = {};

    if (!credential_transaction::valid(record)) {
      record_ = {};
      return failure("The stored device record is invalid; no remote device is authorized until it is repaired");
    }
    record_ = std::move(record);

    if (record_.phase != phase_t::stable) {
      // Recovery continues the transaction the crash interrupted, so it has to
      // carry the id that transaction was started with.
      transaction_id_ = record_.transaction_id;
      if (!step_locked(action_t::recover_closed) || !step_locked(action_t::finish_closed)) {
        return failure(usable_ ? "The interrupted device operation could not be recovered" : unavailable_error);
      }
    }
    transaction_id_ = {};

    // The record is sound and stored from here on, so it stays adopted even
    // when the runtime is not aligned with it yet.
    usable_ = true;
    if (!apply_locked(active_keys(record_))) {
      // The runtime's key set is unknown now. An empty set is the one state
      // that cannot be more permissive than the record.
      const bool closed = apply_locked({});
      return failure(
        closed ? "The remote devices are recorded, but the runtime did not accept them; remote access is closed"
               : "The remote devices are recorded, but the runtime state is unknown"
      );
    }
    return { true, {} };
  }

  std::vector<device_t>
  device_authorizer_t::devices() const {
    std::lock_guard lock(mutex_);
    std::vector<device_t> out;
    out.reserve(record_.active.size());
    for (const auto &[id, device] : record_.active) {
      (void) id;
      out.push_back(device);
    }
    return out;
  }

  authorization_result_t
  device_authorizer_t::authorize(std::uint64_t generation, std::string_view device_id) {
    std::lock_guard lock(mutex_);
    if (!usable_) return failure(unavailable_error);
    if (record_.phase != phase_t::stable) return failure(unfinished_error);
    if (!valid_device_id(device_id)) {
      return failure("A device id must be 1 to 64 letters, digits, hyphens or underscores");
    }

    if (!start_transaction_locked()) return failure("Unable to start an authorization transaction");

    const auto candidate = session_.take_confirmed_candidate(generation);
    if (!candidate) {
      return failure("There is no confirmed enrollment for this invitation; it has expired, was cancelled or was already used");
    }

    const device_t device { std::string(device_id), candidate->client_public, candidate->client_certificate_sha256 };
    const bool added = step_locked(action_t::prepare_add, device) && step_locked(action_t::add_verified) &&
                       step_locked(action_t::commit_add);
    if (!added) {
      return failure(
        usable_ ? "The device was not authorized: its id, key or certificate is already enrolled or was revoked"
                : "The device record could not be stored; the device was not authorized"
      );
    }

    // The record now authorizes the device. The runtime is aligned with it
    // before the transaction is closed.
    if (!apply_locked(active_keys(record_))) {
      const bool closed = apply_locked({});
      // Leave the transaction open on purpose: the next adopt() recovers it
      // and aligns the runtime again.
      return failure(
        closed
          ? "The device is recorded, but the runtime did not accept it; remote access is closed until Sunshine restarts"
          : "The device is recorded, but the runtime state is unknown; restart Sunshine before relying on remote access"
      );
    }
    if (!step_locked(action_t::finish_closed)) return failure(unavailable_error);
    return { true, {} };
  }

  authorization_result_t
  device_authorizer_t::revoke(std::string_view device_id) {
    std::lock_guard lock(mutex_);
    if (!usable_) return failure(unavailable_error);
    if (record_.phase != phase_t::stable) return failure(unfinished_error);
    if (!valid_device_id(device_id)) {
      return failure("A device id must be 1 to 64 letters, digits, hyphens or underscores");
    }
    if (!start_transaction_locked()) return failure("Unable to start a revocation transaction");

    if (!step_locked(action_t::prepare_revoke, std::nullopt, std::string(device_id))) {      return failure(usable_ ? "That device is not enrolled" : "The device record could not be stored; the device was not revoked");
    }

    // The record stopped authorizing the device the moment that was stored.
    if (!apply_locked(active_keys(record_))) {
      const bool closed = apply_locked({});
      return failure(
        closed
          ? "The device is revoked, but the runtime did not accept the change; remote access is closed until Sunshine restarts"
          : "The device is revoked in the record, but the runtime state is unknown; restart Sunshine to close its access"
      );
    }
    if (!step_locked(action_t::revoke_verified) || !step_locked(action_t::commit_revoke) ||
        !step_locked(action_t::finish_closed)) {
      return failure(unavailable_error);
    }
    return { true, {} };
  }
}  // namespace remote_connect
