#include "src/session_runtime.h"

extern "C" {
#include "third-party/moonlight-common-c/src/Session.h"
}

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <vector>

TEST(SessionRuntimeTests, SessionIdKeepsLogicalSessionStableAcrossControlGenerations) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 42;
  identity.launch_session_id = 7;
  identity.control_generation = 1;
  identity.set_client_cert_uuid("client-cert-a");
  identity.client_unique_id = "macbook-pro";

  const auto first = session_runtime::make_session_id(identity);

  identity.control_generation = 2;
  const auto second = session_runtime::make_session_id(identity);

  EXPECT_EQ(first.logical_key, second.logical_key);
  EXPECT_NE(first.control_generation, second.control_generation);
  EXPECT_TRUE(first.trusted_client);
}

TEST(SessionRuntimeTests, ParticipantDistinguishesMultipleDevicesForSameAccount) {
  session_runtime::identity_t mac {};
  mac.runtime_id = 11;
  mac.set_client_cert_uuid("same-user-cert");
  mac.client_unique_id = "macbook";
  mac.client_name = "Sky Mac";

  session_runtime::identity_t ipad = mac;
  ipad.runtime_id = 12;
  ipad.client_unique_id = "ipad";
  ipad.client_name = "Sky iPad";

  const auto mac_participant = session_runtime::make_participant(mac);
  const auto ipad_participant = session_runtime::make_participant(ipad);

  EXPECT_EQ(mac_participant.client_cert_key, ipad_participant.client_cert_key);
  EXPECT_NE(mac_participant.device_key, ipad_participant.device_key);
  EXPECT_NE(mac_participant.participant_key, ipad_participant.participant_key);
  EXPECT_EQ(mac_participant.display_name, "Sky Mac");
  EXPECT_EQ(ipad_participant.device_id, "ipad");
}

TEST(SessionRuntimeTests, ParticipantPreservesMoonlightAdvertisedIdentityKeys) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 77;
  identity.logical_session_id = "ml-session-2026";
  identity.logical_session_key = 0x1010;
  identity.participant_id = "ml-participant-mac";
  identity.participant_key = 0x2020;
  identity.client_key = 0x3030;
  identity.device_key = 0x4040;
  identity.client_unique_id = "MacBookPro18,3";
  identity.client_name = "Sky Mac";

  const auto participant = session_runtime::make_participant(identity);
  const auto path = session_runtime::make_enet_direct_transport_path();
  const session_runtime::session_telemetry_report_t report {
    .participant = participant.id,
    .path_id = path.path_id,
  };

  const auto session = session_runtime::make_li_session(identity,
                                                        path,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        report,
                                                        "desktop",
                                                        "Desktop");

  EXPECT_EQ(participant.participant_key, 0x2020U);
  EXPECT_EQ(participant.client_cert_key, 0x3030U);
  EXPECT_EQ(participant.device_key, 0x4040U);
  EXPECT_EQ(participant.id.participant_key, 0x2020U);
  EXPECT_EQ(participant.id.device_key, 0x4040U);
  EXPECT_EQ(session.logicalSessionKey, 0x1010U);
  EXPECT_STREQ(session.sessionId.value, "ml-session-2026");
  EXPECT_STREQ(session.client.participantId.value, "ml-participant-mac");
  EXPECT_EQ(session.client.participantKey, 0x2020U);
  EXPECT_EQ(session.client.clientKey, 0x3030U);
  EXPECT_EQ(session.client.deviceKey, 0x4040U);
  EXPECT_EQ(session.client.index, 0U);
  EXPECT_EQ(session.host.index, 1U);
  EXPECT_NE(session.host.participantKey, 0U);
  EXPECT_NE(session.host.clientKey, 0U);
  EXPECT_NE(session.host.deviceKey, 0U);
}

TEST(SessionRuntimeTests, FeatureCapsNegotiateRtcAndRendererCapabilities) {
  session_runtime::feature_caps_t client_caps {};
  client_caps.enable(session_runtime::capability_e::native_renderer_metrics);
  client_caps.enable(session_runtime::capability_e::frame_reuse_feedback);
  client_caps.enable(session_runtime::capability_e::transport_cc_lite);
  client_caps.enable(session_runtime::capability_e::nack_rtx);
  client_caps.enable(session_runtime::capability_e::ice_stun);

  session_runtime::feature_caps_t server_caps {};
  server_caps.enable(session_runtime::capability_e::transport_cc_lite);
  server_caps.enable(session_runtime::capability_e::packet_pacer_probe);
  server_caps.enable(session_runtime::capability_e::nack_rtx);
  server_caps.enable(session_runtime::capability_e::flexfec);

  const auto negotiated = client_caps.intersection(server_caps);

  EXPECT_TRUE(negotiated.has(session_runtime::capability_e::transport_cc_lite));
  EXPECT_TRUE(negotiated.has(session_runtime::capability_e::nack_rtx));
  EXPECT_FALSE(negotiated.has(session_runtime::capability_e::ice_stun));
  EXPECT_FALSE(negotiated.has(session_runtime::capability_e::flexfec));
}

TEST(SessionRuntimeTests, RtspCapabilityNamesParseCanonicalClientCapabilities) {
  const auto caps = session_runtime::parse_capability_names(
    "native-renderer-metrics,transport-cc-lite,nack-rtx,relay-quic,relay-tcp-tls");

  EXPECT_TRUE(caps.has(session_runtime::capability_e::native_renderer_metrics));
  EXPECT_TRUE(caps.has(session_runtime::capability_e::transport_cc_lite));
  EXPECT_TRUE(caps.has(session_runtime::capability_e::nack_rtx));
  EXPECT_TRUE(caps.has(session_runtime::capability_e::relay_quic));
  EXPECT_TRUE(caps.has(session_runtime::capability_e::relay_tcp_tls));
  EXPECT_FALSE(caps.has(session_runtime::capability_e::flexfec));

  const auto li_features = session_runtime::li_features_for_caps(caps);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_VIDEO_TELEMETRY, 0ULL);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_TRANSPORT_CC, 0ULL);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_PATH_PROBE, 0ULL);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_NACK_RTX, 0ULL);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_RELAY, 0ULL);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_QUIC, 0ULL);
  EXPECT_NE(li_features & LI_SESSION_FEATURE_TCP, 0ULL);
}

TEST(SessionRuntimeTests, LeaseModelsOwnerObserverAndSharedRoles) {
  const session_runtime::participant_id_t owner {
    .participant_key = 100,
    .runtime_id = 11,
    .device_key = 201,
  };
  const session_runtime::participant_id_t observer {
    .participant_key = 101,
    .runtime_id = 12,
    .device_key = 202,
  };

  session_runtime::lease_t display_lease {
    .feature = session_runtime::feature_e::display,
    .mode = session_runtime::lease_mode_e::exclusive_owner,
    .owner = owner,
  };

  EXPECT_TRUE(display_lease.owned_by(owner));
  EXPECT_FALSE(display_lease.owned_by(observer));
  EXPECT_TRUE(display_lease.can_observe(observer));

  display_lease.mode = session_runtime::lease_mode_e::shared;
  EXPECT_TRUE(display_lease.can_control(observer));
}

