#pragma once

#include <memory>

#include "src/nvhttp.h"

namespace nvhttp::clipboard_api {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  void
  upload_blob(resp_https_t response, req_https_t request);

  void
  get_blob(resp_https_t response, req_https_t request);

}  // namespace nvhttp::clipboard_api
