#pragma once

#include <memory>

#include "src/nvhttp.h"

namespace nvhttp::display_control {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  void
  get_displays(resp_https_t response, req_https_t request);

  void
  rotate(resp_https_t response, req_https_t request);

  // Sets which display is captured for subsequent stream sessions by writing
  // config::video.output_name in memory (takes effect on the next launch without
  // a Sunshine restart). Authenticated by the paired client certificate, so paired
  // Moonlight clients can switch displays with no web username/password.
  // Query params: device_id=<stable display device id> (required).
  void
  set_output(resp_https_t response, req_https_t request);

}  // namespace nvhttp::display_control