TEST(SessionRuntimeTests, TransportPathCanRepresentCompleteFirstStageCandidateSet) {
  const std::vector<session_runtime::transport_route_e> routes {
    session_runtime::transport_route_e::lan_direct,
    session_runtime::transport_route_e::manual_public_port_forward,
    session_runtime::transport_route_e::upnp_public_mapping,
    session_runtime::transport_route_e::ice_stun_p2p,
    session_runtime::transport_route_e::relay_quic,
    session_runtime::transport_route_e::relay_tcp_tls,
  };

  std::vector<session_runtime::transport_path_t> paths;
  for (auto route : routes) {
    paths.push_back(session_runtime::make_transport_path(route));
  }

  EXPECT_EQ(paths.size(), 6U);
  EXPECT_TRUE(std::ranges::all_of(paths, [](const auto &path) {
    return path.path_id != 0 &&
           path.state == session_runtime::transport_path_state_e::candidate;
  }));

  auto relay_tcp = std::ranges::find_if(paths, [](const auto &path) {
    return path.route == session_runtime::transport_route_e::relay_tcp_tls;
  });
  ASSERT_NE(relay_tcp, paths.end());
  EXPECT_EQ(relay_tcp->protocol, session_runtime::transport_protocol_e::tcp_tls);
}

TEST(SessionRuntimeTests, StartupPathRemoteHintOverridesPrivateObservedPeer) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_NE(decision.evidence_flags & LI_SESSION_PATH_EVIDENCE_CLIENT_CONFIGURED, 0U);
  EXPECT_NE(decision.evidence_flags & LI_SESSION_PATH_EVIDENCE_CLIENT_ROUTE_OBSERVED, 0U);
  EXPECT_STREQ(decision.reason, "client-remote-hint");
}

TEST(SessionRuntimeTests, StartupPathRtspPeerRouteHintDoesNotDefeatPhysicalPrivatePeer) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .rtsp_route_remote_hint = true,
    .client_egress_kind = "physical",
    .client_route_host = "192.168.100.133:57989",
    .rtsp_route_host = "192.168.100.133",
    .client_source_endpoint = "192.168.100.182",
    .host_observed_peer_endpoint = "192.168.100.182",
    .host_observed_local_endpoint = "192.168.100.133",
    .client_target_address_candidates = { "192.168.100.133" },
    .host_public_candidates = { "180.173.123.199" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_TRUE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::lan_direct);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_TRUE_LAN);
  EXPECT_EQ(decision.startup_class, LI_SESSION_STARTUP_CLASS_LAN_FAST);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "peer-lan-confirmed");
}

TEST(SessionRuntimeTests, RtspPrivatePeerChangeDoesNotRequireRemoteHint) {
  EXPECT_FALSE(session_runtime::rtsp_peer_route_change_requires_remote_hint("192.168.100.158",
                                                                            "192.168.100.182"));
  EXPECT_FALSE(session_runtime::rtsp_peer_route_change_requires_remote_hint("10.10.0.2:5000",
                                                                            "172.16.0.9:6000"));
  EXPECT_TRUE(session_runtime::rtsp_peer_route_change_requires_remote_hint("192.168.100.158",
                                                                           "93.184.216.34"));
  EXPECT_TRUE(session_runtime::rtsp_peer_route_change_requires_remote_hint("93.184.216.34",
                                                                           "192.168.100.182"));
}

TEST(SessionRuntimeTests, FreshEndpointPreflightConfirmsLanWithoutPublicIdentity) {
  session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = false,
    .rtsp_route_remote_hint = true,
    .client_egress_kind = "physical",
    .client_route_host = "192.168.100.133",
    .rtsp_route_host = "192.168.100.133",
    .preflight_available = true,
    .preflight_observed_peer_endpoint = "192.168.100.182:53000",
    .preflight_host_local_endpoint = "192.168.100.133:47989",
    .preflight_rtt_ms = 3,
    .preflight_age_ms = 80,
  };

  session_runtime::apply_startup_path_preflight_snapshot(evidence);
  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_TRUE(evidence.peer_is_lan_or_pc);
  EXPECT_EQ(evidence.host_observed_peer_endpoint, "192.168.100.182:53000");
  EXPECT_EQ(evidence.host_observed_local_endpoint, "192.168.100.133:47989");
  EXPECT_TRUE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::lan_direct);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_TRUE_LAN);
  EXPECT_EQ(decision.startup_class, LI_SESSION_STARTUP_CLASS_LAN_FAST);
}


TEST(SessionRuntimeTests, StartupPathTunnelEvidenceDisablesLanFastStart) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_route_remote_hint = true,
    .client_route_tunnel = true,
    .client_vpn_active = true,
    .client_egress_kind = "tunnel",
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_TUNNEL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_VPN_TUNNEL);
  EXPECT_NE(decision.evidence_flags & LI_SESSION_PATH_EVIDENCE_CLIENT_ROUTE_OBSERVED, 0U);
  EXPECT_STREQ(decision.reason, "client-tunnel-route");
}

TEST(SessionRuntimeTests, StartupPathPublicIdentityMatchClassifiesHomePortForward) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
    .client_route_host = "home.example.net:57989",
    .client_target_address_candidates = { "93.184.216.34:57998" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "host-public-port-forward");
  EXPECT_GE(decision.identity_confidence_ppm, 850000U);
}

TEST(SessionRuntimeTests, StartupPathLanHairpinPublicIdentityUsesHairpinFastRemoteSafeStartup) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
    .client_route_host = "home.example.net:57989",
    .host_observed_peer_endpoint = "192.168.100.1:53000",
    .host_observed_local_endpoint = "192.168.100.133:57989",
    .client_target_address_candidates = { "93.184.216.34:57998" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD);
  EXPECT_EQ(decision.startup_class, LI_SESSION_STARTUP_CLASS_REMOTE_SAFE);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "router-snat-port-forward");
  EXPECT_GE(decision.identity_confidence_ppm, 850000U);

  const auto policy = session_runtime::startup_ceiling_policy_for_path(decision, 150);
  EXPECT_EQ(policy.bitrate_seed_kbps, 12000);
  EXPECT_EQ(policy.fps_cap, 60);
  EXPECT_STREQ(policy.reason, "router-port-forward-safe");
}

TEST(SessionRuntimeTests, StartupPathPublicIdentityOnHostPublicInterfaceClassifiesHostDirect) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = false,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
    .client_route_host = "public-host.example:57989",
    .host_observed_local_endpoint = "93.184.216.34:47989",
    .client_target_address_candidates = { "93.184.216.34:57989" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_HOST_DIRECT_PUBLIC);
  EXPECT_EQ(decision.startup_class, LI_SESSION_STARTUP_CLASS_REMOTE_SAFE);
  EXPECT_STREQ(decision.reason, "host-direct-public");
  EXPECT_GE(decision.identity_confidence_ppm, 850000U);
}

TEST(SessionRuntimeTests, StartupPathPublicIdentityMismatchClassifiesExternalForwarder) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
    .client_route_host = "edge.example.net:57989",
    .client_target_address_candidates = { "34.117.59.81:57998" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "external-forwarder");
  EXPECT_LE(decision.identity_confidence_ppm, 700000U);
}

TEST(SessionRuntimeTests, StartupPathPublicIdentityOverridesPrivatePeerWithoutClientHint) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_egress_kind = "physical",
    .client_route_host = "home.example.net:57989",
    .client_target_address_candidates = { "93.184.216.34:57998" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "host-public-port-forward");
}

