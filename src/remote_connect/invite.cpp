#include "invite.h"

#include <stdexcept>

#include "src/nvhttp/url_utils.h"

namespace remote_connect {

  std::string
  build_invite(const invite_t &invite) {
    if (invite.host.empty() || invite.pin.empty() || invite.server_name.empty()) {
      throw std::invalid_argument("Pairing invite requires a host, PIN, and server name");
    }

    const auto encode = nvhttp::url_utils::encode;
    std::string url = "moonlight://pair?";
    if (invite.enrollment) url += "v=2&";
    url += "host=" + encode(invite.host) +
           "&port=" + std::to_string(invite.port) +
           "&pin=" + encode(invite.pin) +
           "&name=" + encode(invite.server_name);

    if (!invite.enrollment) return url;

    const auto &enrollment = *invite.enrollment;
    if (enrollment.profile.empty() || enrollment.virtual_ip.empty() ||
        enrollment.network_name.empty() || enrollment.network_secret.empty() ||
        enrollment.peer.empty() || invite.expires_at <= 0) {
      throw std::invalid_argument("Remote pairing invite has incomplete enrollment data");
    }

    url += "&profile=" + encode(enrollment.profile) +
           "&et_host=" + encode(enrollment.virtual_ip) +
           "&et_name=" + encode(enrollment.network_name) +
           "&et_secret=" + encode(enrollment.network_secret) +
           "&et_peer=" + encode(enrollment.peer) +
           "&expires=" + std::to_string(invite.expires_at);
    return url;
  }

}  // namespace remote_connect
