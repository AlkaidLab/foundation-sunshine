#pragma once

#include "types.h"

namespace remote_connect {
  status_t
  status();

  enrollment_t
  enrollment();

  bool
  start();

  void
  stop();

  void
  start_if_enabled();

  operation_result_t
  set_enabled(bool enabled);

  operation_result_t
  reset_enrollment();

}  // namespace remote_connect
