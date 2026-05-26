/**
 * @file src/session_runtime.h
 * @brief Shared runtime session identity primitives.
 */
#pragma once

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <moonlight-common-c/src/Session.h>
#include <alkaidlab/sunshine_adapter/session_control_wire_codec.h>
#include <alkaidlab/route_control/route_control.h>
}

namespace session_runtime {

  inline std::uint64_t
  stable_key(std::string_view value) {
    constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
    constexpr std::uint64_t fnv_prime = 1099511628211ull;

    std::uint64_t hash = fnv_offset;
    for (unsigned char ch : value) {
      hash ^= ch;
      hash *= fnv_prime;
    }

    return hash;
  }

  enum class feature_e : std::uint8_t {
    clipboard,
    microphone,
    display,
    dynamic_params,
    input_focus,
    transport_qos,
    cursor_plane,
    clipboard_bulk,
    dynamic_quality,
  };

  enum class resource_scope_e : std::uint8_t {
    per_session,
    per_device,
    shared_global,
    global_exclusive,
  };

  enum class display_allocation_mode_e : std::uint8_t {
    shared_owner,
    shared_follower,
    dedicated,
  };

  struct identity_t {
    std::uint64_t runtime_id {};
    std::uint64_t client_cert_key {};
    std::uint32_t launch_session_id {};
    std::uint32_t control_generation {};
    std::uint32_t control_connect_data {};
    std::uint64_t logical_session_key {};
    std::uint64_t participant_key {};
    std::uint64_t client_key {};
    std::uint64_t device_key {};
    std::string logical_session_id;
    std::string participant_id;
    std::string client_cert_uuid;
    std::string client_unique_id;
    std::string client_name;
    std::string av_ping_payload;

    bool
    has_trusted_client_identity() const {
      return !client_cert_uuid.empty();
    }

    void
    set_client_cert_uuid(std::string uuid) {
      client_cert_uuid = std::move(uuid);
      client_cert_key = client_cert_uuid.empty() ? 0 : stable_key(client_cert_uuid);
    }
  };

  inline bool
  is_legacy_client_identity_placeholder(std::string_view value) {
    return value.empty() ||
           value == "unknown" ||
           value == "0123456789ABCDEF";
  }

  inline void
  promote_trusted_client_identity(identity_t &identity,
                                  std::string *launch_unique_id,
                                  std::string *launch_client_name,
                                  std::string_view client_cert_uuid,
                                  std::string_view paired_client_name) {
    if (client_cert_uuid.empty()) {
      return;
    }

    identity.set_client_cert_uuid(std::string { client_cert_uuid });

    if (is_legacy_client_identity_placeholder(identity.client_unique_id)) {
      identity.client_unique_id = identity.client_cert_uuid;
    }

    const auto display_name = paired_client_name.empty() ?
                                std::string_view { client_cert_uuid } :
                                paired_client_name;
    if (is_legacy_client_identity_placeholder(identity.client_name)) {
      identity.client_name = std::string { display_name };
    }

    if (launch_unique_id != nullptr &&
        is_legacy_client_identity_placeholder(*launch_unique_id)) {
      *launch_unique_id = identity.client_unique_id;
    }

    if (launch_client_name != nullptr &&
        is_legacy_client_identity_placeholder(*launch_client_name)) {
      *launch_client_name = identity.client_name;
    }
  }

  struct session_id_t {
    std::uint64_t logical_key {};
    std::uint64_t runtime_id {};
    std::uint32_t launch_session_id {};
    std::uint32_t control_generation {};
    bool trusted_client {};
  };

  inline session_id_t
  make_session_id(const identity_t &identity) {
    if (identity.logical_session_key != 0) {
      return {
        .logical_key = identity.logical_session_key,
        .runtime_id = identity.runtime_id,
        .launch_session_id = identity.launch_session_id,
        .control_generation = identity.control_generation,
        .trusted_client = identity.has_trusted_client_identity(),
      };
    }

    const auto trusted_key = identity.client_cert_key != 0 ?
                               identity.client_cert_key :
                               stable_key(identity.client_unique_id.empty() ?
                                            identity.client_name :
                                            identity.client_unique_id);
    const auto launch_key = static_cast<std::uint64_t>(identity.launch_session_id) << 32U;
    return {
      .logical_key = trusted_key ^ launch_key,
      .runtime_id = identity.runtime_id,
      .launch_session_id = identity.launch_session_id,
      .control_generation = identity.control_generation,
      .trusted_client = identity.has_trusted_client_identity(),
    };
  }

  struct participant_id_t {
    std::uint64_t participant_key {};
    std::uint64_t runtime_id {};
    std::uint64_t device_key {};

    friend bool
    operator==(const participant_id_t &, const participant_id_t &) = default;
  };

  struct participant_t {
    participant_id_t id;
    std::uint64_t participant_key {};
    std::uint64_t client_cert_key {};
    std::uint64_t device_key {};
    std::string client_cert_uuid;
    std::string device_id;
    std::string display_name;
    bool trusted_client {};
  };

  inline participant_t
  make_participant(const identity_t &identity) {
    const auto device_id = !identity.client_unique_id.empty() ?
                             identity.client_unique_id :
                             (!identity.av_ping_payload.empty() ? identity.av_ping_payload : identity.client_name);
    const auto client_key = identity.client_key != 0 ?
                              identity.client_key :
                              (identity.client_cert_key != 0 ?
                                 identity.client_cert_key :
                                 stable_key(identity.client_name));
    const auto device_key = identity.device_key != 0 ?
                              identity.device_key :
                              stable_key(device_id.empty() ? identity.client_name : device_id);
    const auto participant_key = identity.participant_key != 0 ?
                                   identity.participant_key :
                                   (client_key ^ (device_key << 1U));

    return {
      .id = {
        .participant_key = participant_key,
        .runtime_id = identity.runtime_id,
        .device_key = device_key,
      },
      .participant_key = participant_key,
      .client_cert_key = client_key,
      .device_key = device_key,
      .client_cert_uuid = identity.client_cert_uuid,
      .device_id = device_id,
      .display_name = identity.client_name,
      .trusted_client = identity.has_trusted_client_identity(),
    };
  }

  enum class capability_e : std::uint8_t {
    native_renderer_metrics,
    metal_renderer_metrics,
    frame_reuse_feedback,
    owd_feedback,
    transport_cc_lite,
    packet_pacer_probe,
    nack_rtx,
    red_fec,
    ulpfec,
    flexfec,
    ice_stun,
    turn_relay,
    quic_direct,
    relay_quic,
    relay_tcp_tls,
    upnp_public_mapping,
    manual_public_port_forward,
    session_telemetry,
    lease_control,
    count,
  };

  inline std::string_view
  capability_name(capability_e capability) {
    switch (capability) {
      case capability_e::native_renderer_metrics:
        return "native-renderer-metrics";
      case capability_e::metal_renderer_metrics:
        return "metal-renderer-metrics";
      case capability_e::frame_reuse_feedback:
        return "frame-reuse-feedback";
      case capability_e::owd_feedback:
        return "owd-feedback";
      case capability_e::transport_cc_lite:
        return "transport-cc-lite";
      case capability_e::packet_pacer_probe:
        return "pacer-probe";
      case capability_e::nack_rtx:
        return "nack-rtx";
      case capability_e::red_fec:
        return "red-fec";
      case capability_e::ulpfec:
        return "ulpfec";
      case capability_e::flexfec:
        return "flexfec";
      case capability_e::ice_stun:
        return "ice-stun";
      case capability_e::turn_relay:
        return "turn-relay";
      case capability_e::quic_direct:
        return "quic-direct";
      case capability_e::relay_quic:
        return "relay-quic";
      case capability_e::relay_tcp_tls:
        return "relay-tcp-tls";
      case capability_e::upnp_public_mapping:
        return "upnp-public-mapping";
      case capability_e::manual_public_port_forward:
        return "manual-public-port-forward";
      case capability_e::session_telemetry:
        return "session-telemetry";
      case capability_e::lease_control:
        return "lease-control";
      case capability_e::count:
        break;
    }
    return "unknown";
  }

  struct feature_caps_t {
    std::bitset<static_cast<std::size_t>(capability_e::count)> bits;

    feature_caps_t &
    enable(capability_e capability) {
      bits.set(static_cast<std::size_t>(capability));
      return *this;
    }

    feature_caps_t &
    disable(capability_e capability) {
      bits.reset(static_cast<std::size_t>(capability));
      return *this;
    }

    bool
    has(capability_e capability) const {
      return bits.test(static_cast<std::size_t>(capability));
    }

    feature_caps_t
    intersection(const feature_caps_t &other) const {
      feature_caps_t result;
      result.bits = bits & other.bits;
      return result;
    }

    bool
    empty() const {
      return bits.none();
    }

    std::vector<std::string_view>
    names() const {
      std::vector<std::string_view> result;
      for (std::size_t i = 0; i < static_cast<std::size_t>(capability_e::count); ++i) {
        if (bits.test(i)) {
          result.push_back(capability_name(static_cast<capability_e>(i)));
        }
      }
      return result;
    }
  };