TEST(SessionRuntimeTests, StartupPathExternalForwarderOverridesPrivatePeerWithoutClientHint) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_egress_kind = "physical",
    .client_route_host = "edge.example.net:57989",
    .client_target_address_candidates = { "34.117.59.81:57998" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::manual_public_port_forward);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "external-forwarder");
}

TEST(SessionRuntimeTests, StartupPathPrivateTargetWithoutRemoteHintKeepsLanFastStart) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_egress_kind = "physical",
    .client_route_host = "192.168.100.133:57989",
    .client_target_address_candidates = { "192.168.100.133:57998" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_TRUE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::lan_direct);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "peer-lan-confirmed");
}

TEST(SessionRuntimeTests, StartupPathPrivatePeerWithoutRemoteEvidenceKeepsLanFastStart) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_egress_kind = "physical",
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_TRUE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::lan_direct);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_NE(decision.evidence_flags & LI_SESSION_PATH_EVIDENCE_HOST_PEER_OBSERVED, 0U);
  EXPECT_STREQ(decision.reason, "peer-lan-confirmed");
}

TEST(SessionRuntimeTests, StartupPathActiveVpnWithoutTunnelRouteDoesNotDefeatLan) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_vpn_active = true,
    .client_egress_kind = "physical",
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_TRUE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::lan_direct);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_STREQ(decision.reason, "peer-lan-confirmed");
}

TEST(SessionRuntimeTests, StartupPathVirtualProxyEgressDoesNotBecomeTrueLan) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .client_egress_kind = "virtual",
    .client_route_host = "192.168.100.133:57989",
    .client_source_endpoint = "198.18.0.1",
    .host_observed_peer_endpoint = "192.168.150.3:56496",
    .host_observed_local_endpoint = "192.168.100.133:57989",
    .client_target_address_candidates = { "192.168.100.133" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::overlay_route);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_VIRTUAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_UDP_TUNNEL);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_VPN_OVERLAY);
  EXPECT_EQ(decision.startup_class, LI_SESSION_STARTUP_CLASS_REMOTE_SAFE);
  EXPECT_NE(decision.evidence_flags & LI_SESSION_PATH_EVIDENCE_CLIENT_ROUTE_OBSERVED, 0U);
  EXPECT_NE(decision.risk_flags & LI_SESSION_PATH_RISK_TUNNEL, 0U);
  EXPECT_STREQ(decision.reason, "client-virtual-overlay");
}

TEST(SessionRuntimeTests, StartupPathTunnelOverlayUsesOverlayRouteIdentity) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .client_route_tunnel = true,
    .startup_profile = "tunnel",
    .client_egress_kind = "virtual",
    .client_route_host = "192.168.100.133:57989",
    .client_route_path_kind = "tunnel-overlay",
    .rtsp_route_host = "192.168.100.133:58010",
    .client_source_endpoint = "198.18.0.1",
    .host_observed_peer_endpoint = "192.168.100.220:55137",
    .host_observed_local_endpoint = "192.168.100.133:57984",
    .client_target_address_candidates = { "192.168.100.133" },
    .host_public_candidates = { "180.173.123.199" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);
  const auto path = session_runtime::make_transport_path(decision, evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::overlay_route);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_VIRTUAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_UDP_TUNNEL);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_VPN_OVERLAY);
  EXPECT_STREQ(decision.reason, "client-virtual-overlay");
  EXPECT_EQ(session_runtime::transport_route_name(path.route), "overlay-route");
  EXPECT_EQ(path.observed_egress_kind, LI_SESSION_PATH_EGRESS_VIRTUAL);
}

TEST(SessionRuntimeTests, HostPublicIdentityCandidatesAcceptOnlyUniquePublicIpv4) {
  std::vector<std::string> candidates { "93.184.216.34" };

  EXPECT_FALSE(session_runtime::append_public_ipv4_identity_candidate(candidates, "192.168.100.1"));
  EXPECT_FALSE(session_runtime::append_public_ipv4_identity_candidate(candidates, "198.18.0.1"));
  EXPECT_FALSE(session_runtime::append_public_ipv4_identity_candidate(candidates, "93.184.216.34:57989"));
  EXPECT_TRUE(session_runtime::append_public_ipv4_identity_candidate(candidates, "https://203.0.114.20:47989/path"));

  ASSERT_EQ(candidates.size(), 2U);
  EXPECT_EQ(candidates[0], "93.184.216.34");
  EXPECT_EQ(candidates[1], "203.0.114.20");
}

