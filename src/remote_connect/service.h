#pragma once

#include <string>

namespace remote_connect {

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

  status_t
  status();

  enrollment_t
  enrollment();

  bool
  start();

  void
  stop();

  void
  start_if_enabled();

  operation_result_t
  set_enabled(bool enabled);

  operation_result_t
  reset_enrollment();

}  // namespace remote_connect
