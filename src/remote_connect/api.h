#pragma once

#include <memory>

#include "src/confighttp/types.h"

namespace remote_connect::api {
  using resp_https_t = confighttp::resp_https_t;
  using req_https_t = confighttp::req_https_t;

  // These handlers are called after the Web UI layer has authenticated the request.
  void
  get_status(resp_https_t response);

  void
  set_enabled(resp_https_t response, req_https_t request);

  void
  reset_enrollment(resp_https_t response);

}  // namespace remote_connect::api