TEST(SessionRuntimeTests, PathIdentityAndObservedEndpointsReachLiSession) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 101;
  identity.launch_session_id = 55;
  identity.control_generation = 7;
  identity.client_unique_id = "macbook-pro";
  identity.client_name = "Sky MacBook";

  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
    .client_route_host = "home.example.net:57989",
    .rtsp_route_host = "home.example.net",
    .client_source_endpoint = "192.168.100.20:53123",
    .host_observed_peer_endpoint = "192.168.100.1:53123",
    .host_observed_local_endpoint = "192.168.100.133:57989",
    .client_target_address_candidates = { "93.184.216.34:57989" },
    .host_public_candidates = { "93.184.216.34" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);
  auto path = session_runtime::make_transport_path(decision, evidence);
  path.state = session_runtime::transport_path_state_e::active;

  const session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = path.path_id,
    .displayed_fps = 60,
  };

  const auto session = session_runtime::make_li_session(identity,
                                                        path,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        report,
                                                        "desktop",
                                                        "Desktop");

  EXPECT_EQ(session.transportPath.pathIdentityKind, LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD);
  EXPECT_EQ(session.transportPath.startupClass, LI_SESSION_STARTUP_CLASS_REMOTE_SAFE);
  EXPECT_NE(session.transportPath.reasonFlags & LI_SESSION_PATH_REASON_HOST_PUBLIC_MATCH, 0U);
  EXPECT_STREQ(session.transportPath.explanationCode, "router-snat-port-forward");
  EXPECT_STREQ(session.transportPath.localEndpoint, "192.168.100.20:53123");
  EXPECT_STREQ(session.transportPath.remoteEndpoint, "home.example.net:57989");
  EXPECT_STREQ(session.transportPath.observedEndpoint, "192.168.100.1:53123");
  EXPECT_STREQ(session.transportPath.hostLocalEndpoint, "192.168.100.133:57989");

  const auto attributes = session_runtime::session_snapshot_attributes(session);
  const auto joined = std::accumulate(attributes.begin(), attributes.end(), std::string {}, [](std::string acc, const std::string &line) {
    acc += line;
    acc += '\n';
    return acc;
  });
  EXPECT_NE(joined.find("x-ss-core.transportPath.pathIdentityKind:router-port-forward"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.startupClass:remote-safe"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.explanationCode:router-snat-port-forward"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.hostLocalEndpoint:192.168.100.133:57989"), std::string::npos);
}

TEST(SessionRuntimeTests, TelemetryKeepsReportsForMultipleParticipantsAndPaths) {
  session_runtime::session_telemetry_t telemetry;

  session_runtime::participant_id_t mac {
    .participant_key = 100,
    .runtime_id = 11,
    .device_key = 201,
  };
  session_runtime::participant_id_t ipad {
    .participant_key = 101,
    .runtime_id = 12,
    .device_key = 202,
  };

  auto lan = session_runtime::make_transport_path(session_runtime::transport_route_e::lan_direct);
  auto relay = session_runtime::make_transport_path(session_runtime::transport_route_e::relay_quic);

  telemetry.submit({
    .participant = mac,
    .path_id = lan.path_id,
    .displayed_fps = 150,
    .rtt_ms = 1,
    .loss_ppm = 0,
    .renderer_backpressure = false,
  });
  telemetry.submit({
    .participant = ipad,
    .path_id = relay.path_id,
    .displayed_fps = 60,
    .rtt_ms = 38,
    .loss_ppm = 12000,
    .renderer_backpressure = true,
  });

  const auto snapshot = telemetry.snapshot();

  ASSERT_EQ(snapshot.reports.size(), 2U);
  EXPECT_EQ(snapshot.report_for(mac)->displayed_fps, 150);
  EXPECT_EQ(snapshot.report_for(ipad)->path_id, relay.path_id);
  EXPECT_EQ(snapshot.active_participant_count(), 2U);
  EXPECT_TRUE(snapshot.has_renderer_backpressure());
}

TEST(SessionRuntimeTests, RtspCapabilityManifestAdvertisesSupportedAndPlannedCoreFeatures) {
  const auto manifest = session_runtime::default_rtsp_capability_manifest();
  const auto attributes = session_runtime::rtsp_capability_attributes(manifest);

  const auto joined = std::accumulate(attributes.begin(), attributes.end(), std::string {}, [](std::string acc, const std::string &line) {
    acc += line;
    acc += '\n';
    return acc;
  });

  EXPECT_NE(joined.find("x-ss-core.supportedCaps:"), std::string::npos);
  EXPECT_NE(joined.find("transport-cc-lite"), std::string::npos);
  EXPECT_NE(joined.find("pacer-probe"), std::string::npos);
  EXPECT_NE(joined.find("nack-rtx"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.plannedCaps:"), std::string::npos);
  EXPECT_NE(joined.find("red-fec"), std::string::npos);
  EXPECT_NE(joined.find("ulpfec"), std::string::npos);
  EXPECT_NE(joined.find("flexfec"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPaths:"), std::string::npos);
  EXPECT_NE(joined.find("ice-stun-p2p"), std::string::npos);
  EXPECT_NE(joined.find("relay-tcp-tls"), std::string::npos);
  EXPECT_EQ(joined.find("x-ss-core.sessionId:"), std::string::npos);
  EXPECT_EQ(joined.find("x-ss-core.telemetry."), std::string::npos);
  EXPECT_EQ(joined.find("x-ss-core.lease."), std::string::npos);
  EXPECT_LT(joined.size(), 512U);
}

TEST(SessionRuntimeTests, SessionSnapshotAttributesCanCarryFullRuntimeStateOutsideRtsp) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 99;
  identity.launch_session_id = 7;
  identity.control_generation = 3;
  identity.set_client_cert_uuid("client-cert-99");
  identity.client_unique_id = "macbook-pro";
  identity.client_name = "Sky MacBook";

  auto path = session_runtime::make_transport_path(session_runtime::transport_route_e::relay_quic);
  path.state = session_runtime::transport_path_state_e::active;
  path.score.rtt_ms = 18;
  path.score.jitter_ms = 3;
  path.score.loss_ppm = 120;

  session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = path.path_id,
    .displayed_fps = 144,
    .rtt_ms = 18,
    .loss_ppm = 120,
    .renderer_backpressure = false,
    .decode_queue_depth = 1,
    .render_queue_depth = 2,
    .audio_queue_depth_ms = 12,
    .input_queue_depth = 3,
    .input_send_latency_us = 800,
    .input_ack_latency_us = 1000,
    .mouse_backlog_us = 600,
    .pointer_mode = LI_SESSION_POINTER_MODE_RELATIVE,
    .cursor_state_flags = LI_SESSION_CURSOR_FLAG_LOCKED,
    .pointer_release_queue_depth = 2,
    .pointer_release_queue_delay_us = 750,
    .pointer_mode_switch_us = 250,
    .pointer_deltas_coalesced = 4,
    .pointer_acceleration_risk_ppm = 9,
  };

  const auto session = session_runtime::make_li_session(identity,
                                                       path,
                                                       session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                       session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                       report,
                                                       "1337",
                                                       "UFO Test");
  const auto attributes = session_runtime::session_snapshot_attributes(session);
  const auto joined = std::accumulate(attributes.begin(), attributes.end(), std::string {}, [](std::string acc, const std::string &line) {
    acc += line;
    acc += '\n';
    return acc;
  });

  EXPECT_NE(joined.find("x-ss-core.sessionVersion:1"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.sessionId:"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.logicalSessionKey:"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.appId:1337"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.appName:UFO Test"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.client.displayName:Sky MacBook"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.host.displayName:Sunshine Host"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.host.participantKey:"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.host.clientKey:"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.host.deviceKey:"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.lease.feature:input-focus"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.kind:relay-quic"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.protocol:quic"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.stackFlags:"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.candidateType:relay"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.discoverySource:relay-directory"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.egressKind:relay"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.encapsulation:relay"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.relayMode:selected"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.p2pState:none"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.p2pFlags:0x0"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.transportPath.routeId:relay-quic"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.telemetry.pointerMode:relative"), std::string::npos);
  EXPECT_NE(joined.find("x-ss-core.telemetry.cursorFlags:"), std::string::npos);
}

TEST(SessionRuntimeTests, SessionControlHelloCarriesRuntimeStateOutsideRtsp) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 77;
  identity.launch_session_id = 11;
  identity.control_generation = 2;
  identity.logical_session_id = "ml-session-77";
  identity.logical_session_key = 0x7777;
  identity.participant_id = "ml-participant-77";
  identity.participant_key = 0x8888;
  identity.client_key = 0x9999;
  identity.device_key = 0xaaaa;
  identity.client_unique_id = "macbook-pro";
  identity.client_name = "Sky MacBook";

  const auto path = session_runtime::make_enet_direct_transport_path();
  const session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = path.path_id,
    .pointer_mode = LI_SESSION_POINTER_MODE_RELATIVE,
    .cursor_state_flags = LI_SESSION_CURSOR_FLAG_LOCKED,
  };

  const auto session = session_runtime::make_li_session(identity,
                                                        path,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        report,
                                                        "steam",
                                                        "UFO Test");
  const auto hello = session_runtime::make_session_control_hello(session);
  EXPECT_EQ(hello.header.magic, LI_SESSION_CONTROL_MAGIC);
  EXPECT_EQ(hello.header.version, LI_SESSION_CONTROL_VERSION);
  EXPECT_EQ(hello.header.messageType, LI_SESSION_CONTROL_MSG_HELLO);
  EXPECT_LT(sizeof(hello), static_cast<std::size_t>(LI_SESSION_CONTROL_MAX_MESSAGE_SIZE));

  const std::string_view hello_payload {
    reinterpret_cast<const char *>(&hello),
    sizeof(hello),
  };
  const auto parsed = session_runtime::parse_session_control_hello(hello_payload);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->logicalSessionKey, session.logicalSessionKey);
  EXPECT_EQ(parsed->runtimeId, session.runtimeId);
  EXPECT_EQ(parsed->participantKey, session.client.participantKey);
  EXPECT_EQ(parsed->clientKey, session.client.clientKey);
  EXPECT_EQ(parsed->deviceKey, session.client.deviceKey);
  EXPECT_STREQ(parsed->sessionId, session.sessionId.value);
  EXPECT_STREQ(parsed->participantId, session.client.participantId.value);
  EXPECT_STREQ(parsed->displayName, session.client.displayName);

  const auto welcome = session_runtime::make_session_control_welcome(session);
  EXPECT_EQ(welcome.header.magic, LI_SESSION_CONTROL_MAGIC);
  EXPECT_EQ(welcome.header.messageType, LI_SESSION_CONTROL_MSG_WELCOME);
  EXPECT_EQ(welcome.hostFeatureBits, session.featureCaps.host);
  EXPECT_EQ(welcome.negotiatedFeatureBits, session.featureCaps.negotiated);
}

