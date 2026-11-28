#pragma once

#include <optional>
#include <string>

namespace remote_connect {

  inline constexpr char default_peer[] = "udp://public.easytier.top:11010";

  struct enrollment_t {
    std::string profile;
    std::string virtual_ip;
    std::string network_name;
    std::string network_secret;
    std::string peer;
  };

  struct status_t {
    bool enabled;
    bool running;
    bool available;
    std::string virtual_ip;
    std::string error;
  };

  struct operation_result_t {
    bool success;
    status_t status;
  };

  struct pairing_state_t {
    bool success;
    bool enabled;
    std::optional<enrollment_t> enrollment;
    std::string error;
  };

}  // namespace remote_connect
