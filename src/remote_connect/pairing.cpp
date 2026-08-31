#include "pairing.h"

#include <exception>
#include <utility>

#include "invite.h"
#include "service.h"

namespace remote_connect {

  pairing_result_t
  create_pairing_invite(pairing_request_t request) {
    auto pairing_state = prepare_pairing();
    if (!pairing_state.success) {
      return { false, pairing_state.enabled, {}, {}, std::move(pairing_state.error) };
    }
    if (pairing_state.enabled) {
      request.host = pairing_state.enrollment->virtual_ip;
    }

    try {
      auto url = build_invite({
        request.host,
        request.port,
        std::move(request.pin),
        std::move(request.server_name),
        std::move(pairing_state.enrollment),
        request.expires_at,
      });
      return { true, pairing_state.enabled, std::move(request.host), std::move(url), {} };
    }
    catch (const std::exception &e) {
      return { false, pairing_state.enabled, {}, {}, e.what() };
    }
  }

}  // namespace remote_connect