TEST(SessionRuntimeTests, SessionControlTelemetryCarriesCanonicalRuntimeBusReport) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 91;
  identity.logical_session_id = "ml-session-91";
  identity.logical_session_key = 0x9191;
  identity.participant_id = "ml-participant-91";
  identity.participant_key = 0x9292;
  identity.client_key = 0x9393;
  identity.device_key = 0x9494;
  identity.client_unique_id = "macbook-pro";
  identity.client_name = "Sky MacBook";

  auto path = session_runtime::make_transport_path(session_runtime::transport_route_e::relay_quic);
  path.state = session_runtime::transport_path_state_e::active;
  path.score.rtt_ms = 42;
  path.score.jitter_ms = 7;
  path.score.loss_ppm = 35000;

  const session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = path.path_id,
    .displayed_fps = 144,
    .rtt_ms = 42,
    .loss_ppm = 35000,
    .renderer_backpressure = true,
    .decode_queue_depth = 3,
    .render_queue_depth = 2,
    .input_queue_depth = 5,
    .input_send_latency_us = 1100,
    .input_ack_latency_us = 2200,
    .mouse_backlog_us = 1500,
    .pointer_mode = LI_SESSION_POINTER_MODE_HYBRID,
    .cursor_state_flags = LI_SESSION_CURSOR_FLAG_LOCKED | LI_SESSION_CURSOR_FLAG_REMOTE_PLANE,
    .pointer_release_queue_depth = 2,
    .pointer_release_queue_delay_us = 900,
  };

  const auto session = session_runtime::make_li_session(identity,
                                                        path,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        report,
                                                        "desktop",
                                                        "Desktop");
  const auto telemetry = session_runtime::make_session_control_telemetry(session, 17);

  EXPECT_EQ(telemetry.header.messageType, LI_SESSION_CONTROL_MSG_TELEMETRY);
  EXPECT_EQ(telemetry.sequence, 17U);
  EXPECT_EQ(telemetry.logicalSessionKey, session.logicalSessionKey);
  EXPECT_EQ(telemetry.runtimeId, session.runtimeId);
  EXPECT_EQ(telemetry.participantKey, session.client.participantKey);
  EXPECT_EQ(telemetry.pathId, session.transportPath.pathId);
  EXPECT_EQ(telemetry.telemetry.inputQueueDepth, 5U);
  EXPECT_EQ(telemetry.telemetry.pointerMode, LI_SESSION_POINTER_MODE_HYBRID);
  EXPECT_EQ(telemetry.transportPath.packetLossPpm, 35000U);
  EXPECT_NE(telemetry.transportPath.stackFlags & LI_SESSION_TRANSPORT_STACK_QUIC, 0U);
  EXPECT_NE(telemetry.transportPath.stackFlags & LI_SESSION_TRANSPORT_STACK_RELAY, 0U);
  EXPECT_LT(sizeof(telemetry), static_cast<std::size_t>(LI_SESSION_CONTROL_MAX_MESSAGE_SIZE));

  const auto parsed = session_runtime::parse_session_control_telemetry({
    reinterpret_cast<const char *>(&telemetry),
    sizeof(telemetry),
  });
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->sequence, 17U);
  EXPECT_EQ(parsed->telemetry.inputAckLatencyUs, 2200U);
  EXPECT_EQ(parsed->transportPath.rttUs, 42000U);
  EXPECT_EQ(parsed->transportPath.candidateType, LI_SESSION_PATH_CANDIDATE_RELAY);
}

TEST(SessionRuntimeTests, SessionControlCursorPlaneCarriesDisplayAndBitmapSemantics) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 92;
  identity.logical_session_id = "ml-session-cursor";
  identity.logical_session_key = 0x9292;
  identity.participant_id = "ml-participant-cursor";
  identity.participant_key = 0x9393;
  identity.client_key = 0x9494;
  identity.device_key = 0x9595;
  identity.client_unique_id = "macbook-pro";
  identity.client_name = "Sky MacBook";

  const auto path = session_runtime::make_enet_direct_transport_path();
  const session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = path.path_id,
    .pointer_mode = LI_SESSION_POINTER_MODE_ABSOLUTE,
    .cursor_state_flags = LI_SESSION_CURSOR_FLAG_VISIBLE | LI_SESSION_CURSOR_FLAG_REMOTE_PLANE,
  };

  auto session = session_runtime::make_li_session(identity,
                                                  path,
                                                  session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                  session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                  report,
                                                  "desktop",
                                                  "Desktop");
  session.cursorPlane.flags = LI_SESSION_CURSOR_FLAG_VISIBLE | LI_SESSION_CURSOR_FLAG_REMOTE_PLANE;
  session.cursorPlane.renderPolicy = LI_SESSION_CURSOR_RENDER_POLICY_CONTENT_SCALED;
  session.cursorPlane.sizeSource = LI_SESSION_CURSOR_SIZE_SOURCE_SYSTEM_METRIC;
  session.cursorPlane.confidencePpm = 900000;
  session.cursorPlane.streamWidth = 3840;
  session.cursorPlane.streamHeight = 2160;
  session.cursorPlane.positionX = 2000;
  session.cursorPlane.positionY = 900;
  session.cursorPlane.displayWidth = 64;
  session.cursorPlane.displayHeight = 64;
  session.cursorPlane.hotspotX = 7;
  session.cursorPlane.hotspotY = 3;
  session.cursorPlane.bitmapWidth = 96;
  session.cursorPlane.bitmapHeight = 96;
  session.cursorPlane.bitmapStride = 384;
  session.cursorPlane.bitmapFormat = LI_SESSION_CURSOR_BITMAP_FORMAT_BGRA;
  session.cursorPlane.hostDpiScalePpm = 2000000;
  session.cursorPlane.userScalePpm = 1000000;
  session.cursorPlane.minClientPointSize = 8;
  session.cursorPlane.maxClientPointSize = 128;
  session.cursorPlane.epoch = 44;

  const auto cursor_plane = session_runtime::make_session_control_cursor_plane(session, 5);

  EXPECT_EQ(cursor_plane.header.messageType, LI_SESSION_CONTROL_MSG_CURSOR_PLANE);
  EXPECT_EQ(cursor_plane.sequence, 5U);
  EXPECT_EQ(cursor_plane.logicalSessionKey, session.logicalSessionKey);
  EXPECT_EQ(cursor_plane.runtimeId, session.runtimeId);
  EXPECT_EQ(cursor_plane.participantKey, session.host.participantKey);
  EXPECT_EQ(cursor_plane.cursorPlane.displayWidth, 64U);
  EXPECT_EQ(cursor_plane.cursorPlane.bitmapWidth, 96U);
  EXPECT_EQ(cursor_plane.cursorPlane.hostDpiScalePpm, 2000000U);
  EXPECT_LT(sizeof(cursor_plane), static_cast<std::size_t>(LI_SESSION_CONTROL_MAX_MESSAGE_SIZE));

  const auto parsed = session_runtime::parse_session_control_cursor_plane({
    reinterpret_cast<const char *>(&cursor_plane),
    sizeof(cursor_plane),
  });
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->sequence, 5U);
  EXPECT_EQ(parsed->cursorPlane.streamWidth, 3840U);
  EXPECT_EQ(parsed->cursorPlane.displayHeight, 64U);
  EXPECT_EQ(parsed->cursorPlane.bitmapStride, 384U);
  EXPECT_EQ(parsed->cursorPlane.epoch, 44U);
}

