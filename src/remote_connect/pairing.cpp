#include "pairing.h"

#include <exception>
#include <utility>

#include "invite.h"
#include "service.h"

namespace remote_connect {

  pairing_result_t
  create_pairing_invite(pairing_request_t request) {
    if (status().enabled) {
      // The old remote invite carried the permanent EasyTier network secret.
      // Refuse to issue it until authenticated, per-device enrollment exists.
      return { false, true, {}, {}, "Secure remote enrollment is not available yet" };
    }

    try {
      auto url = build_invite({
        request.host,
        request.port,
        std::move(request.pin),
        std::move(request.server_name),
        std::nullopt,
        request.expires_at,
      });
      return { true, false, std::move(request.host), std::move(url), {} };
    }
    catch (const std::exception &e) {
      return { false, false, {}, {}, e.what() };
    }
  }

}  // namespace remote_connect
