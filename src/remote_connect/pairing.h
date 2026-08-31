#pragma once

#include <cstdint>
#include <string>

namespace remote_connect {

  struct pairing_request_t {
    std::string host;
    std::uint16_t port;
    std::string pin;
    std::string server_name;
    std::int64_t expires_at;
  };

  struct pairing_result_t {
    bool success;
    bool remote;
    std::string host;
    std::string url;
    std::string error;
  };

  pairing_result_t
  create_pairing_invite(pairing_request_t request);

}  // namespace remote_connect