TEST(SessionRuntimeTests, SessionControlLeaseRequestAckNegotiatesOwnership) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 123;
  identity.logical_session_key = 0x123123;
  identity.participant_key = 0x456456;
  identity.client_key = 0x789789;
  identity.device_key = 0x999999;
  identity.participant_id = "ml-controller";
  identity.client_name = "Sky Controller";

  const auto path = session_runtime::make_enet_direct_transport_path();
  const session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = path.path_id,
  };
  const auto session = session_runtime::make_li_session(identity,
                                                        path,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        report,
                                                        "desktop",
                                                        "Desktop");

  const auto request = session_runtime::make_session_control_lease_request(
    session,
    LI_SESSION_RESOURCE_INPUT_FOCUS,
    LI_SESSION_LEASE_MODE_EXCLUSIVE_OWNER,
    3000);

  EXPECT_EQ(request.header.messageType, LI_SESSION_CONTROL_MSG_LEASE);
  EXPECT_EQ(request.operation, LI_SESSION_CONTROL_LEASE_OP_REQUEST);
  EXPECT_EQ(request.status, LI_SESSION_CONTROL_LEASE_STATUS_PENDING);
  EXPECT_EQ(request.resource, LI_SESSION_RESOURCE_INPUT_FOCUS);
  EXPECT_EQ(request.mode, LI_SESSION_LEASE_MODE_EXCLUSIVE_OWNER);
  EXPECT_EQ(request.lease.ownerRuntimeId, session.runtimeId);
  EXPECT_EQ(request.lease.ownerParticipantKey, session.client.participantKey);

  auto granted_lease = request.lease;
  granted_lease.valid = true;
  granted_lease.ttlMs = 3000;
  const auto ack = session_runtime::make_session_control_lease_ack(
    session,
    granted_lease,
    LI_SESSION_CONTROL_LEASE_OP_ACK,
    LI_SESSION_CONTROL_LEASE_STATUS_GRANTED);

  EXPECT_EQ(ack.operation, LI_SESSION_CONTROL_LEASE_OP_ACK);
  EXPECT_EQ(ack.status, LI_SESSION_CONTROL_LEASE_STATUS_GRANTED);
  EXPECT_TRUE(ack.lease.valid);
  EXPECT_LT(sizeof(ack), static_cast<std::size_t>(LI_SESSION_CONTROL_MAX_MESSAGE_SIZE));

  const auto parsed = session_runtime::parse_session_control_lease({
    reinterpret_cast<const char *>(&ack),
    sizeof(ack),
  });
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->status, LI_SESSION_CONTROL_LEASE_STATUS_GRANTED);
  EXPECT_EQ(parsed->lease.ownerParticipantKey, session.client.participantKey);
}

TEST(SessionRuntimeTests, TrustedClientIdentityReplacesLegacyLaunchPlaceholders) {
  session_runtime::identity_t identity {};
  identity.client_unique_id = "0123456789ABCDEF";
  identity.client_name = "unknown";

  std::string launch_unique_id = identity.client_unique_id;
  std::string launch_client_name = identity.client_name;

  session_runtime::promote_trusted_client_identity(identity,
                                                   &launch_unique_id,
                                                   &launch_client_name,
                                                   "paired-cert-uuid",
                                                   "Sky MacBook");

  EXPECT_EQ(identity.client_cert_uuid, "paired-cert-uuid");
  EXPECT_NE(identity.client_cert_key, 0U);
  EXPECT_EQ(identity.client_unique_id, "paired-cert-uuid");
  EXPECT_EQ(identity.client_name, "Sky MacBook");
  EXPECT_EQ(launch_unique_id, "paired-cert-uuid");
  EXPECT_EQ(launch_client_name, "Sky MacBook");
}

TEST(SessionRuntimeTests, TransportCcLiteSummarizesArrivalAndLossRanges) {
  session_runtime::transport_cc_lite_feedback_t feedback {
    .path_id = 55,
    .base_sequence = 1000,
    .reference_time_us = 5000000,
  };

  feedback.add_received(1000, 5000100, 1200);
  feedback.add_lost(1001);
  feedback.add_received(1002, 5001300, 1200);

  EXPECT_EQ(feedback.received_count(), 2U);
  EXPECT_EQ(feedback.lost_count(), 1U);
  EXPECT_EQ(feedback.total_payload_bytes(), 2400U);
  EXPECT_EQ(feedback.last_sequence(), 1002U);
  EXPECT_EQ(feedback.arrival_span_us(), 1200U);
}

TEST(SessionRuntimeTests, PacerProbePlansUseExponentialStartupAndAlrGain) {
  const auto targets = session_runtime::startup_probe_targets_kbps(8000, 50000);

  ASSERT_GE(targets.size(), 4U);
  EXPECT_EQ(targets.front(), 12800);
  EXPECT_EQ(targets.back(), 50000);
  EXPECT_TRUE(std::ranges::is_sorted(targets));

  const auto plan = session_runtime::make_alr_probe_plan({
    .path_id = 7,
    .current_bitrate_kbps = 20000,
    .ceiling_bitrate_kbps = 50000,
    .duration_ms = 120,
  });

  EXPECT_EQ(plan.path_id, 7U);
  EXPECT_TRUE(plan.app_limited);
  EXPECT_GE(plan.target_bitrate_kbps, 32000);
  EXPECT_LE(plan.target_bitrate_kbps, 50000);
  EXPECT_GT(plan.probe_budget_bytes, 0U);
}

TEST(SessionRuntimeTests, RouterPortForwardStartupPolicySeedsBitrateAndCapsHighRefreshCadence) {
  session_runtime::startup_path_decision_t decision {
    .route = session_runtime::transport_route_e::manual_public_port_forward,
    .allow_lan_fast_start = false,
    .path_identity_kind = LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD,
    .startup_class = LI_SESSION_STARTUP_CLASS_REMOTE_SAFE,
    .reason = "client-remote-hint",
  };

  const auto policy = session_runtime::startup_ceiling_policy_for_path(decision, 100);

  EXPECT_EQ(policy.bitrate_seed_kbps, 12000);
  EXPECT_EQ(policy.fps_cap, 60);
  EXPECT_STREQ(policy.reason, "router-port-forward-safe");
}

