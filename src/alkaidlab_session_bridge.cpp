#include "alkaidlab_session_bridge.h"

#include <cstring>

namespace stream::alkaidlab_session_bridge {

  namespace {

    uint32_t
    map_session_state(uint32_t li_state) {
      switch (li_state) {
        case LI_SESSION_STATE_IDLE:
          return ALK_SESSION_STATE_IDLE;
        case LI_SESSION_STATE_CONNECTING:
          return ALK_SESSION_STATE_CONNECTING;
        case LI_SESSION_STATE_NEGOTIATING:
          return ALK_SESSION_STATE_NEGOTIATING;
        case LI_SESSION_STATE_PROBING:
          return ALK_SESSION_STATE_PROBING;
        case LI_SESSION_STATE_STREAMING:
          return ALK_SESSION_STATE_STREAMING;
        case LI_SESSION_STATE_RECOVERING:
          return ALK_SESSION_STATE_RECOVERING;
        case LI_SESSION_STATE_MIGRATING:
          return ALK_SESSION_STATE_MIGRATING;
        case LI_SESSION_STATE_DISCONNECTING:
          return ALK_SESSION_STATE_DISCONNECTING;
        default:
          return ALK_SESSION_STATE_IDLE;
      }
    }

    uint32_t
    map_transport_protocol_family(uint32_t li_protocol) {
      switch (li_protocol) {
        case LI_SESSION_TRANSPORT_PROTOCOL_ENET_UDP:
        case LI_SESSION_TRANSPORT_PROTOCOL_UDP:
        case LI_SESSION_TRANSPORT_PROTOCOL_QUIC:
          return ALK_TRANSPORT_PROTOCOL_FAMILY_UDP;
        case LI_SESSION_TRANSPORT_PROTOCOL_TCP_TLS:
          return ALK_TRANSPORT_PROTOCOL_FAMILY_TLS;
        default:
          return ALK_TRANSPORT_PROTOCOL_FAMILY_UNKNOWN;
      }
    }

    uint32_t
    map_transport_stack_flags(uint32_t li_stack_flags) {
      uint32_t flags = 0;
      if ((li_stack_flags & (LI_SESSION_TRANSPORT_STACK_ENET |
                             LI_SESSION_TRANSPORT_STACK_UDP |
                             LI_SESSION_TRANSPORT_STACK_QUIC |
                             LI_SESSION_TRANSPORT_STACK_WEBRTC)) != 0) {
        flags |= ALK_TRANSPORT_STACK_CARRIER_DATAGRAM;
      }
      if ((li_stack_flags & (LI_SESSION_TRANSPORT_STACK_TCP |
                             LI_SESSION_TRANSPORT_STACK_TLS |
                             LI_SESSION_TRANSPORT_STACK_SCTP)) != 0) {
        flags |= ALK_TRANSPORT_STACK_CARRIER_STREAM;
      }
      if ((li_stack_flags & (LI_SESSION_TRANSPORT_STACK_TLS |
                             LI_SESSION_TRANSPORT_STACK_QUIC |
                             LI_SESSION_TRANSPORT_STACK_WEBRTC)) != 0) {
        flags |= ALK_TRANSPORT_STACK_ENCRYPTION;
      }
      if ((li_stack_flags & (LI_SESSION_TRANSPORT_STACK_STUN |
                             LI_SESSION_TRANSPORT_STACK_ICE |
                             LI_SESSION_TRANSPORT_STACK_TURN)) != 0) {
        flags |= ALK_TRANSPORT_STACK_NAT_TRAVERSAL;
      }
      if ((li_stack_flags & (LI_SESSION_TRANSPORT_STACK_TURN |
                             LI_SESSION_TRANSPORT_STACK_RELAY)) != 0) {
        flags |= ALK_TRANSPORT_STACK_RELAY;
      }
      if ((li_stack_flags & (LI_SESSION_TRANSPORT_STACK_RTP |
                             LI_SESSION_TRANSPORT_STACK_RTCP)) != 0) {
        flags |= ALK_TRANSPORT_STACK_MEDIA_PACKETIZATION;
      }
      if ((li_stack_flags & LI_SESSION_TRANSPORT_STACK_DATA_CHANNEL) != 0) {
        flags |= ALK_TRANSPORT_STACK_DATA_CHANNEL;
      }
      if ((li_stack_flags & LI_SESSION_TRANSPORT_STACK_P2P) != 0) {
        flags |= ALK_TRANSPORT_STACK_P2P;
      }
      return flags;
    }

    bool
    transport_provider_is_gamestream_enet(const char *provider_id) {
      return provider_id != nullptr && std::strstr(provider_id, "gamestream-enet") != nullptr;
    }

