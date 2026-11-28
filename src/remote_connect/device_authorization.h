#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "credential_transaction.h"
#include "enrollment_session.h"

namespace remote_connect {
  // The two side effects of an authorization decision. The owner of the
  // stores and of the runtime supplies them; tests supply fakes. They run
  // under the authorizer's lock and must not call back into it.
  struct device_authorization_effects_t {
    // Durably stores the record, or returns false. A false return makes the
    // authorizer refuse every operation until adopt() succeeds again.
    std::function<bool(const credential_transaction::record_t &record)> persist;
    // Makes the runtime's authorized client key set exactly `keys`, given as
    // lowercase hex, restarting whatever has to be restarted for that. An
    // empty vector closes remote access entirely.
    std::function<bool(const std::vector<std::string> &keys)> apply;
    // Supplies a fresh transaction id, or std::nullopt when it cannot.
    std::function<std::optional<credential_transaction::transaction_id_t>()> next_transaction_id;
  };

  struct authorization_result_t {
    bool success;
    // Empty on success. On failure it says what state things were left in.
    std::string error;
  };

  // Turns an owner-confirmed enrollment candidate into an authorized device,
  // and an authorized device into a revoked one, through the
  // credential_transaction reducer.
  //
  // The device record is the single source of truth for who is authorized;
  // the runtime's key set is only a projection of it. Every step stores the
  // new record before it touches the runtime, so whatever fails, the runtime
  // never ends up more permissive than the stored record. Where the runtime's
  // state becomes unknown, it is closed.
  //
  // This class authenticates nobody. It trusts only what enrollment_session_t
  // hands over, and only for the generation the caller names. Nothing wires it
  // to a network endpoint yet.
  class device_authorizer_t {
  public:
    device_authorizer_t(enrollment_session_t &session, device_authorization_effects_t effects);

    device_authorizer_t(const device_authorizer_t &) = delete;
    device_authorizer_t &
    operator=(const device_authorizer_t &) = delete;

    // Adopts the record read at startup and aligns the runtime with it. A
    // record left mid-transaction by a crash is recovered to `stable` first: a
    // candidate that was not committed is dropped, and a device whose
    // revocation had started stays revoked.
    authorization_result_t
    adopt(credential_transaction::record_t record);

    // The authorized devices, in id order.
    std::vector<credential_transaction::device_t>
    devices() const;

    // Moves the candidate the owner confirmed for `generation` into the
    // authorized set as `device_id`. The candidate is consumed whatever
    // happens: if the device cannot be added, the phone has to enroll again.
    authorization_result_t
    authorize(std::uint64_t generation, std::string_view device_id);

    // Removes the device for good. Its key and certificate can never be
    // enrolled again; the phone has to bring fresh ones.
    authorization_result_t
    revoke(std::string_view device_id);

  private:
    // Applies one reducer step. The in-memory record changes only once the
    // new revision is durably stored. `candidate` is set only by prepare_add
    // and `target_id` only by prepare_revoke.
    bool
    step_locked(credential_transaction::action_t action, std::optional<credential_transaction::device_t> candidate = std::nullopt, std::string target_id = {});

    bool
    apply_locked(const std::vector<std::string> &keys);

    // Draws the id for a new transaction from the effect.
    bool
    start_transaction_locked();

    mutable std::mutex mutex_;
    enrollment_session_t &session_;
    device_authorization_effects_t effects_;
    credential_transaction::record_t record_;
    credential_transaction::transaction_id_t transaction_id_ {};
    // False until a record has been adopted, and again once storing fails.
    bool usable_ = false;
  };
}  // namespace remote_connect
