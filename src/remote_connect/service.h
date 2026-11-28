#pragma once

#include <string>
#include <vector>

#include "credential_transaction.h"
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

  // The remote devices this host authorizes, in id order.
  //
  // The device record is the single source of truth for who is authorized.
  // The EasyTier core is told the keys of these devices each time it starts,
  // and nowhere else. Returns an empty list when the record could not be
  // adopted; status() then carries the reason.
  std::vector<credential_transaction::device_t>
  devices();

  // Revokes a device for good: its key and certificate can never be enrolled
  // again. The core is restarted with the device's key removed, which
  // interrupts every current remote session on this host. This ends the
  // device's access to the virtual network; a GameStream pairing it already
  // holds is managed by the clients page.
  operation_result_t
  revoke_device(const std::string &device_id);

}  // namespace remote_connect