    uint32_t
    map_transport_stack_flags_to_li(uint32_t stack_flags, const char *provider_id) {
      uint32_t flags = 0;
      if (transport_provider_is_gamestream_enet(provider_id)) {
        flags |= LI_SESSION_TRANSPORT_STACK_ENET |
                 LI_SESSION_TRANSPORT_STACK_UDP |
                 LI_SESSION_TRANSPORT_STACK_RTP |
                 LI_SESSION_TRANSPORT_STACK_RTCP;
      }
      else if ((stack_flags & ALK_TRANSPORT_STACK_CARRIER_DATAGRAM) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_UDP;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_CARRIER_STREAM) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_TCP;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_ENCRYPTION) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_TLS;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_NAT_TRAVERSAL) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_STUN | LI_SESSION_TRANSPORT_STACK_ICE;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_RELAY) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_RELAY;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_MEDIA_PACKETIZATION) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_RTP | LI_SESSION_TRANSPORT_STACK_RTCP;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_DATA_CHANNEL) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_DATA_CHANNEL;
      }
      if ((stack_flags & ALK_TRANSPORT_STACK_P2P) != 0) {
        flags |= LI_SESSION_TRANSPORT_STACK_P2P;
      }
      return flags;
    }

    uint32_t
    map_transport_identity(uint32_t li_identity) {
      switch (li_identity) {
        case LI_SESSION_PATH_IDENTITY_TRUE_LAN:
          return ALK_TRANSPORT_IDENTITY_TRUE_LAN;
        case LI_SESSION_PATH_IDENTITY_HOST_DIRECT_PUBLIC:
          return ALK_TRANSPORT_IDENTITY_HOST_DIRECT_PUBLIC;
        case LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD:
          return ALK_TRANSPORT_IDENTITY_ROUTER_PORT_FORWARD;
        case LI_SESSION_PATH_IDENTITY_EXTERNAL_FORWARD:
          return ALK_TRANSPORT_IDENTITY_EXTERNAL_FORWARD;
        case LI_SESSION_PATH_IDENTITY_VPN_OVERLAY:
          return ALK_TRANSPORT_IDENTITY_VPN_OVERLAY;
        case LI_SESSION_PATH_IDENTITY_RELAY:
          return ALK_TRANSPORT_IDENTITY_RELAY;
        case LI_SESSION_PATH_IDENTITY_ICE_P2P:
          return ALK_TRANSPORT_IDENTITY_P2P_DIRECT;
        default:
          return ALK_TRANSPORT_IDENTITY_UNKNOWN;
      }
    }

    uint32_t
    map_startup_class(uint32_t li_startup_class) {
      switch (li_startup_class) {
        case LI_SESSION_STARTUP_CLASS_LAN_FAST:
          return ALK_STARTUP_CLASS_LAN_FAST;
        case LI_SESSION_STARTUP_CLASS_REMOTE_SAFE:
          return ALK_STARTUP_CLASS_REMOTE_SAFE;
        case LI_SESSION_STARTUP_CLASS_RELAY_SAFE:
          return ALK_STARTUP_CLASS_RELAY_SAFE;
        case LI_SESSION_STARTUP_CLASS_PROBE_FIRST:
          return ALK_STARTUP_CLASS_PROBE_FIRST;
        default:
          return ALK_STARTUP_CLASS_UNKNOWN;
      }
    }

    uint32_t
    map_resource_kind(uint32_t li_resource) {
      switch (li_resource) {
        case LI_SESSION_RESOURCE_INPUT_FOCUS:
          return ALK_RESOURCE_INPUT_FOCUS;
        case LI_SESSION_RESOURCE_MICROPHONE:
          return ALK_RESOURCE_AUDIO_MICROPHONE;
        case LI_SESSION_RESOURCE_CLIPBOARD:
          return ALK_RESOURCE_CLIPBOARD;
        case LI_SESSION_RESOURCE_DISPLAY:
          return ALK_RESOURCE_DISPLAY_OUTPUT;
        case LI_SESSION_RESOURCE_DYNAMIC_QUALITY:
          return ALK_RESOURCE_DYNAMIC_QUALITY;
        case LI_SESSION_RESOURCE_TRANSPORT_QOS:
          return ALK_RESOURCE_TRANSPORT_QOS;
        case LI_SESSION_RESOURCE_CURSOR_PLANE:
          return ALK_RESOURCE_CURSOR_PLANE;
        default:
          return ALK_RESOURCE_UNKNOWN;
      }
    }

    uint32_t
    map_session_state_to_li(uint32_t state) {
      switch (state) {
        case ALK_SESSION_STATE_IDLE:
          return LI_SESSION_STATE_IDLE;
        case ALK_SESSION_STATE_CONNECTING:
          return LI_SESSION_STATE_CONNECTING;
        case ALK_SESSION_STATE_NEGOTIATING:
          return LI_SESSION_STATE_NEGOTIATING;
        case ALK_SESSION_STATE_PROBING:
          return LI_SESSION_STATE_PROBING;
        case ALK_SESSION_STATE_STREAMING:
          return LI_SESSION_STATE_STREAMING;
        case ALK_SESSION_STATE_RECOVERING:
          return LI_SESSION_STATE_RECOVERING;
        case ALK_SESSION_STATE_MIGRATING:
          return LI_SESSION_STATE_MIGRATING;
        case ALK_SESSION_STATE_DISCONNECTING:
          return LI_SESSION_STATE_DISCONNECTING;
        default:
          return LI_SESSION_STATE_IDLE;
      }
    }

    uint32_t
    map_transport_protocol_family_to_li(uint32_t protocol_family, const char *provider_id) {
      if (transport_provider_is_gamestream_enet(provider_id)) {
        return LI_SESSION_TRANSPORT_PROTOCOL_ENET_UDP;
      }
      switch (protocol_family) {
        case ALK_TRANSPORT_PROTOCOL_FAMILY_UDP:
          return LI_SESSION_TRANSPORT_PROTOCOL_UDP;
        case ALK_TRANSPORT_PROTOCOL_FAMILY_TCP:
        case ALK_TRANSPORT_PROTOCOL_FAMILY_TLS:
          return LI_SESSION_TRANSPORT_PROTOCOL_TCP_TLS;
        default:
          return LI_SESSION_TRANSPORT_PROTOCOL_UNKNOWN;
      }
    }

    uint32_t
    map_transport_identity_to_li(uint32_t identity) {
      switch (identity) {
        case ALK_TRANSPORT_IDENTITY_TRUE_LAN:
          return LI_SESSION_PATH_IDENTITY_TRUE_LAN;
        case ALK_TRANSPORT_IDENTITY_HOST_DIRECT_PUBLIC:
          return LI_SESSION_PATH_IDENTITY_HOST_DIRECT_PUBLIC;
        case ALK_TRANSPORT_IDENTITY_ROUTER_PORT_FORWARD:
          return LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD;
        case ALK_TRANSPORT_IDENTITY_EXTERNAL_FORWARD:
          return LI_SESSION_PATH_IDENTITY_EXTERNAL_FORWARD;
        case ALK_TRANSPORT_IDENTITY_VPN_OVERLAY:
          return LI_SESSION_PATH_IDENTITY_VPN_OVERLAY;
        case ALK_TRANSPORT_IDENTITY_RELAY:
          return LI_SESSION_PATH_IDENTITY_RELAY;
        case ALK_TRANSPORT_IDENTITY_P2P_DIRECT:
          return LI_SESSION_PATH_IDENTITY_ICE_P2P;
        default:
          return LI_SESSION_PATH_IDENTITY_UNKNOWN;
      }
    }

    uint32_t
    map_startup_class_to_li(uint32_t startup_class) {
      switch (startup_class) {
        case ALK_STARTUP_CLASS_LAN_FAST:
          return LI_SESSION_STARTUP_CLASS_LAN_FAST;
        case ALK_STARTUP_CLASS_REMOTE_SAFE:
          return LI_SESSION_STARTUP_CLASS_REMOTE_SAFE;
        case ALK_STARTUP_CLASS_RELAY_SAFE:
          return LI_SESSION_STARTUP_CLASS_RELAY_SAFE;
        case ALK_STARTUP_CLASS_PROBE_FIRST:
          return LI_SESSION_STARTUP_CLASS_PROBE_FIRST;
        default:
          return LI_SESSION_STARTUP_CLASS_UNKNOWN;
      }
    }

    uint32_t
    map_resource_kind_to_li(uint32_t resource_kind) {
      switch (resource_kind) {
        case ALK_RESOURCE_INPUT_FOCUS:
          return LI_SESSION_RESOURCE_INPUT_FOCUS;
        case ALK_RESOURCE_AUDIO_MICROPHONE:
          return LI_SESSION_RESOURCE_MICROPHONE;
        case ALK_RESOURCE_CLIPBOARD:
          return LI_SESSION_RESOURCE_CLIPBOARD;
        case ALK_RESOURCE_DISPLAY_OUTPUT:
          return LI_SESSION_RESOURCE_DISPLAY;
        case ALK_RESOURCE_DYNAMIC_QUALITY:
          return LI_SESSION_RESOURCE_DYNAMIC_QUALITY;
        case ALK_RESOURCE_TRANSPORT_QOS:
          return LI_SESSION_RESOURCE_TRANSPORT_QOS;
        case ALK_RESOURCE_CURSOR_PLANE:
          return LI_SESSION_RESOURCE_CURSOR_PLANE;
        default:
          return LI_SESSION_RESOURCE_UNKNOWN;
      }
    }

    uint32_t
    map_lease_mode_to_li(uint32_t mode) {
      switch (mode) {
        case ALK_LEASE_MODE_EXCLUSIVE_OWNER:
          return LI_SESSION_LEASE_MODE_EXCLUSIVE_OWNER;
        case ALK_LEASE_MODE_OBSERVER:
          return LI_SESSION_LEASE_MODE_OBSERVER;
        case ALK_LEASE_MODE_SHARED:
          return LI_SESSION_LEASE_MODE_SHARED;
        default:
          return LI_SESSION_LEASE_MODE_UNKNOWN;
      }
    }

    void
    project_participant_to_li(const AlkParticipant &source, LI_SESSION_PARTICIPANT &destination) {
      destination.role = source.role == ALK_SESSION_ROLE_HOST ? LI_SESSION_ROLE_HOST :
                           source.role == ALK_SESSION_ROLE_CLIENT ? LI_SESSION_ROLE_CLIENT :
                           LI_SESSION_ROLE_UNKNOWN;
      destination.index = source.index;
      destination.flags = source.flags;
      destination.participantKey = source.participant_key;
      destination.clientKey = source.client_key;
      destination.deviceKey = source.device_key;
      if (source.participant_id.value[0] != '\0') {
        alk_session_copy_string(destination.participantId.value,
                                sizeof(destination.participantId.value),
                                source.participant_id.value);
      }
      if (source.display_name[0] != '\0') {
        alk_session_copy_string(destination.displayName,
                                sizeof(destination.displayName),
                                source.display_name);
      }
      if (source.device_name[0] != '\0') {
        alk_session_copy_string(destination.deviceName,
                                sizeof(destination.deviceName),
                                source.device_name);
      }
    }

    AlkResourceLease *
    find_or_append_lease(AlkSessionSnapshot &snapshot, uint32_t resource_kind) {
      for (uint32_t i = 0; i < snapshot.lease_count; ++i) {
        if (snapshot.leases[i].resource_kind == resource_kind) {
          return &snapshot.leases[i];
        }
      }
      if (snapshot.lease_count >= ALK_SESSION_MAX_LEASES) {
        return nullptr;
      }
      auto &lease = snapshot.leases[snapshot.lease_count++];
      alk_resource_lease_init(&lease);
      lease.resource_kind = resource_kind;
      lease.resource_key = alk_session_stable_key(alk_resource_kind_name(resource_kind));
      return &lease;
    }

  }  // namespace

  bool
  update_from_li_session(AlkSessionAdapterContext &context, const LI_SESSION &session) {
    AlkGameStreamHostLegacyLaunch launch;
    alk_gamestream_host_legacy_launch_init(&launch);
    launch.logical_session_key = session.logicalSessionKey;
    launch.runtime_id = session.runtimeId;
    launch.launch_session_id = session.launchSessionId;
    launch.control_generation = session.controlGeneration;
    launch.client_key = session.client.clientKey;
    launch.device_key = session.client.deviceKey;
    launch.participant_key = session.client.participantKey;
    alk_session_copy_string(launch.session_id, sizeof(launch.session_id), session.sessionId.value);
    alk_session_copy_string(launch.client_display_name, sizeof(launch.client_display_name), session.client.displayName);
    alk_session_copy_string(launch.client_device_name, sizeof(launch.client_device_name), session.client.deviceName);
    alk_session_copy_string(launch.host_display_name, sizeof(launch.host_display_name), session.host.displayName);
    alk_session_copy_string(launch.host_device_name, sizeof(launch.host_device_name), session.host.deviceName);
    alk_session_copy_string(launch.app_id, sizeof(launch.app_id), session.appId);
    alk_session_copy_string(launch.app_name, sizeof(launch.app_name), session.appName);

    alk_gamestream_host_adapter_context_init(&context);
    if (!alk_gamestream_host_adapter_apply_launch(&context, &launch)) {
      return false;
    }

    context.snapshot.state = map_session_state(session.state);
    context.snapshot.telemetry.input_queue_depth = session.telemetry.inputQueueDepth;
    context.snapshot.telemetry.input_send_latency_us = session.telemetry.inputSendLatencyUs;
    context.snapshot.telemetry.input_ack_latency_us = session.telemetry.inputAckLatencyUs;
    context.snapshot.telemetry.video_decode_queue_depth = session.telemetry.videoDecodeQueueDepth;
    context.snapshot.telemetry.video_render_queue_depth = session.telemetry.videoRenderQueueDepth;
    context.snapshot.telemetry.audio_queue_depth_ms = session.telemetry.audioQueueDepthMs;
    context.snapshot.telemetry.rtt_us = session.telemetry.rttUs;
    context.snapshot.telemetry.jitter_us = session.telemetry.jitterUs;
    context.snapshot.telemetry.packet_loss_ppm = session.telemetry.packetLossPpm;
    context.snapshot.telemetry.current_framerate = session.telemetry.currentFramerate;
    context.snapshot.telemetry.last_updated_ms = session.telemetry.lastUpdatedMs;

    context.snapshot.cursor_plane.flags = session.cursorPlane.flags;
    context.snapshot.cursor_plane.epoch = session.cursorPlane.epoch;
    context.snapshot.cursor_plane.cursor_shape_id = session.cursorPlane.cursorShapeId;
    context.snapshot.cursor_plane.render_policy = session.cursorPlane.renderPolicy;
    context.snapshot.cursor_plane.size_source = session.cursorPlane.sizeSource;
    context.snapshot.cursor_plane.confidence_ppm = session.cursorPlane.confidencePpm;
    context.snapshot.cursor_plane.stream_width = session.cursorPlane.streamWidth;
    context.snapshot.cursor_plane.stream_height = session.cursorPlane.streamHeight;
    context.snapshot.cursor_plane.position_x = session.cursorPlane.positionX;
    context.snapshot.cursor_plane.position_y = session.cursorPlane.positionY;
    context.snapshot.cursor_plane.display_width = session.cursorPlane.displayWidth;
    context.snapshot.cursor_plane.display_height = session.cursorPlane.displayHeight;
    context.snapshot.cursor_plane.hotspot_x = session.cursorPlane.hotspotX;
    context.snapshot.cursor_plane.hotspot_y = session.cursorPlane.hotspotY;
    context.snapshot.cursor_plane.bitmap_width = session.cursorPlane.bitmapWidth;
    context.snapshot.cursor_plane.bitmap_height = session.cursorPlane.bitmapHeight;
    context.snapshot.cursor_plane.bitmap_stride = session.cursorPlane.bitmapStride;
    context.snapshot.cursor_plane.bitmap_format = session.cursorPlane.bitmapFormat;
    context.snapshot.cursor_plane.host_dpi_scale_ppm = session.cursorPlane.hostDpiScalePpm;
    context.snapshot.cursor_plane.user_scale_ppm = session.cursorPlane.userScalePpm;
    context.snapshot.cursor_plane.min_client_point_size = session.cursorPlane.minClientPointSize;
    context.snapshot.cursor_plane.max_client_point_size = session.cursorPlane.maxClientPointSize;
    context.snapshot.cursor_plane.asset_format = session.cursorPlane.assetFormat;
    context.snapshot.cursor_plane.asset_blend_mode = session.cursorPlane.assetBlendMode;
    context.snapshot.cursor_plane.asset_mask_semantics = session.cursorPlane.assetMaskSemantics;
    context.snapshot.cursor_plane.asset_fallback_policy = session.cursorPlane.assetFallbackPolicy;
    context.snapshot.cursor_plane.animation_epoch = session.cursorPlane.animationEpoch;
    context.snapshot.cursor_plane.frame_index = session.cursorPlane.frameIndex;
    context.snapshot.cursor_plane.frame_count = session.cursorPlane.frameCount;
    context.snapshot.cursor_plane.frame_duration_ms = session.cursorPlane.frameDurationMs;

    if (session.transportPath.pathId != 0) {
      AlkGameStreamHostTransportPathDetails details;
      alk_gamestream_host_transport_path_details_init(&details);
      details.path_id = session.transportPath.pathId;
      details.protocol_family = map_transport_protocol_family(session.transportPath.protocol);
      details.identity_kind = map_transport_identity(session.transportPath.pathIdentityKind);
      details.startup_class = map_startup_class(session.transportPath.startupClass);
      details.priority = session.transportPath.priority;
      details.flags = session.transportPath.flags;
      details.stack_flags = map_transport_stack_flags(session.transportPath.stackFlags);
      details.rtt_us = session.transportPath.rttUs;
      details.jitter_us = session.transportPath.jitterUs;
      details.packet_loss_ppm = session.transportPath.packetLossPpm;
      details.candidate_type = session.transportPath.candidateType;
      details.discovery_source = session.transportPath.discoverySource;
      details.address_family = session.transportPath.addressFamily;
      details.address_scope = session.transportPath.addressScope;
      details.egress_kind = session.transportPath.egressKind;
      details.underlay_kind = session.transportPath.underlayKind;
      details.encapsulation = session.transportPath.encapsulation;
      details.evidence_flags = session.transportPath.evidenceFlags;
      details.identity_confidence_ppm = session.transportPath.identityConfidencePpm;
      details.quality_confidence_ppm = session.transportPath.qualityConfidencePpm;
      details.relay_mode = (session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_RELAY) != 0 ?
                             ALK_TRANSPORT_RELAY_MODE_SELECTED :
                             ALK_TRANSPORT_RELAY_MODE_NONE;
      details.p2p_state =
        (session.transportPath.stackFlags & LI_SESSION_TRANSPORT_STACK_P2P) != 0 &&
        (session.transportPath.evidenceFlags & LI_SESSION_PATH_EVIDENCE_ICE_VALIDATED) != 0 ?
          ALK_TRANSPORT_P2P_PUNCH_SUCCEEDED :
          ALK_TRANSPORT_P2P_NONE;
      details.p2p_flags = details.p2p_state == ALK_TRANSPORT_P2P_PUNCH_SUCCEEDED ?
                            ALK_TRANSPORT_P2P_FLAG_FULL_PUNCH :
                            0u;
      details.reason_flags = session.transportPath.reasonFlags;
      details.risk_flags = session.transportPath.riskFlags;
      details.local_port = session.transportPath.localPort;
      details.remote_port = session.transportPath.remotePort;
      alk_session_copy_string(details.provider_id, sizeof(details.provider_id), session.transportPath.providerId);
      alk_session_copy_string(details.route_id, sizeof(details.route_id), session.transportPath.routeId);
      alk_session_copy_string(details.local_endpoint, sizeof(details.local_endpoint), session.transportPath.localEndpoint);
      alk_session_copy_string(details.remote_endpoint, sizeof(details.remote_endpoint), session.transportPath.remoteEndpoint);
      alk_session_copy_string(details.observed_endpoint, sizeof(details.observed_endpoint), session.transportPath.observedEndpoint);
      alk_session_copy_string(details.host_local_endpoint, sizeof(details.host_local_endpoint), session.transportPath.hostLocalEndpoint);
      alk_session_copy_string(details.explanation_code, sizeof(details.explanation_code), session.transportPath.explanationCode);
      alk_session_copy_string(details.relay_name, sizeof(details.relay_name), session.transportPath.relayName);
      alk_gamestream_host_adapter_set_active_path_details(&context, &details);
    }

    if (session.lease.valid) {
      const auto resource_kind = map_resource_kind(session.lease.feature);
      if (resource_kind != ALK_RESOURCE_UNKNOWN) {
        alk_gamestream_host_adapter_grant_resource(&context,
                                            resource_kind,
                                            session.lease.ownerParticipantKey);
      }
    }

    return true;
  }

  bool
  project_to_li_session(const AlkSessionAdapterContext &context, LI_SESSION &session) {
    const auto &snapshot = context.snapshot;
    if (snapshot.version == 0) {
      return false;
    }

    if (session.version != LI_SESSION_VERSION) {
      LiInitializeSession(&session);
    }

    session.state = map_session_state_to_li(snapshot.state);
    session.logicalSessionKey = snapshot.logical_session_key;
    session.runtimeId = snapshot.runtime_id;
    session.launchSessionId = snapshot.launch_session_id;
    session.controlGeneration = snapshot.control_generation;
    if (snapshot.session_id.value[0] != '\0') {
      alk_session_copy_string(session.sessionId.value,
                              sizeof(session.sessionId.value),
                              snapshot.session_id.value);
    }
    if (snapshot.app_id[0] != '\0') {
      alk_session_copy_string(session.appId, sizeof(session.appId), snapshot.app_id);
    }
    if (snapshot.app_name[0] != '\0') {
      alk_session_copy_string(session.appName, sizeof(session.appName), snapshot.app_name);
    }

    for (uint32_t i = 0; i < snapshot.participant_count; ++i) {
      const auto &participant = snapshot.participants[i];
      if (participant.role == ALK_SESSION_ROLE_CLIENT) {
        project_participant_to_li(participant, session.client);
      }
      else if (participant.role == ALK_SESSION_ROLE_HOST) {
        project_participant_to_li(participant, session.host);
      }
    }

    session.telemetry.inputQueueDepth = snapshot.telemetry.input_queue_depth;
    session.telemetry.inputSendLatencyUs = snapshot.telemetry.input_send_latency_us;
    session.telemetry.inputAckLatencyUs = snapshot.telemetry.input_ack_latency_us;
    session.telemetry.videoDecodeQueueDepth = snapshot.telemetry.video_decode_queue_depth;
    session.telemetry.videoRenderQueueDepth = snapshot.telemetry.video_render_queue_depth;
    session.telemetry.audioQueueDepthMs = snapshot.telemetry.audio_queue_depth_ms;
    session.telemetry.rttUs = snapshot.telemetry.rtt_us;
    session.telemetry.jitterUs = snapshot.telemetry.jitter_us;
    session.telemetry.packetLossPpm = snapshot.telemetry.packet_loss_ppm;
    session.telemetry.currentFramerate = snapshot.telemetry.current_framerate;
    session.telemetry.lastUpdatedMs = snapshot.telemetry.last_updated_ms;

    if (snapshot.cursor_plane.epoch != 0) {
      session.cursorPlane.flags = snapshot.cursor_plane.flags;
      session.cursorPlane.epoch = snapshot.cursor_plane.epoch;
      session.cursorPlane.cursorShapeId = snapshot.cursor_plane.cursor_shape_id;
      session.cursorPlane.renderPolicy = snapshot.cursor_plane.render_policy;
      session.cursorPlane.sizeSource = snapshot.cursor_plane.size_source;
      session.cursorPlane.confidencePpm = snapshot.cursor_plane.confidence_ppm;
      session.cursorPlane.streamWidth = snapshot.cursor_plane.stream_width;
      session.cursorPlane.streamHeight = snapshot.cursor_plane.stream_height;
      session.cursorPlane.positionX = snapshot.cursor_plane.position_x;
      session.cursorPlane.positionY = snapshot.cursor_plane.position_y;
      session.cursorPlane.displayWidth = snapshot.cursor_plane.display_width;
      session.cursorPlane.displayHeight = snapshot.cursor_plane.display_height;
      session.cursorPlane.hotspotX = snapshot.cursor_plane.hotspot_x;
      session.cursorPlane.hotspotY = snapshot.cursor_plane.hotspot_y;
      session.cursorPlane.bitmapWidth = snapshot.cursor_plane.bitmap_width;
      session.cursorPlane.bitmapHeight = snapshot.cursor_plane.bitmap_height;
      session.cursorPlane.bitmapStride = snapshot.cursor_plane.bitmap_stride;
      session.cursorPlane.bitmapFormat = snapshot.cursor_plane.bitmap_format;
      session.cursorPlane.hostDpiScalePpm = snapshot.cursor_plane.host_dpi_scale_ppm;
      session.cursorPlane.userScalePpm = snapshot.cursor_plane.user_scale_ppm;
      session.cursorPlane.minClientPointSize = snapshot.cursor_plane.min_client_point_size;
      session.cursorPlane.maxClientPointSize = snapshot.cursor_plane.max_client_point_size;
      session.cursorPlane.assetFormat = snapshot.cursor_plane.asset_format;
      session.cursorPlane.assetBlendMode = snapshot.cursor_plane.asset_blend_mode;
      session.cursorPlane.assetMaskSemantics = snapshot.cursor_plane.asset_mask_semantics;
      session.cursorPlane.assetFallbackPolicy = snapshot.cursor_plane.asset_fallback_policy;
      session.cursorPlane.animationEpoch = snapshot.cursor_plane.animation_epoch;
      session.cursorPlane.frameIndex = snapshot.cursor_plane.frame_index;
      session.cursorPlane.frameCount = snapshot.cursor_plane.frame_count;
      session.cursorPlane.frameDurationMs = snapshot.cursor_plane.frame_duration_ms;
    }

    if (snapshot.transport_path_count > 0) {
      const auto &path = snapshot.transport_paths[0];
      session.transportPath.pathId = path.path_id;
      session.transportPath.protocol =
        map_transport_protocol_family_to_li(path.protocol_family, path.provider_id);
      session.transportPath.pathIdentityKind = map_transport_identity_to_li(path.identity_kind);
      session.transportPath.startupClass = map_startup_class_to_li(path.startup_class);
      session.transportPath.priority = path.priority;
      session.transportPath.flags = path.flags;
      session.transportPath.stackFlags = map_transport_stack_flags_to_li(path.stack_flags, path.provider_id);
      session.transportPath.rttUs = path.rtt_us;
      session.transportPath.jitterUs = path.jitter_us;
      session.transportPath.packetLossPpm = path.packet_loss_ppm;
      session.transportPath.candidateType = path.candidate_type;
      session.transportPath.discoverySource = path.discovery_source;
      session.transportPath.addressFamily = path.address_family;
      session.transportPath.addressScope = path.address_scope;
      session.transportPath.egressKind = path.egress_kind;
      session.transportPath.underlayKind = path.underlay_kind;
      session.transportPath.encapsulation = path.encapsulation;
      session.transportPath.evidenceFlags = path.evidence_flags;
      session.transportPath.identityConfidencePpm = path.identity_confidence_ppm;
      session.transportPath.qualityConfidencePpm = path.quality_confidence_ppm;
      session.transportPath.reasonFlags = path.reason_flags;
      session.transportPath.riskFlags = path.risk_flags;
      session.transportPath.localPort = path.local_port;
      session.transportPath.remotePort = path.remote_port;
      alk_session_copy_string(session.transportPath.providerId,
                              sizeof(session.transportPath.providerId),
                              path.provider_id);
      alk_session_copy_string(session.transportPath.routeId,
                              sizeof(session.transportPath.routeId),
                              path.route_id);
      alk_session_copy_string(session.transportPath.localEndpoint,
                              sizeof(session.transportPath.localEndpoint),
                              path.local_endpoint);
      alk_session_copy_string(session.transportPath.remoteEndpoint,
                              sizeof(session.transportPath.remoteEndpoint),
                              path.remote_endpoint);
      alk_session_copy_string(session.transportPath.observedEndpoint,
                              sizeof(session.transportPath.observedEndpoint),
                              path.observed_endpoint);
      alk_session_copy_string(session.transportPath.hostLocalEndpoint,
                              sizeof(session.transportPath.hostLocalEndpoint),
                              path.host_local_endpoint);
      alk_session_copy_string(session.transportPath.explanationCode,
                              sizeof(session.transportPath.explanationCode),
                              path.explanation_code);
      alk_session_copy_string(session.transportPath.relayName,
                              sizeof(session.transportPath.relayName),
                              path.relay_name);
    }

    if (snapshot.lease_count > 0) {
      const auto &lease = snapshot.leases[0];
      session.lease.feature = map_resource_kind_to_li(lease.resource_kind);
      session.lease.mode = map_lease_mode_to_li(lease.mode);
      session.lease.ownerRuntimeId = lease.owner_runtime_id;
      session.lease.ownerParticipantKey = lease.owner_participant_key;
      session.lease.grantedAtMs = lease.granted_at_ms;
      session.lease.expiresAtMs = lease.expires_at_ms;
      session.lease.renewAfterMs = lease.renew_after_ms;
      session.lease.ttlMs = lease.ttl_ms;
      session.lease.valid = lease.status == ALK_LEASE_STATUS_GRANTED ||
                            lease.status == ALK_LEASE_STATUS_PENDING ||
                            lease.status == ALK_LEASE_STATUS_CONFLICT;
      session.lease.renewable = true;
    }

    return true;
  }

  bool
  update_cursor_plane(AlkSessionAdapterContext &context, const LI_SESSION_CURSOR_PLANE &cursor_plane) {
    if (context.snapshot.version == 0) {
      return false;
    }

    context.snapshot.cursor_plane.flags = cursor_plane.flags;
    context.snapshot.cursor_plane.epoch = cursor_plane.epoch;
    context.snapshot.cursor_plane.cursor_shape_id = cursor_plane.cursorShapeId;
    context.snapshot.cursor_plane.render_policy = cursor_plane.renderPolicy;
    context.snapshot.cursor_plane.size_source = cursor_plane.sizeSource;
    context.snapshot.cursor_plane.confidence_ppm = cursor_plane.confidencePpm;
    context.snapshot.cursor_plane.stream_width = cursor_plane.streamWidth;
    context.snapshot.cursor_plane.stream_height = cursor_plane.streamHeight;
    context.snapshot.cursor_plane.position_x = cursor_plane.positionX;
    context.snapshot.cursor_plane.position_y = cursor_plane.positionY;
    context.snapshot.cursor_plane.display_width = cursor_plane.displayWidth;
    context.snapshot.cursor_plane.display_height = cursor_plane.displayHeight;
    context.snapshot.cursor_plane.hotspot_x = cursor_plane.hotspotX;
    context.snapshot.cursor_plane.hotspot_y = cursor_plane.hotspotY;
    context.snapshot.cursor_plane.bitmap_width = cursor_plane.bitmapWidth;
    context.snapshot.cursor_plane.bitmap_height = cursor_plane.bitmapHeight;
    context.snapshot.cursor_plane.bitmap_stride = cursor_plane.bitmapStride;
    context.snapshot.cursor_plane.bitmap_format = cursor_plane.bitmapFormat;
    context.snapshot.cursor_plane.host_dpi_scale_ppm = cursor_plane.hostDpiScalePpm;
    context.snapshot.cursor_plane.user_scale_ppm = cursor_plane.userScalePpm;
    context.snapshot.cursor_plane.min_client_point_size = cursor_plane.minClientPointSize;
    context.snapshot.cursor_plane.max_client_point_size = cursor_plane.maxClientPointSize;
    context.snapshot.cursor_plane.asset_format = cursor_plane.assetFormat;
    context.snapshot.cursor_plane.asset_blend_mode = cursor_plane.assetBlendMode;
    context.snapshot.cursor_plane.asset_mask_semantics = cursor_plane.assetMaskSemantics;
    context.snapshot.cursor_plane.asset_fallback_policy = cursor_plane.assetFallbackPolicy;
    context.snapshot.cursor_plane.animation_epoch = cursor_plane.animationEpoch;
    context.snapshot.cursor_plane.frame_index = cursor_plane.frameIndex;
    context.snapshot.cursor_plane.frame_count = cursor_plane.frameCount;
    context.snapshot.cursor_plane.frame_duration_ms = cursor_plane.frameDurationMs;
    return true;
  }

  bool
  update_lease(AlkSessionAdapterContext &context,
               const LI_SESSION_LEASE &lease,
               std::uint32_t status) {
    const auto resource_kind = map_resource_kind(lease.feature);
    if (context.snapshot.version == 0 || resource_kind == ALK_RESOURCE_UNKNOWN) {
      return false;
    }

    auto *target = find_or_append_lease(context.snapshot, resource_kind);
    if (!target) {
      return false;
    }

    target->resource_kind = resource_kind;
    target->mode = lease.mode == LI_SESSION_LEASE_MODE_SHARED ? ALK_LEASE_MODE_SHARED :
                     lease.mode == LI_SESSION_LEASE_MODE_OBSERVER ? ALK_LEASE_MODE_OBSERVER :
                     ALK_LEASE_MODE_EXCLUSIVE_OWNER;
    target->status = status == LI_SESSION_CONTROL_LEASE_STATUS_RELEASED ? ALK_LEASE_STATUS_RELEASED :
                       status == LI_SESSION_CONTROL_LEASE_STATUS_CONFLICT ? ALK_LEASE_STATUS_CONFLICT :
                       status == LI_SESSION_CONTROL_LEASE_STATUS_DENIED ? ALK_LEASE_STATUS_DENIED :
                       ALK_LEASE_STATUS_GRANTED;
    target->generation = context.snapshot.epoch + 1;
    target->ttl_ms = lease.ttlMs;
    target->owner_runtime_id = lease.ownerRuntimeId;
    target->owner_participant_key = lease.ownerParticipantKey;
    target->granted_at_ms = lease.grantedAtMs;
    target->expires_at_ms = lease.expiresAtMs;
    target->renew_after_ms = lease.renewAfterMs;
    return true;
  }

}  // namespace stream::alkaidlab_session_bridge
