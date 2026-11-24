#include "invite.h"

#include <stdexcept>

#include "src/nvhttp/url_utils.h"

namespace remote_connect {

  std::string
  build_invite(const invite_t &invite) {
    if (invite.enrollment) {
      throw std::invalid_argument("Secure remote enrollment is not available yet");
    }
    if (invite.host.empty() || invite.pin.empty() || invite.server_name.empty()) {
      throw std::invalid_argument("Pairing invite requires a host, PIN, and server name");
    }

    const auto encode = nvhttp::url_utils::encode;
    std::string url = "moonlight://pair?";
    url += "host=" + encode(invite.host) +
           "&port=" + std::to_string(invite.port) +
           "&pin=" + encode(invite.pin) +
           "&name=" + encode(invite.server_name);

    return url;
  }

}  // namespace remote_connect
