#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/nvhttp.h"

namespace nvhttp::display_scale {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

#ifdef _WIN32
  nlohmann::json
  build_windows_displays(const std::vector<std::string> &display_names);
#endif

  void
  get_options(resp_https_t response, req_https_t request);

  void
  set(resp_https_t response, req_https_t request);

}  // namespace nvhttp::display_scale
