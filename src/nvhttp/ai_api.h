#pragma once

#include <memory>

#include "src/nvhttp.h"

namespace nvhttp::ai_api {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  void
  completions(resp_https_t response, req_https_t request);

}  // namespace nvhttp::ai_api
