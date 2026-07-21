#pragma once

#include <memory>

#include "src/nvhttp.h"

namespace nvhttp::dynamic_params {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  void
  change_bitrate(resp_https_t response, req_https_t request);

  void
  change(resp_https_t response, req_https_t request);

}  // namespace nvhttp::dynamic_params
