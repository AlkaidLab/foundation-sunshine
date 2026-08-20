#pragma once

#include <memory>

#include "src/nvhttp.h"

namespace nvhttp::abr_api {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  void
  capabilities(resp_https_t response, req_https_t request);

  void
  configure(resp_https_t response, req_https_t request);

  void
  feedback(resp_https_t response, req_https_t request);

}  // namespace nvhttp::abr_api
