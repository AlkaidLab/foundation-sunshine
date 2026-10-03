#pragma once

#include "src/network.h"
#include "src/nvhttp.h"
#include "src/stream.h"
#include "src/transport/transport_policy_json.h"

namespace nvhttp::legacy_control {
  struct error_t : std::runtime_error {
    int code;
    error_t(int code, const char *message): std::runtime_error(message), code(code) {}
    SimpleWeb::StatusCode http_status() const {
      switch (code) {
        case 403: return SimpleWeb::StatusCode::client_error_forbidden;
        case 404: return SimpleWeb::StatusCode::client_error_not_found;
        case 409: return SimpleWeb::StatusCode::client_error_conflict;
        default: return SimpleWeb::StatusCode::client_error_bad_request;
      }
    }
  };

  struct target_t {
    stream::session_info_t session;
    std::string owner;
    std::string abr_key;
  };

  template <class Args>
  std::optional<transport::legacy_scope_t> query_scope(const Args &args) {
    const auto field = [&](const char *name) -> std::optional<std::string_view> {
      if (args.count(name) > 1) throw std::invalid_argument("Duplicate legacy identity");
      const auto found = args.find(name);
      return found == args.end() ? std::nullopt : std::optional<std::string_view>(found->second);
    };
    return transport::parse_legacy_scope(field("sessionId"), field("connectionEpoch"));
  }

  inline bool still_legacy(const target_t &target) {
    const auto state = stream::session::get_transport_policy(target.owner, target.session.session_id, target.session.connection_epoch);
    if (!state) return false;
    const auto snapshot = state->snapshot();
    return !snapshot.stopped && !snapshot.experimental_packet_control_negotiated &&
           snapshot.accepted->basis == transport::budget_basis_e::legacy &&
           snapshot.accepted->control_source == transport::control_source_e::legacy;
  }

  template <class Request>
  target_t resolve(const Request &request, std::optional<transport::legacy_scope_t> scope,
    std::string_view client_name = {}) {
    const auto owner = get_client_cert_uuid_from_request(request);
    bool paired = false;
    for (const auto &client : nvhttp::get_all_clients()) {
      if (client.contains("uuid") && client.at("uuid").is_string() && client.at("uuid").get<std::string>() == owner) paired = true;
    }
    if (owner.empty() || !paired) throw error_t(403, "paired_identity_required");
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    std::optional<stream::session_info_t> match;
    for (const auto &session : stream::session::get_all_sessions_info()) {
      if (session.client_uuid != owner || session.state != "RUNNING" || !session.connection_epoch) continue;
      if (scope) {
        if (session.session_id != scope->session_id || session.connection_epoch != scope->connection_epoch) continue;
      }
      else {
        if (client_name.empty()) {
          // ENet can expose an IPv4-mapped peer while HTTPS already exposes its
          // normalized IPv4 address. Compare routes in the same representation.
          boost::system::error_code error;
          const auto peer = boost::asio::ip::make_address(session.client_address, error);
          if (error || net::addr_to_normalized_string(peer) != address) continue;
        }
        else if (session.client_name != client_name) continue;
        if (session.legacy_scope_required) throw error_t(409, "connection_identity_required");
      }
      if (match) throw error_t(409, "ambiguous_legacy_session");
      match = session;
    }
    if (!match) throw error_t(404, "session_not_found");
    target_t result { *match, owner, transport::legacy_scope_key(owner, { match->session_id, match->connection_epoch }) };
    if (!still_legacy(result)) throw error_t(409, "legacy_control_unavailable");
    return result;
  }

  inline bool submit(const target_t &target, const video::dynamic_param_t &param) {
    return stream::session::change_legacy_param_for_session(target.owner, target.session.session_id,
      target.session.connection_epoch, param);
  }
}
