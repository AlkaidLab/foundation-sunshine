#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "types.h"

namespace remote_connect {

  struct invite_t {
    std::string host;
    std::uint16_t port;
    std::string pin;
    std::string server_name;
    std::optional<enrollment_t> enrollment;
    std::int64_t expires_at;
  };

  std::string
  build_invite(const invite_t &invite);

}  // namespace remote_connect