TEST(SessionRuntimeTests, HairpinPortForwardUsesElevenOClockRouterBaseline) {
  session_runtime::startup_path_decision_t decision {
    .route = session_runtime::transport_route_e::manual_public_port_forward,
    .allow_lan_fast_start = false,
    .path_identity_kind = LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD,
    .startup_class = LI_SESSION_STARTUP_CLASS_REMOTE_SAFE,
    .reason = "router-snat-port-forward",
  };

  const auto policy = session_runtime::startup_ceiling_policy_for_path(decision, 150);

  EXPECT_EQ(policy.bitrate_seed_kbps, 12000);
  EXPECT_EQ(policy.fps_cap, 60);
  EXPECT_STREQ(policy.reason, "router-port-forward-safe");
}

TEST(SessionRuntimeTests, RouterPortForwardFreshLowLatencyPreflightAllowsRequestedHighRefreshStartup) {
  session_runtime::startup_path_decision_t decision {
    .route = session_runtime::transport_route_e::manual_public_port_forward,
    .allow_lan_fast_start = false,
    .path_identity_kind = LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD,
    .startup_class = LI_SESSION_STARTUP_CLASS_REMOTE_SAFE,
    .reason = "router-snat-port-forward",
  };
  session_runtime::startup_path_evidence_t evidence {
    .preflight_available = true,
    .preflight_observed_peer_endpoint = "192.168.100.1:53000",
    .preflight_host_local_endpoint = "192.168.100.133:57989",
    .preflight_target_endpoint = "home.example.net:57989",
    .preflight_rtt_ms = 18,
    .preflight_age_ms = 250,
  };

  const auto policy = session_runtime::startup_ceiling_policy_for_path(decision, 150, &evidence);

  EXPECT_EQ(policy.bitrate_seed_kbps, 0);
  EXPECT_EQ(policy.fps_cap, 150);
  EXPECT_TRUE(policy.allow_preflight_startup_lift);
  EXPECT_STREQ(policy.reason, "router-port-forward-preflight-fast");
}

TEST(SessionRuntimeTests, RouterPortForwardFreshUsablePreflightStartsAboveLegacySixty) {
  session_runtime::startup_path_decision_t decision {
    .route = session_runtime::transport_route_e::manual_public_port_forward,
    .allow_lan_fast_start = false,
    .path_identity_kind = LI_SESSION_PATH_IDENTITY_ROUTER_PORT_FORWARD,
    .startup_class = LI_SESSION_STARTUP_CLASS_REMOTE_SAFE,
    .reason = "router-snat-port-forward",
  };
  session_runtime::startup_path_evidence_t evidence {
    .preflight_available = true,
    .preflight_observed_peer_endpoint = "203.0.113.7:53000",
    .preflight_host_local_endpoint = "192.168.100.133:57989",
    .preflight_target_endpoint = "home.example.net:57989",
    .preflight_rtt_ms = 52,
    .preflight_age_ms = 1000,
  };

  const auto policy = session_runtime::startup_ceiling_policy_for_path(decision, 150, &evidence);

  EXPECT_EQ(policy.bitrate_seed_kbps, 0);
  EXPECT_EQ(policy.fps_cap, 120);
  EXPECT_TRUE(policy.allow_preflight_startup_lift);
  EXPECT_STREQ(policy.reason, "router-port-forward-preflight-usable");
}

TEST(SessionRuntimeTests, RuntimeProfileResolutionReconfigEnabledForConstrainedHighMotionRecovery) {
  EXPECT_TRUE(session_runtime::runtime_profile_resolution_reconfig_enabled());
}

TEST(SessionRuntimeTests, NackRtxHistorySelectsOnlyUsefulMissingPackets) {
  session_runtime::rtx_history_t history { 1000 };
  history.remember({ .sequence = 10, .frame_id = 1, .payload_size = 1000, .sent_at_ms = 1000 });
  history.remember({ .sequence = 11, .frame_id = 1, .payload_size = 1000, .sent_at_ms = 1005 });
  history.remember({ .sequence = 12, .frame_id = 1, .payload_size = 1000, .sent_at_ms = 1010 });
  history.remember({ .sequence = 13, .frame_id = 1, .payload_size = 1000, .sent_at_ms = 1015 });

  const std::vector<session_runtime::missing_packet_range_t> missing {
    { .first_sequence = 11, .last_sequence = 12 },
    { .first_sequence = 99, .last_sequence = 99 },
  };

  const auto selected = history.select_for_retransmission(missing, 1200, 1500, 3000);

  ASSERT_EQ(selected.size(), 2U);
  EXPECT_EQ(selected[0].sequence, 11U);
  EXPECT_EQ(selected[1].sequence, 12U);
  EXPECT_EQ(history.lookup(99), nullptr);

  history.evict_older_than(2600);
  EXPECT_EQ(history.lookup(10), nullptr);
}

TEST(SessionRuntimeTests, PathRacingChoosesBestReachableCandidateAndFallsBackToRelay) {
  session_runtime::path_racing_t racing;
  const auto lan = session_runtime::make_transport_path(session_runtime::transport_route_e::lan_direct);
  const auto ice = session_runtime::make_transport_path(session_runtime::transport_route_e::ice_stun_p2p);
  const auto relay = session_runtime::make_transport_path(session_runtime::transport_route_e::relay_quic);

  racing.add_candidate(lan);
  racing.add_candidate(ice);
  racing.add_candidate(relay);

  racing.submit_check_result({ .path_id = lan.path_id, .reachable = false });
  racing.submit_check_result({ .path_id = ice.path_id, .reachable = true, .rtt_ms = 28, .loss_ppm = 2000 });
  racing.submit_check_result({ .path_id = relay.path_id, .reachable = true, .rtt_ms = 38, .loss_ppm = 0 });

  auto selected = racing.select_best();
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->route, session_runtime::transport_route_e::ice_stun_p2p);

  racing.submit_check_result({ .path_id = ice.path_id, .reachable = false });
  selected = racing.select_best();
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->route, session_runtime::transport_route_e::relay_quic);
}

