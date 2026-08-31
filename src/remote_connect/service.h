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

}  // namespace remote_connect
