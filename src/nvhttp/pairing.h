#pragma once

#include <memory>
#include <string>

#include <openssl/x509.h>

#include "src/nvhttp.h"

namespace nvhttp::pairing {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  void
  set_credentials(const std::string &pkey, const std::string &cert);

  void
  load_state();

  std::string
  client_uuid_for_cert(const std::string &cert);

  const char *
  verify_client_certificate(X509 *cert, bool close_verify_safe);

  void
  pair_https(resp_https_t response, req_https_t request);

  void
  pair_http(resp_http_t response, req_http_t request);

}  // namespace nvhttp::pairing
