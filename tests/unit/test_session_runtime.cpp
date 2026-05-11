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
  EXPECT_NE(session.featureCaps.client & LI_SESSION_FEATURE_INPUT_RELEASE_SMOOTHING, 0U);
  EXPECT_NE(session.featureCaps.negotiated & LI_SESSION_FEATURE_INPUT_TELEMETRY, 0U);
  EXPECT_EQ(session.transportPath.kind, LI_SESSION_TRANSPORT_RELAY_QUIC);
  EXPECT_NE(session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_RELAY, 0U);
  EXPECT_NE(session.transportPath.flags & LI_SESSION_TRANSPORT_FLAG_ENCRYPTED, 0U);
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
