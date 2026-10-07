#pragma once

#include "transport_feedback.h"
#include "transport_policy.h"
#include <nlohmann/json.hpp>
#include <string_view>

namespace transport {
  struct policy_update_t {
    std::uint32_t session_id;
    std::uint64_t connection_epoch, control_epoch, expected_revision;
    std::string request_id;
    budget_request_t budget;
    unsigned fec_base, fec_key, fec_recovery;
  };
  struct control_update_t {
    std::uint32_t session_id;
    std::uint64_t connection_epoch, control_epoch, expected_revision;
    std::string request_id;
    bool automatic_bitrate, automatic_fec;
    int maximum_total_kbps;
  };
  // Strict canonical decimal identities; throws invalid_argument on malformed
  // input. Automatic control is not advertised before its runtime gates pass.
  std::uint64_t parse_policy_identity(std::string_view value);
  struct legacy_scope_t {
    std::uint32_t session_id;
    std::uint64_t connection_epoch;
  };
  // Absence of both fields is the old-client protocol. Partial, zero and
  // noncanonical identities must never silently downgrade to that protocol.
  std::optional<legacy_scope_t>
  parse_legacy_scope(std::optional<std::string_view> session_id, std::optional<std::string_view> epoch);
  std::optional<legacy_scope_t>
  parse_legacy_scope(const nlohmann::json &body);
  nlohmann::json
  parse_legacy_control_body(std::string_view body);
  std::string
  legacy_scope_key(std::string_view owner, legacy_scope_t scope);
  policy_update_t
  parse_policy_update(std::string_view body);
  control_update_t
  parse_control_update(std::string_view body);
  nlohmann::json
  policy_status_json(const policy_snapshot_t &snapshot, std::uint32_t session_id);
  nlohmann::json
  network_statistics_json(const network_statistics_t &statistics);
}
