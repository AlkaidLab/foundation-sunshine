/**
 * @file src/session_runtime.h
 * @brief Shared runtime session identity primitives.
 */
#pragma once

#include <algorithm>
#include <bitset>
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

  struct session_id_t {
    std::uint64_t logical_key {};
    std::uint64_t runtime_id {};
    std::uint32_t launch_session_id {};
    std::uint32_t control_generation {};
    bool trusted_client {};
  };

  inline session_id_t
  make_session_id(const identity_t &identity) {
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
    const auto client_key = identity.client_cert_key != 0 ?
                              identity.client_cert_key :
                              stable_key(identity.client_name);
    const auto device_key = stable_key(device_id.empty() ? identity.client_name : device_id);
    const auto participant_key = client_key ^ (device_key << 1U);

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
    return features;
  }

  inline std::uint64_t
  li_client_baseline_features() {
    return LI_SESSION_FEATURE_CURSOR_PLANE |
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
           LI_SESSION_FEATURE_DYNAMIC_QUALITY;
  }

  inline std::uint64_t
  li_host_baseline_features() {
    return LI_SESSION_FEATURE_CURSOR_PLANE |
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
      "x-ss-core.supportedCaps:" + join_names(manifest.supported_caps.names()),
      "x-ss-core.plannedCaps:" + join_names(manifest.planned_caps.names()),
      "x-ss-core.transportPaths:" + join_names(path_names),
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
    std::snprintf(session.sessionId.value,
                  sizeof(session.sessionId.value),
                  "%llx:%u",
                  static_cast<unsigned long long>(session.logicalSessionKey),
                  identity.launch_session_id);
    copy_li_string(session.appId, sizeof(session.appId), app_id);
    copy_li_string(session.appName, sizeof(session.appName), app_name);

    session.client.role = LI_SESSION_ROLE_CLIENT;
    session.client.flags = LI_SESSION_PARTICIPANT_FLAG_PRIMARY | LI_SESSION_PARTICIPANT_FLAG_REMOTE;
    session.client.participantKey = participant.participant_key;
    session.client.clientKey = participant.client_cert_key;
    session.client.deviceKey = participant.device_key;
    std::snprintf(session.client.participantId.value,
                  sizeof(session.client.participantId.value),
                  "%llx",
                  static_cast<unsigned long long>(participant.participant_key));
    copy_li_string(session.client.displayName, sizeof(session.client.displayName), participant.display_name);
    copy_li_string(session.client.deviceName, sizeof(session.client.deviceName), participant.device_id);

    session.host.role = LI_SESSION_ROLE_HOST;
    session.host.flags = LI_SESSION_PARTICIPANT_FLAG_LOCAL;
    copy_li_string(session.host.participantId.value, sizeof(session.host.participantId.value), "sunshine-host");
    copy_li_string(session.host.displayName, sizeof(session.host.displayName), "Sunshine Host");
    copy_li_string(session.host.deviceName, sizeof(session.host.deviceName), "host");

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
    session.transportPath.rttUs = active_path.score.rtt_ms * 1000U;
    session.transportPath.jitterUs = active_path.score.jitter_ms * 1000U;
    session.transportPath.packetLossPpm = active_path.score.loss_ppm;
    copy_li_string(session.transportPath.routeId,
                   sizeof(session.transportPath.routeId),
                   transport_route_name(active_path.route));
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
