#pragma once

#include <memory>

#include "src/nvhttp.h"

namespace nvhttp::apps {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  void
  list(resp_https_t response, req_https_t request);

  void
  asset(resp_https_t response, req_https_t request);

  void
  exec_super_cmd(resp_https_t response, req_https_t request);

}  // namespace nvhttp::apps