  inline std::string_view
  trim_capability_token(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' ||
                              value.front() == '\t' ||
                              value.front() == '\r' ||
                              value.front() == '\n')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' ||
                              value.back() == '\t' ||
                              value.back() == '\r' ||
                              value.back() == '\n')) {
      value.remove_suffix(1);
    }
    return value;
  }

  inline std::optional<capability_e>
  capability_from_name(std::string_view name) {
    name = trim_capability_token(name);
    for (std::size_t i = 0; i < static_cast<std::size_t>(capability_e::count); ++i) {
      const auto capability = static_cast<capability_e>(i);
      if (capability_name(capability) == name) {
        return capability;
      }
    }
    return std::nullopt;
  }

  inline feature_caps_t
  parse_capability_names(std::string_view names) {
    feature_caps_t caps;
    while (!names.empty()) {
      const auto comma = names.find(',');
      const auto token = names.substr(0, comma);
      if (const auto capability = capability_from_name(token)) {
        caps.enable(*capability);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      names.remove_prefix(comma + 1);
    }
    return caps;
  }

  inline std::uint64_t
  li_features_for_caps(const feature_caps_t &caps) {
    std::uint64_t features {};
    if (caps.has(capability_e::native_renderer_metrics) ||
        caps.has(capability_e::metal_renderer_metrics) ||
        caps.has(capability_e::frame_reuse_feedback)) {
      features |= LI_SESSION_FEATURE_VIDEO_TELEMETRY;
    }
    if (caps.has(capability_e::owd_feedback) ||
        caps.has(capability_e::transport_cc_lite) ||
        caps.has(capability_e::packet_pacer_probe)) {
      features |= LI_SESSION_FEATURE_TRANSPORT_CC |
                  LI_SESSION_FEATURE_PATH_PROBE;
    }
    if (caps.has(capability_e::nack_rtx)) {
      features |= LI_SESSION_FEATURE_NACK_RTX;
    }
    if (caps.has(capability_e::relay_quic)) {
      features |= LI_SESSION_FEATURE_RELAY |
                  LI_SESSION_FEATURE_QUIC;
    }
    if (caps.has(capability_e::relay_tcp_tls)) {
      features |= LI_SESSION_FEATURE_RELAY |
                  LI_SESSION_FEATURE_TCP;
    }
    if (caps.has(capability_e::quic_direct)) {
      features |= LI_SESSION_FEATURE_QUIC;
    }
    if (caps.has(capability_e::ice_stun) ||
        caps.has(capability_e::turn_relay) ||
        caps.has(capability_e::upnp_public_mapping) ||
        caps.has(capability_e::manual_public_port_forward)) {
      features |= LI_SESSION_FEATURE_PATH_PROBE;
    }
    if (caps.has(capability_e::red_fec) ||
        caps.has(capability_e::ulpfec) ||
        caps.has(capability_e::flexfec)) {
      features |= LI_SESSION_FEATURE_VIDEO_TELEMETRY;
    }
    if (caps.has(capability_e::session_telemetry)) {
      features |= LI_SESSION_FEATURE_SESSION_TELEMETRY;
    }
    if (caps.has(capability_e::lease_control)) {
      features |= LI_SESSION_FEATURE_LEASE_CONTROL;
    }
    return features;
  }

  inline std::uint64_t
  li_client_baseline_features() {
    return LI_SESSION_FEATURE_CURSOR_PLANE |
           LI_SESSION_FEATURE_CURSOR_PLANE_V2 |
           LI_SESSION_FEATURE_ABSOLUTE_POINTER |
           LI_SESSION_FEATURE_RELATIVE_POINTER |
           LI_SESSION_FEATURE_POINTER_LOCK |
           LI_SESSION_FEATURE_CLIPBOARD |
           LI_SESSION_FEATURE_MICROPHONE |
           LI_SESSION_FEATURE_INPUT_TELEMETRY |
           LI_SESSION_FEATURE_VIDEO_TELEMETRY |
           LI_SESSION_FEATURE_SYSTEM_CURSOR_STATE |
           LI_SESSION_FEATURE_POINTER_MODE_SWITCH |
           LI_SESSION_FEATURE_INPUT_RELEASE_SMOOTHING |
           LI_SESSION_FEATURE_SESSION_TELEMETRY |
           LI_SESSION_FEATURE_LEASE_CONTROL |
           LI_SESSION_FEATURE_DYNAMIC_QUALITY;
  }

  inline std::uint64_t
  li_host_baseline_features() {
    return LI_SESSION_FEATURE_CURSOR_PLANE |
           LI_SESSION_FEATURE_CURSOR_PLANE_V2 |
           LI_SESSION_FEATURE_ABSOLUTE_POINTER |
           LI_SESSION_FEATURE_RELATIVE_POINTER |
           LI_SESSION_FEATURE_POINTER_LOCK |
           LI_SESSION_FEATURE_CLIPBOARD |
           LI_SESSION_FEATURE_MICROPHONE |
           LI_SESSION_FEATURE_PATH_PROBE |
           LI_SESSION_FEATURE_INPUT_TELEMETRY |
           LI_SESSION_FEATURE_VIDEO_TELEMETRY |
           LI_SESSION_FEATURE_SYSTEM_CURSOR_STATE |
           LI_SESSION_FEATURE_POINTER_MODE_SWITCH |
           LI_SESSION_FEATURE_INPUT_RELEASE_SMOOTHING |
           LI_SESSION_FEATURE_SESSION_TELEMETRY |
           LI_SESSION_FEATURE_LEASE_CONTROL |
           LI_SESSION_FEATURE_DYNAMIC_QUALITY;
  }

  struct owner_token_t {
    feature_e feature {};
    std::uint64_t runtime_id {};
    std::uint64_t client_cert_key {};
    std::uint32_t control_generation {};

    explicit operator bool() const {
      return runtime_id != 0;
    }
  };

  struct display_allocation_t {
    owner_token_t owner { feature_e::display };
    resource_scope_e scope { resource_scope_e::shared_global };
    display_allocation_mode_e mode { display_allocation_mode_e::shared_owner };
    std::uint32_t resource_slot {};
  };

  enum class lease_mode_e : std::uint8_t {
    exclusive_owner,
    observer,
    shared,
  };

  struct lease_t {
    feature_e feature {};
    lease_mode_e mode { lease_mode_e::exclusive_owner };
    participant_id_t owner;

    bool
    owned_by(const participant_id_t &participant) const {
      return owner == participant;
    }

    bool
    can_control(const participant_id_t &participant) const {
      return mode == lease_mode_e::shared || owned_by(participant);
    }

    bool
    can_observe(const participant_id_t &) const {
      return true;
    }
  };

  enum class transport_protocol_e : std::uint8_t {
    enet_udp,
    udp,
    quic,
    tcp_tls,
  };

  enum class transport_route_e : std::uint8_t {
    lan_direct,
    manual_public_port_forward,
    upnp_public_mapping,
    ice_stun_p2p,
    relay_quic,
    relay_tcp_tls,
  };

  enum class transport_path_state_e : std::uint8_t {
    candidate,
    checking,
    active,
    failed,
    standby,
  };

  struct transport_path_score_t {
    std::uint32_t rtt_ms {};
    std::uint32_t jitter_ms {};
    std::uint32_t loss_ppm {};
    std::uint32_t throughput_kbps {};
    std::uint32_t cost {};
  };

  struct transport_path_t {
    std::uint64_t path_id {};
    transport_route_e route { transport_route_e::lan_direct };
    transport_protocol_e protocol { transport_protocol_e::enet_udp };
    transport_path_state_e state { transport_path_state_e::candidate };
    transport_path_score_t score;
    feature_caps_t required_caps;
    std::uint32_t observed_egress_kind { LI_SESSION_PATH_EGRESS_UNKNOWN };
    std::uint32_t observed_encapsulation { LI_SESSION_PATH_ENCAPSULATION_UNKNOWN };
    std::uint32_t extra_evidence_flags {};
    std::uint32_t identity_confidence_ppm {};
    std::uint32_t path_identity_kind { LI_SESSION_PATH_IDENTITY_UNKNOWN };
    std::uint32_t startup_class { LI_SESSION_STARTUP_CLASS_UNKNOWN };
    std::uint32_t reason_flags {};
    std::uint32_t risk_flags {};
    std::string explanation_code;
    std::string local_endpoint;
    std::string remote_endpoint;
    std::string observed_endpoint;
    std::string host_local_endpoint;
    std::string provider_id;
  };

  struct startup_path_evidence_t {
    bool peer_is_lan_or_pc {};
    bool remote_streaming_hint {};
    bool rtsp_route_remote_hint {};
    bool client_route_remote_hint {};
    bool client_route_tunnel {};
    bool client_vpn_active {};
    std::string startup_profile;
    std::string client_egress_kind;
    std::string client_route_host;
    std::string client_route_path_kind;
    std::string rtsp_route_host;
    std::string client_source_endpoint;
    std::string host_observed_peer_endpoint;
    std::string host_observed_local_endpoint;
    std::vector<std::string> client_target_address_candidates;
    std::vector<std::string> host_public_candidates;
  };

  struct startup_path_decision_t {
    transport_route_e route { transport_route_e::lan_direct };
    bool allow_lan_fast_start { true };
    std::uint32_t egress_kind { LI_SESSION_PATH_EGRESS_UNKNOWN };
    std::uint32_t encapsulation { LI_SESSION_PATH_ENCAPSULATION_UNKNOWN };
    std::uint32_t evidence_flags {};
    std::uint32_t identity_confidence_ppm { 500000U };
    std::uint32_t path_identity_kind { LI_SESSION_PATH_IDENTITY_TRUE_LAN };
    std::uint32_t startup_class { LI_SESSION_STARTUP_CLASS_LAN_FAST };
    std::uint32_t reason_flags { LI_SESSION_PATH_REASON_HOST_PEER_OBSERVED };
    std::uint32_t risk_flags {};
    const char *reason { "peer-lan-confirmed" };
  };

  inline std::string_view
  transport_route_name(transport_route_e route) {
    switch (route) {
      case transport_route_e::lan_direct:
        return "lan-direct";
      case transport_route_e::manual_public_port_forward:
        return "manual-public-port-forward";
      case transport_route_e::upnp_public_mapping:
        return "upnp-public-mapping";
      case transport_route_e::ice_stun_p2p:
        return "ice-stun-p2p";
      case transport_route_e::relay_quic:
        return "relay-quic";
      case transport_route_e::relay_tcp_tls:
        return "relay-tcp-tls";
    }
    return "unknown";
  }

  inline transport_protocol_e
  protocol_for_route(transport_route_e route) {
    switch (route) {
      case transport_route_e::relay_quic:
        return transport_protocol_e::quic;
      case transport_route_e::relay_tcp_tls:
        return transport_protocol_e::tcp_tls;
      case transport_route_e::lan_direct:
      case transport_route_e::manual_public_port_forward:
      case transport_route_e::upnp_public_mapping:
        return transport_protocol_e::enet_udp;
      case transport_route_e::ice_stun_p2p:
        return transport_protocol_e::udp;
    }
    return transport_protocol_e::enet_udp;
  }

  inline feature_caps_t
  required_caps_for_route(transport_route_e route) {
    feature_caps_t caps;
    switch (route) {
      case transport_route_e::manual_public_port_forward:
        caps.enable(capability_e::manual_public_port_forward);
        break;
      case transport_route_e::upnp_public_mapping:
        caps.enable(capability_e::upnp_public_mapping);
        break;
      case transport_route_e::ice_stun_p2p:
        caps.enable(capability_e::ice_stun);
        break;
      case transport_route_e::relay_quic:
        caps.enable(capability_e::relay_quic);
        break;
      case transport_route_e::relay_tcp_tls:
        caps.enable(capability_e::relay_tcp_tls);
        break;
      case transport_route_e::lan_direct:
        break;
    }
    return caps;
  }

  inline transport_path_t
  make_transport_path(transport_route_e route) {
    const auto route_name = transport_route_name(route);
    return {
      .path_id = stable_key(route_name),
      .route = route,
      .protocol = protocol_for_route(route),
      .state = transport_path_state_e::candidate,
      .required_caps = required_caps_for_route(route),
    };
  }

  inline transport_path_t
  make_enet_direct_transport_path() {
    return make_transport_path(transport_route_e::lan_direct);
  }

  inline bool
  path_profile_requests_remote_safe_startup(std::string_view profile) {
    return profile == "remote" ||
           profile == "public" ||
           profile == "wan" ||
           profile == "tunnel" ||
           profile == "vpn" ||
           profile == "relay";
  }

  inline std::uint32_t
  li_path_egress_kind_for_client_hint(std::string_view egress_kind) {
    if (egress_kind == "physical" || egress_kind == "direct") {
      return LI_SESSION_PATH_EGRESS_PHYSICAL;
    }
    if (egress_kind == "tunnel" || egress_kind == "vpn") {
      return LI_SESSION_PATH_EGRESS_TUNNEL;
    }
    if (egress_kind == "virtual") {
      return LI_SESSION_PATH_EGRESS_VIRTUAL;
    }
    if (egress_kind == "loopback") {
      return LI_SESSION_PATH_EGRESS_LOOPBACK;
    }
    if (egress_kind == "proxy") {
      return LI_SESSION_PATH_EGRESS_PROXY;
    }
    if (egress_kind == "relay") {
      return LI_SESSION_PATH_EGRESS_RELAY;
    }
    return LI_SESSION_PATH_EGRESS_UNKNOWN;
  }

  inline bool
  ascii_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
  }

  inline std::string
  trim_ascii(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
      first++;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
      last--;
    }
    return std::string { value.substr(first, last - first) };
  }

  inline std::string
  canonical_endpoint_host(std::string_view raw_value) {
    auto value = trim_ascii(raw_value);
    if (value.empty()) {
      return {};
    }

    const auto scheme_pos = value.find("://");
    if (scheme_pos != std::string::npos) {
      value.erase(0, scheme_pos + 3);
    }

    const auto slash_pos = value.find_first_of("/?#");
    if (slash_pos != std::string::npos) {
      value.erase(slash_pos);
    }

    const auto at_pos = value.rfind('@');
    if (at_pos != std::string::npos) {
      value.erase(0, at_pos + 1);
    }

    if (!value.empty() && value.front() == '[') {
      const auto close = value.find(']');
      if (close != std::string::npos) {
        value = value.substr(1, close - 1);
      }
    }
    else {
      const auto colon_count = static_cast<std::size_t>(std::count(value.begin(), value.end(), ':'));
      if (colon_count == 1) {
        const auto colon = value.rfind(':');
        const auto port = std::string_view { value }.substr(colon + 1);
        if (!port.empty() && std::ranges::all_of(port, ascii_is_digit)) {
          value.erase(colon);
        }
      }
    }

    while (!value.empty() && value.back() == '.') {
      value.pop_back();
    }
    std::ranges::transform(value, value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  }

  inline bool
  canonical_endpoint_matches(std::string_view lhs, std::string_view rhs) {
    const auto left = canonical_endpoint_host(lhs);
    const auto right = canonical_endpoint_host(rhs);
    return !left.empty() && left == right;
  }

  inline bool
  parse_ipv4_literal(std::string_view raw_value, std::uint8_t octets[4]) {
    const auto value = canonical_endpoint_host(raw_value);
    if (value.empty()) {
      return false;
    }

    std::size_t start = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const auto dot = value.find('.', start);
      const auto end = dot == std::string::npos ? value.size() : dot;
      if (end == start || (index < 3 && dot == std::string::npos)) {
        return false;
      }

      unsigned int parsed = 0;
      for (std::size_t pos = start; pos < end; ++pos) {
        const char ch = value[pos];
        if (!ascii_is_digit(ch)) {
          return false;
        }
        parsed = parsed * 10U + static_cast<unsigned int>(ch - '0');
        if (parsed > 255U) {
          return false;
        }
      }
      octets[index] = static_cast<std::uint8_t>(parsed);
      start = end + 1;
    }

    return start == value.size() + 1;
  }

  inline bool
  is_public_ipv4_literal(std::string_view raw_value) {
    std::uint8_t ip[4] {};
    if (!parse_ipv4_literal(raw_value, ip)) {
      return false;
    }

    if (ip[0] == 0 || ip[0] == 10 || ip[0] == 127 || ip[0] >= 224) {
      return false;
    }
    if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127) {
      return false;
    }
    if (ip[0] == 169 && ip[1] == 254) {
      return false;
    }
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) {
      return false;
    }
    if (ip[0] == 192 && ip[1] == 168) {
      return false;
    }
    if (ip[0] == 198 && (ip[1] == 18 || ip[1] == 19)) {
      return false;
    }
    if ((ip[0] == 192 && ip[1] == 0 && ip[2] == 2) ||
        (ip[0] == 198 && ip[1] == 51 && ip[2] == 100) ||
        (ip[0] == 203 && ip[1] == 0 && ip[2] == 113)) {
      return false;
    }
    return true;
  }

  inline bool
  is_lan_or_pc_ipv4_literal(std::string_view raw_value) {
    std::uint8_t ip[4] {};
    if (!parse_ipv4_literal(raw_value, ip)) {
      return false;
    }

    if (ip[0] == 10 || ip[0] == 127) {
      return true;
    }
    if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127) {
      return true;
    }
    if (ip[0] == 169 && ip[1] == 254) {
      return true;
    }
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) {
      return true;
    }
    if (ip[0] == 192 && ip[1] == 168) {
      return true;
    }
    return false;
  }

  inline bool
  append_public_ipv4_identity_candidate(std::vector<std::string> &candidates,
                                        std::string_view raw_value) {
    const auto candidate = canonical_endpoint_host(raw_value);
    if (!is_public_ipv4_literal(candidate)) {
      return false;
    }
    if (std::ranges::find(candidates, candidate) != candidates.end()) {
      return false;
    }
    candidates.push_back(candidate);
    return true;
  }

  inline bool
  has_public_target_address_candidate(const startup_path_evidence_t &evidence) {
    if (is_public_ipv4_literal(evidence.client_route_host) ||
        is_public_ipv4_literal(evidence.rtsp_route_host)) {
      return true;
    }
    return std::ranges::any_of(evidence.client_target_address_candidates, [](const auto &candidate) {
      return is_public_ipv4_literal(candidate);
    });
  }

  inline bool
  target_matches_host_public_identity(const startup_path_evidence_t &evidence) {
    if (evidence.host_public_candidates.empty()) {
      return false;
    }

    std::vector<std::string_view> target_candidates;
    if (!evidence.client_route_host.empty()) {
      target_candidates.push_back(evidence.client_route_host);
    }
    if (!evidence.rtsp_route_host.empty()) {
      target_candidates.push_back(evidence.rtsp_route_host);
    }
    for (const auto &candidate : evidence.client_target_address_candidates) {
      if (!candidate.empty()) {
        target_candidates.push_back(candidate);
      }
    }

    for (const auto target : target_candidates) {
      for (const auto &host_public : evidence.host_public_candidates) {
        if (canonical_endpoint_matches(target, host_public)) {
          return true;
        }
      }
    }
    return false;
  }

  inline bool
  host_local_endpoint_matches_public_identity(const startup_path_evidence_t &evidence) {
    if (!is_public_ipv4_literal(evidence.host_observed_local_endpoint)) {
      return false;
    }
    return std::ranges::any_of(evidence.host_public_candidates, [&](const auto &host_public) {
      return canonical_endpoint_matches(evidence.host_observed_local_endpoint, host_public);
    });
  }

  inline bool
  host_peer_observed_as_lan_or_pc(const startup_path_evidence_t &evidence) {
    return !evidence.host_observed_peer_endpoint.empty() &&
           is_lan_or_pc_ipv4_literal(evidence.host_observed_peer_endpoint);
  }

  inline bool
  rtsp_peer_route_change_requires_remote_hint(std::string_view expected_peer,
                                              std::string_view observed_peer) {
    if (expected_peer.empty() || observed_peer.empty()) {
      return true;
    }
    return !(is_lan_or_pc_ipv4_literal(expected_peer) &&
             is_lan_or_pc_ipv4_literal(observed_peer));
  }

  inline bool
  client_egress_allows_lan_fast_start(std::uint32_t egress_kind) {
    return egress_kind == LI_SESSION_PATH_EGRESS_UNKNOWN ||
           egress_kind == LI_SESSION_PATH_EGRESS_PHYSICAL;
  }

  inline bool
  client_egress_is_virtual_overlay(std::uint32_t egress_kind) {
    return egress_kind == LI_SESSION_PATH_EGRESS_VIRTUAL ||
           egress_kind == LI_SESSION_PATH_EGRESS_PROXY;
  }

  inline std::uint32_t
  virtual_overlay_encapsulation_for_egress(std::uint32_t egress_kind) {
    if (egress_kind == LI_SESSION_PATH_EGRESS_PROXY) {
      return LI_SESSION_PATH_ENCAPSULATION_TCP_PROXY;
    }
    return LI_SESSION_PATH_ENCAPSULATION_UDP_TUNNEL;
  }

  inline bool
  has_public_identity_comparison(const startup_path_evidence_t &evidence) {
    return !evidence.host_public_candidates.empty() &&
           (!evidence.client_route_host.empty() ||
            !evidence.rtsp_route_host.empty() ||
            !evidence.client_target_address_candidates.empty());
  }

  inline transport_route_e
  transport_route_from_alk_route(std::uint32_t route_kind) {
    switch (route_kind) {
      case ALK_ROUTE_KIND_LAN_DIRECT:
        return transport_route_e::lan_direct;
      case ALK_ROUTE_KIND_MANUAL_PUBLIC_PORT_FORWARD:
        return transport_route_e::manual_public_port_forward;
      case ALK_ROUTE_KIND_UPNP_PUBLIC_MAPPING:
        return transport_route_e::upnp_public_mapping;
      case ALK_ROUTE_KIND_ICE_STUN_P2P:
        return transport_route_e::ice_stun_p2p;
      case ALK_ROUTE_KIND_RELAY_QUIC:
        return transport_route_e::relay_quic;
      case ALK_ROUTE_KIND_RELAY_TCP_TLS:
        return transport_route_e::relay_tcp_tls;
    }
    return transport_route_e::lan_direct;
  }

  inline const char *
  route_control_reason_literal(std::string_view reason) {
    if (reason == "peer-lan-confirmed") {
      return "peer-lan-confirmed";
    }
    if (reason == "client-tunnel-to-host-public") {
      return "client-tunnel-to-host-public";
    }
    if (reason == "client-tunnel-to-external-forwarder") {
      return "client-tunnel-to-external-forwarder";
    }
    if (reason == "client-tunnel-route") {
      return "client-tunnel-route";
    }
    if (reason == "host-direct-public") {
      return "host-direct-public";
    }
    if (reason == "router-snat-port-forward" ||
        reason == "host-public-port-forward-lan-hairpin") {
      return "router-snat-port-forward";
    }
    if (reason == "host-public-port-forward") {
      return "host-public-port-forward";
    }
    if (reason == "external-forwarder") {
      return "external-forwarder";
    }
    if (reason == "client-remote-hint") {
      return "client-remote-hint";
    }
    if (reason == "rtsp-route-remote" || reason == "advertised-route-remote") {
      return "rtsp-route-remote";
    }
    if (reason == "peer-not-lan") {
      return "peer-not-lan";
    }
    if (reason == "client-virtual-overlay") {
      return "client-virtual-overlay";
    }
    return "unknown";
  }

  inline void
  copy_alk_route_string(char *destination, std::size_t destination_length, std::string_view source) {
    if (destination == nullptr || destination_length == 0) {
      return;
    }
    std::memset(destination, 0, destination_length);
    const auto length = std::min(source.size(), destination_length - 1);
    if (length != 0) {
      std::memcpy(destination, source.data(), length);
    }
  }

  inline AlkRouteStartupEvidence
  make_alk_route_startup_evidence(const startup_path_evidence_t &evidence) {
    AlkRouteStartupEvidence route_evidence {};
    alk_route_control_evidence_init(&route_evidence);
    route_evidence.peer_is_lan_or_pc = evidence.peer_is_lan_or_pc;
    route_evidence.remote_streaming_hint = evidence.remote_streaming_hint;
    route_evidence.advertised_route_remote_hint = evidence.rtsp_route_remote_hint;
    route_evidence.client_route_remote_hint = evidence.client_route_remote_hint;
    route_evidence.client_route_tunnel = evidence.client_route_tunnel;
    route_evidence.client_vpn_active = evidence.client_vpn_active;
    copy_alk_route_string(route_evidence.startup_profile, sizeof(route_evidence.startup_profile), evidence.startup_profile);
    copy_alk_route_string(route_evidence.client_egress_kind, sizeof(route_evidence.client_egress_kind), evidence.client_egress_kind);
    copy_alk_route_string(route_evidence.client_route_host, sizeof(route_evidence.client_route_host), evidence.client_route_host);
    copy_alk_route_string(route_evidence.advertised_route_host, sizeof(route_evidence.advertised_route_host), evidence.rtsp_route_host);
    copy_alk_route_string(route_evidence.client_source_endpoint, sizeof(route_evidence.client_source_endpoint), evidence.client_source_endpoint);
    copy_alk_route_string(route_evidence.host_observed_peer_endpoint, sizeof(route_evidence.host_observed_peer_endpoint), evidence.host_observed_peer_endpoint);
    copy_alk_route_string(route_evidence.host_observed_local_endpoint, sizeof(route_evidence.host_observed_local_endpoint), evidence.host_observed_local_endpoint);
    for (const auto &candidate : evidence.client_target_address_candidates) {
      if (route_evidence.client_target_address_candidate_count >= ALK_ROUTE_CONTROL_MAX_CANDIDATES) {
        break;
      }
      copy_alk_route_string(route_evidence.client_target_address_candidates[route_evidence.client_target_address_candidate_count],
                            sizeof(route_evidence.client_target_address_candidates[route_evidence.client_target_address_candidate_count]),
                            candidate);
      ++route_evidence.client_target_address_candidate_count;
    }
    for (const auto &candidate : evidence.host_public_candidates) {
      if (route_evidence.host_public_candidate_count >= ALK_ROUTE_CONTROL_MAX_CANDIDATES) {
        break;
      }
      copy_alk_route_string(route_evidence.host_public_candidates[route_evidence.host_public_candidate_count],
                            sizeof(route_evidence.host_public_candidates[route_evidence.host_public_candidate_count]),
                            candidate);
      ++route_evidence.host_public_candidate_count;
    }
    return route_evidence;
  }

  inline bool
  client_route_path_is_private_overlay(std::string_view path_kind) {
    return path_kind == "private-overlay" ||
           path_kind == "private_overlay" ||
           path_kind == "private-overlay-direct";
  }

  inline startup_path_decision_t
  private_overlay_startup_path_decision(const startup_path_evidence_t &evidence) {
    const auto egress_kind = li_path_egress_kind_for_client_hint(evidence.client_egress_kind);
    std::uint32_t reason_flags = LI_SESSION_PATH_REASON_CLIENT_ROUTE_OBSERVED |
                                 LI_SESSION_PATH_REASON_REMOTE_HINT;
    if (evidence.peer_is_lan_or_pc || !evidence.host_observed_peer_endpoint.empty()) {
      reason_flags |= LI_SESSION_PATH_REASON_HOST_PEER_OBSERVED;
    }

    return {
      .route = transport_route_e::manual_public_port_forward,
      .allow_lan_fast_start = false,
      .egress_kind = egress_kind == LI_SESSION_PATH_EGRESS_UNKNOWN ?
                       LI_SESSION_PATH_EGRESS_PHYSICAL :
                       egress_kind,
      .encapsulation = LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP,
      .evidence_flags = LI_SESSION_PATH_EVIDENCE_QUALITY_SAMPLED |
                        LI_SESSION_PATH_EVIDENCE_CLIENT_CONFIGURED |
                        LI_SESSION_PATH_EVIDENCE_CLIENT_ROUTE_OBSERVED |
                        LI_SESSION_PATH_EVIDENCE_HOST_PEER_OBSERVED,
      .identity_confidence_ppm = 820000U,
      .path_identity_kind = LI_SESSION_PATH_IDENTITY_VPN_OVERLAY,
      .startup_class = LI_SESSION_STARTUP_CLASS_REMOTE_SAFE,
      .reason_flags = reason_flags,
      .risk_flags = 0U,
      .reason = "client-private-overlay",
    };
  }

  inline startup_path_decision_t
  classify_startup_path(const startup_path_evidence_t &evidence) {
    if (client_route_path_is_private_overlay(evidence.client_route_path_kind) &&
        !evidence.client_route_tunnel &&
        evidence.client_egress_kind != "tunnel" &&
        evidence.client_egress_kind != "vpn") {
      return private_overlay_startup_path_decision(evidence);
    }
    const auto route_evidence = make_alk_route_startup_evidence(evidence);
    AlkRouteStartupDecision route_decision {};
    if (!alk_route_control_classify_startup_path(&route_evidence, &route_decision)) {
      return {};
    }

    return {
      .route = transport_route_from_alk_route(route_decision.route_kind),
      .allow_lan_fast_start = route_decision.allow_lan_fast_start,
      .egress_kind = route_decision.egress_kind,
      .encapsulation = route_decision.encapsulation,
      .evidence_flags = route_decision.evidence_flags,
      .identity_confidence_ppm = route_decision.identity_confidence_ppm,
      .path_identity_kind = route_decision.identity_kind,
      .startup_class = route_decision.startup_class,
      .reason_flags = route_decision.reason_flags,
      .risk_flags = route_decision.risk_flags,
      .reason = route_control_reason_literal(route_decision.explanation_code),
    };
  }

  inline transport_path_t
  make_transport_path(const startup_path_decision_t &decision,
                      const startup_path_evidence_t &evidence = {}) {
    auto path = make_transport_path(decision.route);
    path.observed_egress_kind = decision.egress_kind;
    path.observed_encapsulation = decision.encapsulation;
    path.extra_evidence_flags = decision.evidence_flags;
    path.identity_confidence_ppm = decision.identity_confidence_ppm;
    path.path_identity_kind = decision.path_identity_kind;
    path.startup_class = decision.startup_class;
    path.reason_flags = decision.reason_flags;
    path.risk_flags = decision.risk_flags;
    path.explanation_code = decision.reason;
    path.local_endpoint = evidence.client_source_endpoint;
    path.remote_endpoint = !evidence.client_route_host.empty() ?
      evidence.client_route_host :
      evidence.rtsp_route_host;
    path.observed_endpoint = evidence.host_observed_peer_endpoint;
    path.host_local_endpoint = evidence.host_observed_local_endpoint;
    path.provider_id = "enet-primary";
    return path;
  }

  inline std::uint32_t
  li_transport_kind(transport_route_e route) {
    switch (route) {
      case transport_route_e::lan_direct:
        return LI_SESSION_TRANSPORT_LOCAL;
      case transport_route_e::manual_public_port_forward:
      case transport_route_e::upnp_public_mapping:
      case transport_route_e::ice_stun_p2p:
        return LI_SESSION_TRANSPORT_DIRECT_ENET;
      case transport_route_e::relay_quic:
        return LI_SESSION_TRANSPORT_RELAY_QUIC;
      case transport_route_e::relay_tcp_tls:
        return LI_SESSION_TRANSPORT_RELAY_TCP;
    }
    return LI_SESSION_TRANSPORT_UNKNOWN;
  }

  inline std::uint32_t
  li_transport_protocol(transport_protocol_e protocol) {
    switch (protocol) {
      case transport_protocol_e::enet_udp:
        return LI_SESSION_TRANSPORT_PROTOCOL_ENET_UDP;
      case transport_protocol_e::udp:
        return LI_SESSION_TRANSPORT_PROTOCOL_UDP;
      case transport_protocol_e::quic:
        return LI_SESSION_TRANSPORT_PROTOCOL_QUIC;
      case transport_protocol_e::tcp_tls:
        return LI_SESSION_TRANSPORT_PROTOCOL_TCP_TLS;
    }
    return LI_SESSION_TRANSPORT_PROTOCOL_UNKNOWN;
  }

  inline std::uint32_t
  li_transport_flags(const transport_path_t &path) {
    std::uint32_t flags = LI_SESSION_TRANSPORT_FLAG_LOW_LATENCY;
    if (path.route == transport_route_e::relay_quic ||
        path.route == transport_route_e::relay_tcp_tls) {
      flags |= LI_SESSION_TRANSPORT_FLAG_RELAY;
    }
    if (path.protocol == transport_protocol_e::quic ||
        path.protocol == transport_protocol_e::tcp_tls) {
      flags |= LI_SESSION_TRANSPORT_FLAG_ENCRYPTED |
               LI_SESSION_TRANSPORT_FLAG_PACED;
    }
    if (path.protocol == transport_protocol_e::quic ||
        path.protocol == transport_protocol_e::enet_udp ||
        path.protocol == transport_protocol_e::udp) {
      flags |= LI_SESSION_TRANSPORT_FLAG_DATAGRAM;
    }
    if (path.protocol == transport_protocol_e::tcp_tls) {
      flags |= LI_SESSION_TRANSPORT_FLAG_STREAM;
    }
    if (path.protocol == transport_protocol_e::quic) {
      flags |= LI_SESSION_TRANSPORT_FLAG_MIGRATABLE;
    }
    return flags;
  }

  inline std::uint32_t
  li_transport_stack_flags(const transport_path_t &path) {
    std::uint32_t flags = 0;

    switch (path.protocol) {
      case transport_protocol_e::enet_udp:
        flags |= LI_SESSION_TRANSPORT_STACK_ENET |
                 LI_SESSION_TRANSPORT_STACK_UDP |
                 LI_SESSION_TRANSPORT_STACK_RTP |
                 LI_SESSION_TRANSPORT_STACK_RTCP |
                 LI_SESSION_TRANSPORT_STACK_RTSP;
        break;
      case transport_protocol_e::udp:
        flags |= LI_SESSION_TRANSPORT_STACK_UDP |
                 LI_SESSION_TRANSPORT_STACK_RTP |
                 LI_SESSION_TRANSPORT_STACK_RTCP |
                 LI_SESSION_TRANSPORT_STACK_RTSP;
        break;
      case transport_protocol_e::quic:
        flags |= LI_SESSION_TRANSPORT_STACK_QUIC;
        break;
      case transport_protocol_e::tcp_tls:
        flags |= LI_SESSION_TRANSPORT_STACK_TCP |
                 LI_SESSION_TRANSPORT_STACK_TLS;
        break;
    }

    if (path.route == transport_route_e::ice_stun_p2p) {
      flags |= LI_SESSION_TRANSPORT_STACK_STUN |
               LI_SESSION_TRANSPORT_STACK_ICE |
               LI_SESSION_TRANSPORT_STACK_P2P;
    }
    if (path.route == transport_route_e::relay_quic ||
        path.route == transport_route_e::relay_tcp_tls) {
      flags |= LI_SESSION_TRANSPORT_STACK_RELAY;
    }

    return flags;
  }

  inline std::uint32_t
  li_path_candidate_type(transport_route_e route) {
    switch (route) {
      case transport_route_e::relay_quic:
      case transport_route_e::relay_tcp_tls:
        return LI_SESSION_PATH_CANDIDATE_RELAY;
      case transport_route_e::lan_direct:
      case transport_route_e::manual_public_port_forward:
      case transport_route_e::upnp_public_mapping:
        return LI_SESSION_PATH_CANDIDATE_HOST;
      case transport_route_e::ice_stun_p2p:
        return LI_SESSION_PATH_CANDIDATE_SERVER_REFLEXIVE;
    }
    return LI_SESSION_PATH_CANDIDATE_UNKNOWN;
  }

  inline std::uint32_t
  li_path_discovery_source(transport_route_e route) {
    switch (route) {
      case transport_route_e::lan_direct:
        return LI_SESSION_PATH_DISCOVERY_LAN_DISCOVERY;
      case transport_route_e::manual_public_port_forward:
        return LI_SESSION_PATH_DISCOVERY_USER_CONFIG;
      case transport_route_e::upnp_public_mapping:
        return LI_SESSION_PATH_DISCOVERY_HOST_ADVERTISEMENT;
      case transport_route_e::ice_stun_p2p:
        return LI_SESSION_PATH_DISCOVERY_ICE;
      case transport_route_e::relay_quic:
      case transport_route_e::relay_tcp_tls:
        return LI_SESSION_PATH_DISCOVERY_RELAY_DIRECTORY;
    }
    return LI_SESSION_PATH_DISCOVERY_UNKNOWN;
  }

  inline std::uint32_t
  li_path_egress_kind(transport_route_e route) {
    switch (route) {
      case transport_route_e::relay_quic:
      case transport_route_e::relay_tcp_tls:
        return LI_SESSION_PATH_EGRESS_RELAY;
      default:
        return LI_SESSION_PATH_EGRESS_UNKNOWN;
    }
  }

  inline std::uint32_t
  li_path_encapsulation(transport_route_e route) {
    switch (route) {
      case transport_route_e::relay_quic:
      case transport_route_e::relay_tcp_tls:
        return LI_SESSION_PATH_ENCAPSULATION_RELAY;
      case transport_route_e::ice_stun_p2p:
        return LI_SESSION_PATH_ENCAPSULATION_ICE;
      default:
        return LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP;
    }
  }

  inline std::uint32_t
  li_path_evidence_flags(const transport_path_t &path) {
    std::uint32_t flags = LI_SESSION_PATH_EVIDENCE_QUALITY_SAMPLED;
    switch (path.route) {
      case transport_route_e::lan_direct:
        flags |= LI_SESSION_PATH_EVIDENCE_HOST_PEER_OBSERVED;
        break;
      case transport_route_e::manual_public_port_forward:
        flags |= LI_SESSION_PATH_EVIDENCE_CLIENT_CONFIGURED;
        break;
      case transport_route_e::upnp_public_mapping:
        flags |= LI_SESSION_PATH_EVIDENCE_HOST_PEER_OBSERVED;
        break;
      case transport_route_e::ice_stun_p2p:
        flags |= LI_SESSION_PATH_EVIDENCE_STUN_OBSERVED |
                 LI_SESSION_PATH_EVIDENCE_ICE_VALIDATED;
        break;
      case transport_route_e::relay_quic:
      case transport_route_e::relay_tcp_tls:
        flags |= LI_SESSION_PATH_EVIDENCE_RELAY_ALLOCATED;
        break;
    }
    return flags;
  }

  enum class ice_candidate_type_e : std::uint8_t {
    host,
    server_reflexive,
    peer_reflexive,
    relay,
  };

  struct path_candidate_t {
    transport_path_t path;
    ice_candidate_type_e ice_type { ice_candidate_type_e::host };
    std::string foundation;
    std::string address;
    std::uint16_t port {};
    std::uint32_t priority {};
    bool relay {};
  };

  struct path_check_result_t {
    std::uint64_t path_id {};
    bool reachable {};
    std::uint32_t rtt_ms {};
    std::uint32_t loss_ppm {};
    std::uint32_t throughput_kbps {};
  };

  class path_racing_t {
  public:
    void
    add_candidate(transport_path_t path) {
      paths_.push_back(std::move(path));
    }

    void
    submit_check_result(const path_check_result_t &result) {
      for (auto &path : paths_) {
        if (path.path_id != result.path_id) {
          continue;
        }

        path.state = result.reachable ?
                       transport_path_state_e::active :
                       transport_path_state_e::failed;
        path.score.rtt_ms = result.rtt_ms;
        path.score.loss_ppm = result.loss_ppm;
        path.score.throughput_kbps = result.throughput_kbps;
        path.score.cost = path_cost(path);
        return;
      }
    }

    std::optional<transport_path_t>
    select_best() const {
      const transport_path_t *best = nullptr;
      for (const auto &path : paths_) {
        if (path.state != transport_path_state_e::active) {
          continue;
        }
        if (!best || path_cost(path) < path_cost(*best)) {
          best = &path;
        }
      }

      if (!best) {
        return std::nullopt;
      }
      return *best;
    }

  private:
    static std::uint32_t
    path_cost(const transport_path_t &path) {
      const auto relay_penalty =
        path.route == transport_route_e::relay_quic ||
        path.route == transport_route_e::relay_tcp_tls ? 15U : 0U;
      const auto tcp_penalty = path.protocol == transport_protocol_e::tcp_tls ? 20U : 0U;
      return path.score.rtt_ms + path.score.jitter_ms + path.score.loss_ppm / 1000U +
             relay_penalty + tcp_penalty;
    }

    std::vector<transport_path_t> paths_;
  };

  inline std::string
  join_names(const std::vector<std::string_view> &names) {
    std::string result;
    for (const auto name : names) {
      if (!result.empty()) {
        result += ',';
      }
      result += name;
    }
    return result;
  }

  struct rtsp_capability_manifest_t {
    feature_caps_t supported_caps;
    feature_caps_t planned_caps;
    std::vector<transport_route_e> transport_paths;
  };

  inline rtsp_capability_manifest_t
  default_rtsp_capability_manifest() {
    feature_caps_t supported;
    supported.enable(capability_e::native_renderer_metrics)
      .enable(capability_e::metal_renderer_metrics)
      .enable(capability_e::frame_reuse_feedback)
      .enable(capability_e::owd_feedback)
      .enable(capability_e::transport_cc_lite)
      .enable(capability_e::packet_pacer_probe)
      .enable(capability_e::nack_rtx);

    feature_caps_t planned;
    planned.enable(capability_e::red_fec)
      .enable(capability_e::ulpfec)
      .enable(capability_e::flexfec)
      .enable(capability_e::ice_stun)
      .enable(capability_e::turn_relay)
      .enable(capability_e::quic_direct)
      .enable(capability_e::relay_quic)
      .enable(capability_e::relay_tcp_tls)
      .enable(capability_e::upnp_public_mapping)
      .enable(capability_e::manual_public_port_forward);

    return {
      .supported_caps = supported,
      .planned_caps = planned,
      .transport_paths = {
        transport_route_e::lan_direct,
        transport_route_e::manual_public_port_forward,
        transport_route_e::upnp_public_mapping,
        transport_route_e::ice_stun_p2p,
        transport_route_e::relay_quic,
        transport_route_e::relay_tcp_tls,
      },
    };
  }

  inline std::vector<std::string>
  rtsp_capability_attributes(const rtsp_capability_manifest_t &manifest) {
    std::vector<std::string_view> path_names;
    path_names.reserve(manifest.transport_paths.size());
    for (const auto route : manifest.transport_paths) {
      path_names.push_back(transport_route_name(route));
    }

    return {
      "x-ss-core.sessionVersion:1",
      "x-ss-core.feedbackVersion:1",
      "x-ss-core.featureBits:" + std::to_string(li_host_baseline_features() |
                                                li_features_for_caps(manifest.supported_caps)),
      "x-ss-core.supportedCaps:" + join_names(manifest.supported_caps.names()),
      "x-ss-core.plannedCaps:" + join_names(manifest.planned_caps.names()),
      "x-ss-core.transportPaths:" + join_names(path_names),
    };
  }

  inline std::string
  to_string(std::uint64_t value) {
    return std::to_string(value);
  }

  inline std::string
  to_hex_string(std::uint64_t value) {
    char buffer[32] {};
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
  }

  inline std::string_view
  lease_feature_name(std::uint32_t feature) {
    switch (feature) {
      case LI_SESSION_RESOURCE_INPUT_FOCUS:
        return "input-focus";
      case LI_SESSION_RESOURCE_MICROPHONE:
        return "microphone";
      case LI_SESSION_RESOURCE_CLIPBOARD:
        return "clipboard";
      case LI_SESSION_RESOURCE_DISPLAY:
        return "display";
      case LI_SESSION_RESOURCE_DYNAMIC_QUALITY:
        return "dynamic-quality";
      case LI_SESSION_RESOURCE_TRANSPORT_QOS:
        return "transport-qos";
      case LI_SESSION_RESOURCE_CURSOR_PLANE:
        return "cursor-plane";
      case LI_SESSION_RESOURCE_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  lease_mode_name(std::uint32_t mode) {
    switch (mode) {
      case LI_SESSION_LEASE_MODE_EXCLUSIVE_OWNER:
        return "exclusive-owner";
      case LI_SESSION_LEASE_MODE_OBSERVER:
        return "observer";
      case LI_SESSION_LEASE_MODE_SHARED:
        return "shared";
      case LI_SESSION_LEASE_MODE_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_state_name(std::uint32_t state) {
    switch (state) {
      case LI_SESSION_STATE_IDLE:
        return "idle";
      case LI_SESSION_STATE_CONNECTING:
        return "connecting";
      case LI_SESSION_STATE_NEGOTIATING:
        return "negotiating";
      case LI_SESSION_STATE_PROBING:
        return "probing";
      case LI_SESSION_STATE_STREAMING:
        return "streaming";
      case LI_SESSION_STATE_RECOVERING:
        return "recovering";
      case LI_SESSION_STATE_MIGRATING:
        return "migrating";
      case LI_SESSION_STATE_DISCONNECTING:
        return "disconnecting";
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_pointer_mode_name(std::uint32_t mode) {
    switch (mode) {
      case LI_SESSION_POINTER_MODE_ABSOLUTE:
        return "absolute";
      case LI_SESSION_POINTER_MODE_RELATIVE:
        return "relative";
      case LI_SESSION_POINTER_MODE_HYBRID:
        return "hybrid";
      case LI_SESSION_POINTER_MODE_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_transport_kind_name(std::uint32_t kind) {
    switch (kind) {
      case LI_SESSION_TRANSPORT_DIRECT_ENET:
        return "direct-enet";
      case LI_SESSION_TRANSPORT_DIRECT_QUIC:
        return "direct-quic";
      case LI_SESSION_TRANSPORT_RELAY_QUIC:
        return "relay-quic";
      case LI_SESSION_TRANSPORT_RELAY_TCP:
        return "relay-tcp";
      case LI_SESSION_TRANSPORT_LOCAL:
        return "local";
      case LI_SESSION_TRANSPORT_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_transport_protocol_name(std::uint32_t protocol) {
    switch (protocol) {
      case LI_SESSION_TRANSPORT_PROTOCOL_ENET_UDP:
        return "enet-udp";
      case LI_SESSION_TRANSPORT_PROTOCOL_UDP:
        return "udp";
      case LI_SESSION_TRANSPORT_PROTOCOL_QUIC:
        return "quic";
      case LI_SESSION_TRANSPORT_PROTOCOL_TCP_TLS:
        return "tcp-tls";
      case LI_SESSION_TRANSPORT_PROTOCOL_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_candidate_type_name(std::uint32_t candidate_type) {
    switch (candidate_type) {
      case LI_SESSION_PATH_CANDIDATE_HOST:
        return "host";
      case LI_SESSION_PATH_CANDIDATE_SERVER_REFLEXIVE:
        return "server-reflexive";
      case LI_SESSION_PATH_CANDIDATE_PEER_REFLEXIVE:
        return "peer-reflexive";
      case LI_SESSION_PATH_CANDIDATE_RELAY:
        return "relay";
      case LI_SESSION_PATH_CANDIDATE_OPAQUE:
        return "opaque";
      case LI_SESSION_PATH_CANDIDATE_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_discovery_source_name(std::uint32_t discovery_source) {
    switch (discovery_source) {
      case LI_SESSION_PATH_DISCOVERY_USER_CONFIG:
        return "user-config";
      case LI_SESSION_PATH_DISCOVERY_LAN_DISCOVERY:
        return "lan-discovery";
      case LI_SESSION_PATH_DISCOVERY_HOST_ADVERTISEMENT:
        return "host-advertisement";
      case LI_SESSION_PATH_DISCOVERY_DNS:
        return "dns";
      case LI_SESSION_PATH_DISCOVERY_STUN:
        return "stun";
      case LI_SESSION_PATH_DISCOVERY_ICE:
        return "ice";
      case LI_SESSION_PATH_DISCOVERY_RELAY_DIRECTORY:
        return "relay-directory";
      case LI_SESSION_PATH_DISCOVERY_OS_ROUTE:
        return "os-route";
      case LI_SESSION_PATH_DISCOVERY_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_address_family_name(std::uint32_t address_family) {
    switch (address_family) {
      case LI_SESSION_PATH_ADDRESS_FAMILY_IPV4:
        return "ipv4";
      case LI_SESSION_PATH_ADDRESS_FAMILY_IPV6:
        return "ipv6";
      case LI_SESSION_PATH_ADDRESS_FAMILY_DUAL_STACK:
        return "dual-stack";
      case LI_SESSION_PATH_ADDRESS_FAMILY_DOMAIN:
        return "domain";
      case LI_SESSION_PATH_ADDRESS_FAMILY_OPAQUE:
        return "opaque";
      case LI_SESSION_PATH_ADDRESS_FAMILY_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_address_scope_name(std::uint32_t address_scope) {
    switch (address_scope) {
      case LI_SESSION_PATH_ADDRESS_SCOPE_LOOPBACK:
        return "loopback";
      case LI_SESSION_PATH_ADDRESS_SCOPE_LINK_LOCAL:
        return "link-local";
      case LI_SESSION_PATH_ADDRESS_SCOPE_PRIVATE:
        return "private";
      case LI_SESSION_PATH_ADDRESS_SCOPE_CGNAT:
        return "cgnat";
      case LI_SESSION_PATH_ADDRESS_SCOPE_PUBLIC:
        return "public";
      case LI_SESSION_PATH_ADDRESS_SCOPE_RESERVED:
        return "reserved";
      case LI_SESSION_PATH_ADDRESS_SCOPE_FAKE:
        return "fake";
      case LI_SESSION_PATH_ADDRESS_SCOPE_OPAQUE:
        return "opaque";
      case LI_SESSION_PATH_ADDRESS_SCOPE_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_egress_kind_name(std::uint32_t egress_kind) {
    switch (egress_kind) {
      case LI_SESSION_PATH_EGRESS_PHYSICAL:
        return "physical";
      case LI_SESSION_PATH_EGRESS_TUNNEL:
        return "tunnel";
      case LI_SESSION_PATH_EGRESS_VIRTUAL:
        return "virtual";
      case LI_SESSION_PATH_EGRESS_LOOPBACK:
        return "loopback";
      case LI_SESSION_PATH_EGRESS_PROXY:
        return "proxy";
      case LI_SESSION_PATH_EGRESS_RELAY:
        return "relay";
      case LI_SESSION_PATH_EGRESS_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_underlay_kind_name(std::uint32_t underlay_kind) {
    switch (underlay_kind) {
      case LI_SESSION_PATH_UNDERLAY_WIRED:
        return "wired";
      case LI_SESSION_PATH_UNDERLAY_WIFI:
        return "wifi";
      case LI_SESSION_PATH_UNDERLAY_CELLULAR:
        return "cellular";
      case LI_SESSION_PATH_UNDERLAY_VIRTUAL:
        return "virtual";
      case LI_SESSION_PATH_UNDERLAY_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_encapsulation_name(std::uint32_t encapsulation) {
    switch (encapsulation) {
      case LI_SESSION_PATH_ENCAPSULATION_NONE:
        return "none";
      case LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP:
        return "native-ip";
      case LI_SESSION_PATH_ENCAPSULATION_VPN_TUNNEL:
        return "vpn-tunnel";
      case LI_SESSION_PATH_ENCAPSULATION_UDP_TUNNEL:
        return "udp-tunnel";
      case LI_SESSION_PATH_ENCAPSULATION_TCP_PROXY:
        return "tcp-proxy";
      case LI_SESSION_PATH_ENCAPSULATION_TLS_PROXY:
        return "tls-proxy";
      case LI_SESSION_PATH_ENCAPSULATION_RELAY:
        return "relay";
      case LI_SESSION_PATH_ENCAPSULATION_ICE:
        return "ice";
      case LI_SESSION_PATH_ENCAPSULATION_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_path_identity_kind_name(std::uint32_t kind) {
    switch (kind) {
      case LI_SESSION_PATH_IDENTITY_TRUE_LAN:
        return "true-lan";
      case LI_SESSION_PATH_IDENTITY_HOST_DIRECT_PUBLIC:
        return "host-direct-public";
      case LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD:
        return "router-port-forward";
      case LI_SESSION_PATH_IDENTITY_EXTERNAL_FORWARD:
        return "external-forward";
      case LI_SESSION_PATH_IDENTITY_VPN_OVERLAY:
        return "vpn-overlay";
      case LI_SESSION_PATH_IDENTITY_RELAY:
        return "relay";
      case LI_SESSION_PATH_IDENTITY_ICE_P2P:
        return "ice-p2p";
      case LI_SESSION_PATH_IDENTITY_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::string_view
  li_startup_class_name(std::uint32_t startup_class) {
    switch (startup_class) {
      case LI_SESSION_STARTUP_CLASS_LAN_FAST:
        return "lan-fast";
      case LI_SESSION_STARTUP_CLASS_REMOTE_SAFE:
        return "remote-safe";
      case LI_SESSION_STARTUP_CLASS_RELAY_SAFE:
        return "relay-safe";
      case LI_SESSION_STARTUP_CLASS_PROBE_FIRST:
        return "probe-first";
      case LI_SESSION_STARTUP_CLASS_UNKNOWN:
      default:
        return "unknown";
    }
  }

  inline std::vector<std::string>
  session_snapshot_attributes(const LI_SESSION &session) {
    const bool relay_selected = (session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_RELAY) != 0;
    const bool p2p_valid = (session.transportPath.stackFlags & LI_SESSION_TRANSPORT_STACK_P2P) != 0 &&
                           (session.transportPath.evidenceFlags & LI_SESSION_PATH_EVIDENCE_ICE_VALIDATED) != 0;
    const auto relay_mode_name = relay_selected ? "selected" : "none";
    const auto p2p_state_name = p2p_valid ? "punch-succeeded" : "none";
    const auto p2p_flags_value = p2p_valid ? "0x1" : "0x0";

    return {
      "x-ss-core.sessionVersion:" + std::to_string(LI_SESSION_VERSION),
      "x-ss-core.sessionId:" + std::string { session.sessionId.value },
      "x-ss-core.logicalSessionKey:" + to_hex_string(session.logicalSessionKey),
      "x-ss-core.runtimeId:" + to_string(session.runtimeId),
      "x-ss-core.launchSessionId:" + std::to_string(session.launchSessionId),
      "x-ss-core.controlGeneration:" + std::to_string(session.controlGeneration),
      "x-ss-core.state:" + std::string { li_state_name(session.state) },
      "x-ss-core.appId:" + std::string { session.appId },
      "x-ss-core.appName:" + std::string { session.appName },
      "x-ss-core.client.participantId:" + std::string { session.client.participantId.value },
      "x-ss-core.client.participantKey:" + to_hex_string(session.client.participantKey),
      "x-ss-core.client.clientKey:" + to_hex_string(session.client.clientKey),
      "x-ss-core.client.deviceKey:" + to_hex_string(session.client.deviceKey),
      "x-ss-core.client.displayName:" + std::string { session.client.displayName },
      "x-ss-core.client.deviceName:" + std::string { session.client.deviceName },
      "x-ss-core.host.participantId:" + std::string { session.host.participantId.value },
      "x-ss-core.host.participantKey:" + to_hex_string(session.host.participantKey),
      "x-ss-core.host.clientKey:" + to_hex_string(session.host.clientKey),
      "x-ss-core.host.deviceKey:" + to_hex_string(session.host.deviceKey),
      "x-ss-core.host.displayName:" + std::string { session.host.displayName },
      "x-ss-core.host.deviceName:" + std::string { session.host.deviceName },
      "x-ss-core.lease.feature:" + std::string { lease_feature_name(session.lease.feature) },
      "x-ss-core.lease.mode:" + std::string { lease_mode_name(session.lease.mode) },
      "x-ss-core.lease.ownerRuntimeId:" + to_string(session.lease.ownerRuntimeId),
      "x-ss-core.lease.ownerParticipantKey:" + to_hex_string(session.lease.ownerParticipantKey),
      "x-ss-core.lease.ttlMs:" + std::to_string(session.lease.ttlMs),
      "x-ss-core.lease.graceMs:" + std::to_string(session.lease.graceMs),
      "x-ss-core.lease.valid:" + std::to_string(session.lease.valid ? 1 : 0),
      "x-ss-core.transportPath.pathId:" + to_hex_string(session.transportPath.pathId),
      "x-ss-core.transportPath.kind:" + std::string { li_transport_kind_name(session.transportPath.kind) },
      "x-ss-core.transportPath.protocol:" + std::string { li_transport_protocol_name(session.transportPath.protocol) },
      "x-ss-core.transportPath.flags:" + to_hex_string(session.transportPath.flags),
      "x-ss-core.transportPath.stackFlags:" + to_hex_string(session.transportPath.stackFlags),
      "x-ss-core.transportPath.candidateType:" + std::string { li_path_candidate_type_name(session.transportPath.candidateType) },
      "x-ss-core.transportPath.discoverySource:" + std::string { li_path_discovery_source_name(session.transportPath.discoverySource) },
      "x-ss-core.transportPath.addressFamily:" + std::string { li_path_address_family_name(session.transportPath.addressFamily) },
      "x-ss-core.transportPath.addressScope:" + std::string { li_path_address_scope_name(session.transportPath.addressScope) },
      "x-ss-core.transportPath.egressKind:" + std::string { li_path_egress_kind_name(session.transportPath.egressKind) },
      "x-ss-core.transportPath.underlayKind:" + std::string { li_path_underlay_kind_name(session.transportPath.underlayKind) },
      "x-ss-core.transportPath.encapsulation:" + std::string { li_path_encapsulation_name(session.transportPath.encapsulation) },
      "x-ss-core.transportPath.evidenceFlags:" + to_hex_string(session.transportPath.evidenceFlags),
      "x-ss-core.transportPath.identityConfidencePpm:" + std::to_string(session.transportPath.identityConfidencePpm),
      "x-ss-core.transportPath.qualityConfidencePpm:" + std::to_string(session.transportPath.qualityConfidencePpm),
      "x-ss-core.transportPath.pathIdentityKind:" + std::string { li_path_identity_kind_name(session.transportPath.pathIdentityKind) },
      "x-ss-core.transportPath.startupClass:" + std::string { li_startup_class_name(session.transportPath.startupClass) },
      "x-ss-core.transportPath.relayMode:" + std::string { relay_mode_name },
      "x-ss-core.transportPath.p2pState:" + std::string { p2p_state_name },
      "x-ss-core.transportPath.p2pFlags:" + std::string { p2p_flags_value },
      "x-ss-core.transportPath.reasonFlags:" + to_hex_string(session.transportPath.reasonFlags),
      "x-ss-core.transportPath.riskFlags:" + to_hex_string(session.transportPath.riskFlags),
      "x-ss-core.transportPath.routeId:" + std::string { session.transportPath.routeId },
      "x-ss-core.transportPath.explanationCode:" + std::string { session.transportPath.explanationCode },
      "x-ss-core.transportPath.localEndpoint:" + std::string { session.transportPath.localEndpoint },
      "x-ss-core.transportPath.remoteEndpoint:" + std::string { session.transportPath.remoteEndpoint },
      "x-ss-core.transportPath.observedEndpoint:" + std::string { session.transportPath.observedEndpoint },
      "x-ss-core.transportPath.hostLocalEndpoint:" + std::string { session.transportPath.hostLocalEndpoint },
      "x-ss-core.transportPath.providerId:" + std::string { session.transportPath.providerId },
      "x-ss-core.transportPath.relayName:" + std::string { session.transportPath.relayName },
      "x-ss-core.transportPath.rttUs:" + std::to_string(session.transportPath.rttUs),
      "x-ss-core.transportPath.jitterUs:" + std::to_string(session.transportPath.jitterUs),
      "x-ss-core.transportPath.lossPpm:" + std::to_string(session.transportPath.packetLossPpm),
      "x-ss-core.telemetry.framerate:" + std::to_string(session.telemetry.currentFramerate),
      "x-ss-core.telemetry.resolution:" + std::string { session.telemetry.currentResolution },
      "x-ss-core.telemetry.rttUs:" + std::to_string(session.telemetry.rttUs),
      "x-ss-core.telemetry.jitterUs:" + std::to_string(session.telemetry.jitterUs),
      "x-ss-core.telemetry.lossPpm:" + std::to_string(session.telemetry.packetLossPpm),
      "x-ss-core.telemetry.decodeQueue:" + std::to_string(session.telemetry.videoDecodeQueueDepth),
      "x-ss-core.telemetry.renderQueue:" + std::to_string(session.telemetry.videoRenderQueueDepth),
      "x-ss-core.telemetry.audioQueueMs:" + std::to_string(session.telemetry.audioQueueDepthMs),
      "x-ss-core.telemetry.inputQueue:" + std::to_string(session.telemetry.inputQueueDepth),
      "x-ss-core.telemetry.inputLatencyUs:" + std::to_string(session.telemetry.inputSendLatencyUs),
      "x-ss-core.telemetry.inputAckLatencyUs:" + std::to_string(session.telemetry.inputAckLatencyUs),
      "x-ss-core.telemetry.mouseBacklogUs:" + std::to_string(session.telemetry.mouseBacklogUs),
      "x-ss-core.telemetry.pointerMode:" + std::string { li_pointer_mode_name(session.telemetry.pointerMode) },
      "x-ss-core.telemetry.cursorFlags:" + to_hex_string(session.telemetry.cursorStateFlags),
      "x-ss-core.telemetry.pointerReleaseQueueDepth:" + std::to_string(session.telemetry.pointerReleaseQueueDepth),
      "x-ss-core.telemetry.pointerReleaseQueueDelayUs:" + std::to_string(session.telemetry.pointerReleaseQueueDelayUs),
      "x-ss-core.telemetry.pointerModeSwitchUs:" + std::to_string(session.telemetry.pointerModeSwitchUs),
      "x-ss-core.telemetry.pointerDeltasCoalesced:" + std::to_string(session.telemetry.pointerDeltasCoalesced),
      "x-ss-core.telemetry.pointerAccelerationRiskPpm:" + std::to_string(session.telemetry.pointerAccelerationRiskPpm),
    };
  }

  struct transport_cc_packet_feedback_t {
    std::uint32_t sequence {};
    bool received {};
    std::uint64_t arrival_time_us {};
    std::uint32_t payload_size {};
  };

  struct transport_cc_lite_feedback_t {
    std::uint64_t path_id {};
    std::uint32_t base_sequence {};
    std::uint64_t reference_time_us {};
    std::vector<transport_cc_packet_feedback_t> packets;

    void
    add_received(std::uint32_t sequence, std::uint64_t arrival_time_us, std::uint32_t payload_size) {
      packets.push_back({
        .sequence = sequence,
        .received = true,
        .arrival_time_us = arrival_time_us,
        .payload_size = payload_size,
      });
    }

    void
    add_lost(std::uint32_t sequence) {
      packets.push_back({
        .sequence = sequence,
        .received = false,
      });
    }

    std::size_t
    received_count() const {
      return static_cast<std::size_t>(std::count_if(packets.begin(), packets.end(), [](const auto &packet) {
        return packet.received;
      }));
    }

    std::size_t
    lost_count() const {
      return packets.size() - received_count();
    }

    std::uint32_t
    total_payload_bytes() const {
      std::uint32_t total {};
      for (const auto &packet : packets) {
        if (packet.received) {
          total += packet.payload_size;
        }
      }
      return total;
    }

    std::uint32_t
    last_sequence() const {
      std::uint32_t result = base_sequence;
      for (const auto &packet : packets) {
        result = std::max(result, packet.sequence);
      }
      return result;
    }

    std::uint64_t
    arrival_span_us() const {
      std::optional<std::uint64_t> first;
      std::uint64_t last {};
      for (const auto &packet : packets) {
        if (!packet.received) {
          continue;
        }
        if (!first) {
          first = packet.arrival_time_us;
        }
        last = packet.arrival_time_us;
      }
      return first ? last - *first : 0;
    }
  };

  struct probe_request_t {
    std::uint64_t path_id {};
    int current_bitrate_kbps {};
    int ceiling_bitrate_kbps {};
    std::uint32_t duration_ms { 120 };
  };

  struct startup_ceiling_policy_t {
    int bitrate_seed_kbps {};
    int fps_cap {};
    const char *reason { "default" };
  };

  // Keep risky-route startup conservative, but still align with the 150 Hz
  // high-refresh startup floor used by stream_quality::startup_fps_for_bitrate().
  inline constexpr int kStartupHighRefreshCadenceCap = 120;
  inline constexpr int kStartupRouterPortForwardCadenceCap = 90;

  inline startup_ceiling_policy_t
  startup_ceiling_policy_for_path(const startup_path_decision_t &decision,
                                  int requested_fps) {
    (void) requested_fps;
    if (decision.startup_class == LI_SESSION_STARTUP_CLASS_LAN_FAST) {
      return {};
    }

    if (decision.path_identity_kind == LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD) {
      return {
        .bitrate_seed_kbps = 12000,
        .fps_cap = std::min(std::max(requested_fps, 1), kStartupRouterPortForwardCadenceCap),
        .reason = "router-port-forward-safe",
      };
    }

    if ((decision.risk_flags & (LI_SESSION_PATH_RISK_TUNNEL |
                                LI_SESSION_PATH_RISK_EXTERNAL_FORWARDER |
                                LI_SESSION_PATH_RISK_UNKNOWN_IDENTITY)) != 0 ||
        decision.startup_class == LI_SESSION_STARTUP_CLASS_PROBE_FIRST ||
        decision.startup_class == LI_SESSION_STARTUP_CLASS_RELAY_SAFE) {
      return {
        .bitrate_seed_kbps = 10000,
        .fps_cap = std::min(std::max(requested_fps, 1), kStartupHighRefreshCadenceCap),
        .reason = "remote-risk-safe",
      };
    }

    return {};
  }

  inline bool
  runtime_profile_resolution_reconfig_enabled() {
    // Keep runtime encoder-size reconfiguration disabled by default. The 11:00
    // baseline only used bitrate/FEC/IDR safety actions; enabling runtime scale
    // caused rescue/profile decisions to oscillate encoder resolution and force
    // repeated encoder restarts, which is far more visible than a short quality
    // dip. Runtime scale can be reintroduced later behind a stronger hold/hysteresis
    // gate or a true secondary rescue stream.
    return false;
  }

  struct pacer_probe_plan_t {
    std::uint64_t path_id {};
    bool app_limited {};
    int target_bitrate_kbps {};
    std::uint32_t duration_ms {};
    std::uint32_t probe_budget_bytes {};
    double pacing_gain { 1.6 };
    std::uint32_t min_probe_packets { 5 };
  };

  inline std::vector<int>
  startup_probe_targets_kbps(int start_bitrate_kbps, int ceiling_bitrate_kbps) {
    std::vector<int> targets;
    if (start_bitrate_kbps <= 0 || ceiling_bitrate_kbps <= start_bitrate_kbps) {
      return targets;
    }

    auto current = start_bitrate_kbps;
    while (current < ceiling_bitrate_kbps) {
      const auto next = std::min(ceiling_bitrate_kbps, (current * 16 + 9) / 10);
      if (next <= current) {
        break;
      }
      targets.push_back(next);
      current = next;
    }
    return targets;
  }

  inline pacer_probe_plan_t
  make_alr_probe_plan(const probe_request_t &request) {
    const auto target = std::min(request.ceiling_bitrate_kbps,
                                 (request.current_bitrate_kbps * 16 + 9) / 10);
    const auto budget = static_cast<std::uint32_t>(
      std::max(0, target) * 1000LL / 8LL * request.duration_ms / 1000LL);
    return {
      .path_id = request.path_id,
      .app_limited = true,
      .target_bitrate_kbps = target,
      .duration_ms = request.duration_ms,
      .probe_budget_bytes = budget,
    };
  }

  enum class fec_scheme_e : std::uint8_t {
    frame_rs,
    red,
    ulpfec,
    flexfec,
  };

  struct missing_packet_range_t {
    std::uint32_t first_sequence {};
    std::uint32_t last_sequence {};
  };

  struct rtx_packet_record_t {
    std::uint32_t sequence {};
    std::uint32_t frame_id {};
    std::uint32_t payload_size {};
    std::uint64_t sent_at_ms {};
    bool key_frame {};
  };

  class rtx_history_t {
  public:
    explicit rtx_history_t(std::uint64_t history_window_ms = 1000):
        history_window_ms_(history_window_ms) {}

    void
    remember(rtx_packet_record_t packet) {
      records_.push_back(packet);
    }

    void
    evict_older_than(std::uint64_t now_ms) {
      records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const auto &packet) {
                       return now_ms > packet.sent_at_ms + history_window_ms_;
                     }),
                     records_.end());
    }

    const rtx_packet_record_t *
    lookup(std::uint32_t sequence) const {
      for (const auto &packet : records_) {
        if (packet.sequence == sequence) {
          return &packet;
        }
      }
      return nullptr;
    }

    std::vector<rtx_packet_record_t>
    select_for_retransmission(const std::vector<missing_packet_range_t> &missing_ranges,
                              std::uint64_t now_ms,
                              std::uint64_t playout_deadline_ms,
                              std::uint32_t byte_budget) const {
      std::vector<rtx_packet_record_t> selected;
      if (now_ms >= playout_deadline_ms || byte_budget == 0) {
        return selected;
      }

      std::uint32_t used_bytes {};
      for (const auto &range : missing_ranges) {
        for (auto sequence = range.first_sequence; sequence <= range.last_sequence; ++sequence) {
          const auto *packet = lookup(sequence);
          if (!packet) {
            continue;
          }
          if (now_ms > packet->sent_at_ms + history_window_ms_) {
            continue;
          }
          if (used_bytes + packet->payload_size > byte_budget) {
            return selected;
          }
          selected.push_back(*packet);
          used_bytes += packet->payload_size;
        }
      }
      return selected;
    }

  private:
    std::uint64_t history_window_ms_ {};
    std::vector<rtx_packet_record_t> records_;
  };

  struct session_telemetry_report_t {
    participant_id_t participant;
    std::uint64_t path_id {};
    std::uint32_t displayed_fps {};
    std::uint32_t rtt_ms {};
    std::uint32_t loss_ppm {};
    bool renderer_backpressure {};
    std::uint32_t decode_queue_depth {};
    std::uint32_t render_queue_depth {};
    std::uint32_t audio_queue_depth_ms {};
    std::uint32_t input_queue_depth {};
    std::uint32_t input_send_latency_us {};
    std::uint32_t input_ack_latency_us {};
    std::uint32_t mouse_backlog_us {};
    std::uint32_t pointer_mode { LI_SESSION_POINTER_MODE_UNKNOWN };
    std::uint32_t cursor_state_flags {};
    std::uint32_t pointer_release_queue_depth {};
    std::uint32_t pointer_release_queue_delay_us {};
    std::uint32_t pointer_mode_switch_us {};
    std::uint32_t pointer_deltas_coalesced {};
    std::uint32_t pointer_acceleration_risk_ppm {};
  };
  using telemetry_report_t = session_telemetry_report_t;

  struct input_smoothing_snapshot_t {
    std::uint32_t queue_depth {};
    std::uint32_t queue_delay_us {};
    std::uint32_t mode_switch_us {};
    std::uint32_t deltas_coalesced {};
    std::uint32_t acceleration_risk_ppm {};
    bool release_smoothing_active {};
  };

  inline void
  apply_input_smoothing_snapshot(session_telemetry_report_t &report,
                                 const input_smoothing_snapshot_t &snapshot) {
    report.input_queue_depth = std::max(report.input_queue_depth, snapshot.queue_depth);
    report.input_send_latency_us = std::max(report.input_send_latency_us, snapshot.queue_delay_us);
    report.mouse_backlog_us = std::max(report.mouse_backlog_us, snapshot.queue_delay_us);
    report.pointer_release_queue_depth = snapshot.queue_depth;
    report.pointer_release_queue_delay_us = snapshot.queue_delay_us;
    report.pointer_mode_switch_us = snapshot.mode_switch_us;
    report.pointer_deltas_coalesced = snapshot.deltas_coalesced;
    report.pointer_acceleration_risk_ppm = snapshot.acceleration_risk_ppm;
    if (snapshot.release_smoothing_active ||
        snapshot.queue_depth > 0 ||
        snapshot.queue_delay_us > 0 ||
        snapshot.deltas_coalesced > 0) {
      report.cursor_state_flags |= LI_SESSION_CURSOR_FLAG_RELEASE_SMOOTHING_ACTIVE;
    }
  }

  struct session_telemetry_snapshot_t {
    std::vector<session_telemetry_report_t> reports;

    const session_telemetry_report_t *
    report_for(const participant_id_t &participant) const {
      for (const auto &report : reports) {
        if (report.participant == participant) {
          return &report;
        }
      }
      return nullptr;
    }

    std::size_t
    active_participant_count() const {
      return reports.size();
    }

    bool
    has_renderer_backpressure() const {
      for (const auto &report : reports) {
        if (report.renderer_backpressure) {
          return true;
        }
      }
      return false;
    }
  };
  using telemetry_snapshot_t = session_telemetry_snapshot_t;

  class session_telemetry_t {
  public:
    void
    submit(session_telemetry_report_t report) {
      for (auto &existing : reports_) {
        if (existing.participant == report.participant && existing.path_id == report.path_id) {
          existing = std::move(report);
          return;
        }
      }
      reports_.push_back(std::move(report));
    }

    session_telemetry_snapshot_t
    snapshot() const {
      return { .reports = reports_ };
    }

  private:
    std::vector<session_telemetry_report_t> reports_;
  };

  inline void
  copy_li_string(char *destination, std::size_t destination_length, std::string_view value) {
    if (destination == nullptr || destination_length == 0) {
      return;
    }
    const auto length = std::min(destination_length - 1, value.size());
    std::memcpy(destination, value.data(), length);
    destination[length] = '\0';
  }

  inline LI_SESSION_CONTROL_HELLO
  make_session_control_hello(const LI_SESSION &session) {
    LI_SESSION_CONTROL_HELLO hello {};
    (void) alk_session_control_build_hello(&session, &hello);
    return hello;
  }

  inline LI_SESSION_CONTROL_WELCOME
  make_session_control_welcome(const LI_SESSION &session) {
    LI_SESSION_CONTROL_WELCOME welcome {};
    (void) alk_session_control_build_welcome(&session, &welcome);
    return welcome;
  }

  inline std::optional<LI_SESSION_CONTROL_HELLO>
  parse_session_control_hello(std::string_view payload) {
    LI_SESSION_CONTROL_HELLO hello {};
    if (!alk_session_control_parse_hello(payload.data(), payload.size(), &hello)) {
      return std::nullopt;
    }
    return hello;
  }

  inline std::optional<LI_SESSION_CONTROL_WELCOME>
  parse_session_control_welcome(std::string_view payload) {
    LI_SESSION_CONTROL_WELCOME welcome {};
    if (!alk_session_control_parse_welcome(payload.data(), payload.size(), &welcome)) {
      return std::nullopt;
    }
    return welcome;
  }

  inline LI_SESSION_CONTROL_TELEMETRY
  make_session_control_telemetry(const LI_SESSION &session,
                                 std::uint32_t sequence,
                                 std::uint64_t sent_at_ms = 0) {
    LI_SESSION_CONTROL_TELEMETRY telemetry {};
    (void) alk_session_control_build_telemetry(&session, sequence, sent_at_ms, &telemetry);
    return telemetry;
  }

  inline std::optional<LI_SESSION_CONTROL_TELEMETRY>
  parse_session_control_telemetry(std::string_view payload) {
    LI_SESSION_CONTROL_TELEMETRY telemetry {};
    if (!alk_session_control_parse_telemetry(payload.data(), payload.size(), &telemetry)) {
      return std::nullopt;
    }
    return telemetry;
  }

  inline LI_SESSION_CONTROL_LEASE
  make_session_control_lease_request(const LI_SESSION &session,
                                     std::uint32_t resource,
                                     std::uint32_t mode,
                                     std::uint32_t ttl_ms) {
    LI_SESSION_CONTROL_LEASE lease {};
    (void) alk_session_control_build_lease_request(&session, resource, mode, ttl_ms, &lease);
    return lease;
  }

  inline LI_SESSION_CONTROL_LEASE
  make_session_control_lease_ack(const LI_SESSION &session,
                                 const LI_SESSION_LEASE &granted_lease,
                                 std::uint32_t operation,
                                 std::uint32_t status) {
    LI_SESSION_CONTROL_LEASE lease {};
    (void) alk_session_control_build_lease_ack(&session, &granted_lease, operation, status, &lease);
    return lease;
  }

  inline std::optional<LI_SESSION_CONTROL_LEASE>
  parse_session_control_lease(std::string_view payload) {
    LI_SESSION_CONTROL_LEASE lease {};
    if (!alk_session_control_parse_lease(payload.data(), payload.size(), &lease)) {
      return std::nullopt;
    }
    return lease;
  }

  inline LI_SESSION_CONTROL_CURSOR_PLANE
  make_session_control_cursor_plane(const LI_SESSION &session,
                                    std::uint32_t sequence,
                                    std::uint64_t sent_at_ms = 0) {
    LI_SESSION_CONTROL_CURSOR_PLANE cursor_plane {};
    (void) alk_session_control_build_cursor_plane(&session, sequence, sent_at_ms, &cursor_plane);
    return cursor_plane;
  }

  inline std::optional<LI_SESSION_CONTROL_CURSOR_PLANE>
  parse_session_control_cursor_plane(std::string_view payload) {
    LI_SESSION_CONTROL_CURSOR_PLANE cursor_plane {};
    if (!alk_session_control_parse_cursor_plane(payload.data(), payload.size(), &cursor_plane)) {
      return std::nullopt;
    }
    return cursor_plane;
  }

  inline LI_SESSION
  make_li_session(const identity_t &identity,
                  const transport_path_t &active_path,
                  const feature_caps_t &client_caps,
                  const feature_caps_t &host_caps,
                  const session_telemetry_report_t &report,
                  std::string_view app_id,
                  std::string_view app_name) {
    LI_SESSION session;
    LiInitializeSession(&session);

    const auto session_id = make_session_id(identity);
    const auto participant = make_participant(identity);

    session.state = LI_SESSION_STATE_STREAMING;
    session.logicalSessionKey = session_id.logical_key;
    session.runtimeId = identity.runtime_id;
    session.launchSessionId = identity.launch_session_id;
    session.controlGeneration = identity.control_generation;
    if (!identity.logical_session_id.empty()) {
      copy_li_string(session.sessionId.value, sizeof(session.sessionId.value), identity.logical_session_id);
    }
    else {
      std::snprintf(session.sessionId.value,
                    sizeof(session.sessionId.value),
                    "%llx:%u",
                    static_cast<unsigned long long>(session.logicalSessionKey),
                    identity.launch_session_id);
    }
    copy_li_string(session.appId, sizeof(session.appId), app_id);
    copy_li_string(session.appName, sizeof(session.appName), app_name);

    session.client.role = LI_SESSION_ROLE_CLIENT;
    session.client.index = 0;
    session.client.flags = LI_SESSION_PARTICIPANT_FLAG_PRIMARY | LI_SESSION_PARTICIPANT_FLAG_REMOTE;
    session.client.participantKey = participant.participant_key;
    session.client.clientKey = participant.client_cert_key;
    session.client.deviceKey = participant.device_key;
    if (!identity.participant_id.empty()) {
      copy_li_string(session.client.participantId.value,
                     sizeof(session.client.participantId.value),
                     identity.participant_id);
    }
    else {
      std::snprintf(session.client.participantId.value,
                    sizeof(session.client.participantId.value),
                    "%llx",
                    static_cast<unsigned long long>(participant.participant_key));
    }
    copy_li_string(session.client.displayName, sizeof(session.client.displayName), participant.display_name);
    copy_li_string(session.client.deviceName, sizeof(session.client.deviceName), participant.device_id);

    session.host.role = LI_SESSION_ROLE_HOST;
    session.host.index = 1;
    session.host.flags = LI_SESSION_PARTICIPANT_FLAG_LOCAL;
    constexpr std::string_view host_participant_id = "sunshine-host";
    constexpr std::string_view host_display_name = "Sunshine Host";
    constexpr std::string_view host_device_name = "host";
    const auto host_client_key = stable_key(host_display_name);
    const auto host_device_key = stable_key(host_device_name);
    const auto host_participant_key = host_client_key ^ (host_device_key << 1U);
    session.host.participantKey = host_participant_key;
    session.host.clientKey = host_client_key;
    session.host.deviceKey = host_device_key;
    copy_li_string(session.host.participantId.value, sizeof(session.host.participantId.value), host_participant_id);
    copy_li_string(session.host.displayName, sizeof(session.host.displayName), host_display_name);
    copy_li_string(session.host.deviceName, sizeof(session.host.deviceName), host_device_name);

    session.featureCaps.client = li_client_baseline_features() | li_features_for_caps(client_caps);
    session.featureCaps.host = li_host_baseline_features() | li_features_for_caps(host_caps);
    session.featureCaps.negotiated = session.featureCaps.client & session.featureCaps.host;

    session.lease.feature = LI_SESSION_RESOURCE_INPUT_FOCUS;
    session.lease.mode = LI_SESSION_LEASE_MODE_EXCLUSIVE_OWNER;
    session.lease.ownerRuntimeId = identity.runtime_id;
    session.lease.ownerParticipantKey = participant.participant_key;
    session.lease.valid = true;
    session.lease.renewable = true;

    session.transportPath.pathId = active_path.path_id;
    session.transportPath.kind = li_transport_kind(active_path.route);
    session.transportPath.protocol = li_transport_protocol(active_path.protocol);
    session.transportPath.flags = li_transport_flags(active_path);
    session.transportPath.stackFlags = li_transport_stack_flags(active_path);
    session.transportPath.candidateType = li_path_candidate_type(active_path.route);
    session.transportPath.discoverySource = li_path_discovery_source(active_path.route);
    session.transportPath.egressKind =
      active_path.observed_egress_kind != LI_SESSION_PATH_EGRESS_UNKNOWN ?
        active_path.observed_egress_kind :
        li_path_egress_kind(active_path.route);
    session.transportPath.encapsulation =
      active_path.observed_encapsulation != LI_SESSION_PATH_ENCAPSULATION_UNKNOWN ?
        active_path.observed_encapsulation :
        li_path_encapsulation(active_path.route);
    session.transportPath.evidenceFlags =
      li_path_evidence_flags(active_path) | active_path.extra_evidence_flags;
    session.transportPath.identityConfidencePpm =
      active_path.identity_confidence_ppm != 0 ? active_path.identity_confidence_ppm : 500000U;
    session.transportPath.qualityConfidencePpm =
      active_path.score.rtt_ms != 0 || active_path.score.loss_ppm != 0 ? 800000U : 0U;
    session.transportPath.pathIdentityKind = active_path.path_identity_kind;
    session.transportPath.startupClass = active_path.startup_class;
    session.transportPath.reasonFlags = active_path.reason_flags;
    session.transportPath.riskFlags = active_path.risk_flags;
    session.transportPath.rttUs = active_path.score.rtt_ms * 1000U;
    session.transportPath.jitterUs = active_path.score.jitter_ms * 1000U;
    session.transportPath.packetLossPpm = active_path.score.loss_ppm;
    copy_li_string(session.transportPath.routeId,
                   sizeof(session.transportPath.routeId),
                   transport_route_name(active_path.route));
    copy_li_string(session.transportPath.explanationCode,
                   sizeof(session.transportPath.explanationCode),
                   active_path.explanation_code);
    copy_li_string(session.transportPath.localEndpoint,
                   sizeof(session.transportPath.localEndpoint),
                   active_path.local_endpoint);
    copy_li_string(session.transportPath.remoteEndpoint,
                   sizeof(session.transportPath.remoteEndpoint),
                   active_path.remote_endpoint);
    copy_li_string(session.transportPath.observedEndpoint,
                   sizeof(session.transportPath.observedEndpoint),
                   active_path.observed_endpoint);
    copy_li_string(session.transportPath.hostLocalEndpoint,
                   sizeof(session.transportPath.hostLocalEndpoint),
                   active_path.host_local_endpoint);
    copy_li_string(session.transportPath.providerId,
                   sizeof(session.transportPath.providerId),
                   active_path.provider_id);
    if ((session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_RELAY) != 0) {
      copy_li_string(session.transportPath.relayName,
                     sizeof(session.transportPath.relayName),
                     transport_route_name(active_path.route));
    }

    session.telemetry.currentFramerate = report.displayed_fps;
    session.telemetry.rttUs = report.rtt_ms * 1000U;
    session.telemetry.jitterUs = session.transportPath.jitterUs;
    session.telemetry.packetLossPpm = report.loss_ppm;
    session.telemetry.videoDecodeQueueDepth = report.decode_queue_depth;
    session.telemetry.videoRenderQueueDepth = report.render_queue_depth;
    session.telemetry.audioQueueDepthMs = report.audio_queue_depth_ms;
    session.telemetry.inputQueueDepth = report.input_queue_depth;
    session.telemetry.inputSendLatencyUs = report.input_send_latency_us;
    session.telemetry.inputAckLatencyUs = report.input_ack_latency_us;
    session.telemetry.mouseBacklogUs = report.mouse_backlog_us;
    session.telemetry.pointerMode = report.pointer_mode;
    session.telemetry.cursorStateFlags = report.cursor_state_flags;
    session.telemetry.pointerReleaseQueueDepth = report.pointer_release_queue_depth;
    session.telemetry.pointerReleaseQueueDelayUs = report.pointer_release_queue_delay_us;
    session.telemetry.pointerModeSwitchUs = report.pointer_mode_switch_us;
    session.telemetry.pointerDeltasCoalesced = report.pointer_deltas_coalesced;
    session.telemetry.pointerAccelerationRiskPpm = report.pointer_acceleration_risk_ppm;
    session.telemetry.connectionQuality = report.loss_ppm >= 1000000U ?
                                            0.0 :
                                            1.0 - (static_cast<double>(report.loss_ppm) / 1000000.0);
    return session;
  }

}  // namespace session_runtime
