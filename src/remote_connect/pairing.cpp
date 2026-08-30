#include "pairing.h"

#include <exception>
#include <optional>
#include <utility>

#include "invite.h"
#include "service.h"
#include "src/config.h"

namespace remote_connect {

  pairing_result_t
  create_pairing_invite(pairing_request_t request) {
    const bool use_remote_connect = config::nvhttp.remote_connect_enabled;
    std::optional<enrollment_t> remote_enrollment;

    if (use_remote_connect) {
      if (!start()) {
        return { false, true, {}, {}, status().error };
      }
      remote_enrollment = enrollment();
      request.host = remote_enrollment->virtual_ip;
    }

    try {
      auto url = build_invite({
        request.host,
        request.port,
        std::move(request.pin),
        std::move(request.server_name),
        std::move(remote_enrollment),
        request.expires_at,
      });
      return { true, use_remote_connect, std::move(request.host), std::move(url), {} };
    }
    catch (const std::exception &e) {
      return { false, use_remote_connect, {}, {}, e.what() };
    }
  }

}  // namespace remote_connect
