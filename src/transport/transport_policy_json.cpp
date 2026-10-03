#include "transport_policy_json.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace transport {
  nlohmann::json
  network_statistics_json(const network_statistics_t &statistics) {
    const auto &w = statistics.window;
    const auto &c = statistics.cumulative;
    const auto sampled = static_cast<std::uint64_t>(w.received) + w.missing + w.unknown;
    const auto known = static_cast<std::uint64_t>(w.received) + w.missing;
    const auto fresh_time = [&](std::int64_t time) {
      return time >= 0 && w.sampled_at_us >= time && w.sampled_at_us - time <= 1000000;
    };
    const bool fresh = c.negotiated && w.receiver_clock_epoch &&
                       fresh_time(c.last_new_feedback_us) && fresh_time(c.latest_covered_send_us);
    // Relative lifetime, never a timestamp to compare across machine clocks.
    // Clients deduct the whole request interval and their local holding time.
    const auto remaining_us = fresh ? 1000000 - std::max(w.sampled_at_us - c.last_new_feedback_us,
      w.sampled_at_us - c.latest_covered_send_us) : 0;
    std::string reason = !c.negotiated ? "not_negotiated" :
                         !w.valid || !c.ledger.counters_valid ? "unavailable" :
                         !sampled ? "no_samples" :
                         !fresh ? "feedback_stale" :
                         w.history_truncated ? "history_truncated" :
                         w.unknown ? "coverage_incomplete" : "valid";
    // Counts remain decimal strings even when the present window is bounded.
    nlohmann::json result {
      { "version", 2 }, { "connectionEpoch", std::to_string(w.connection_epoch) },
      { "receiverClockEpoch", std::to_string(w.receiver_clock_epoch) },
      { "sampleTimeUs", w.sampled_at_us >= 0 ? nlohmann::json(std::to_string(w.sampled_at_us)) : nlohmann::json(nullptr) },
      { "windowBeginUs", w.valid ? nlohmann::json(std::to_string(w.begins_at_us)) : nlohmann::json(nullptr) },
      { "windowEndUs", w.valid ? nlohmann::json(std::to_string(w.ends_at_us)) : nlohmann::json(nullptr) },
      { "receivedPackets", std::to_string(w.received) }, { "missingPackets", std::to_string(w.missing) },
      { "unknownPackets", std::to_string(w.unknown) }, { "sampledPackets", std::to_string(sampled) },
      { "committedPackets", std::to_string(c.ledger.committed_packets) },
      { "committedIpBytes", std::to_string(c.ledger.committed_ip_bytes) },
      { "missingDeclarations", std::to_string(c.ledger.missing_declarations) },
      { "lateCorrections", std::to_string(c.ledger.late_corrections) },
      { "unresolvedEvictions", std::to_string(c.ledger.unresolved_evictions) },
      { "historyTruncated", w.history_truncated }, { "fresh", fresh },
      { "freshnessRemainingUs", std::to_string(remaining_us) },
      { "reason", reason }, { "rawLossPercent", nullptr }, { "coveragePercent", nullptr }
    };
    if (reason == "valid" && known) result["rawLossPercent"] = static_cast<double>(w.missing) * 100.0 / known;
    if (w.valid && sampled && !w.history_truncated)
      result["coveragePercent"] = static_cast<double>(known) * 100.0 / sampled;
    return result;
  }

  namespace {
    using json = nlohmann::json;
    void
    fields(const json &object, std::initializer_list<std::string_view> names) {
      if (!object.is_object() || object.size() != names.size()) throw std::invalid_argument("Unexpected policy fields");
      for (const auto name : names)
        if (!object.contains(std::string(name))) throw std::invalid_argument("Missing policy field");
    }
    int
    integer(const json &v, int minimum, int maximum) {
      if (!v.is_number_integer() || v < minimum || v > maximum) throw std::invalid_argument("Invalid policy integer");
      return v.get<int>();
    }
    std::uint64_t
    identity(const json &v) {
      if (!v.is_string()) throw std::invalid_argument("Policy identity must be a decimal string");
      return parse_policy_identity(v.get_ref<const std::string &>());
    }
    json
    parse_body(std::string_view body) {
      if (body.empty() || body.size() > 8192) throw std::invalid_argument("Invalid policy body size");
      // Reject duplicates before either authenticated operation is interpreted.
      std::vector<std::vector<std::string>> keys;
      return json::parse(body, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 16) throw std::invalid_argument("Policy nesting exceeds limit");
        if (event == json::parse_event_t::object_start)
          keys.emplace_back();
        else if (event == json::parse_event_t::key) {
          const auto name = value.get<std::string>();
          for (const auto &old : keys.back())
            if (old == name) throw std::invalid_argument("Duplicate policy field");
          keys.back().push_back(name);
        }
        else if (event == json::parse_event_t::object_end)
          keys.pop_back();
        return true;
      });
    }
    template <class Update>
    void
    parse_identity_fields(const json &j, Update &out) {
      integer(j.at("version"), 2, 2);
      const auto session = identity(j.at("sessionId"));
      if (session > std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Session identity overflow");
      out.session_id = static_cast<std::uint32_t>(session);
      out.connection_epoch = identity(j.at("connectionEpoch"));
      out.control_epoch = identity(j.at("controlEpoch"));
      out.expected_revision = identity(j.at("expectedRevision"));
      if (!out.connection_epoch || !out.control_epoch || !out.expected_revision) throw std::invalid_argument("Zero policy epoch or revision");
      if (!j.at("requestId").is_string()) throw std::invalid_argument("Invalid request identity");
      out.request_id = j.at("requestId").get<std::string>();
      if (out.request_id.empty() || out.request_id.size() > 128) throw std::invalid_argument("Invalid request identity");
      for (const char c : out.request_id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) throw std::invalid_argument("Invalid request identity");
    }
    const char *
    failure_name(policy_failure_e value) {
      switch (value) {
        case policy_failure_e::none:
          return "none";
        case policy_failure_e::unsupported:
          return "unsupported";
        case policy_failure_e::backend_failure:
          return "backend_failure";
        case policy_failure_e::superseded:
          return "superseded";
        case policy_failure_e::stopped:
          return "stopped";
      }
      return "unknown";
    }
    const char *
    source_name(control_source_e value) {
      switch (value) {
        case control_source_e::legacy:
          return "legacy";
        case control_source_e::manual:
          return "manual";
        case control_source_e::googcc:
          return "googcc";
        case control_source_e::local:
          return "local";
      }
      return "unknown";
    }
    json
    policy_json(const frame_policy_t &p) {
      return { { "revision", std::to_string(p.revision) }, { "budgetBasis", p.basis == budget_basis_e::legacy ? "legacy" : "normalized" },
        { "controlEpoch", std::to_string(p.control_epoch) }, { "controlSource", source_name(p.control_source) },
        { "wireBudgetKbps", p.budget.total_kbps }, { "encoderKbps", p.encoder_kbps },
        { "encoderCeilingKbps", p.encoder_ceiling_kbps ? json(*p.encoder_ceiling_kbps) : json(nullptr) },
        { "automaticControl", p.automatic_control ? json {
                                                      { "automaticBitrate", p.automatic_control->bitrate }, { "automaticFec", p.automatic_control->fec },
                                                      { "maximumTotalKbps", p.automatic_control->maximum_total_kbps },
                                                      { "activationEpoch", std::to_string(p.automatic_control->activation_epoch) } } :
                                                    json(nullptr) },
        { "fec", { { "base", p.fec_base }, { "key", p.fec_key }, { "recovery", p.fec_recovery } } }, { "reservesKbps", { { "otherTraffic", p.budget.other_kbps }, { "repair", p.budget.repair_kbps }, { "probe", p.budget.probe_kbps }, { "videoOverhead", p.budget.video_overhead_kbps } } } };
    }
  }  // namespace

  std::optional<legacy_scope_t>
  parse_legacy_scope(std::optional<std::string_view> session_id, std::optional<std::string_view> epoch) {
    if (!session_id && !epoch) return std::nullopt;
    if (!session_id || !epoch) throw std::invalid_argument("Incomplete legacy session identity");
    const auto id = parse_policy_identity(*session_id);
    const auto connection = parse_policy_identity(*epoch);
    if (!id || id > std::numeric_limits<std::uint32_t>::max() || !connection)
      throw std::invalid_argument("Invalid legacy session identity");
    return legacy_scope_t { static_cast<std::uint32_t>(id), connection };
  }

  std::optional<legacy_scope_t>
  parse_legacy_scope(const nlohmann::json &body) {
    if (!body.is_object()) throw std::invalid_argument("Legacy body must be an object");
    const auto value = [&](const char *key) -> std::optional<std::string_view> {
      if (!body.contains(key)) return std::nullopt;
      if (!body.at(key).is_string()) throw std::invalid_argument("Legacy identity must be a decimal string");
      return body.at(key).get_ref<const std::string &>();
    };
    return parse_legacy_scope(value("sessionId"), value("connectionEpoch"));
  }

  nlohmann::json
  parse_legacy_control_body(std::string_view body) {
    auto result = parse_body(body);
    (void) parse_legacy_scope(result);
    return result;
  }

  std::string
  legacy_scope_key(std::string_view owner, legacy_scope_t scope) {
    // Length-prefix the opaque certificate identity, rather than assuming it
    // cannot contain a separator. The epoch isolates asynchronous ABR workers.
    return std::to_string(owner.size()) + ":" + std::string(owner) + ":" +
           std::to_string(scope.session_id) + ":" + std::to_string(scope.connection_epoch);
  }

  std::uint64_t
  parse_policy_identity(std::string_view value) {
    if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0'))
      throw std::invalid_argument("Invalid decimal identity");
    for (const char ch : value)
      if (ch < '0' || ch > '9') throw std::invalid_argument("Invalid decimal identity");
    std::uint64_t out;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (error != std::errc() || end != value.data() + value.size()) throw std::invalid_argument("Identity overflow");
    return out;
  }

  policy_update_t
  parse_policy_update(std::string_view body) {
    const auto j = parse_body(body);
    fields(j, { "version", "sessionId", "connectionEpoch", "controlEpoch", "expectedRevision", "requestId", "budget", "fec" });
    policy_update_t out {};
    parse_identity_fields(j, out);
    const auto &b = j.at("budget");
    fields(b, { "totalKbps", "otherTrafficKbps", "repairReserveKbps", "probeReserveKbps", "videoOverheadKbps" });
    out.budget.total_kbps = integer(b.at("totalKbps"), 1, 800000);
    out.budget.other_kbps = integer(b.at("otherTrafficKbps"), 0, 800000);
    out.budget.repair_kbps = integer(b.at("repairReserveKbps"), 0, 800000);
    out.budget.probe_kbps = integer(b.at("probeReserveKbps"), 0, 800000);
    out.budget.video_overhead_kbps = integer(b.at("videoOverheadKbps"), 0, 800000);
    const auto &f = j.at("fec");
    fields(f, { "base", "key", "recovery" });
    out.fec_base = integer(f.at("base"), 0, 100);
    out.fec_key = integer(f.at("key"), 0, 100);
    out.fec_recovery = integer(f.at("recovery"), 0, 100);
    auto budget = out.budget;
    budget.fec_numerator = std::max({ out.fec_base, out.fec_key, out.fec_recovery });
    const auto allocation = allocate_budget(budget);
    if (!allocation || !allocation->encoder_kbps) throw std::invalid_argument("No remaining encoder budget");
    return out;
  }

  control_update_t
  parse_control_update(std::string_view body) {
    const auto j = parse_body(body);
    fields(j, { "version", "sessionId", "connectionEpoch", "controlEpoch", "expectedRevision", "requestId",
                "automaticBitrate", "automaticFec", "maximumTotalKbps" });
    control_update_t out {};
    parse_identity_fields(j, out);
    if (!j.at("automaticBitrate").is_boolean() || !j.at("automaticFec").is_boolean())
      throw std::invalid_argument("Automatic control requires boolean modes");
    out.automatic_bitrate = j.at("automaticBitrate").get<bool>();
    out.automatic_fec = j.at("automaticFec").get<bool>();
    out.maximum_total_kbps = integer(j.at("maximumTotalKbps"), 1, 800000);
    return out;
  }

  json
  policy_status_json(const policy_snapshot_t &s, std::uint32_t session_id) {
    json receipts = json::array();
    bool pending = false;
    bool confirmed = false;
    for (const auto &receipt : s.receipts) {
      auto item = policy_json(*receipt.policy);
      item["encoderApplied"] = receipt.encoder_applied;
      item["firstSentFrame"] = receipt.first_sent_frame ? json(std::to_string(*receipt.first_sent_frame)) : json(nullptr);
      item["failure"] = failure_name(receipt.failure);
      receipts.push_back(std::move(item));
      if (receipt.policy == s.applied && receipt.encoder_applied) confirmed = true;
      if (receipt.policy == s.accepted && !receipt.encoder_applied && receipt.failure == policy_failure_e::none) pending = true;
    }
    return { { "version", 2 }, { "sessionId", std::to_string(session_id) },
      { "connectionEpoch", std::to_string(s.accepted->connection_epoch) }, { "controlEpoch", std::to_string(s.accepted->control_epoch) },
      { "controlOwner", s.accepted->control_source == control_source_e::local ? "local" : "server" },
      { "controlSource", source_name(s.accepted->control_source) }, { "acceptedRevision", std::to_string(s.accepted->revision) },
      { "encoderAppliedRevision", confirmed ? json(std::to_string(s.applied->revision)) : json(nullptr) },
      { "encoderReady", s.encoder_initialized && !s.stopped }, { "pending", pending && !s.stopped }, { "stopped", s.stopped },
      { "packetControlAvailable", false }, { "pacerAvailable", false }, { "wireBudgetEnforced", false },
      { "experimentalPacketControlNegotiated", s.experimental_packet_control_negotiated && !s.stopped },
      { "experimentalLiveControlAvailable", s.experimental_packet_control_negotiated && s.experimental_video_pacer_enabled && !s.stopped },
      { "experimentalVideoPacerEnabled", s.experimental_video_pacer_enabled && !s.stopped },
      { "experimentalControllerActive", s.experimental_packet_control_negotiated && s.experimental_video_pacer_enabled &&
                                          s.accepted->control_source == control_source_e::googcc && !s.stopped },
      { "accepted", policy_json(*s.accepted) }, { "confirmed", confirmed ? policy_json(*s.applied) : json(nullptr) }, { "receipts", std::move(receipts) } };
  }
}  // namespace transport
