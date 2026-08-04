#pragma once

#include <memory>

#include "src/nvhttp.h"
#include "src/nvhttp/network_probe_limiter.h"

namespace nvhttp::network_probe {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;

  class service_t {
  public:
    service_t();

    void capabilities(resp_https_t response, req_https_t request);
    void probe(resp_https_t response, req_https_t request);

  private:
    struct impl_t;
    std::shared_ptr<impl_t> impl_;
  };
}  // namespace nvhttp::network_probe
