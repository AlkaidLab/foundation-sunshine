#pragma once

#include <memory>

#include <nlohmann/json_fwd.hpp>

#include "src/nvhttp.h"

namespace stream {
  struct session_info_t;
}

namespace nvhttp::sessions {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  nlohmann::json
  make_session_json(const stream::session_info_t &session_info);

  void
  get(resp_https_t response, req_https_t request);

}  // namespace nvhttp::sessions
