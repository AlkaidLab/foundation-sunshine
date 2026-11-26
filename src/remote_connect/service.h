#pragma once

#include "types.h"

namespace remote_connect {
  status_t
  status();

  // Atomically observe the enabled state, start the runtime when needed, and
  // snapshot the credentials used by that runtime.
  pairing_state_t
  prepare_pairing();

  void
  stop();

  void
  start_if_enabled();

  operation_result_t
  set_enabled(bool enabled);

  operation_result_t
  reset_enrollment();

  // Registers or removes one enrolled remote client public key (64 hex
  // characters, X25519). The caller must have authenticated the key through
  // the enrollment protocol first; these functions only persist and apply it.
  //
  // Both are idempotent and serialized against every other service operation.
  // The whole list is persisted before the in-memory value changes, and a
  // running core is restarted so the new policy takes effect; if the restart
  // fails the previous list is restored. The first enrolled key turns this
  // host strict, which disconnects any client that is not enrolled.
  operation_result_t
  enroll_strict_client_key(const std::string &key_hex);

  operation_result_t
  revoke_strict_client_key(const std::string &key_hex);

}  // namespace remote_connect