TEST(SessionRuntimeTests, RuntimeExportsCanonicalLiSessionContract) {
  session_runtime::identity_t identity {};
  identity.runtime_id = 42;
  identity.launch_session_id = 7;
  identity.control_generation = 3;
  identity.set_client_cert_uuid("client-cert-a");
  identity.client_unique_id = "macbook-pro";
  identity.client_name = "Sky Mac";

  auto active_path = session_runtime::make_transport_path(session_runtime::transport_route_e::relay_quic);
  active_path.state = session_runtime::transport_path_state_e::active;
  active_path.score.rtt_ms = 38;
  active_path.score.jitter_ms = 8;
  active_path.score.loss_ppm = 12000;
  active_path.score.throughput_kbps = 50000;

  session_runtime::feature_caps_t client_caps {};
  client_caps.enable(session_runtime::capability_e::native_renderer_metrics)
    .enable(session_runtime::capability_e::metal_renderer_metrics)
    .enable(session_runtime::capability_e::transport_cc_lite)
    .enable(session_runtime::capability_e::nack_rtx)
    .enable(session_runtime::capability_e::relay_quic);

  session_runtime::session_telemetry_report_t report {
    .participant = session_runtime::make_participant(identity).id,
    .path_id = active_path.path_id,
    .displayed_fps = 150,
    .rtt_ms = 38,
    .loss_ppm = 12000,
    .renderer_backpressure = false,
    .input_queue_depth = 2,
    .input_send_latency_us = 700,
    .input_ack_latency_us = 2100,
    .mouse_backlog_us = 350,
    .pointer_mode = LI_SESSION_POINTER_MODE_RELATIVE,
    .cursor_state_flags = LI_SESSION_CURSOR_FLAG_LOCKED | LI_SESSION_CURSOR_FLAG_RELEASE_SMOOTHING_ACTIVE,
    .pointer_release_queue_depth = 1,
    .pointer_release_queue_delay_us = 850,
    .pointer_mode_switch_us = 430,
  };

  const auto session = session_runtime::make_li_session(identity,
                                                        active_path,
                                                        client_caps,
                                                        session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                        report,
                                                        "steam",
                                                        "Call of Duty");

  EXPECT_EQ(session.version, LI_SESSION_VERSION);
  EXPECT_EQ(session.state, LI_SESSION_STATE_STREAMING);
  EXPECT_EQ(session.runtimeId, 42U);
  EXPECT_EQ(session.launchSessionId, 7U);
  EXPECT_EQ(session.controlGeneration, 3U);
  EXPECT_NE(session.logicalSessionKey, 0U);
  EXPECT_EQ(session.client.role, LI_SESSION_ROLE_CLIENT);
  EXPECT_EQ(session.host.role, LI_SESSION_ROLE_HOST);
  EXPECT_EQ(session.client.index, 0U);
  EXPECT_EQ(session.host.index, 1U);
  EXPECT_NE(session.host.participantKey, 0U);
  EXPECT_NE(session.featureCaps.client & LI_SESSION_FEATURE_INPUT_RELEASE_SMOOTHING, 0U);
  EXPECT_NE(session.featureCaps.negotiated & LI_SESSION_FEATURE_INPUT_TELEMETRY, 0U);
  EXPECT_EQ(session.transportPath.kind, LI_SESSION_TRANSPORT_RELAY_QUIC);
  EXPECT_NE(session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_RELAY, 0U);
  EXPECT_NE(session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_ENCRYPTED, 0U);
  EXPECT_NE(session.transportPath.stackFlags & LI_SESSION_TRANSPORT_STACK_QUIC, 0U);
  EXPECT_NE(session.transportPath.stackFlags & LI_SESSION_TRANSPORT_STACK_RELAY, 0U);
  EXPECT_EQ(session.transportPath.candidateType, LI_SESSION_PATH_CANDIDATE_RELAY);
  EXPECT_EQ(session.transportPath.discoverySource, LI_SESSION_PATH_DISCOVERY_RELAY_DIRECTORY);
  EXPECT_EQ(session.transportPath.encapsulation, LI_SESSION_PATH_ENCAPSULATION_RELAY);
  EXPECT_EQ(session.transportPath.rttUs, 38000U);
  EXPECT_EQ(session.transportPath.jitterUs, 8000U);
  EXPECT_EQ(session.transportPath.packetLossPpm, 12000U);
  EXPECT_EQ(session.telemetry.pointerMode, LI_SESSION_POINTER_MODE_RELATIVE);
  EXPECT_NE(session.telemetry.cursorStateFlags & LI_SESSION_CURSOR_FLAG_RELEASE_SMOOTHING_ACTIVE, 0U);
  EXPECT_EQ(session.telemetry.pointerReleaseQueueDepth, 1U);
  EXPECT_EQ(session.telemetry.pointerReleaseQueueDelayUs, 850U);
  EXPECT_EQ(session.telemetry.pointerModeSwitchUs, 430U);
}

TEST(SessionRuntimeTests, InputSmoothingSnapshotPromotesSessionTelemetryAndFlags) {
  session_runtime::session_telemetry_report_t report {
    .input_queue_depth = 1,
    .input_send_latency_us = 400,
    .mouse_backlog_us = 400,
    .pointer_mode = LI_SESSION_POINTER_MODE_ABSOLUTE,
    .cursor_state_flags = LI_SESSION_CURSOR_FLAG_SYSTEM_CURSOR_ACTIVE,
  };

  session_runtime::input_smoothing_snapshot_t snapshot {
    .queue_depth = 3,
    .queue_delay_us = 1250,
    .mode_switch_us = 540,
    .deltas_coalesced = 9,
    .acceleration_risk_ppm = 22000,
    .release_smoothing_active = true,
  };

  session_runtime::apply_input_smoothing_snapshot(report, snapshot);

  EXPECT_EQ(report.input_queue_depth, 3U);
  EXPECT_EQ(report.input_send_latency_us, 1250U);
  EXPECT_EQ(report.mouse_backlog_us, 1250U);
  EXPECT_EQ(report.pointer_release_queue_depth, 3U);
  EXPECT_EQ(report.pointer_release_queue_delay_us, 1250U);
  EXPECT_EQ(report.pointer_mode_switch_us, 540U);
  EXPECT_EQ(report.pointer_deltas_coalesced, 9U);
  EXPECT_EQ(report.pointer_acceleration_risk_ppm, 22000U);
  EXPECT_NE(report.cursor_state_flags & LI_SESSION_CURSOR_FLAG_RELEASE_SMOOTHING_ACTIVE, 0U);
}

TEST(SessionRuntimeTests, StartupPathPrivateOverlayUsesRemoteSafeWithoutTunnelRisk) {
  const session_runtime::startup_path_evidence_t evidence {
    .peer_is_lan_or_pc = true,
    .remote_streaming_hint = true,
    .client_route_remote_hint = true,
    .startup_profile = "remote",
    .client_egress_kind = "physical",
    .client_route_host = "10.89.0.1",
    .client_route_path_kind = "private-overlay",
    .rtsp_route_host = "10.89.0.1",
    .client_source_endpoint = "10.89.0.5",
    .host_observed_peer_endpoint = "10.89.0.5:53000",
    .host_observed_local_endpoint = "10.89.0.1:47989",
    .client_target_address_candidates = { "10.89.0.1" },
    .host_public_candidates = { "218.1.2.3" },
  };

  const auto decision = session_runtime::classify_startup_path(evidence);

  EXPECT_FALSE(decision.allow_lan_fast_start);
  EXPECT_EQ(decision.route, session_runtime::transport_route_e::overlay_route);
  EXPECT_EQ(decision.egress_kind, LI_SESSION_PATH_EGRESS_PHYSICAL);
  EXPECT_EQ(decision.encapsulation, LI_SESSION_PATH_ENCAPSULATION_NATIVE_IP);
  EXPECT_EQ(decision.path_identity_kind, LI_SESSION_PATH_IDENTITY_VPN_OVERLAY);
  EXPECT_EQ(decision.startup_class, LI_SESSION_STARTUP_CLASS_REMOTE_SAFE);
  EXPECT_EQ(decision.risk_flags, 0U);
  EXPECT_STREQ(decision.reason, "client-private-overlay");

  const auto policy = session_runtime::startup_ceiling_policy_for_path(decision, 150);
  EXPECT_EQ(policy.fps_cap, 0);
  EXPECT_STREQ(policy.reason, "default");
}
