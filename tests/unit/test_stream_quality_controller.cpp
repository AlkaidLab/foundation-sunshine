#include "src/stream_quality_controller.h"

#include <alkaidlab/rescue_control/rescue_control_contract.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

TEST(StreamQualityControllerTests, DropsBitrateAndRaisesFecWhenFramesAreUnrecoverable) {
  stream_quality::controller_t controller;
  controller.configure({ .baseline_bitrate_kbps = 50000, .baseline_fec_percentage = 20 });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 40,
    .recovered_frames = 4,
    .unrecoverable_frames = 16,
    .missing_packets = 320,
    .total_packets = 2400,
    .received_packets = 2080,
    .video_bytes = 5 * 1024 * 1024,
    .rtt_ms = 80,
    .rtt_variance_ms = 30,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_TRUE(action.changed);
  EXPECT_LT(action.target_bitrate_kbps, 50000);
  EXPECT_GT(action.fec_percentage, 20);
  EXPECT_TRUE(action.request_idr);
}

TEST(StreamQualityControllerTests, DiagnosesAudioOnlyAsAudioDomain) {
  auto diagnosis = stream_quality::diagnose_feedback({
    .duration_ms = 1000,
    .frames_seen = 120,
    .complete_frames = 120,
    .video_bytes = 8 * 1024 * 1024,
    .rtt_ms = 18,
    .rtt_variance_ms = 4,
    .audio_underruns = 24,
    .displayed_frames = 120,
    .audio_plc_ms = 360,
  });

  EXPECT_EQ(diagnosis.primary_domain, ALK_CONTINUITY_DOMAIN_AUDIO);
  EXPECT_NE(diagnosis.allowed_action_mask & ALK_CONTINUITY_ACTION_AUDIO_RECOVER, 0u);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_VIDEO_QUALITY_DESTRUCTIVE, 0u);
}

TEST(StreamQualityControllerTests, DiagnosesSourceLimitedLoadingWithoutNetworkLearning) {
  auto diagnosis = stream_quality::diagnose_feedback({
    .duration_ms = 1000,
    .frames_seen = 15,
    .complete_frames = 15,
    .video_bytes = 1200 * 1024,
    .rtt_ms = 22,
    .rtt_variance_ms = 3,
    .displayed_frames = 15,
  });

  EXPECT_EQ(diagnosis.primary_domain, ALK_CONTINUITY_DOMAIN_SOURCE);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_NETWORK_LONG_TERM_MEMORY, 0u);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_VIDEO_QUALITY_DESTRUCTIVE, 0u);
}

TEST(StreamQualityControllerTests, DiagnosesRecoverableLossAsNonDestructiveMedia) {
  auto diagnosis = stream_quality::diagnose_feedback({
    .duration_ms = 1000,
    .frames_seen = 120,
    .complete_frames = 120,
    .recovered_frames = 6,
    .unrecoverable_frames = 0,
    .missing_packets = 48,
    .total_packets = 3000,
    .received_packets = 2952,
    .video_bytes = 9 * 1024 * 1024,
    .rtt_ms = 31,
    .rtt_variance_ms = 5,
    .displayed_frames = 118,
  });

  EXPECT_EQ(diagnosis.primary_domain, ALK_CONTINUITY_DOMAIN_VIDEO_MEDIA);
  EXPECT_NE(diagnosis.allowed_action_mask & ALK_CONTINUITY_ACTION_VIDEO_FEC, 0u);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_NETWORK_LONG_TERM_MEMORY, 0u);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_VIDEO_BITRATE, 0u);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_VIDEO_FPS, 0u);
  EXPECT_NE(diagnosis.forbidden_action_mask & ALK_CONTINUITY_ACTION_VIDEO_SCALE, 0u);
}


TEST(StreamQualityControllerTests, DiagnosisGatesDestructiveRuntimeVideoApply) {
  stream_quality::action_t action {};
  action.changed = true;
  action.state = stream_quality::state_e::crisis;
  action.availability = stream_quality::availability_e::low;
  action.reason = stream_quality::reason_e::random_loss;
  action.scenario = stream_quality::scenario_e::random_loss;
  action.target_bitrate_kbps = 12000;
  action.fec_percentage = 60;
  action.pacing_bitrate_kbps = 19200;
  action.target_fps = 30;
  action.resolution_scale_percent = 75;
  action.chroma_sampling_type = 1;
  action.dynamic_range = 0;
  action.profile_tier_supported = true;
  action.profile_tier_changed = true;
  action.request_idr = true;

  const stream_quality::runtime_action_context_t context {
    .last_applied_bitrate_kbps = 80000,
    .last_applied_fec_percentage = 20,
    .last_applied_fps = 150,
    .last_applied_resolution_scale_percent = 100,
    .last_applied_chroma_sampling_type = 0,
    .last_applied_dynamic_range = 1,
    .elapsed_ms_since_bitrate_apply = 2000,
    .elapsed_ms_since_fec_apply = 2000,
    .elapsed_ms_since_fps_apply = 2000,
    .elapsed_ms_since_profile_apply = 2000,
  };

  AlkContinuityDiagnosisFrame diagnosis;
  alk_continuity_diagnosis_frame_init(&diagnosis);
  diagnosis.primary_domain = ALK_CONTINUITY_DOMAIN_SOURCE;
  diagnosis.confidence_ppm = 900000;
  diagnosis.allowed_action_mask = ALK_CONTINUITY_ACTION_OBSERVE;
  diagnosis.forbidden_action_mask = ALK_CONTINUITY_ACTION_NETWORK_LONG_TERM_MEMORY |
                                    ALK_CONTINUITY_ACTION_VIDEO_QUALITY_DESTRUCTIVE;

  const auto plan = stream_quality::plan_runtime_action_with_diagnosis(action, context, diagnosis);

  EXPECT_FALSE(plan.apply_bitrate);
  EXPECT_FALSE(plan.apply_fec);
  EXPECT_FALSE(plan.apply_fps);
  EXPECT_FALSE(plan.apply_profile);
  EXPECT_FALSE(plan.apply_idr);
  EXPECT_FALSE(plan.update_runtime_totals);
}

TEST(StreamQualityControllerTests, DiagnosisAllowsRecoverableLossFecAndIdrOnly) {
  stream_quality::action_t action {};
  action.changed = true;
  action.state = stream_quality::state_e::recovering;
  action.availability = stream_quality::availability_e::recovering;
  action.reason = stream_quality::reason_e::media_continuity;
  action.scenario = stream_quality::scenario_e::media_continuity;
  action.target_bitrate_kbps = 80000;
  action.fec_percentage = 28;
  action.pacing_bitrate_kbps = 102400;
  action.target_fps = 150;
  action.resolution_scale_percent = 100;
  action.chroma_sampling_type = 0;
  action.dynamic_range = 1;
  action.request_idr = true;

  const stream_quality::runtime_action_context_t context {
    .last_applied_bitrate_kbps = 80000,
    .last_applied_fec_percentage = 20,
    .last_applied_fps = 150,
    .last_applied_resolution_scale_percent = 100,
    .last_applied_chroma_sampling_type = 0,
    .last_applied_dynamic_range = 1,
    .elapsed_ms_since_bitrate_apply = 2000,
    .elapsed_ms_since_fec_apply = 2000,
    .elapsed_ms_since_fps_apply = 2000,
    .elapsed_ms_since_profile_apply = 2000,
  };

  AlkContinuityDiagnosisFrame diagnosis;
  alk_continuity_diagnosis_frame_init(&diagnosis);
  diagnosis.primary_domain = ALK_CONTINUITY_DOMAIN_VIDEO_MEDIA;
  diagnosis.secondary_domain = ALK_CONTINUITY_DOMAIN_NETWORK_TRANSPORT;
  diagnosis.confidence_ppm = 780000;
  diagnosis.allowed_action_mask = ALK_CONTINUITY_ACTION_OBSERVE |
                                  ALK_CONTINUITY_ACTION_VIDEO_FEC |
                                  ALK_CONTINUITY_ACTION_VIDEO_IDR;
  diagnosis.forbidden_action_mask = ALK_CONTINUITY_ACTION_NETWORK_LONG_TERM_MEMORY |
                                    ALK_CONTINUITY_ACTION_VIDEO_BITRATE |
                                    ALK_CONTINUITY_ACTION_VIDEO_FPS |
                                    ALK_CONTINUITY_ACTION_VIDEO_SCALE;

  const auto plan = stream_quality::plan_runtime_action_with_diagnosis(action, context, diagnosis);

  EXPECT_FALSE(plan.apply_bitrate);
  EXPECT_TRUE(plan.apply_fec);
  EXPECT_FALSE(plan.apply_fps);
  EXPECT_FALSE(plan.apply_profile);
  EXPECT_TRUE(plan.apply_idr);
  EXPECT_TRUE(plan.update_runtime_totals);
}

TEST(StreamQualityControllerTests, RestrictedDiagnosisKeepsRuntimePlanAsPureActionProjection) {
  stream_quality::action_t action {};
  action.changed = true;
  action.state = stream_quality::state_e::recovering;
  action.availability = stream_quality::availability_e::recovering;
  action.reason = stream_quality::reason_e::recovering;
  action.scenario = stream_quality::scenario_e::recovering;
  action.target_bitrate_kbps = 500;
  action.fec_percentage = 35;
  action.pacing_bitrate_kbps = 700;
  action.target_fps = 90;
  action.resolution_scale_percent = 100;
  action.chroma_sampling_type = 0;
  action.dynamic_range = 0;
  action.profile_tier_supported = true;

  const stream_quality::runtime_action_context_t context {
    .last_applied_bitrate_kbps = 11000,
    .last_applied_fec_percentage = 35,
    .last_applied_fps = 45,
    .last_applied_resolution_scale_percent = 100,
    .last_applied_chroma_sampling_type = 0,
    .last_applied_dynamic_range = 0,
    .elapsed_ms_since_bitrate_apply = 2000,
    .elapsed_ms_since_fec_apply = 2000,
    .elapsed_ms_since_fps_apply = 2000,
    .elapsed_ms_since_profile_apply = 2000,
  };

  AlkContinuityDiagnosisFrame diagnosis;
  alk_continuity_diagnosis_frame_init(&diagnosis);
  diagnosis.primary_domain = ALK_CONTINUITY_DOMAIN_MIXED;
  diagnosis.confidence_ppm = 350000;
  diagnosis.allowed_action_mask = ALK_CONTINUITY_ACTION_OBSERVE;
  diagnosis.forbidden_action_mask = ALK_CONTINUITY_ACTION_NETWORK_LONG_TERM_MEMORY |
                                    ALK_CONTINUITY_ACTION_VIDEO_QUALITY_DESTRUCTIVE;

  const auto plan = stream_quality::plan_runtime_action_with_diagnosis(action, context, diagnosis);

  EXPECT_FALSE(plan.apply_bitrate);
  EXPECT_FALSE(plan.apply_fec);
  EXPECT_FALSE(plan.apply_fps);
  EXPECT_FALSE(plan.apply_profile);
  EXPECT_FALSE(plan.update_runtime_totals);
}

TEST(StreamQualityControllerTests, DiagnosisStillBlocksFpsDropsWithoutDomainEvidence) {
  stream_quality::action_t action {};
  action.changed = true;
  action.state = stream_quality::state_e::crisis;
  action.availability = stream_quality::availability_e::low;
  action.reason = stream_quality::reason_e::random_loss;
  action.scenario = stream_quality::scenario_e::random_loss;
  action.target_bitrate_kbps = 500;
  action.fec_percentage = 35;
  action.pacing_bitrate_kbps = 700;
  action.target_fps = 30;
  action.resolution_scale_percent = 100;

  const stream_quality::runtime_action_context_t context {
    .last_applied_bitrate_kbps = 11000,
    .last_applied_fec_percentage = 35,
    .last_applied_fps = 60,
    .last_applied_resolution_scale_percent = 100,
    .elapsed_ms_since_bitrate_apply = 2000,
    .elapsed_ms_since_fec_apply = 2000,
    .elapsed_ms_since_fps_apply = 2000,
    .elapsed_ms_since_profile_apply = 2000,
  };

  AlkContinuityDiagnosisFrame diagnosis;
  alk_continuity_diagnosis_frame_init(&diagnosis);
  diagnosis.primary_domain = ALK_CONTINUITY_DOMAIN_MIXED;
  diagnosis.confidence_ppm = 350000;
  diagnosis.allowed_action_mask = ALK_CONTINUITY_ACTION_OBSERVE;
  diagnosis.forbidden_action_mask = ALK_CONTINUITY_ACTION_NETWORK_LONG_TERM_MEMORY |
                                    ALK_CONTINUITY_ACTION_VIDEO_QUALITY_DESTRUCTIVE;

  const auto plan = stream_quality::plan_runtime_action_with_diagnosis(action, context, diagnosis);

  EXPECT_FALSE(plan.apply_fps);
}

TEST(StreamQualityControllerTests, RecoversTowardBaselineWithoutExceedingUserSpec) {
  stream_quality::controller_t controller;
  controller.configure({ .baseline_bitrate_kbps = 50000, .baseline_fec_percentage = 20 });

  controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 35,
    .recovered_frames = 5,
    .unrecoverable_frames = 20,
    .missing_packets = 400,
    .total_packets = 2400,
    .received_packets = 2000,
    .video_bytes = 5 * 1024 * 1024,
    .rtt_ms = 90,
    .rtt_variance_ms = 40,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 4 * 1024 * 1024,
      .rtt_ms = 45,
      .rtt_variance_ms = 5,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::recovering);
  EXPECT_TRUE(action.changed);
  EXPECT_LE(action.target_bitrate_kbps, 50000);
  EXPECT_LE(action.fec_percentage, stream_quality::controller_t::max_fec_percentage);
}

TEST(StreamQualityControllerTests, CapsHighConfiguredFecToSafeBudget) {
  stream_quality::controller_t controller;
  controller.configure({ .baseline_bitrate_kbps = 10000, .baseline_fec_percentage = 100 });

  EXPECT_LE(controller.current_fec_percentage(), stream_quality::controller_t::max_fec_percentage);
  EXPECT_LE(controller.current_fec_percentage(), 100);
}

TEST(StreamQualityControllerTests, RaisesFecTowardConfiguredMaximumOnlyWhenNeeded) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 10000,
    .baseline_fec_percentage = 20,
    .max_fec_percentage = 80,
  });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 40,
    .unrecoverable_frames = 20,
    .missing_packets = 400,
    .total_packets = 2400,
    .received_packets = 2000,
  });

  EXPECT_GT(action.fec_percentage, 20);
  EXPECT_LE(action.fec_percentage, stream_quality::controller_t::max_fec_percentage);
  EXPECT_LE(action.fec_percentage, 80);
}

TEST(StreamQualityControllerTests, AppliesFecOverheadOnceToPacingBudget) {
  stream_quality::controller_t controller;
  controller.configure({ .baseline_bitrate_kbps = 10000, .baseline_fec_percentage = 80 });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
  });

  EXPECT_EQ(action.target_bitrate_kbps, 10000);
  EXPECT_GT(controller.pacing_bitrate_kbps(), action.target_bitrate_kbps);
  EXPECT_GE(controller.pacing_bitrate_kbps(), 18000);
  EXPECT_LE(controller.pacing_bitrate_kbps(), 18800);
  EXPECT_GE(action.pacing_bitrate_kbps, 18000);
  EXPECT_LE(action.pacing_bitrate_kbps, 18800);
}


TEST(StreamQualityControllerTests, RuntimePlanAppliesBitrateBeforeFpsDrop) {
  stream_quality::action_t action {};
  action.changed = true;
  action.state = stream_quality::state_e::constrained;
  action.availability = stream_quality::availability_e::low;
  action.reason = stream_quality::reason_e::media_continuity;
  action.target_bitrate_kbps = 90000;
  action.fec_percentage = 10;
  action.pacing_bitrate_kbps = 99000;
  action.target_fps = 120;
  action.resolution_scale_percent = 100;
  action.actual_scale_percent = 100;
  action.chroma_sampling_type = -1;
  action.dynamic_range = -1;
  action.pressures.random_loss = 0.60;
  action.pressures.burst_loss = 0.35;
  action.pressures.delay_congestion = 0.25;
  action.pressures.render = 0.20;

  stream_quality::runtime_action_context_t context {};
  context.last_applied_bitrate_kbps = 126000;
  context.last_applied_fec_percentage = 10;
  context.last_applied_fps = 150;
  context.last_applied_resolution_scale_percent = 100;
  context.last_applied_chroma_sampling_type = -1;
  context.last_applied_dynamic_range = -1;
  context.elapsed_ms_since_bitrate_apply = 1000;
  context.elapsed_ms_since_fec_apply = 1000;
  context.elapsed_ms_since_fps_apply = 1000;
  context.elapsed_ms_since_profile_apply = 1000;

  const auto plan = stream_quality::plan_runtime_action(action, context);
  EXPECT_TRUE(plan.apply_bitrate);
  EXPECT_FALSE(plan.apply_fps) << "Sunshine must honour the SDK runtime plan: lower bitrate/quality first, FPS last";
  EXPECT_TRUE(plan.fps_deferred);
  EXPECT_EQ(plan.target_bitrate_kbps, 90000);
  EXPECT_EQ(plan.target_fps, 120);
}

TEST(StreamQualityControllerTests, VerifiedCapacityRescueAllowsTinyTransportLoss) {
  EXPECT_TRUE(stream_quality::verified_capacity_rescue_transport_clean(
    15000,
    5000,
    152,
    0));
  EXPECT_TRUE(stream_quality::verified_capacity_rescue_transport_clean(
    16000,
    6000,
    259,
    1));
  EXPECT_FALSE(stream_quality::verified_capacity_rescue_transport_clean(
    15000,
    5000,
    20000,
    0));
  EXPECT_FALSE(stream_quality::verified_capacity_rescue_transport_clean(
    15000,
    5000,
    152,
    8));
}

TEST(StreamQualityControllerTests, MediaRescueWithoutQualityFieldsIsIdrOnly) {
  EXPECT_FALSE(stream_quality::rescue_request_requests_quality_downgrade(
    ALK_RESCUE_TRIGGER_RFI_WAIT | ALK_RESCUE_TRIGGER_UNRECOVERABLE_FRAMES,
    0,
    0,
    100,
    false));

  EXPECT_TRUE(stream_quality::rescue_request_requests_quality_downgrade(
    ALK_RESCUE_TRIGGER_RFI_WAIT,
    18000,
    0,
    100,
    false));

  EXPECT_TRUE(stream_quality::rescue_request_requests_quality_downgrade(
    ALK_RESCUE_TRIGGER_CONTROL_STALLING,
    0,
    0,
    100,
    false));

  EXPECT_TRUE(stream_quality::rescue_request_requests_quality_downgrade(
    ALK_RESCUE_TRIGGER_RFI_WAIT,
    0,
    0,
    75,
    false));

  EXPECT_TRUE(stream_quality::rescue_request_requests_quality_downgrade(
    ALK_RESCUE_TRIGGER_RFI_WAIT,
    0,
    0,
    100,
    true));
}

TEST(StreamQualityControllerTests, MediaRescueQualityDowngradeRequiresHostVideoEvidence) {
  EXPECT_FALSE(stream_quality::media_rescue_quality_downgrade_allowed(
    ALK_RESCUE_TRIGGER_UNRECOVERABLE_FRAMES,
    true,
    0,
    0,
    0,
    0,
    0,
    0));

  EXPECT_FALSE(stream_quality::media_rescue_quality_downgrade_allowed(
    ALK_RESCUE_TRIGGER_RFI_WAIT,
    true,
    0,
    0,
    0,
    0,
    0,
    65000));

  EXPECT_TRUE(stream_quality::media_rescue_quality_downgrade_allowed(
    ALK_RESCUE_TRIGGER_RFI_WAIT,
    true,
    4,
    100,
    0,
    0,
    0,
    0));

  EXPECT_TRUE(stream_quality::media_rescue_quality_downgrade_allowed(
    ALK_RESCUE_TRIGGER_UNRECOVERABLE_FRAMES,
    true,
    0,
    0,
    1,
    0,
    0,
    0));

  EXPECT_TRUE(stream_quality::media_rescue_quality_downgrade_allowed(
    ALK_RESCUE_TRIGGER_CONTROL_STALLING,
    true,
    0,
    0,
    0,
    0,
    0,
    0));
}

TEST(StreamQualityControllerTests, VerifiedLanRecoveredMediaSampleIsNotNetworkConstrained) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 2,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 126000,
    .ceiling_total_bitrate_kbps = 148478,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 150,
    .frame_width = 3840,
    .frame_height = 2160,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 145567,
    .fps_needed_kbps = 126000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1080,
      .frames_seen = 126,
      .complete_frames = 126,
      .video_bytes = 11000000,
      .rtt_ms = 15,
      .rtt_variance_ms = 4,
      .displayed_frames = 126,
      .render_queue_depth = 2,
      .frame_area = 3840ULL * 2160ULL,
      .path_lan_direct = true,
      .path_identity_confident = true,
    });
  }

  action = controller.on_feedback({
    .duration_ms = 1124,
    .frames_seen = 127,
    .complete_frames = 122,
    .recovered_frames = 3,
    .unrecoverable_frames = 0,
    .missing_packets = 3,
    .total_packets = 45,
    .received_packets = 41,
    .video_bytes = 1433344,
    .rtt_ms = 15,
    .rtt_variance_ms = 5,
    .decode_queue_depth = 6,
    .render_queue_depth = 2,
    .displayed_frames = 128,
    .duplicate_frames = 9,
    .rfi_requests = 1,
    .local_display_pressure = 350,
    .frame_area = 3840ULL * 2160ULL,
    .path_lan_direct = true,
    .path_identity_confident = true,
  });

  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_NE(action.scenario, stream_quality::scenario_e::random_loss);
  EXPECT_EQ(action.link_quality_state, stream_quality::link_quality_state_e::verified_good);
  EXPECT_EQ(action.state, stream_quality::state_e::healthy);
  EXPECT_GE(action.target_bitrate_kbps, 126000);
  EXPECT_GE(action.target_fps, 150);
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, ReducesFpsBeforeCrushingBitrateWhenQueuesMissDeadline) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 12,
    .baseline_fps = 60,
    .min_fps = 24,
  });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 55,
    .missing_packets = 60,
    .total_packets = 2400,
    .received_packets = 2340,
    .rtt_ms = 55,
    .rtt_variance_ms = 70,
    .decode_queue_depth = 4,
    .render_queue_depth = 3,
    .late_frames = 12,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::constrained);
  EXPECT_LT(action.target_fps, 60);
  EXPECT_GE(action.target_fps, 24);
  EXPECT_GE(action.target_bitrate_kbps, 16000);
  EXPECT_LE(action.fec_percentage, stream_quality::controller_t::max_fec_percentage);
}

TEST(StreamQualityControllerTests, RecoversFpsAndBitrateAfterStableWindows) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 12,
    .baseline_fps = 60,
    .min_fps = 24,
  });

  controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 40,
    .unrecoverable_frames = 15,
    .missing_packets = 280,
    .total_packets = 2400,
    .received_packets = 2120,
    .rtt_ms = 90,
    .rtt_variance_ms = 100,
    .decode_queue_depth = 5,
    .render_queue_depth = 4,
    .late_frames = 18,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 10; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 25,
      .rtt_variance_ms = 3,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::recovering);
  EXPECT_LE(action.target_bitrate_kbps, 20000);
  EXPECT_LE(action.target_fps, 60);
  EXPECT_GT(action.target_fps, 24);
}

TEST(StreamQualityControllerTests, TreatsConfiguredBitrateAsCeilingAndStartsFromSafeWorkingPoint) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 24,
  });

  EXPECT_EQ(controller.current_bitrate_kbps(), 20000);

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 18,
    .rtt_variance_ms = 2,
    .decode_queue_depth = 0,
    .render_queue_depth = 0,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::recovering);
  EXPECT_GT(action.target_bitrate_kbps, 20000);
  EXPECT_LT(action.target_bitrate_kbps, 120000);
  EXPECT_LE(action.target_bitrate_kbps, 35000);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(StreamQualityControllerTests, HighRefresh4kStartupUsesSafeFirstFrameFloor) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 1000,
    .ceiling_total_bitrate_kbps = 132000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 120,
    .startup_fps = 120,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  EXPECT_GE(controller.current_bitrate_kbps(), 6000);
}

TEST(StreamQualityControllerTests, FirstIdrProtectionDoesNotCutStartupBitrateOnInitialLoss) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 6000,
    .ceiling_total_bitrate_kbps = 132000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 120,
    .startup_fps = 120,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 0,
    .recovered_frames = 0,
    .unrecoverable_frames = 60,
    .missing_packets = 1200,
    .total_packets = 2400,
    .received_packets = 1200,
    .rtt_ms = 42,
    .rtt_variance_ms = 18,
    .rfi_requests = 12,
    .waiting_for_rfi_frames = 60,
    .frame_area = 3840U * 2160U,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_GE(action.target_bitrate_kbps, 6000);
}

TEST(StreamQualityControllerTests, ContinuouslyProbesTowardCeilingWhenNetworkAndQueuesStayHealthy) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 24,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 16; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 18,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::recovering);
  EXPECT_GT(action.target_bitrate_kbps, 70000);
  EXPECT_LT(action.target_bitrate_kbps, 90000);
  EXPECT_LE(action.target_bitrate_kbps, 120000);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(StreamQualityControllerTests, LowBitrateCeilingDoesNotLockHighRefreshFpsWhenFeedbackIsHealthy) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 3250,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 3250,
    .baseline_fps = 120,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 1000,
    .received_packets = 1000,
    .rtt_ms = 18,
    .rtt_variance_ms = 2,
    .decode_queue_depth = 0,
    .render_queue_depth = 0,
    .late_frames = 0,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::healthy);
  EXPECT_EQ(action.target_bitrate_kbps, 3250);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(StreamQualityControllerTests, HighRefreshCrisisKeepsEmergencyInteractiveFloorAndRecoversGradually) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 35,
      .unrecoverable_frames = 20,
      .missing_packets = 400,
      .total_packets = 2400,
      .received_packets = 2000,
      .rtt_ms = 90,
      .rtt_variance_ms = 120,
      .decode_queue_depth = 6,
      .render_queue_depth = 5,
      .late_frames = 20,
      .input_queue_depth = 3,
      .input_send_latency_us = 90000,
      .input_ack_latency_us = 85000,
    });
  }

  const auto crisis_fps = action.target_fps;
  EXPECT_LE(crisis_fps, 90);
  EXPECT_GE(crisis_fps, 72);

  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 18,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .late_frames = 0,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::recovering);
  EXPECT_GT(action.target_fps, crisis_fps);
  EXPECT_LT(action.target_fps, crisis_fps + 20);
}

TEST(StreamQualityControllerTests, KeepsFpsStableForTransientNetworkLossWithoutRenderPressure) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 60000,
    .baseline_fps = 120,
    .min_fps = 24,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 56,
    .recovered_frames = 2,
    .unrecoverable_frames = 1,
    .missing_packets = 100,
    .total_packets = 2400,
    .received_packets = 2300,
    .rtt_ms = 45,
    .rtt_variance_ms = 42,
    .decode_queue_depth = 0,
    .render_queue_depth = 0,
    .late_frames = 0,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::constrained);
  EXPECT_LT(action.target_bitrate_kbps, 60000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, stream_quality::controller_t::max_fec_percentage);
}

TEST(StreamQualityControllerTests, PreservesBitrateForRenderPressureWithoutNetworkLoss) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  const stream_quality::feedback_t feedback {
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 18,
    .rtt_variance_ms = 3,
    .decode_queue_depth = 3,
    .render_queue_depth = 3,
    .late_frames = 8,
    .displayed_frames = 52,
  };
  controller.on_feedback(feedback);
  auto action = controller.on_feedback(feedback);

  EXPECT_EQ(action.state, stream_quality::state_e::constrained);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_LT(action.target_fps, 120);
  EXPECT_GE(action.target_fps, 110);
}

TEST(StreamQualityControllerTests, InputOnlyPressureDoesNotReconfigureVideoTransport) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 18,
    .rtt_variance_ms = 3,
    .decode_queue_depth = 0,
    .render_queue_depth = 0,
    .late_frames = 0,
    .displayed_frames = 60,
    .input_queue_depth = 10,
    .input_send_latency_us = 180000,
    .input_ack_latency_us = 160000,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::constrained);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(StreamQualityControllerTests, DoesNotRaiseFecForDelayOnlyCongestion) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 420,
    .rtt_variance_ms = 160,
    .decode_queue_depth = 2,
    .render_queue_depth = 2,
    .late_frames = 2,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_LT(action.target_bitrate_kbps, 20000);
}

TEST(StreamQualityControllerTests, DoesNotRequestIdrForDelayOnlyQueueingCrisis) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 620,
    .rtt_variance_ms = 180,
    .decode_queue_depth = 1,
    .render_queue_depth = 1,
    .late_frames = 1,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_FALSE(action.request_idr);
  EXPECT_LE(action.fec_percentage, 10);
}

TEST(StreamQualityControllerTests, ScalesFecLinearlyWithObservedRandomLoss) {
  stream_quality::controller_t mild_controller;
  mild_controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto mild_action = mild_controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 59,
    .missing_packets = 72,
    .total_packets = 2400,
    .received_packets = 2328,
    .rtt_ms = 42,
    .rtt_variance_ms = 14,
  });

  stream_quality::controller_t severe_controller;
  severe_controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto severe_action = severe_controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 53,
    .recovered_frames = 4,
    .unrecoverable_frames = 3,
    .missing_packets = 288,
    .total_packets = 2400,
    .received_packets = 2112,
    .rtt_ms = 58,
    .rtt_variance_ms = 22,
  });

  EXPECT_EQ(mild_action.fec_percentage, 10);
  EXPECT_GE(severe_action.fec_percentage, 30);
  EXPECT_LE(severe_action.fec_percentage, 30);
  EXPECT_GT(severe_action.fec_percentage, mild_action.fec_percentage);
}

TEST(StreamQualityControllerTests, AudioUnderrunsConstrainVideoWithoutRaisingFecOrIdr) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 24,
    .rtt_variance_ms = 4,
    .audio_underruns = 8,
  });

  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(StreamQualityControllerTests, AudioContinuityPressureConstrainVideoWithoutUnderruns) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 24,
    .rtt_variance_ms = 4,
    .audio_concealed_ms = 80,
    .late_audio_drops = 3,
    .audio_plc_ms = 20,
    .audio_fade_ms = 12,
    .audio_buffer_depth_ms = 0,
  });

  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(StreamQualityControllerTests, SustainedAudioUnderrunsApplyGentleLinearBitratePressure) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 10; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 24,
      .rtt_variance_ms = 4,
      .audio_underruns = 8,
    });
  }

  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(StreamQualityControllerTests, AudioOnlyPressureCannotCollapseVideoWhenNetworkIsClean) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 25,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 45; i++) {
    action = controller.on_feedback({
      .duration_ms = 1080,
      .frames_seen = 45,
      .complete_frames = 45,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 600 * 1024,
      .rtt_ms = 55,
      .rtt_variance_ms = 25,
      .audio_underruns = 102,
      .audio_concealed_ms = 1122,
      .audio_fade_ms = 816,
    });
  }

  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_GE(action.target_bitrate_kbps, 15300);
}

TEST(StreamQualityControllerTests, AudioOnlyPressureDoesNotBlockVisualRecoveryAfterLoss) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12829,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .frame_width = 3024,
    .frame_height = 1964,
    .chroma_sampling_type = 0,
    .dynamic_range = 0,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 24,
      .recovered_frames = 8,
      .unrecoverable_frames = 10,
      .missing_packets = 420,
      .total_packets = 1800,
      .received_packets = 1380,
      .video_bytes = 520 * 1024,
      .rtt_ms = 58,
      .rtt_variance_ms = 18,
      .displayed_frames = 24,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
      .rfi_requests = 12,
    });
  }
  ASSERT_EQ(action.state, stream_quality::state_e::crisis);
  ASSERT_LT(action.target_bitrate_kbps, 10000);
  ASSERT_GE(action.resolution_scale_percent, 90);

  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 1080,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1500 * 1024,
      .rtt_ms = 42,
      .rtt_variance_ms = 12,
      .displayed_frames = 60,
      .audio_underruns = 8,
      .audio_concealed_ms = 88,
      .audio_fade_ms = 64,
      .frame_area = 3024U * 1964U,
      .dirty_area = 0,
    });
  }

  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_GE(action.target_bitrate_kbps, 5000);
  EXPECT_GE(action.target_fps, 78);
  EXPECT_GE(action.resolution_scale_percent, 90);
}

TEST(StreamQualityControllerTests, RaisesFecForRandomLossWithAdaptiveHeadroom) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 56,
    .recovered_frames = 3,
    .unrecoverable_frames = 1,
    .missing_packets = 96,
    .total_packets = 2400,
    .received_packets = 2304,
    .rtt_ms = 50,
    .rtt_variance_ms = 18,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::constrained);
  EXPECT_GT(action.fec_percentage, 10);
  EXPECT_LE(action.fec_percentage, 30);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(StreamQualityControllerTests, HighRefreshDefaultUsesSixtyFpsEmergencyFloorAndRecovers) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 5000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 5000,
    .baseline_fps = 120,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 35,
      .unrecoverable_frames = 20,
      .missing_packets = 400,
      .total_packets = 2400,
      .received_packets = 2000,
      .rtt_ms = 90,
      .rtt_variance_ms = 120,
      .decode_queue_depth = 6,
      .render_queue_depth = 5,
      .late_frames = 20,
    });
  }

  EXPECT_LE(action.target_fps, 90);
  EXPECT_GE(action.target_fps, 60);

  const auto crisis_fps = action.target_fps;
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 18,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .late_frames = 0,
    });
  }

  EXPECT_GT(action.target_fps, crisis_fps);
  EXPECT_LE(action.target_fps, 120);
}

TEST(StreamQualityControllerTests, RecoveredOnlyRandomLossRaisesFecWithoutCrushingInteractiveVideo) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 22000,
    .baseline_fps = 120,
    .startup_fps = 120,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 20,
      .unrecoverable_frames = 0,
      .missing_packets = 240,
      .total_packets = 2400,
      .received_packets = 2160,
      .video_bytes = 1050 * 1024,
      .rtt_ms = 42,
      .rtt_variance_ms = 18,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .late_frames = 0,
      .input_queue_depth = 2,
      .input_send_latency_us = 24000,
      .input_ack_latency_us = 22000,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::constrained);
  EXPECT_GE(action.fec_percentage, 35);
  EXPECT_GE(action.target_bitrate_kbps, 12000);
  EXPECT_GE(action.target_fps, 90);
  EXPECT_GT(action.fec_budget_kbps, 0);
  EXPECT_GT(action.encoding_budget_kbps, action.fec_budget_kbps);
  EXPECT_GT(action.recovered_loss, 0.20);
  EXPECT_EQ(action.unrecoverable_loss, 0.0);
}

TEST(StreamQualityControllerTests, SustainedUnrecoverableBurstsCanOpenFecTowardFullParity) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 50000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 50000,
    .ceiling_total_bitrate_kbps = 100000,
    .baseline_fps = 120,
    .startup_fps = 120,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 24,
      .recovered_frames = 24,
      .unrecoverable_frames = 12,
      .missing_packets = 480,
      .total_packets = 2400,
      .received_packets = 1920,
      .video_bytes = 2500 * 1024,
      .rtt_ms = 58,
      .rtt_variance_ms = 20,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .late_frames = 0,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_EQ(action.reason, stream_quality::reason_e::media_continuity);
  EXPECT_GE(action.fec_percentage, 80);
  EXPECT_LE(action.fec_percentage, 100);
  EXPECT_GE(action.target_fps, 72);
  EXPECT_GE(action.target_bitrate_kbps, 9000);
  EXPECT_GT(action.fec_efficiency, 0.0);
}

TEST(StreamQualityControllerTests, RecoveryDoesNotExceedTotalCeilingWhileFecIsElevated) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 24,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 35,
      .unrecoverable_frames = 20,
      .missing_packets = 400,
      .total_packets = 2400,
      .received_packets = 2000,
      .rtt_ms = 90,
      .rtt_variance_ms = 120,
      .decode_queue_depth = 6,
      .render_queue_depth = 5,
      .late_frames = 20,
    });
  }

  EXPECT_LE(action.fec_percentage, 35);
  EXPECT_GE(action.fec_percentage, 20);

  for (int i = 0; i < 20; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 18,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .late_frames = 0,
    });

    EXPECT_LE(action.pacing_bitrate_kbps, 20000);
  }

  EXPECT_LE(controller.pacing_bitrate_kbps(), 20000);
  EXPECT_LE(action.target_bitrate_kbps, 18000);
}

TEST(StreamQualityControllerTests, ManualHighCeilingRecoversTowardSustainableEstimateAfterCongestion) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 1000000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 80,
    .startup_bitrate_kbps = 30000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 2500 * 1024,
      .rtt_ms = 18,
      .rtt_variance_ms = 2,
    });
  }

  EXPECT_GT(action.target_bitrate_kbps, 30000);
  EXPECT_EQ(action.requested_ceiling_kbps, 1000000);

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 45,
    .recovered_frames = 5,
    .unrecoverable_frames = 10,
    .missing_packets = 360,
    .total_packets = 2400,
    .received_packets = 2040,
    .video_bytes = 2600 * 1024,
    .rtt_ms = 260,
    .rtt_variance_ms = 150,
  });

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_EQ(action.reason, stream_quality::reason_e::media_continuity);

  for (int i = 0; i < 18; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3200 * 1024,
      .rtt_ms = 22,
      .rtt_variance_ms = 3,
    });

    EXPECT_LE(action.target_bitrate_kbps, action.effective_ceiling_kbps);
  }

  EXPECT_EQ(action.requested_ceiling_kbps, 1000000);
  EXPECT_GT(action.sustainable_estimate_kbps, 40000);
  EXPECT_LT(action.effective_ceiling_kbps, 1000000);
  EXPECT_LE(action.target_bitrate_kbps, 85000);
  EXPECT_EQ(action.reason, stream_quality::reason_e::recovering);
}

TEST(StreamQualityControllerTests, FullFrameMotionPressurePrefersScaleBeforePptFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 1,
    .dynamic_range = 0,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 56,
      .recovered_frames = 10,
      .unrecoverable_frames = 2,
      .missing_packets = 220,
      .total_packets = 2400,
      .received_packets = 2180,
      .video_bytes = 750 * 1024,
      .rtt_ms = 52,
      .rtt_variance_ms = 20,
      .late_frames = 1,
      .displayed_frames = 58,
      .input_queue_depth = 2,
      .input_send_latency_us = 26000,
      .input_ack_latency_us = 23000,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::media_continuity);
  EXPECT_GE(action.pressures.motion, 0.85);
  EXPECT_GE(action.target_fps, 90);
  EXPECT_GE(action.resolution_scale_percent, 90);
  EXPECT_EQ(action.chroma_sampling_type, 0);
  EXPECT_TRUE(action.profile_tier_changed);
  EXPECT_TRUE(action.profile_tier_deferred);
  EXPECT_FALSE(action.profile_tier_supported);
}

TEST(StreamQualityControllerTests, ProfileTierFallbackDoesNotForceDestructiveActionWhenRuntimeScaleCannotApply) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .dynamic_range = 0,
    .runtime_profile_tier_supported = false,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 5; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 720 * 1024,
      .rtt_ms = 35,
      .rtt_variance_ms = 8,
      .displayed_frames = 60,
      .input_queue_depth = 2,
      .input_send_latency_us = 18000,
      .input_ack_latency_us = 17000,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_NE(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_FALSE(action.profile_tier_changed);
  EXPECT_FALSE(action.profile_tier_deferred);
  EXPECT_FALSE(action.profile_tier_supported);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
}

TEST(StreamQualityControllerTests, DelayOnlyCongestionReducesPacingWithoutOpeningFullFec) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 20000,
    .ceiling_total_bitrate_kbps = 25000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 900 * 1024,
    .rtt_ms = 480,
    .rtt_variance_ms = 170,
    .late_frames = 1,
    .displayed_frames = 59,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_GE(action.pressures.delay_congestion, 0.80);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LT(action.pacing_bitrate_kbps, 25000);
  EXPECT_GE(action.target_fps, 60);
}


TEST(StreamQualityControllerTests, QueueBackpressureWithJitterCapsTotalAndDoesNotRaiseFec) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 116000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 80000,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 58,
      .recovered_frames = 2,
      .unrecoverable_frames = 0,
      .missing_packets = 12,
      .total_packets = 2400,
      .received_packets = 2388,
      .video_bytes = 1200 * 1024,
      .rtt_ms = 165,
      .rtt_variance_ms = 72,
      .decode_queue_depth = 9,
      .render_queue_depth = 4,
      .late_frames = 5,
      .displayed_frames = 52,
      .input_queue_depth = 2,
      .input_send_latency_us = 36000,
      .input_ack_latency_us = 34000,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
    });
  }

  const int total_kbps = action.target_bitrate_kbps * (100 + action.fec_percentage) / 100;
  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LE(total_kbps, 13500);
  EXPECT_LE(action.pacing_bitrate_kbps, 14000);
  EXPECT_LT(action.target_bitrate_kbps, 18000);
}

TEST(StreamQualityControllerTests, RenderOnlyBackpressureDoesNotMasqueradeAsNetworkDelayCongestion) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 116000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 80000,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1200 * 1024,
      .rtt_ms = 32,
      .rtt_variance_ms = 5,
      .decode_queue_depth = 9,
      .render_queue_depth = 5,
      .late_frames = 8,
      .displayed_frames = 44,
      .input_queue_depth = 0,
      .input_send_latency_us = 5000,
      .input_ack_latency_us = 5000,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_NE(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_GE(action.pacing_bitrate_kbps, 15000);
}

TEST(StreamQualityControllerTests, RfiStormIsCooledDownAndForcesCrisisTier) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 50000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 50000,
    .ceiling_total_bitrate_kbps = 100000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  auto first = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 30,
    .recovered_frames = 6,
    .unrecoverable_frames = 24,
    .missing_packets = 640,
    .total_packets = 2400,
    .received_packets = 1760,
    .rtt_ms = 64,
    .rtt_variance_ms = 24,
    .rfi_requests = 42,
    .waiting_for_rfi_frames = 60,
  });

  auto second = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 30,
    .recovered_frames = 6,
    .unrecoverable_frames = 24,
    .missing_packets = 640,
    .total_packets = 2400,
    .received_packets = 1760,
    .rtt_ms = 64,
    .rtt_variance_ms = 24,
    .rfi_requests = 42,
    .waiting_for_rfi_frames = 60,
  });

  EXPECT_EQ(first.state, stream_quality::state_e::crisis);
  EXPECT_TRUE(first.request_idr);
  EXPECT_EQ(second.state, stream_quality::state_e::crisis);
  EXPECT_FALSE(second.request_idr);
  EXPECT_GE(second.pressures.burst_loss, 0.95);
  EXPECT_GE(second.resolution_scale_percent, 90);
}

TEST(StreamQualityControllerTests, IsolatedRfiWithoutLossDoesNotTriggerRandomLossCrisis) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 7500,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .dynamic_range = 0,
  });

  auto action = controller.on_feedback({
    .duration_ms = 1100,
    .frames_seen = 1,
    .complete_frames = 1,
    .recovered_frames = 0,
    .unrecoverable_frames = 0,
    .missing_packets = 0,
    .total_packets = 0,
    .received_packets = 0,
    .video_bytes = 0,
    .rtt_ms = 53,
    .rtt_variance_ms = 17,
    .late_frames = 0,
    .displayed_frames = 1,
    .frame_area = 3840U * 2160U,
    .dirty_area = 0,
    .full_frame_dirty = false,
    .rfi_requests = 1,
    .waiting_for_rfi_frames = 0,
  });

  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_FALSE(action.request_idr);
  EXPECT_LT(action.pressures.burst_loss, 0.50);
  EXPECT_GE(action.target_bitrate_kbps, 7500);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(StreamQualityControllerTests, StaticIdleWithoutVideoSamplesDoesNotReduceForRttJitter) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18182,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .dynamic_range = 0,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 1080,
      .frames_seen = 0,
      .complete_frames = 0,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 0,
      .rtt_ms = 106,
      .rtt_variance_ms = 96,
      .late_frames = 0,
      .displayed_frames = 0,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .input_queue_depth = 0,
      .input_send_latency_us = 0,
      .input_ack_latency_us = 0,
      .frame_area = 3840U * 2160U,
      .dirty_area = 0,
      .full_frame_dirty = false,
      .rfi_requests = 0,
      .waiting_for_rfi_frames = 0,
    });
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_NE(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_FALSE(action.request_idr);
  EXPECT_EQ(action.target_bitrate_kbps, 18182);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_EQ(action.pressures.delay_congestion, 0.0);
  EXPECT_EQ(action.pressures.motion, 0.0);
  EXPECT_EQ(action.pressures.render, 0.0);
}

TEST(StreamQualityControllerTests, RecoveredOnlyLossDoesNotConsumeHalfOfManualCeilingWithFec) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12410,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 23,
    .complete_frames = 1,
    .recovered_frames = 22,
    .unrecoverable_frames = 0,
    .missing_packets = 42,
    .total_packets = 211,
    .received_packets = 169,
    .video_bytes = 650 * 1024,
    .rtt_ms = 36,
    .rtt_variance_ms = 5,
    .displayed_frames = 1,
    .frame_area = 3024U * 1900U,
    .dirty_area = 1375920,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_EQ(action.unrecoverable_loss, 0.0);
  EXPECT_LE(action.fec_percentage, 60);
  EXPECT_GE(action.target_bitrate_kbps, 11000);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(StreamQualityControllerTests, ManualHighCeilingTreatsRecoveredLossAsFecSignalNotProbePermission) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 136364,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12410,
    .ceiling_total_bitrate_kbps = 150000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 40,
    .recovered_frames = 20,
    .unrecoverable_frames = 0,
    .missing_packets = 240,
    .total_packets = 2400,
    .received_packets = 2160,
    .video_bytes = 1200 * 1024,
    .rtt_ms = 36,
    .rtt_variance_ms = 5,
    .displayed_frames = 40,
    .frame_area = 3024U * 1900U,
    .dirty_area = 1375920,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_EQ(action.unrecoverable_loss, 0.0);
  EXPECT_LE(action.fec_percentage, 60);
  EXPECT_LE(action.target_bitrate_kbps, 20000);
  EXPECT_LE(action.pacing_bitrate_kbps, 32000);
}

TEST(StreamQualityControllerTests, FullFrameMotionDoesNotCollapseToUnreadableBitrateWhenProfileFallbackIsDeferred) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12829,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .frame_width = 3024,
    .frame_height = 1964,
    .chroma_sampling_type = 0,
    .dynamic_range = 0,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 16; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 58,
      .recovered_frames = 2,
      .unrecoverable_frames = 0,
      .missing_packets = 12,
      .total_packets = 120,
      .received_packets = 108,
      .video_bytes = 430 * 1024,
      .rtt_ms = i < 10 ? 72U : 180U,
      .rtt_variance_ms = i < 10 ? 32U : 74U,
      .late_frames = 1,
      .displayed_frames = 58,
      .input_queue_depth = 2,
      .input_send_latency_us = 32000,
      .input_ack_latency_us = 28000,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.unrecoverable_loss, 0.0);
  EXPECT_GE(action.target_fps, 72);
  EXPECT_GE(action.target_bitrate_kbps, 7200);
  EXPECT_LE(action.fec_percentage, 40);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(StreamQualityControllerTests, DelayOnlyCongestionBleedsOffElevatedFecBeforeCrushingEncodingBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12829,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .frame_width = 3024,
    .frame_height = 1964,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 40,
    .recovered_frames = 20,
    .unrecoverable_frames = 0,
    .missing_packets = 240,
    .total_packets = 2400,
    .received_packets = 2160,
    .video_bytes = 900 * 1024,
    .rtt_ms = 42,
    .rtt_variance_ms = 18,
    .displayed_frames = 40,
    .frame_area = 3024U * 1964U,
    .dirty_area = 3024U * 1964U,
    .full_frame_dirty = true,
  });
  ASSERT_GT(action.fec_percentage, 10);

  for (int i = 0; i < 4; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 420 * 1024,
      .rtt_ms = 360,
      .rtt_variance_ms = 120,
      .late_frames = 1,
      .displayed_frames = 59,
      .input_queue_depth = 1,
      .input_send_latency_us = 38000,
      .input_ack_latency_us = 30000,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_LT(action.fec_percentage, 35);
  EXPECT_GE(action.target_bitrate_kbps, 7200);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(StreamQualityControllerTests, TotalCeilingBoundsEncodingAndFecForAnyConfiguredBitrate) {
  const int ceilings[] = { 1000, 5000, 20000, 100000, 1000000 };

  for (const auto ceiling_kbps : ceilings) {
    stream_quality::controller_t controller;
    controller.configure({
      .baseline_bitrate_kbps = ceiling_kbps,
      .baseline_fec_percentage = 10,
      .max_fec_percentage = 100,
      .startup_bitrate_kbps = std::max(500, ceiling_kbps / 3),
      .ceiling_total_bitrate_kbps = ceiling_kbps,
      .baseline_fps = 120,
      .startup_fps = 120,
      .min_fps = 60,
      .frame_width = 3840,
      .frame_height = 2160,
    });

    stream_quality::action_t action {};
    for (int i = 0; i < 6; i++) {
      action = controller.on_feedback({
        .duration_ms = 500,
        .frames_seen = 60,
        .complete_frames = 10,
        .recovered_frames = 0,
        .unrecoverable_frames = 50,
        .missing_packets = 1200,
        .total_packets = 2400,
        .received_packets = 1200,
        .video_bytes = 700 * 1024,
        .rtt_ms = 180,
        .rtt_variance_ms = 90,
        .rfi_requests = 40,
        .waiting_for_rfi_frames = 60,
      });
    }

    EXPECT_LE(action.encoding_budget_kbps + action.fec_budget_kbps, ceiling_kbps)
      << "ceiling=" << ceiling_kbps;
    EXPECT_LE(action.pacing_bitrate_kbps, ceiling_kbps)
      << "ceiling=" << ceiling_kbps;
    EXPECT_LE(action.target_bitrate_kbps, action.effective_ceiling_kbps)
      << "ceiling=" << ceiling_kbps;
  }
}

TEST(StreamQualityControllerTests, IneffectiveLossRecoveryReducesFecInsteadOfBlindlyOpeningParity) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 16000,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 8,
      .recovered_frames = 0,
      .unrecoverable_frames = 52,
      .missing_packets = 1400,
      .total_packets = 2400,
      .received_packets = 1000,
      .video_bytes = 900 * 1024,
      .rtt_ms = 64,
      .rtt_variance_ms = 18,
      .rfi_requests = 48,
      .waiting_for_rfi_frames = 60,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_EQ(action.reason, stream_quality::reason_e::media_continuity);
  EXPECT_EQ(action.fec_efficiency, 0.0);
  EXPECT_LE(action.fec_percentage, 25);
  EXPECT_LT(action.target_bitrate_kbps, 12000);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(StreamQualityControllerTests, BandwidthLimitedLinkCutsTotalBudgetRatherThanAddingFec) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 54545,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 50000,
    .ceiling_total_bitrate_kbps = 60000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .recovered_frames = 18,
    .unrecoverable_frames = 0,
    .missing_packets = 220,
    .total_packets = 2400,
    .received_packets = 2180,
    .video_bytes = 2500 * 1024,
    .rtt_ms = 38,
    .rtt_variance_ms = 8,
  });
  ASSERT_GT(action.fec_percentage, 10);

  for (int i = 0; i < 4; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 5000 * 1024,
      .rtt_ms = 420,
      .rtt_variance_ms = 160,
      .late_frames = 2,
      .displayed_frames = 58,
      .input_queue_depth = 2,
      .input_send_latency_us = 95000,
      .input_ack_latency_us = 90000,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_LT(action.fec_percentage, 30);
  EXPECT_LT(action.target_bitrate_kbps, 50000);
  EXPECT_LE(action.pacing_bitrate_kbps, 60000);
}

TEST(StreamQualityControllerTests, CongestionAntiSpiralFiresWhenElevatedFecCannotFitSustainableBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 54545,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 50000,
    .ceiling_total_bitrate_kbps = 60000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  bool anti_spiral_fired = false;
  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 75,
      .complete_frames = 0,
      .recovered_frames = 2,
      .unrecoverable_frames = 56,
      .missing_packets = 700,
      .total_packets = 1700,
      .received_packets = 1000,
      .video_bytes = 360 * 1024,
      .rtt_ms = 24,
      .rtt_variance_ms = 5,
      .displayed_frames = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .rfi_requests = 3,
      .waiting_for_rfi_frames = 60,
    });
    anti_spiral_fired = anti_spiral_fired || action.congestion_anti_spiral;
  }

  EXPECT_TRUE(anti_spiral_fired)
    << "When elevated FEC keeps total send far above the measured sustainable point, "
       "the controller must bleed FEC instead of re-opening parity every window";
  EXPECT_LE(action.fec_percentage, 25);
  EXPECT_LE(action.pacing_bitrate_kbps, 16000);
}

TEST(StreamQualityControllerTests, RemoteHighMotionWithPersistentTransportLossPrefersStableCadence) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 2,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 35000,
    .ceiling_total_bitrate_kbps = 148478,
    .baseline_fps = 150,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 145567,
    .fps_needed_kbps = 126000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 90,
      .video_bytes = 6500 * 1024,
      .transport_packet_loss_ppm = 44860,
      .rtt_ms = 45,
      .rtt_variance_ms = 10,
      .displayed_frames = 104,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .local_display_pressure = 180,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .user_input_active = true,
      .path_identity_confident = true,
    });
  }

  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 72,
      .recovered_frames = static_cast<std::uint32_t>(i % 2 == 0 ? 4 : 3),
      .unrecoverable_frames = 0,
      .missing_packets = static_cast<std::uint32_t>(i % 2 == 0 ? 36 : 30),
      .total_packets = static_cast<std::uint32_t>(i % 2 == 0 ? 760 : 650),
      .received_packets = static_cast<std::uint32_t>(i % 2 == 0 ? 724 : 620),
      .video_bytes = 6500 * 1024,
      .transport_packet_loss_ppm = 44860,
      .rtt_ms = 52,
      .rtt_variance_ms = 8,
      .displayed_frames = 106,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .local_display_pressure = 180,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .user_input_active = true,
      .path_identity_confident = true,
    });
  }

  for (int i = 0; i < 6; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 90,
      .video_bytes = 6500 * 1024,
      .transport_packet_loss_ppm = 44860,
      .rtt_ms = 45,
      .rtt_variance_ms = 10,
      .displayed_frames = 104,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .local_display_pressure = 180,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .user_input_active = true,
      .path_identity_confident = true,
    });
  }

  EXPECT_LE(action.target_fps, 72);
  EXPECT_GE(action.target_bitrate_kbps, 24000);
  EXPECT_LE(action.fec_percentage, 18);
  EXPECT_LE(action.pacing_bitrate_kbps, 45000);
  EXPECT_EQ(action.resolution_scale_percent, 100);
}

TEST(StreamQualityControllerTests, GoodNetworkRecoversTowardCeilingWithoutQualityPenalty) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 181818,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 30000,
    .ceiling_total_bitrate_kbps = 200000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 1,
    .dynamic_range = 0,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 18; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 4500 * 1024,
      .rtt_ms = 12,
      .rtt_variance_ms = 2,
      .late_frames = 0,
      .displayed_frames = 60,
      .frame_area = 3840U * 2160U,
      .dirty_area = 0,
      .full_frame_dirty = false,
    });
  }

  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_EQ(action.chroma_sampling_type, 1);
  EXPECT_GT(action.target_bitrate_kbps, 70000);
  EXPECT_LE(action.pacing_bitrate_kbps, 200000);
}

TEST(StreamQualityControllerTests, MotionReadableFloorCannotOverrideWeakRouteSustainableLimit) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 113000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 150000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 20,
      .recovered_frames = 20,
      .unrecoverable_frames = 20,
      .missing_packets = 720,
      .total_packets = 2400,
      .received_packets = 1680,
      .video_bytes = 600 * 1024,
      .rtt_ms = 62,
      .rtt_variance_ms = 22,
      .displayed_frames = 20,
      .input_queue_depth = 2,
      .input_send_latency_us = 28000,
      .input_ack_latency_us = 26000,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
      .rfi_requests = 16,
      .waiting_for_rfi_frames = 40,
    });
  }
  ASSERT_EQ(action.state, stream_quality::state_e::crisis);
  ASSERT_TRUE(action.sustainable_estimate_kbps > 0);

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 58,
    .recovered_frames = 2,
    .unrecoverable_frames = 0,
    .missing_packets = 12,
    .total_packets = 2400,
    .received_packets = 2388,
    .video_bytes = 780 * 1024,
    .rtt_ms = 36,
    .rtt_variance_ms = 5,
    .displayed_frames = 58,
    .input_queue_depth = 2,
    .input_send_latency_us = 24000,
    .input_ack_latency_us = 22000,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
  });

  EXPECT_EQ(action.unrecoverable_loss, 0.0);
  EXPECT_LE(action.target_bitrate_kbps, action.sustainable_estimate_kbps + 8000);
  EXPECT_LE(action.effective_ceiling_kbps, action.sustainable_estimate_kbps + 10000);
  EXPECT_LT(action.target_bitrate_kbps, 30000);
  EXPECT_LT(action.pacing_bitrate_kbps, 60000);
}

TEST(StreamQualityControllerTests, SustainedRfiAudioCrisisUsesTightSustainableBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 113000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 150000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 10; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 24,
      .recovered_frames = 32,
      .unrecoverable_frames = 8,
      .missing_packets = 300,
      .total_packets = 900,
      .received_packets = 600,
      .video_bytes = 420 * 1024,
      .rtt_ms = 55,
      .rtt_variance_ms = 20,
      .audio_underruns = 100,
      .displayed_frames = 24,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3686400,
      .rfi_requests = 40,
      .waiting_for_rfi_frames = 40,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_TRUE(action.rfi_limited);
  EXPECT_GE(action.pressures.audio, 0.75);
  EXPECT_LE(action.fec_percentage, 70);
  EXPECT_LE(action.pacing_bitrate_kbps, 10000);
  EXPECT_LE(action.effective_ceiling_kbps, action.sustainable_estimate_kbps + 3000);
  EXPECT_GE(action.target_fps, 60);
}

TEST(StreamQualityControllerTests, StartupAudioUnderrunsWithoutVideoSamplesDoNotProbeToCeiling) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 113000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12410,
    .ceiling_total_bitrate_kbps = 113000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; i++) {
    action = controller.on_feedback({
      .duration_ms = 1070,
      .frames_seen = 0,
      .complete_frames = 0,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 0,
      .rtt_ms = 53,
      .rtt_variance_ms = 11,
      .audio_underruns = 100,
      .displayed_frames = 0,
      .frame_area = 3024U * 1900U,
      .dirty_area = 0,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::audio_pressure);
  EXPECT_EQ(action.packet_loss, 0.0);
  EXPECT_LE(action.target_bitrate_kbps, 15000);
  EXPECT_LE(action.pacing_bitrate_kbps, 18000);
  EXPECT_LT(action.target_bitrate_kbps, action.requested_ceiling_kbps / 2);
}

TEST(StreamQualityControllerTests, StrongCleanNetworkCanExceedUserQualityBudgetToProtectHighRefreshFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 5000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 5000,
    .ceiling_total_bitrate_kbps = 130000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 5000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 20000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1200 * 1024,
      .rtt_ms = 12,
      .rtt_variance_ms = 2,
      .late_frames = 0,
      .displayed_frames = 60,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.target_fps, 120);
  EXPECT_GT(action.target_bitrate_kbps, 5000);
  EXPECT_LE(action.target_bitrate_kbps, 20000);
  EXPECT_LE(action.pacing_bitrate_kbps, 130000);
  EXPECT_LE(action.fec_percentage, 10);
}

TEST(StreamQualityControllerTests, CongestionImmediatelyReturnsOvershootTowardUserQualityBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 5000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 5000,
    .ceiling_total_bitrate_kbps = 130000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .user_quality_kbps = 5000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 20000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1200 * 1024,
      .rtt_ms = 12,
      .rtt_variance_ms = 2,
      .displayed_frames = 60,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }
  ASSERT_GT(action.target_bitrate_kbps, 5000);

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .missing_packets = 0,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 2200 * 1024,
    .rtt_ms = 420,
    .rtt_variance_ms = 160,
    .late_frames = 4,
    .displayed_frames = 56,
    .frame_area = 3840U * 2160U,
    .dirty_area = 3840U * 2160U,
    .full_frame_dirty = true,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_LE(action.target_bitrate_kbps, 6500);
  EXPECT_LE(action.fec_percentage, 10);
}

TEST(StreamQualityControllerTests, RfiRecoveryRampsMediaParametersWithoutSawtoothJumps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12410,
    .ceiling_total_bitrate_kbps = 26000,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 1,
    .user_quality_kbps = 20000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 20000,
  });

  auto clean_probe = controller.on_feedback({
    .duration_ms = 540,
    .frames_seen = 60,
    .complete_frames = 60,
    .recovered_frames = 5,
    .unrecoverable_frames = 0,
    .missing_packets = 48,
    .total_packets = 640,
    .received_packets = 592,
    .video_bytes = 630 * 1024,
    .rtt_ms = 33,
    .rtt_variance_ms = 4,
    .displayed_frames = 60,
    .input_queue_depth = 4,
    .input_send_latency_us = 5000,
    .input_ack_latency_us = 5000,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
  });

  auto first_crisis = controller.on_feedback({
    .duration_ms = 550,
    .frames_seen = 60,
    .complete_frames = 17,
    .recovered_frames = 6,
    .unrecoverable_frames = 39,
    .missing_packets = 209,
    .total_packets = 807,
    .received_packets = 486,
    .video_bytes = 342 * 1024,
    .rtt_ms = 30,
    .rtt_variance_ms = 4,
    .displayed_frames = 17,
    .input_queue_depth = 6,
    .input_send_latency_us = 13000,
    .input_ack_latency_us = 13000,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
    .rfi_requests = 47,
    .waiting_for_rfi_frames = 47,
  });

  auto second_crisis = controller.on_feedback({
    .duration_ms = 540,
    .frames_seen = 60,
    .complete_frames = 60,
    .recovered_frames = 18,
    .unrecoverable_frames = 5,
    .missing_packets = 51,
    .total_packets = 372,
    .received_packets = 321,
    .video_bytes = 425 * 1024,
    .rtt_ms = 34,
    .rtt_variance_ms = 4,
    .displayed_frames = 58,
    .input_queue_depth = 3,
    .input_send_latency_us = 8000,
    .input_ack_latency_us = 8000,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
    .rfi_requests = 8,
    .waiting_for_rfi_frames = 8,
  });

  auto recovery_probe = controller.on_feedback({
    .duration_ms = 540,
    .frames_seen = 60,
    .complete_frames = 62,
    .recovered_frames = 35,
    .unrecoverable_frames = 0,
    .missing_packets = 48,
    .total_packets = 443,
    .received_packets = 395,
    .video_bytes = 321 * 1024,
    .rtt_ms = 31,
    .rtt_variance_ms = 4,
    .displayed_frames = 62,
    .input_queue_depth = 4,
    .input_send_latency_us = 8000,
    .input_ack_latency_us = 8000,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
  });

  EXPECT_EQ(first_crisis.state, stream_quality::state_e::crisis);
  EXPECT_GE(first_crisis.unrecoverable_loss, 0.5);
  EXPECT_EQ(second_crisis.state, stream_quality::state_e::crisis);
  EXPECT_LE(recovery_probe.target_bitrate_kbps,
            second_crisis.target_bitrate_kbps + std::max(3600, second_crisis.target_bitrate_kbps / 2));
  EXPECT_LE(std::abs(recovery_probe.fec_percentage - second_crisis.fec_percentage), 8);
  EXPECT_LE(std::abs(recovery_probe.target_fps - second_crisis.target_fps), 8);
  EXPECT_LE(recovery_probe.pacing_bitrate_kbps,
            second_crisis.pacing_bitrate_kbps + std::max(5200, second_crisis.pacing_bitrate_kbps / 2));
  EXPECT_GT(clean_probe.target_bitrate_kbps, first_crisis.target_bitrate_kbps);
}

TEST(StreamQualityControllerTests, DelayOnlyCongestionCapsTotalNearUserBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 30000,
    .min_bitrate_kbps = 1500,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 24000,
  });

  stream_quality::action_t action;
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 90,
      .rtt_ms = 320,
      .rtt_variance_ms = 150,
      .displayed_frames = 72,
      .video_bytes = 1800 * 1024,
      .input_ack_latency_us = 180000,
      .frame_area = 3024ULL * 1900ULL,
      .dirty_area = 3024ULL * 1900ULL,
      .full_frame_dirty = true,
    });
  }

  const int total_kbps = action.target_bitrate_kbps * (100 + action.fec_percentage) / 100;
  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_LE(total_kbps, 13000);
  EXPECT_LE(action.pacing_bitrate_kbps, 13500);
}

TEST(StreamQualityControllerTests, RandomRecoveredLossCanRaiseFecWithoutDelayOvershoot) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 30000,
    .min_bitrate_kbps = 1500,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 24000,
  });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 90,
    .complete_frames = 70,
    .recovered_frames = 20,
    .missing_packets = 400,
    .total_packets = 5000,
    .received_packets = 4600,
    .rtt_ms = 70,
    .rtt_variance_ms = 10,
    .displayed_frames = 90,
    .video_bytes = 1400 * 1024,
    .frame_area = 3024ULL * 1900ULL,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_GT(action.fec_percentage, 10);
  EXPECT_GE(action.target_fps, 72);
}

TEST(StreamQualityControllerTests, FullResFallbackDoesNotStayAtMosaicBitrateWhenNetworkIsClean) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 1800,
    .ceiling_total_bitrate_kbps = 30000,
    .min_bitrate_kbps = 1500,
    .baseline_fps = 120,
    .startup_fps = 72,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = false,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 116000,
    .fps_needed_kbps = 24000,
  });

  stream_quality::action_t action;
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 72,
      .complete_frames = 72,
      .rtt_ms = 68,
      .rtt_variance_ms = 8,
      .displayed_frames = 72,
      .video_bytes = 260 * 1024,
      .input_queue_depth = 1,
      .frame_area = 3024ULL * 1900ULL,
      .dirty_area = 3024ULL * 1900ULL,
      .full_frame_dirty = true,
    });
  }

  EXPECT_GE(action.target_bitrate_kbps, 5000);
  EXPECT_GE(action.target_fps, 72);
  EXPECT_FALSE(action.profile_tier_deferred);
}

TEST(StreamQualityControllerTests, PublicForwardHighMotionLossKeepsPlayableFloor) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 126000,
    .ceiling_total_bitrate_kbps = 150000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 126000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 18; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1060,
      .frames_seen = 26,
      .complete_frames = 25,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 350000,
      .total_packets = 2000000,
      .received_packets = 1650000,
      .video_bytes = 38 * 1024,
      .rtt_ms = 31,
      .rtt_variance_ms = 21,
      .displayed_frames = 62,
      .visual_stale_frames = 0,
      .duplicate_frames = 36,
      .local_display_pressure = i % 4 == 0 ? 1000U : 0U,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 1479168,
      .full_frame_dirty = false,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_EQ(action.scene_class, stream_quality::scene_class_e::high_motion);
  EXPECT_GE(action.target_bitrate_kbps, 5000)
    << "Sustained delivered cadence must not collapse to an unreadable 500 Kbps floor";
  EXPECT_GE(action.target_fps, 45)
    << "Public-forward loss should reduce cadence, not stick a high-refresh game at 24 FPS";
  EXPECT_GE(action.resolution_scale_percent, 90);
}

TEST(StreamQualityControllerTests, LowRefreshStreamsAreNeverPromotedToSeventyTwoFpsByInteractivePressure) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 8000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 8000,
    .baseline_fps = 45,
    .startup_fps = 45,
    .min_fps = 18,
    .frame_width = 1920,
    .frame_height = 1080,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 5; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 45,
      .complete_frames = 45,
      .total_packets = 1600,
      .received_packets = 1600,
      .rtt_ms = 180,
      .rtt_variance_ms = 80,
      .late_frames = 5,
      .displayed_frames = 32,
      .input_queue_depth = 2,
      .input_send_latency_us = 40000,
      .input_ack_latency_us = 42000,
      .frame_area = 1920U * 1080U,
      .dirty_area = 1920U * 1080U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_LE(action.target_fps, 45);
  EXPECT_NE(action.target_fps, 72);
}

TEST(StreamQualityControllerTests, HighRefreshDelayPressureStepsDownByAboutFiveFpsNotToStickySeventyTwo) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  auto first = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 120,
    .complete_frames = 120,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 160,
    .rtt_variance_ms = 82,
    .decode_queue_depth = 5,
    .render_queue_depth = 3,
    .late_frames = 8,
    .displayed_frames = 92,
    .input_queue_depth = 2,
    .input_send_latency_us = 50000,
    .input_ack_latency_us = 52000,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
  });

  EXPECT_LT(first.target_fps, 120);
  EXPECT_GE(first.target_fps, 110);
  EXPECT_NE(first.target_fps, 72);
}

TEST(StreamQualityControllerTests, CleanRecoveryRaisesFpsOneStepTowardTarget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 120,
      .complete_frames = 120,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 180,
      .rtt_variance_ms = 90,
      .decode_queue_depth = 5,
      .render_queue_depth = 3,
      .late_frames = 8,
      .displayed_frames = 90,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
    });
  }
  ASSERT_LT(action.target_fps, 120);
  const auto pressured_fps = action.target_fps;

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 120,
    .complete_frames = 120,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 18,
    .rtt_variance_ms = 2,
    .decode_queue_depth = 0,
    .render_queue_depth = 0,
    .late_frames = 0,
    .displayed_frames = 120,
    .frame_area = 3024U * 1900U,
    .dirty_area = 0,
  });

  EXPECT_GE(action.target_fps, pressured_fps);
  EXPECT_LE(action.target_fps, std::min(120, pressured_fps + 1));
}

TEST(StreamQualityControllerTests, AudioOnlyRecoveryStillRaisesFpsOneStepToAvoidVisibleJumps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 80,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  const auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 120,
    .complete_frames = 120,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 18,
    .rtt_variance_ms = 2,
    .audio_concealed_ms = 24,
    .displayed_frames = 120,
    .frame_area = 3024U * 1900U,
    .dirty_area = 0,
  });

  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_GE(action.target_fps, 80);
  EXPECT_LE(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
}


TEST(StreamQualityControllerTests, MildAudioContinuityPressureDoesNotJitterFpsOrBlockCleanVideoRecovery) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 24000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
    .user_quality_kbps = 24000,
    .fps_needed_kbps = 26000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1200 * 1024,
      .rtt_ms = 18,
      .rtt_variance_ms = 2,
      .audio_concealed_ms = 24,
      .audio_fade_ms = 8,
      .displayed_frames = 60,
      .frame_area = 3024U * 1900U,
      .dirty_area = 0,
    });
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::audio_pressure)
    << "minor PLC/fade should not make video transport visibly oscillate";
  EXPECT_GE(action.target_fps, 97)
    << "clean video should still recover FPS one step at a time";
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(StreamQualityControllerTests, RenderBackpressureAfterRecoveryProbeUsesLongHoldToAvoidSawtooth) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 90000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 100000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 1,
    .frame_width = 3024,
    .frame_height = 1964,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3000 * 1024,
      .rtt_ms = 25,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
  }
  ASSERT_GT(action.target_fps, 96);
  const auto probed_fps = action.target_fps;

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 3000 * 1024,
    .rtt_ms = 25,
    .rtt_variance_ms = 4,
    .late_frames = 24,
    .displayed_frames = 32,
    .visual_stale_frames = 8,
    .frame_area = 3024U * 1964U,
    .dirty_area = 3024U * 1964U,
    .full_frame_dirty = true,
  });
  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_LE(action.target_fps, probed_fps - 5);
  const auto held_fps = action.target_fps;

  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3000 * 1024,
      .rtt_ms = 25,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
    EXPECT_LE(action.target_fps, held_fps)
      << "window " << i << " should stay in hold after render backpressure failed probe";
  }
}

TEST(StreamQualityControllerTests, VisualFreshnessPressureTreatsDuplicateOnlyFramesAsRenderPressure) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 12000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 24,
      .complete_frames = 24,
      .total_packets = 600,
      .received_packets = 600,
      .video_bytes = 220 * 1024,
      .rtt_ms = 35,
      .rtt_variance_ms = 4,
      .late_frames = 0,
      .displayed_frames = 1,
      .frame_area = 3024U * 1900U,
      .dirty_area = 0,
      .visual_stale_frames = 24,
      .duplicate_frames = 24,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_GE(action.pressures.render, 0.40);
  EXPECT_TRUE(action.request_idr);
}

TEST(StreamQualityControllerTests, RuntimeFpsApplyAllowsSmallDropAfterShortCooldown) {
  const auto decision = stream_quality::runtime_fps_apply_decision(120, 115, 500);

  EXPECT_TRUE(decision.target_changed);
  EXPECT_TRUE(decision.apply);
  EXPECT_FALSE(decision.deferred);
  EXPECT_EQ(decision.cooldown_ms, 500);
}

TEST(StreamQualityControllerTests, RuntimeFpsApplyAllowsOneFpsRecoveryStep) {
  const auto decision = stream_quality::runtime_fps_apply_decision(83, 84, 1000);

  EXPECT_TRUE(decision.target_changed);
  EXPECT_TRUE(decision.apply);
  EXPECT_FALSE(decision.deferred);
  EXPECT_EQ(decision.cooldown_ms, 1000);
}

TEST(StreamQualityControllerTests, RuntimeFpsApplyDefersOnlyDuringCooldown) {
  const auto decision = stream_quality::runtime_fps_apply_decision(120, 119, 100);

  EXPECT_TRUE(decision.target_changed);
  EXPECT_FALSE(decision.apply);
  EXPECT_TRUE(decision.deferred);
  EXPECT_EQ(decision.cooldown_ms, 500);
}

TEST(StreamQualityControllerTests, FailedFpsProbeAddsRecoveryHoldAndBackoff) {
  const auto first = stream_quality::fps_probe_backoff_after_failed_recovery(96,
                                                                       97,
                                                                       92,
                                                                       true,
                                                                       0,
                                                                       0);
  EXPECT_EQ(first.failed_probe_count, 1);
  EXPECT_GE(first.recovery_hold_windows, 8);
  EXPECT_GE(first.recovery_probe_interval_windows, 2);

  const auto repeated = stream_quality::fps_probe_backoff_after_failed_recovery(92,
                                                                          93,
                                                                          88,
                                                                          true,
                                                                          first.failed_probe_count,
                                                                          first.recovery_hold_windows);
  EXPECT_GT(repeated.failed_probe_count, first.failed_probe_count);
  EXPECT_GE(repeated.recovery_probe_interval_windows, first.recovery_probe_interval_windows);
  EXPECT_GT(repeated.recovery_hold_windows, first.recovery_hold_windows);
}

TEST(StreamQualityControllerTests, CleanStableWindowsDecayFailedFpsProbeBackoff) {
  const auto stable = stream_quality::fps_probe_backoff_after_failed_recovery(96,
                                                                        0,
                                                                        97,
                                                                        false,
                                                                        3,
                                                                        2);
  EXPECT_EQ(stable.failed_probe_count, 2);
  EXPECT_EQ(stable.recovery_hold_windows, 1);
  EXPECT_GE(stable.recovery_probe_interval_windows, 2);
}

TEST(StreamQualityControllerTests, FailedRecoveryProbeDropsFiveFpsAndHoldsBeforeNextProbe) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 60000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 100000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 1,
    .frame_width = 3024,
    .frame_height = 1964,
  });

  stream_quality::action_t action;
  for (int i = 0; i < 3; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3000 * 1024,
      .rtt_ms = 25,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
  }
  ASSERT_GT(action.target_fps, 96);
  const auto probed_fps = action.target_fps;

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 3000 * 1024,
    .rtt_ms = 25,
    .rtt_variance_ms = 4,
    .late_frames = 20,
    .displayed_frames = 38,
    .frame_area = 3024U * 1964U,
    .dirty_area = 3024U * 1964U,
    .full_frame_dirty = true,
  });
  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_LE(action.target_fps, probed_fps - 5);

  const auto post_failure_fps = action.target_fps;
  for (int i = 0; i < 8; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3000 * 1024,
      .rtt_ms = 25,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
    EXPECT_LE(action.target_fps, post_failure_fps)
      << "FPS should hold after a failed upward probe";
  }
}

TEST(StreamQualityControllerTests, MildAudioOnlyPressureKeepsFpsAndFecStableWhileAllowingVisualRecovery) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 30000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 60000,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1964,
    .user_quality_kbps = 18000,
    .ideal_demand_kbps = 60000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1250 * 1024,
      .rtt_ms = 24,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .audio_underruns = 1,
      .audio_concealed_ms = 22,
      .late_audio_drops = 1,
      .audio_plc_ms = 12,
      .audio_fade_ms = 6,
      .audio_buffer_depth_ms = 130,
      .frame_area = 3024U * 1964U,
      .dirty_area = 0,
      .full_frame_dirty = false,
    });
    EXPECT_LE(action.fec_percentage, 10);
    EXPECT_GE(action.target_fps, 90);
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::audio_pressure);
  EXPECT_NE(action.state, stream_quality::state_e::crisis);
  EXPECT_GE(action.target_fps, 96);
  EXPECT_LE(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_GT(action.target_bitrate_kbps, 18000);
}

TEST(StreamQualityControllerTests, RenderBackpressureAfterProbeExtendsHoldToAvoidSawtooth) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 60000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 100000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 1,
    .frame_width = 3024,
    .frame_height = 1964,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3000 * 1024,
      .rtt_ms = 25,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
  }
  ASSERT_GT(action.target_fps, 96);
  const auto probed_fps = action.target_fps;

  action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 3000 * 1024,
    .rtt_ms = 25,
    .rtt_variance_ms = 4,
    .decode_queue_depth = 7,
    .render_queue_depth = 5,
    .late_frames = 16,
    .displayed_frames = 40,
    .frame_area = 3024U * 1964U,
    .dirty_area = 3024U * 1964U,
    .full_frame_dirty = true,
  });
  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_LE(action.target_fps, probed_fps - 5);

  const auto held_fps = action.target_fps;
  for (int i = 0; i < 14; i++) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 3000 * 1024,
      .rtt_ms = 25,
      .rtt_variance_ms = 4,
      .displayed_frames = 60,
      .frame_area = 3024U * 1964U,
      .dirty_area = 3024U * 1964U,
      .full_frame_dirty = true,
    });
    EXPECT_EQ(action.target_fps, held_fps)
      << "FPS should not probe upward immediately after render backpressure; window=" << i;
  }
}

TEST(StreamQualityControllerTests, AudioOnlyPressureDoesNotRapidlyProbeVideoBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 8911,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 68,
    .min_fps = 1,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 18182,
    .fps_needed_kbps = 18182,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 1080,
      .frames_seen = 52,
      .complete_frames = 52,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 360 * 1024,
      .rtt_ms = 58,
      .rtt_variance_ms = 17,
      .displayed_frames = 52,
      .audio_underruns = 101,
      .audio_concealed_ms = 1010,
      .audio_fade_ms = 606,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::audio_pressure);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_LE(action.target_bitrate_kbps, 10000)
    << "audio-only continuity pressure should not rapidly probe video bitrate upward";
  EXPECT_LE(action.target_fps, 69)
    << "audio-only continuity pressure should not ramp FPS upward";
}

TEST(StreamQualityControllerTests, StartupDelayRenderAudioPressureDoesNotBypassLinearBitrateClamp) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 8274,
    .ceiling_total_bitrate_kbps = 88735,
    .min_bitrate_kbps = 1500,
    .baseline_fps = 120,
    .startup_fps = 64,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 80668,
    .fps_needed_kbps = 13789,
  });

  auto action = controller.on_feedback({
    .duration_ms = 4360,
    .frames_seen = 0,
    .complete_frames = 21,
    .displayed_frames = 21,
    .video_bytes = 13680,
    .rtt_ms = 61,
    .rtt_variance_ms = 12,
    .frame_area = 3024ULL * 1900ULL,
    .dirty_area = 1280,
  });
  const int first_bitrate = action.target_bitrate_kbps;
  EXPECT_GE(first_bitrate, 8200);

  action = controller.on_feedback({
    .duration_ms = 541,
    .frames_seen = 12,
    .complete_frames = 12,
    .displayed_frames = 12,
    .video_bytes = 32832,
    .rtt_ms = 152,
    .rtt_variance_ms = 108,
    .decode_queue_depth = 12,
    .render_queue_depth = 0,
    .late_frames = 3,
    .audio_underruns = 51,
    .audio_concealed_ms = 510,
    .audio_fade_ms = 306,
    .input_queue_depth = 1,
    .input_ack_latency_us = 1000,
    .frame_area = 3024ULL * 1900ULL,
    .dirty_area = 3591,
  });
  const int second_bitrate = action.target_bitrate_kbps;
  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_GE(second_bitrate, first_bitrate - 1200);

  action = controller.on_feedback({
    .duration_ms = 548,
    .frames_seen = 7,
    .complete_frames = 7,
    .displayed_frames = 7,
    .video_bytes = 19152,
    .rtt_ms = 149,
    .rtt_variance_ms = 115,
    .decode_queue_depth = 5,
    .render_queue_depth = 0,
    .audio_underruns = 52,
    .audio_concealed_ms = 520,
    .audio_fade_ms = 312,
    .frame_area = 3024ULL * 1900ULL,
    .dirty_area = 0,
  });
  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_GE(action.target_bitrate_kbps, second_bitrate - 1200);
  EXPECT_GT(action.target_bitrate_kbps, 5000);
  EXPECT_GT(action.effective_ceiling_kbps, 5000);
}

TEST(StreamQualityControllerTests, ZeroFrameFeedbackDoesNotProbeBitrateOrBleedFec) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 10342,
    .ceiling_total_bitrate_kbps = 88735,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
    .user_quality_kbps = 20000,
    .ideal_demand_kbps = 80668,
    .fps_needed_kbps = 20000,
  });

  stream_quality::action_t action {};
  int peak_bitrate = 0;
  int min_fec = 100;
  for (int i = 0; i < 10; i++) {
    action = controller.on_feedback({
      .duration_ms = 550,
      .frames_seen = 0,
      .complete_frames = 0,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .total_packets = 0,
      .received_packets = 0,
      .missing_packets = 0,
      .video_bytes = 0,
      .rtt_ms = static_cast<unsigned int>(12 + (i % 2)),
      .rtt_variance_ms = static_cast<unsigned int>(i == 8 ? 28 : 2),
      .displayed_frames = 0,
      .input_queue_depth = i == 3 ? 1U : 0U,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
    min_fec = std::min(min_fec, action.fec_percentage);
  }

  EXPECT_NE(action.state, stream_quality::state_e::healthy);
  EXPECT_LE(peak_bitrate, 10342)
    << "no-delivery feedback must not be treated as clean recovery/probe evidence";
  EXPECT_GE(min_fec, 10)
    << "no-delivery feedback cannot prove the route is clean enough to bleed FEC";
  EXPECT_GE(action.target_bitrate_kbps, 1500);
}

TEST(StreamQualityControllerTests, UserQualityCapsFpsProtectionOvershootOnWeakRoute) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18182,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 8274,
    .ceiling_total_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .startup_fps = 64,
    .min_fps = 45,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 80668,
    .fps_needed_kbps = 13789,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 18; i++) {
    action = controller.on_feedback({
      .duration_ms = 550,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 650 * 1024,
      .rtt_ms = 38,
      .rtt_variance_ms = 5,
      .displayed_frames = 60,
      .input_queue_depth = 1,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
    });

    EXPECT_LE(action.target_bitrate_kbps, 12000);
    EXPECT_LT(action.pacing_bitrate_kbps, 15000);
  }

  EXPECT_GT(action.target_fps, 64);
  EXPECT_LE(action.target_fps, 82);
}

TEST(StreamQualityControllerTests, RandomLossCountsFecInsideUserQualityBudget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 80668,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 88735,
    .min_bitrate_kbps = 1500,
    .baseline_fps = 120,
    .startup_fps = 110,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = false,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 80668,
    .fps_needed_kbps = 24000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; i++) {
    action = controller.on_feedback({
      .duration_ms = 544,
      .frames_seen = 61,
      .complete_frames = 17,
      .recovered_frames = 18,
      .unrecoverable_frames = 15,
      .missing_packets = 150,
      .total_packets = 446,
      .received_packets = 296,
      .video_bytes = 253536,
      .rtt_ms = 36,
      .rtt_variance_ms = 11,
      .displayed_frames = 54,
      .duplicate_frames = 54,
      .render_queue_depth = 2,
      .input_queue_depth = 1,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
      .rfi_requests = 1,
    });
  }

  EXPECT_GE(action.fec_percentage, 25);
  EXPECT_LE(action.encoding_budget_kbps + action.fec_budget_kbps, 12000)
    << "FEC overhead must be paid from the user's remote-safe send budget";
  EXPECT_LE(action.pacing_bitrate_kbps, 12000)
    << "Pacing must not add a second headroom layer on a lossy user-quality path";
  EXPECT_LE(action.target_bitrate_kbps, 9600)
    << "When FEC rises, encoding must drop instead of pushing total send rate over budget";
}

TEST(StreamQualityControllerTests, RenderBackpressureDropsFpsLinearlyAndHoldsProbe) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 12000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 15000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
    .user_quality_kbps = 12000,
  });

  auto action = controller.on_feedback({
    .duration_ms = 550,
    .frames_seen = 60,
    .complete_frames = 60,
    .total_packets = 2400,
    .received_packets = 2400,
    .rtt_ms = 36,
    .rtt_variance_ms = 4,
    .displayed_frames = 55,
    .decode_queue_depth = 0,
    .render_queue_depth = 4,
    .late_frames = 8,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_EQ(action.target_fps, 115);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_LE(action.target_bitrate_kbps, 12000);

  for (int i = 0; i < 3; i++) {
    action = controller.on_feedback({
      .duration_ms = 550,
      .frames_seen = 60,
      .complete_frames = 60,
      .total_packets = 2400,
      .received_packets = 2400,
      .rtt_ms = 28,
      .rtt_variance_ms = 3,
      .displayed_frames = 60,
      .frame_area = 3024U * 1900U,
      .dirty_area = 0,
    });
  }

  EXPECT_LE(action.target_fps, 115);
}

TEST(StreamQualityControllerTests, CleanRouteStopsBitrateProbeAfterNoCadenceGain) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 250000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 300000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  int peak_bitrate = 0;
  for (int i = 0; i < 16; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 95,
      .complete_frames = 95,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 6 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 95,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .frame_area = 3840U * 2160U,
      .dirty_area = 2897886,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
  }

  EXPECT_LT(peak_bitrate, 210000)
    << "Clean LAN without displayed-fps or render benefit should stop probing before huge bitrate";
  EXPECT_LE(action.fec_percentage, 2)
    << "Clean route should bleed fixed startup FEC down to low overhead";
  EXPECT_EQ(action.target_fps, 150);
}

TEST(StreamQualityControllerTests, RenderBackpressureAfterBitrateProbeLocksPlateau) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 250000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 300000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 95,
      .complete_frames = 95,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 6 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 95,
      .frame_area = 3840U * 2160U,
      .dirty_area = 2897886,
    });
  }
  const auto pre_pressure_bitrate = action.target_bitrate_kbps;

  action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 95,
    .complete_frames = 95,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 6 * 1024 * 1024,
    .rtt_ms = 1,
    .rtt_variance_ms = 1,
    .displayed_frames = 95,
    .decode_queue_depth = 1,
    .render_queue_depth = 4,
    .frame_area = 3840U * 2160U,
    .dirty_area = 2897886,
  });
  EXPECT_LE(action.target_bitrate_kbps, pre_pressure_bitrate)
    << "Render backpressure should reject the last upward bitrate probe";

  const auto held_bitrate = action.target_bitrate_kbps;
  for (int i = 0; i < 10; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 95,
      .complete_frames = 95,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 6 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 95,
      .decode_queue_depth = 1,
      .render_queue_depth = 1,
      .frame_area = 3840U * 2160U,
      .dirty_area = 2897886,
    });
    EXPECT_LE(action.target_bitrate_kbps, held_bitrate + 3500)
      << "Plateau hold should avoid immediate sawtooth probing; window=" << i;
  }
}

TEST(StreamQualityControllerTests, IsolatedNativeRenderQueueDoesNotDropHighRefreshFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 250000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 300000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 8 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 150,
      .decode_queue_depth = 1,
      .render_queue_depth = 3,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.target_fps, 150)
    << "Light Native renderer queue on a clean route should not cut FPS";
}

TEST(StreamQualityControllerTests, LightInputActivityDoesNotConstrainCleanHighRefreshRoute) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 250000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 300000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 8 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 150,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .input_queue_depth = 1,
      .input_send_latency_us = 1000,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::input_pressure)
    << "A one-packet input queue with ~1ms latency is activity, not congestion";
  EXPECT_EQ(action.target_fps, 150);
}

TEST(StreamQualityControllerTests, SustainedNativeRenderStallCanReduceFpsLinearly) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 250000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 300000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 5; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 8 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 120,
      .decode_queue_depth = 1,
      .render_queue_depth = 5,
      .late_frames = 28,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_GE(action.target_fps, 145)
    << "Sustained render pressure should step down linearly, not collapse high refresh";
  EXPECT_LT(action.target_fps, 150);
}

TEST(StreamQualityControllerTests, WeakRouteSweetSpotDoesNotProbeAboveSustainableEstimateWithoutGain) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 40000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 60,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 50000,
    .baseline_fps = 120,
    .startup_fps = 90,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
    .user_quality_kbps = 40000,
    .fps_needed_kbps = 40000,
  });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 90,
    .complete_frames = 90,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 1400 * 1024,
    .rtt_ms = 260,
    .rtt_variance_ms = 95,
    .displayed_frames = 86,
    .decode_queue_depth = 2,
    .render_queue_depth = 1,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
  });
  ASSERT_EQ(action.reason, stream_quality::reason_e::delay_congestion);

  int peak_bitrate = action.target_bitrate_kbps;
  for (int i = 0; i < 18; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 90,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1400 * 1024,
      .rtt_ms = 42,
      .rtt_variance_ms = 6,
      .displayed_frames = 86,
      .decode_queue_depth = 1,
      .render_queue_depth = 1,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
  }

  EXPECT_LT(peak_bitrate, 22000)
    << "Weak-route recovery without cadence gain should settle near a sustainable sweet spot";
  EXPECT_GE(action.target_fps, 90)
    << "Weak-route recovery should preserve FPS before chasing bitrate";
}

TEST(StreamQualityControllerTests, StableUsefulRouteIsHighAvailabilityEvenWithoutLanRttOrAutoBitrate) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 60000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 90000,
    .ceiling_total_bitrate_kbps = 110000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .user_quality_kbps = 90000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 12; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 120,
      .complete_frames = 120,
      .total_packets = 3000,
      .received_packets = 3000,
      .video_bytes = 9000 * 1024,
      .rtt_ms = 38,
      .rtt_variance_ms = 6,
      .displayed_frames = 120,
      .decode_queue_depth = 0,
      .render_queue_depth = 1,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_LE(action.fec_percentage, 2)
    << "High availability is measured by stable displayed cadence, not by LAN RTT or auto bitrate";
}

TEST(StreamQualityControllerTests, AutoCeilingDoesNotProbeWhenActualMotionCadenceIsNotHighAvailability) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 1000000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 1200000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .ideal_demand_kbps = 1000000,
    .fps_needed_kbps = 240000,
  });

  stream_quality::action_t action {};
  int peak_bitrate = 0;
  for (int i = 0; i < 10; i++) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 96,
      .complete_frames = 96,
      .total_packets = 3000,
      .received_packets = 3000,
      .video_bytes = 8 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 96,
      .decode_queue_depth = 0,
      .render_queue_depth = 1,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
  }

  EXPECT_EQ(action.target_fps, 150)
    << "A cadence-limited but queue-clean stream should not make the controller lower requested FPS";
  EXPECT_LE(peak_bitrate, 181000)
    << "Auto/high ceiling is not high availability; without displayed cadence gain, do not probe bitrate";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, StartupRfiBurstEntersGuardWithoutCrushingMediaParameters) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 60,
    .startup_bitrate_kbps = 160000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  auto first = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 36,
    .recovered_frames = 8,
    .unrecoverable_frames = 16,
    .missing_packets = 520,
    .total_packets = 3000,
    .received_packets = 2480,
    .video_bytes = 7 * 1024 * 1024,
    .rtt_ms = 8,
    .rtt_variance_ms = 2,
    .displayed_frames = 36,
    .frame_area = 3840U * 2160U,
    .dirty_area = 3840U * 2160U,
    .full_frame_dirty = true,
    .rfi_requests = 44,
    .waiting_for_rfi_frames = 44,
  });

  auto second = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 60,
    .complete_frames = 60,
    .recovered_frames = 0,
    .unrecoverable_frames = 0,
    .missing_packets = 0,
    .total_packets = 3000,
    .received_packets = 3000,
    .video_bytes = 7 * 1024 * 1024,
    .rtt_ms = 8,
    .rtt_variance_ms = 2,
    .displayed_frames = 60,
    .frame_area = 3840U * 2160U,
    .dirty_area = 3840U * 2160U,
    .full_frame_dirty = true,
  });

  EXPECT_TRUE(first.request_idr);
  EXPECT_GE(first.target_bitrate_kbps, 120000)
    << "startup RFI guard may request an IDR, but must not crush a clean-LAN media budget";
  EXPECT_GE(first.target_fps, 145)
    << "startup guard should preserve high-refresh cadence until real sustained pressure is proven";
  EXPECT_LE(first.fec_percentage, 35)
    << "startup RFI burst should not open long-lived high FEC before the route is measured";

  EXPECT_FALSE(second.request_idr);
  EXPECT_GE(second.target_bitrate_kbps, first.target_bitrate_kbps)
    << "clean feedback after startup RFI should not remain stuck in a crushed recovery point";
  EXPECT_EQ(second.target_fps, 150);
}

TEST(StreamQualityControllerTests, LowAvailabilityVideoLossDoesNotHoldHighFpsAtMosaicBitrate) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 113000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 150000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = false,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 113000,
    .fps_needed_kbps = 20000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 6; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 43,
      .complete_frames = 0,
      .recovered_frames = 12,
      .unrecoverable_frames = 16,
      .missing_packets = 35,
      .total_packets = 113,
      .received_packets = 78,
      .video_bytes = 170 * 1024,
      .rtt_ms = 12,
      .rtt_variance_ms = 4,
      .displayed_frames = 0,
      .input_queue_depth = 1,
      .input_send_latency_us = 12000,
      .input_ack_latency_us = 12000,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
      .rfi_requests = 12,
      .waiting_for_rfi_frames = 30,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_LT(action.target_fps, 120)
    << "Low availability must reduce cadence instead of preserving high FPS at unreadable bitrate";
  EXPECT_GE(action.target_bitrate_kbps, 7000)
    << "Full-res fallback should preserve a minimally readable floor instead of collapsing to 1.5-3 Mbps";
  EXPECT_LE(action.fec_percentage, 35);
}

TEST(StreamQualityControllerTests, LowAvailabilityRecoveryRequiresDeliveredCadenceBeforeHighAvailabilityProbe) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 113000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 150000,
    .baseline_fps = 120,
    .startup_fps = 96,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = false,
    .user_quality_kbps = 12000,
    .ideal_demand_kbps = 113000,
    .fps_needed_kbps = 20000,
  });

  controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 43,
    .complete_frames = 0,
    .recovered_frames = 12,
    .unrecoverable_frames = 16,
    .missing_packets = 35,
    .total_packets = 113,
    .received_packets = 78,
    .video_bytes = 170 * 1024,
    .rtt_ms = 12,
    .rtt_variance_ms = 4,
    .displayed_frames = 0,
    .frame_area = 3024U * 1900U,
    .dirty_area = 3024U * 1900U,
    .full_frame_dirty = true,
    .rfi_requests = 12,
    .waiting_for_rfi_frames = 30,
  });

  stream_quality::action_t action {};
  int peak_bitrate = 0;
  for (int i = 0; i < 10; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 96,
      .complete_frames = 96,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 1100 * 1024,
      .rtt_ms = 42,
      .rtt_variance_ms = 7,
      .displayed_frames = 86,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
  }

  EXPECT_LT(peak_bitrate, 25000)
    << "Recovering route without full delivered cadence should stay near a sweet spot";
  EXPECT_LE(action.target_fps, 106)
    << "FPS recovery should wait for high-availability evidence, not merely clean packet counters";
}

TEST(StreamQualityControllerTests, CleanStartupCadenceRampsFpsFasterThanWeakRouteRecovery) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    const auto delivered_fps = std::max(120, controller.current_fps());
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = static_cast<std::uint32_t>(delivered_fps),
      .complete_frames = static_cast<std::uint32_t>(delivered_fps),
      .missing_packets = 0,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 8 * 1024 * 1024,
      .rtt_ms = 3,
      .rtt_variance_ms = 1,
      .displayed_frames = static_cast<std::uint32_t>(delivered_fps),
      .decode_queue_depth = 0,
      .render_queue_depth = 1,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_GE(action.target_fps, 135)
    << "Clean LAN should confirm the conservative startup cadence and ramp quickly, not crawl at 1fps/window";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, CleanPingWithoutDeliveredCadenceDoesNotFastRampFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 120,
      .complete_frames = 120,
      .missing_packets = 0,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 8 * 1024 * 1024,
      .rtt_ms = 3,
      .rtt_variance_ms = 1,
      .displayed_frames = 82,
      .decode_queue_depth = 2,
      .render_queue_depth = 4,
      .late_frames = 18,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_NE(action.state, stream_quality::state_e::healthy);
  EXPECT_LE(action.target_fps, 120)
    << "Low RTT/bandwidth is not high availability when the client cannot actually display the current target";
  EXPECT_LE(action.fec_percentage, 10)
    << "Render/cadence pressure without loss should not open FEC";
}

TEST(StreamQualityControllerTests, RemoteStartupWithVideoLossFallsToUsableSeedInsteadOfHoldingManualTarget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 113000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 80000,
    .ceiling_total_bitrate_kbps = 150000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3024,
    .frame_height = 1900,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = false,
    .user_quality_kbps = 80000,
    .ideal_demand_kbps = 113000,
    .fps_needed_kbps = 80000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 120,
      .complete_frames = 0,
      .recovered_frames = 1,
      .unrecoverable_frames = 110,
      .missing_packets = 1300,
      .total_packets = 3000,
      .received_packets = 1700,
      .video_bytes = 500 * 1024,
      .rtt_ms = 38,
      .rtt_variance_ms = 12,
      .displayed_frames = 0,
      .decode_queue_depth = 0,
      .render_queue_depth = 0,
      .frame_area = 3024U * 1900U,
      .dirty_area = 3024U * 1900U,
      .full_frame_dirty = true,
      .rfi_requests = 18,
      .waiting_for_rfi_frames = 110,
    });
  }

  EXPECT_EQ(action.state, stream_quality::state_e::crisis);
  EXPECT_LT(action.target_fps, 110)
    << "A low-availability startup cannot keep trying the manual high-FPS target";
  EXPECT_LE(action.target_bitrate_kbps, 16000)
    << "A tunnel/remote startup with no displayed frames must fall back to a measured usable seed";
  EXPECT_GE(action.target_bitrate_kbps, 7000)
    << "The seed still needs to be readable enough to recover, not collapse to mosaic bitrate";
  EXPECT_LE(action.fec_percentage, 35);
}

TEST(StreamQualityControllerTests, RemoteSafeStartupDoesNotTreatCleanAlrAsCapacityProof) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 145567,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 10000,
    .ceiling_total_bitrate_kbps = 160124,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 145567,
    .fps_needed_kbps = 126000,
  });

  int peak_bitrate = controller.current_bitrate_kbps();
  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 540,
      .frames_seen = 0,
      .complete_frames = 81,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 744192,
      .rtt_ms = 22,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .late_frames = 0,
      .displayed_frames = 81,
      .visual_stale_frames = 0,
      .duplicate_frames = 1,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
  }

  EXPECT_EQ(action.scenario, stream_quality::scenario_e::clean_alr);
  EXPECT_LE(peak_bitrate, 24000)
    << "A remote-safe low startup seed must not jump 10->38->66Mbps from clean ALR/last-frame reuse alone";
  EXPECT_EQ(action.target_fps, 150)
    << "This guard should limit only bitrate probing, not high-FPS cadence when delivery is clean";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, RemoteSafeRecoveryKeepsSustainableCapAcrossCleanAlr) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 145567,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 10000,
    .ceiling_total_bitrate_kbps = 160124,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 145567,
    .fps_needed_kbps = 126000,
  });

  for (int i = 0; i < 4; ++i) {
    controller.on_feedback({
      .duration_ms = 540,
      .frames_seen = 0,
      .complete_frames = 81,
      .video_bytes = 744192,
      .rtt_ms = 22,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .displayed_frames = 81,
      .duplicate_frames = 1,
      .frame_area = 3840ULL * 2160ULL,
    });
  }

  auto action = controller.on_feedback({
    .duration_ms = 541,
    .frames_seen = 145,
    .complete_frames = 20,
    .recovered_frames = 15,
    .unrecoverable_frames = 111,
    .missing_packets = 591,
    .total_packets = 1820,
    .received_packets = 1177,
    .video_bytes = 620160,
    .rtt_ms = 22,
    .rtt_variance_ms = 2,
    .decode_queue_depth = 4,
    .render_queue_depth = 2,
    .displayed_frames = 111,
    .duplicate_frames = 90,
    .local_display_pressure = 350,
    .frame_area = 3840ULL * 2160ULL,
  });
  ASSERT_EQ(action.state, stream_quality::state_e::crisis);
  ASSERT_LT(action.sustainable_estimate_kbps, 16000);

  int peak_bitrate = action.target_bitrate_kbps;
  int peak_effective_ceiling = action.effective_ceiling_kbps;
  int peak_scale = action.resolution_scale_percent;
  for (int i = 0; i < 7; ++i) {
    action = controller.on_feedback({
      .duration_ms = 540,
      .frames_seen = 72,
      .complete_frames = 72,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 560 * 1024,
      .rtt_ms = 23,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .late_frames = 0,
      .displayed_frames = 72,
      .visual_stale_frames = 0,
      .duplicate_frames = 2,
      .frame_area = 3840ULL * 2160ULL,
    });
    peak_bitrate = std::max(peak_bitrate, action.target_bitrate_kbps);
    peak_effective_ceiling = std::max(peak_effective_ceiling, action.effective_ceiling_kbps);
    peak_scale = std::max(peak_scale, action.resolution_scale_percent);
  }

  EXPECT_LE(peak_bitrate, 18000)
    << "A weak public route that just proved 20Mbps unsafe should not re-probe to the same cliff from clean ALR alone";
  EXPECT_LT(peak_effective_ceiling, 30000)
    << "Clean ALR after a loss cliff is not capacity proof, so the sustainable cap should remain active";
  EXPECT_LE(peak_scale, 90)
    << "Weak-route recovery may use a readable intermediate tier, but must not reconfigure straight back to full scale from clean ALR alone";
  EXPECT_NE(action.state, stream_quality::state_e::healthy);
}

TEST(StreamQualityControllerTests, HighAvailabilityAfterConservativeStartReachesFullCadenceQuickly) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; ++i) {
    const auto delivered_fps = std::max(120, controller.current_fps());
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = static_cast<std::uint32_t>(delivered_fps),
      .complete_frames = static_cast<std::uint32_t>(delivered_fps),
      .missing_packets = 0,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 16 * 1024 * 1024,
      .rtt_ms = 4,
      .rtt_variance_ms = 1,
      .displayed_frames = static_cast<std::uint32_t>(delivered_fps),
      .decode_queue_depth = 0,
      .render_queue_depth = 1,
      .frame_area = 3840U * 2160U,
      .dirty_area = 3840U * 2160U,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(action.target_fps, 150)
    << "Clean delivered cadence should graduate from the conservative seed to full target quickly";
  EXPECT_LE(action.fec_percentage, 2);
  EXPECT_GT(action.target_bitrate_kbps, 60000)
    << "High availability should regain quality after cadence is proven";
}

TEST(StreamQualityControllerTests, RecoverableLossAtCurrentCadenceDoesNotPinHighRefreshStartupFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 2,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 10000,
    .ceiling_total_bitrate_kbps = 148478,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 126000,
    .fps_needed_kbps = 126000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 18; ++i) {
    action = controller.on_feedback({
      .duration_ms = 540,
      .frames_seen = 68,
      .complete_frames = 68,
      .recovered_frames = 4,
      .unrecoverable_frames = 0,
      .missing_packets = 4,
      .total_packets = 75,
      .received_packets = 71,
      .video_bytes = 1700 * 1024,
      .rtt_ms = 11,
      .rtt_variance_ms = 2,
      .audio_underruns = 50,
      .audio_concealed_ms = 500,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .displayed_frames = 68,
      .local_display_pressure = 300,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
    });
  }

  EXPECT_GE(action.target_fps, 130)
    << "FEC-recovered loss with stable current cadence should probe above the 120fps startup seed toward 150fps";
}

TEST(StreamQualityControllerTests, LowLatencyRecoveredPublicPathCanProbeToHighRefreshTarget) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 2,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 10000,
    .ceiling_total_bitrate_kbps = 148478,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 126000,
    .fps_needed_kbps = 126000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 48; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1110,
      .frames_seen = 133,
      .complete_frames = 133,
      .recovered_frames = static_cast<std::uint32_t>((i % 5 == 0) ? 30 : 24),
      .unrecoverable_frames = 0,
      .missing_packets = static_cast<std::uint32_t>((i % 7 == 0) ? 34 : 28),
      .total_packets = static_cast<std::uint32_t>((i % 7 == 0) ? 617 : 515),
      .received_packets = static_cast<std::uint32_t>((i % 7 == 0) ? 583 : 487),
      .video_bytes = 4100 * 1024,
      .rtt_ms = static_cast<std::uint32_t>((i % 11 == 0) ? 14 : 12),
      .rtt_variance_ms = static_cast<std::uint32_t>((i % 13 == 0) ? 3 : 2),
      .audio_underruns = 104,
      .audio_concealed_ms = 1040,
      .decode_queue_depth = static_cast<std::uint32_t>((i % 11 == 0) ? 5 : ((i % 3 == 0) ? 3 : 2)),
      .render_queue_depth = 2,
      .displayed_frames = static_cast<std::uint32_t>((i % 4 == 0) ? 144 : 139),
      .local_display_pressure = static_cast<std::uint32_t>((i % 11 == 0) ? 625 : ((i % 3 == 0) ? 350 : 262)),
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
    });
  }

  EXPECT_GE(action.target_fps, 145)
    << "Low-RTT public paths with fully FEC-recovered loss should treat 120fps as a seed, not a long-term ceiling";
}

TEST(StreamQualityControllerTests, CleanAlrReuseDoesNotLearnLowSendRateAsLinkCapacity) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 120,
      .complete_frames = 0,
      .recovered_frames = 0,
      .unrecoverable_frames = 110,
      .missing_packets = 1800,
      .total_packets = 3600,
      .received_packets = 1800,
      .video_bytes = 520 * 1024,
      .rtt_ms = 42,
      .rtt_variance_ms = 12,
      .displayed_frames = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .rfi_requests = 16,
      .waiting_for_rfi_frames = 110,
    });
  }
  ASSERT_LT(action.target_bitrate_kbps, 18000);

  const auto low_estimate = controller.sustainable_estimate_kbps();
  for (int i = 0; i < 5; ++i) {
    const auto frames = static_cast<std::uint32_t>(std::max(60, controller.current_fps()));
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = frames,
      .complete_frames = frames,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 360 * 1024,
      .rtt_ms = 24,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 1,
      .render_queue_depth = 0,
      .late_frames = 0,
      .displayed_frames = frames + 12,
      .visual_stale_frames = 0,
      .duplicate_frames = frames + 12,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    });
  }

  EXPECT_GE(controller.sustainable_estimate_kbps(), low_estimate)
    << "Low byte count during clean ALR/last-frame reuse is app-limited, not measured path capacity";
  EXPECT_GT(action.target_bitrate_kbps, 24000)
    << "Clean app-limited windows should actively probe upward instead of staying pinned near the weak-route seed";
  EXPECT_GT(action.target_fps, 90);
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, CleanClientCadenceLimitDropsQuicklyWithoutNetworkDowngrade) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 45000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 45,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .ideal_demand_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  const stream_quality::feedback_t feedback {
    .duration_ms = 542,
    .frames_seen = 80,
    .complete_frames = 80,
    .recovered_frames = 0,
    .unrecoverable_frames = 0,
    .missing_packets = 0,
    .total_packets = 2400,
    .received_packets = 2400,
    .video_bytes = 1650 * 1024,
    .rtt_ms = 1,
    .rtt_variance_ms = 1,
    .decode_queue_depth = 8,
    .render_queue_depth = 0,
    .late_frames = 16,
    .displayed_frames = 33,
    .visual_stale_frames = 0,
    .duplicate_frames = 0,
    .frame_area = 3840ULL * 2160ULL,
    .dirty_area = 0,
    .full_frame_dirty = false,
  };

  auto action = controller.on_feedback(feedback);

  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_EQ(action.packet_loss, 0.0);
  EXPECT_LE(action.fec_percentage, 10)
    << "Display cadence pressure on a clean route must not open FEC";
  EXPECT_LE(action.target_fps, 72)
    << "When the client is visibly displaying about 60fps with a full decode queue, do not walk down by 5fps";
  EXPECT_EQ(action.resolution_scale_percent, 100)
    << "This is a cadence cap, not a network-quality downgrade";

  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback(feedback);
  }
  EXPECT_LE(action.target_fps, 72)
    << "The cadence cap should hold instead of immediately probing back upward";
}

TEST(StreamQualityControllerTests, CleanFocusDisplayTransitionDoesNotStickBelowFullFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 120000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  auto clean_full_rate = []() {
    return stream_quality::feedback_t {
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 4500,
      .received_packets = 4500,
      .video_bytes = 18 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .displayed_frames = 150,
      .decode_queue_depth = 0,
      .render_queue_depth = 1,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
    };
  };

  auto action = controller.on_feedback(clean_full_rate());
  ASSERT_EQ(action.target_fps, 150);
  const auto fec_before_focus_transition = action.fec_percentage;

  for (int i = 0; i < 2; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 4500,
      .received_packets = 4500,
      .video_bytes = 18 * 1024 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 8,
      .render_queue_depth = 2,
      .late_frames = 2,
      .displayed_frames = 0,
      .visual_stale_frames = 1,
      .duplicate_frames = 1,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .rfi_requests = 1,
      .waiting_for_rfi_frames = 2,
    });
  }

  EXPECT_GE(action.target_fps, 145)
    << "A clean focus/display-layer transition should not permanently push LAN cadence down";
  EXPECT_LE(action.fec_percentage, fec_before_focus_transition)
    << "Display-layer stalls on a clean route must not open FEC";

  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback(clean_full_rate());
  }

  EXPECT_EQ(action.target_fps, 150)
    << "Once displayed cadence returns on a clean route, FPS hold should clear quickly";
  EXPECT_NE(action.state, stream_quality::state_e::constrained);
  EXPECT_NE(action.state, stream_quality::state_e::crisis);
}

TEST(StreamQualityControllerTests, HighMotionFecSkipEntersFastTierAndRequestsIdrQuickly) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 120000,
    .ceiling_total_bitrate_kbps = 230000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .ideal_demand_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 2; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 75,
      .complete_frames = 75,
      .missing_packets = 0,
      .total_packets = 3600,
      .received_packets = 3600,
      .video_bytes = 12 * 1024 * 1024,
      .rtt_ms = 8,
      .rtt_variance_ms = 2,
      .decode_queue_depth = 5,
      .render_queue_depth = 5,
      .late_frames = 18,
      .displayed_frames = 18,
      .visual_stale_frames = 14,
      .duplicate_frames = 10,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .large_frame_fec_skipped = 1,
      .waiting_for_rfi_frames = 8,
    });
  }

  EXPECT_EQ(action.reason, stream_quality::reason_e::media_continuity);
  EXPECT_EQ(action.availability, stream_quality::availability_e::low);
  EXPECT_EQ(action.tier, stream_quality::tier_e::fast);
  EXPECT_LT(action.target_bitrate_kbps, 120000);
  EXPECT_GE(action.actual_scale_percent, 90);
  EXPECT_TRUE(action.request_idr);
}

TEST(StreamQualityControllerTests, CleanRecoveryClimbsTiersWithoutJumpingStraightToBluray) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 90000,
    .ceiling_total_bitrate_kbps = 230000,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .ideal_demand_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  for (int i = 0; i < 2; ++i) {
    controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 75,
      .complete_frames = 75,
      .total_packets = 3600,
      .received_packets = 3600,
      .decode_queue_depth = 5,
      .render_queue_depth = 5,
      .late_frames = 18,
      .displayed_frames = 18,
      .visual_stale_frames = 14,
      .duplicate_frames = 10,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .large_frame_fec_skipped = 1,
      .waiting_for_rfi_frames = 8,
    });
  }

  stream_quality::action_t first_clean {};
  for (int i = 0; i < 2; ++i) {
    first_clean = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .missing_packets = 0,
      .total_packets = 5000,
      .received_packets = 5000,
      .video_bytes = 14 * 1024 * 1024,
      .rtt_ms = 6,
      .rtt_variance_ms = 1,
      .displayed_frames = 150,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
    });
  }

  EXPECT_NE(first_clean.tier, stream_quality::tier_e::bluray)
    << "The first clean recovery window should prove stability before returning to the top tier";

  stream_quality::action_t recovered {};
  for (int i = 0; i < 8; ++i) {
    recovered = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 150,
      .complete_frames = 150,
      .missing_packets = 0,
      .total_packets = 5000,
      .received_packets = 5000,
      .video_bytes = 20 * 1024 * 1024,
      .rtt_ms = 4,
      .rtt_variance_ms = 1,
      .displayed_frames = 150,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
    });
  }

  EXPECT_EQ(recovered.availability, stream_quality::availability_e::high);
  EXPECT_EQ(recovered.tier, stream_quality::tier_e::bluray);
  EXPECT_EQ(recovered.target_fps, 150);
}

TEST(StreamQualityControllerTests, LocalDisplayPressureDoesNotLookLikeNetworkLoss) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 120000,
    .ceiling_total_bitrate_kbps = 160000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 75,
    .complete_frames = 75,
    .missing_packets = 0,
    .total_packets = 3600,
    .received_packets = 3600,
    .rtt_ms = 2,
    .rtt_variance_ms = 1,
    .decode_queue_depth = 8,
    .render_queue_depth = 2,
    .late_frames = 18,
    .displayed_frames = 25,
    .local_display_pressure = 1000,
    .frame_area = 3840ULL * 2160ULL,
  });

  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_EQ(action.packet_loss, 0.0);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_EQ(action.availability, stream_quality::availability_e::recovering);
}

TEST(StreamQualityControllerTests, DisplayStarvationWithRecoveredLossDoesNotReopenFec) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 120000,
    .ceiling_total_bitrate_kbps = 160000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
  });

  auto action = controller.on_feedback({
    .duration_ms = 550,
    .frames_seen = 15,
    .complete_frames = 0,
    .recovered_frames = 7,
    .unrecoverable_frames = 0,
    .missing_packets = 7,
    .total_packets = 32,
    .received_packets = 25,
    .video_bytes = 40 * 1024,
    .rtt_ms = 24,
    .rtt_variance_ms = 4,
    .displayed_frames = 0,
    .local_display_pressure = 1000,
    .frame_area = 3840ULL * 2160ULL,
  });

  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss)
    << "Recovered-only packet loss during a display/capture stall is not a reason to spend more bandwidth on FEC";
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_EQ(action.availability, stream_quality::availability_e::recovering);
}

TEST(StreamQualityControllerTests, DuplicateOnlyFrameReuseDoesNotLookLikeStaleVideo) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 120000,
    .ceiling_total_bitrate_kbps = 160000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
  });

  auto action = controller.on_feedback({
    .duration_ms = 500,
    .frames_seen = 0,
    .complete_frames = 0,
    .recovered_frames = 0,
    .unrecoverable_frames = 0,
    .missing_packets = 0,
    .total_packets = 0,
    .received_packets = 0,
    .rtt_ms = 24,
    .rtt_variance_ms = 4,
    .displayed_frames = 30,
    .visual_stale_frames = 0,
    .duplicate_frames = 30,
    .frame_area = 3840ULL * 2160ULL,
  });

  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline)
    << "Intentional last-frame reuse should keep visual stats fresh without looking like a render stall";
  EXPECT_NE(action.availability, stream_quality::availability_e::low);
  EXPECT_LE(action.fec_percentage, 10);
}

TEST(StreamQualityControllerTests, CleanLastFrameReuseDoesNotHoldLanAtLowFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 60,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  auto clean_reuse_feedback = []() {
    stream_quality::feedback_t feedback {
      .duration_ms = 1000,
      .frames_seen = 58,
      .complete_frames = 58,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2400,
      .received_packets = 2400,
      .video_bytes = 2300 * 1024,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 0,
      .render_queue_depth = 1,
      .late_frames = 0,
      .displayed_frames = 83,
      .visual_stale_frames = 0,
      .duplicate_frames = 25,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
    };
    feedback.local_display_pressure = stream_quality::infer_local_display_pressure(feedback);
    return feedback;
  };

  EXPECT_LT(clean_reuse_feedback().local_display_pressure, 200U)
    << "Intentional last-frame reuse on a clean LAN path is not client render pressure";

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback(clean_reuse_feedback());
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline)
    << "Frame reuse should not be reported back to Sunshine as client-render pressure";
  EXPECT_GT(action.target_fps, 60)
    << "A clean LAN path that is meeting the current target should probe upward from the conservative seed";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, SparseDisplaySlipReuseDoesNotPinLanBelowTargetFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 58227,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  auto clean_slip_reuse_feedback = [](int target_fps) {
    const auto frames = static_cast<std::uint32_t>(
      std::max(1, static_cast<int>(std::lround(static_cast<double>(target_fps) * 0.55))));
    const auto reused = std::max(2U, frames / 10U);
    stream_quality::feedback_t feedback {
      .duration_ms = 550,
      .frames_seen = frames,
      .complete_frames = frames,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 1198208,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .late_frames = 1,
      .displayed_frames = frames + reused,
      .visual_stale_frames = 1,
      .duplicate_frames = reused,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    };
    feedback.local_display_pressure = stream_quality::infer_local_display_pressure(feedback);
    return feedback;
  };

  EXPECT_EQ(clean_slip_reuse_feedback(90).local_display_pressure, 0U)
    << "One stale/late present in a clean reuse window is display smoothing, not client-render pressure";

  stream_quality::action_t action {};
  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback(clean_slip_reuse_feedback(controller.current_fps()));
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_GE(action.target_fps, 120)
    << "Clean LAN reuse with only sparse display slips should keep probing toward the 150fps target";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, ModerateDisplayReuseSlipDoesNotTriggerClientRenderOnCleanLan) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 58227,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 104,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  auto lan_reuse_feedback = []() {
    stream_quality::feedback_t feedback {
      .duration_ms = 1104,
      .frames_seen = 107,
      .complete_frames = 107,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 1198208,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 2,
      .render_queue_depth = 2,
      .late_frames = 6,
      .displayed_frames = 119,
      .visual_stale_frames = 6,
      .duplicate_frames = 20,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    };
    feedback.local_display_pressure = stream_quality::infer_local_display_pressure(feedback);
    return feedback;
  };

  EXPECT_EQ(lan_reuse_feedback().local_display_pressure, 0U)
    << "Clean LAN last-frame reuse with small stale/late slips is visual smoothing, not client-render pressure";

  stream_quality::action_t action {};
  for (int i = 0; i < 6; ++i) {
    action = controller.on_feedback(lan_reuse_feedback());
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_GE(action.target_fps, 104)
    << "The controller should not turn clean reuse feedback into a downward client-render clamp";
}

TEST(StreamQualityControllerTests, DuplicateDominantNativeAppSwitchFeedbackDoesNotClampLanFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 145567,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 126000,
    .ceiling_total_bitrate_kbps = 160124,
    .baseline_fps = 150,
    .startup_fps = 81,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .fps_needed_kbps = 126000,
  });

  auto native_app_switch_feedback = []() {
    stream_quality::feedback_t feedback {
      .duration_ms = 1085,
      .frames_seen = 80,
      .complete_frames = 80,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 1313664,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .late_frames = 0,
      .displayed_frames = 71,
      .visual_stale_frames = 0,
      .duplicate_frames = 71,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    };
    feedback.local_display_pressure = stream_quality::infer_local_display_pressure(feedback);
    return feedback;
  };

  EXPECT_EQ(native_app_switch_feedback().local_display_pressure, 0U)
    << "Clean duplicate-dominant Native/app-switch feedback is ALR/display reuse, not render pressure";

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback(native_app_switch_feedback());
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::local_render);
  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_GE(action.target_fps, 81)
    << "The LAN controller must not down-clamp clean duplicate-heavy app-switch feedback";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, CleanAlrReuseNearCurrentCadenceReachesFullHighRefreshFps) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 58227,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 104,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });

  auto clean_alr_reuse_feedback = [](int current_fps) {
    const auto displayed = static_cast<std::uint32_t>(
      std::max(1, static_cast<int>(std::lround(static_cast<double>(current_fps) * 0.96))));
    const auto frames = static_cast<std::uint32_t>(
      std::max(1, static_cast<int>(std::lround(static_cast<double>(current_fps) * 0.84))));
    const auto reused = std::max(4U, displayed > frames ? displayed - frames : 4U);
    const auto sparse_slip = std::max(1U, reused / 4U);
    stream_quality::feedback_t feedback {
      .duration_ms = 1000,
      .frames_seen = frames,
      .complete_frames = frames,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 1198208,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 2,
      .render_queue_depth = 2,
      .late_frames = sparse_slip,
      .displayed_frames = displayed,
      .visual_stale_frames = sparse_slip,
      .duplicate_frames = reused,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    };
    feedback.local_display_pressure = stream_quality::infer_local_display_pressure(feedback);
    return feedback;
  };

  EXPECT_EQ(clean_alr_reuse_feedback(120).local_display_pressure, 0U)
    << "Near-cadence last-frame reuse with shallow decode/render queues should be clean ALR, not render pressure";

  stream_quality::action_t action {};
  for (int i = 0; i < 6; ++i) {
    action = controller.on_feedback(clean_alr_reuse_feedback(controller.current_fps()));
  }

  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_EQ(action.target_fps, 150)
    << "Reference-client-style ALR probing should not leave a clean LAN 4K high-refresh session parked around 120-140fps";
  EXPECT_GT(action.target_bitrate_kbps, 120000)
    << "Clean app-limited reuse must probe bitrate upward too; low send bytes are not path capacity";
  EXPECT_LE(action.fec_percentage, 2);
}

TEST(StreamQualityControllerTests, GeminiScenarioMatrixClassifiesStrongLanAlrRandomLossCongestionAndLocalRender) {
  stream_quality::controller_t strong_lan;
  strong_lan.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });
  auto strong = strong_lan.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 150,
    .complete_frames = 150,
    .total_packets = 8000,
    .received_packets = 8000,
    .video_bytes = 21 * 1024 * 1024,
    .rtt_ms = 1,
    .rtt_variance_ms = 1,
    .displayed_frames = 150,
    .frame_area = 3840ULL * 2160ULL,
    .dirty_area = 3840ULL * 2160ULL,
    .full_frame_dirty = true,
  });
  EXPECT_EQ(strong.scenario, stream_quality::scenario_e::strong_lan);
  EXPECT_LE(strong.fec_percentage, 2);
  EXPECT_EQ(strong.target_fps, 150);

  stream_quality::controller_t alr;
  alr.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 58227,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 120,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .user_quality_kbps = 180000,
    .fps_needed_kbps = 180000,
  });
  auto clean_alr = alr.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 120,
    .complete_frames = 120,
    .video_bytes = 420 * 1024,
    .rtt_ms = 1,
    .rtt_variance_ms = 1,
    .decode_queue_depth = 1,
    .render_queue_depth = 1,
    .displayed_frames = 144,
    .duplicate_frames = 24,
    .frame_area = 3840ULL * 2160ULL,
    .dirty_area = 0,
  });
  EXPECT_EQ(clean_alr.scenario, stream_quality::scenario_e::clean_alr);
  EXPECT_GT(clean_alr.target_bitrate_kbps, 58227);
  EXPECT_LE(clean_alr.fec_percentage, 2);

  stream_quality::controller_t random_loss;
  random_loss.configure({
    .baseline_bitrate_kbps = 60000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 90000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
  });
  auto random = random_loss.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 120,
    .complete_frames = 112,
    .recovered_frames = 8,
    .missing_packets = 90,
    .total_packets = 3000,
    .received_packets = 2910,
    .video_bytes = 6 * 1024 * 1024,
    .rtt_ms = 12,
    .rtt_variance_ms = 2,
    .displayed_frames = 120,
  });
  EXPECT_EQ(random.scenario, stream_quality::scenario_e::random_loss);
  EXPECT_GT(random.fec_percentage, 10);
  EXPECT_NE(random.reason, stream_quality::reason_e::delay_congestion);

  stream_quality::controller_t congestion;
  congestion.configure({
    .baseline_bitrate_kbps = 60000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 90000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
  });
  auto congested = congestion.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 120,
    .complete_frames = 120,
    .video_bytes = 6 * 1024 * 1024,
    .rtt_ms = 260,
    .rtt_variance_ms = 95,
    .displayed_frames = 116,
    .delay_gradient_us = 24000,
    .interarrival_jitter_us = 9000,
    .delay_samples = 120,
    .delay_gradient_valid = true,
  });
  EXPECT_EQ(congested.scenario, stream_quality::scenario_e::delay_congestion);
  EXPECT_LE(congested.fec_percentage, 10);

  stream_quality::controller_t local_render;
  local_render.configure({
    .baseline_bitrate_kbps = 180000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 180000,
    .ceiling_total_bitrate_kbps = 220000,
    .baseline_fps = 150,
    .startup_fps = 150,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
  });
  auto render = local_render.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 150,
    .complete_frames = 150,
    .video_bytes = 20 * 1024 * 1024,
    .rtt_ms = 1,
    .rtt_variance_ms = 1,
    .decode_queue_depth = 1,
    .render_queue_depth = 2,
    .late_frames = 18,
    .displayed_frames = 150,
    .visual_stale_frames = 18,
    .local_display_pressure = 700,
    .frame_area = 3840ULL * 2160ULL,
  });
  EXPECT_EQ(render.scenario, stream_quality::scenario_e::local_render);
  EXPECT_EQ(render.sustainable_estimate_kbps, 180000)
    << "Local render-only pressure must not teach the link estimator that the network capacity dropped";
}

TEST(StreamQualityControllerTests, CleanLanNativeDisplayBackpressureAppliesFpsOnlyPacingBrake) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 145567,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 126000,
    .ceiling_total_bitrate_kbps = 160124,
    .baseline_fps = 150,
    .startup_fps = 146,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .fps_needed_kbps = 126000,
  });

  auto clean_alr_reuse_feedback = [](int current_fps) {
    const auto displayed = static_cast<std::uint32_t>(
      std::max(1, static_cast<int>(std::lround(static_cast<double>(current_fps) * 0.96))));
    const auto frames = static_cast<std::uint32_t>(
      std::max(1, static_cast<int>(std::lround(static_cast<double>(current_fps) * 0.84))));
    const auto reused = std::max(4U, displayed > frames ? displayed - frames : 4U);
    const auto sparse_slip = std::max(1U, reused / 4U);
    stream_quality::feedback_t feedback {
      .duration_ms = 1000,
      .frames_seen = frames,
      .complete_frames = frames,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 1198208,
      .rtt_ms = 1,
      .rtt_variance_ms = 1,
      .decode_queue_depth = 2,
      .render_queue_depth = 2,
      .late_frames = sparse_slip,
      .displayed_frames = displayed,
      .visual_stale_frames = sparse_slip,
      .duplicate_frames = reused,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
    };
    feedback.local_display_pressure = stream_quality::infer_local_display_pressure(feedback);
    return feedback;
  };

  for (int i = 0; i < 4; ++i) {
    (void) controller.on_feedback(clean_alr_reuse_feedback(controller.current_fps()));
  }

  EXPECT_GE(controller.current_fps(), 140);
  const auto fps_before_pressure = controller.current_fps();
  const auto bitrate_before_pressure = controller.current_bitrate_kbps();
  const auto fec_before_pressure = controller.current_fec_percentage();

  stream_quality::feedback_t native_display_backpressure {
    .duration_ms = 1080,
    .frames_seen = 130,
    .complete_frames = 130,
    .recovered_frames = 0,
    .unrecoverable_frames = 0,
    .missing_packets = 0,
    .total_packets = 0,
    .received_packets = 0,
    .video_bytes = 1275648,
    .rtt_ms = 1,
    .rtt_variance_ms = 0,
    .decode_queue_depth = 28,
    .render_queue_depth = 2,
    .late_frames = 1,
    .displayed_frames = 28,
    .visual_stale_frames = 0,
    .duplicate_frames = 25,
    .frame_area = 3840ULL * 2160ULL,
    .dirty_area = 0,
    .full_frame_dirty = false,
  };
  native_display_backpressure.local_display_pressure =
    stream_quality::infer_local_display_pressure(native_display_backpressure);

  EXPECT_EQ(native_display_backpressure.local_display_pressure, 1000U)
    << "This reproduces the native display-layer backpressure window from the clean-LAN trace";

  const auto action = controller.on_feedback(native_display_backpressure);

  EXPECT_EQ(action.scenario, stream_quality::scenario_e::local_render);
  EXPECT_EQ(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_NE(action.state, stream_quality::state_e::constrained);
  EXPECT_LT(action.target_fps, fps_before_pressure)
    << "Clean LAN local-render pressure should shed host pacing FPS before the client decode queue overflows";
  EXPECT_GE(action.target_fps, 120)
    << "Clean LAN display backpressure must not collapse the strong path to 60fps";
  EXPECT_GE(action.target_bitrate_kbps, bitrate_before_pressure)
    << "Local-render pressure is not network congestion, so video bitrate should not be cut";
  EXPECT_LE(action.fec_percentage, fec_before_pressure)
    << "Local-render pressure must not be treated as packet loss that needs extra FEC";
  EXPECT_EQ(action.resolution_scale_percent, 100)
    << "Local-render pressure must not trigger encoder resolution/profile downgrade";
  EXPECT_FALSE(action.profile_tier_changed)
    << "Local-render pressure should stay a pacing brake, not a profile churn signal";
  EXPECT_FALSE(action.runtime_scale_applied)
    << "Local-render pressure should not ask the encoder to reconfigure resolution";
  EXPECT_FALSE(action.request_idr)
    << "The local-render brake should avoid IDR snowballs rather than request another one";
}

TEST(StreamQualityControllerTests, OwdGradientClassifiesEarlyQueueGrowthAsDelayCongestionBeforeLoss) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 80000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 80000,
    .ceiling_total_bitrate_kbps = 120000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 3; ++i) {
    action = controller.on_feedback({
      .duration_ms = 500,
      .frames_seen = 60,
      .complete_frames = 60,
      .video_bytes = 5 * 1024 * 1024,
      .rtt_ms = 22,
      .rtt_variance_ms = 3,
      .displayed_frames = 60,
      .delay_gradient_us = 36000,
      .interarrival_jitter_us = 3500,
      .delay_samples = 60,
      .delay_gradient_valid = true,
    });
  }

  EXPECT_EQ(action.scenario, stream_quality::scenario_e::delay_congestion)
    << "Receiver-side queue growth should trip the stream-quality brake before packet loss starts";
  EXPECT_EQ(action.reason, stream_quality::reason_e::delay_congestion);
  EXPECT_LE(action.fec_percentage, 10)
    << "Delay-only congestion should shed or hold FEC, not add parity";
  EXPECT_GT(action.owd_gradient_us, 0);
  EXPECT_GT(action.owd_pressure, 0.0);
}

TEST(StreamQualityControllerTests, QoSPolicerLossBacksOffInsteadOfOpeningFecOrIdr) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 60000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 60000,
    .ceiling_total_bitrate_kbps = 90000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 60,
    .user_quality_kbps = 60000,
    .fps_needed_kbps = 60000,
  });

  auto action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 120,
    .complete_frames = 108,
    .recovered_frames = 0,
    .unrecoverable_frames = 12,
    .missing_packets = 420,
    .total_packets = 3000,
    .received_packets = 2580,
    .video_bytes = 6 * 1024 * 1024,
    .rtt_ms = 18,
    .rtt_variance_ms = 3,
    .displayed_frames = 104,
    .frame_area = 3024ULL * 1900ULL,
    .dirty_area = 3024ULL * 1900ULL,
    .full_frame_dirty = true,
  });

  EXPECT_EQ(action.scenario, stream_quality::scenario_e::qos_policer);
  EXPECT_EQ(action.reason, stream_quality::reason_e::qos_policer);
  EXPECT_LE(action.fec_percentage, 20)
    << "Policer-like loss is a send-rate problem; extra parity would inflate the bucket";
  EXPECT_LT(action.target_bitrate_kbps, 60000);
  EXPECT_FALSE(action.request_idr)
    << "Do not add a server-side IDR burst on top of a suspected token-bucket drop";
  EXPECT_LE(action.pacing_bitrate_kbps, 90000);
}

TEST(StreamQualityControllerTests, HandoverBlackholeFallsBackWithoutLearningZeroCapacity) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 90000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 120000,
    .baseline_fps = 120,
    .startup_fps = 72,
    .min_fps = 45,
    .frame_width = 3024,
    .frame_height = 1900,
    .user_quality_kbps = 90000,
    .fps_needed_kbps = 90000,
  });

  for (int i = 0; i < 4; ++i) {
    controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 120,
      .complete_frames = 120,
      .total_packets = 3200,
      .received_packets = 3200,
      .video_bytes = 10 * 1024 * 1024,
      .rtt_ms = 4,
      .rtt_variance_ms = 1,
      .displayed_frames = 120,
      .frame_area = 3024ULL * 1900ULL,
      .dirty_area = 3024ULL * 1900ULL,
      .full_frame_dirty = true,
    });
  }

  stream_quality::action_t action {};
  for (int i = 0; i < 2; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 0,
      .complete_frames = 0,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 0,
      .rtt_ms = 0,
      .rtt_variance_ms = 0,
      .displayed_frames = 0,
    });
  }

  EXPECT_EQ(action.scenario, stream_quality::scenario_e::handover);
  EXPECT_EQ(action.reason, stream_quality::reason_e::handover);
  EXPECT_EQ(action.availability, stream_quality::availability_e::low);
  EXPECT_LE(action.target_bitrate_kbps, 18000)
    << "A path blackhole should reset to a safe seed while the new route is probed";
  EXPECT_EQ(action.sustainable_estimate_kbps, 18000)
    << "No-delivery feedback must not be learned as zero physical bandwidth";
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_LE(action.target_fps, 72);
}

TEST(StreamQualityControllerTests, DisplayUnderrunStillReportsRenderPressure) {
  stream_quality::feedback_t feedback {
    .duration_ms = 1070,
    .frames_seen = 94,
    .complete_frames = 94,
    .recovered_frames = 0,
    .unrecoverable_frames = 0,
    .missing_packets = 0,
    .total_packets = 0,
    .received_packets = 0,
    .rtt_ms = 1,
    .rtt_variance_ms = 3,
    .decode_queue_depth = 1,
    .render_queue_depth = 2,
    .late_frames = 43,
    .displayed_frames = 64,
    .visual_stale_frames = 43,
    .duplicate_frames = 47,
    .frame_area = 3840ULL * 2160ULL,
  };

  EXPECT_GT(stream_quality::infer_local_display_pressure(feedback), 500U)
    << "If the display loop is presenting far fewer frames than it receives, keep treating it as local render pressure";
}

TEST(StreamQualityControllerTests, InfersLocalDisplayPressureFromRenderBackpressure) {
  const stream_quality::feedback_t feedback {
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 60,
    .decode_queue_depth = 5,
    .render_queue_depth = 4,
    .late_frames = 12,
    .displayed_frames = 20,
    .visual_stale_frames = 8,
    .duplicate_frames = 6,
  };

  EXPECT_GE(stream_quality::infer_local_display_pressure(feedback), 500U);
}

TEST(StreamQualityControllerTests, DampensLocalDisplayPressureWhenNetworkLossIsPresent) {
  stream_quality::feedback_t feedback {
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 40,
    .unrecoverable_frames = 12,
    .missing_packets = 160,
    .decode_queue_depth = 5,
    .render_queue_depth = 4,
    .late_frames = 12,
    .displayed_frames = 20,
    .visual_stale_frames = 8,
    .duplicate_frames = 6,
  };

  EXPECT_LT(stream_quality::infer_local_display_pressure(feedback), 500U);
}

TEST(StreamQualityControllerTests, TreatsFirstEmptyNoVideoFeedbackAsTransientStartupNoise) {
  stream_quality::feedback_t feedback {};
  feedback.duration_ms = 513;
  feedback.frames_seen = 0;
  feedback.complete_frames = 0;
  feedback.displayed_frames = 0;
  feedback.video_bytes = 0;
  feedback.total_packets = 0;
  feedback.missing_packets = 0;
  feedback.received_packets = 0;
  feedback.rtt_ms = 37;
  feedback.rtt_variance_ms = 9;

  EXPECT_TRUE(stream_quality::startup_feedback_is_empty_no_delivery(feedback));

  feedback.frames_seen = 1;
  EXPECT_FALSE(stream_quality::startup_feedback_is_empty_no_delivery(feedback));
}

TEST(StreamQualityControllerTests, DoesNotPromoteCleanReliableBacklogWithoutInputLatency) {
  stream_quality::feedback_t clean_media {};
  clean_media.duration_ms = 8140;
  clean_media.frames_seen = 51;
  clean_media.complete_frames = 51;
  clean_media.displayed_frames = 111;
  clean_media.duplicate_frames = 71;
  clean_media.local_display_pressure = 1000;
  clean_media.decode_queue_depth = 8;
  clean_media.render_queue_depth = 2;
  clean_media.rtt_ms = 77;
  clean_media.rtt_variance_ms = 29;
  clean_media.input_queue_depth = 0;
  clean_media.input_send_latency_us = 0;
  clean_media.input_ack_latency_us = 0;

  EXPECT_FALSE(stream_quality::transport_backlog_should_promote_input_queue(clean_media, 30));

  clean_media.input_queue_depth = 1;
  EXPECT_FALSE(stream_quality::transport_backlog_should_promote_input_queue(clean_media, 30));

  clean_media.input_send_latency_us = 32000;
  EXPECT_TRUE(stream_quality::transport_backlog_should_promote_input_queue(clean_media, 30));
}

TEST(StreamQualityControllerTests, StartupFecFloorBlocksEarlyDecreaseButAllowsIncrease) {
  EXPECT_TRUE(stream_quality::startup_fec_floor_should_block_decrease(true, 20, 10));
  EXPECT_FALSE(stream_quality::startup_fec_floor_should_block_decrease(true, 20, 20));
  EXPECT_FALSE(stream_quality::startup_fec_floor_should_block_decrease(true, 20, 25));
  EXPECT_FALSE(stream_quality::startup_fec_floor_should_block_decrease(false, 20, 10));
}

TEST(StreamQualityControllerTests, TierAndAvailabilityNamesAreStableForLogs) {
  EXPECT_STREQ(stream_quality::reason_name(stream_quality::reason_e::qos_policer), "qos-policer");
  EXPECT_STREQ(stream_quality::reason_name(stream_quality::reason_e::handover), "handover");
  EXPECT_STREQ(stream_quality::scenario_name(stream_quality::scenario_e::qos_policer), "qos-policer");
  EXPECT_STREQ(stream_quality::scenario_name(stream_quality::scenario_e::handover), "handover");
  EXPECT_STREQ(stream_quality::tier_name(stream_quality::tier_e::fast), "fast");
  EXPECT_STREQ(stream_quality::tier_name(stream_quality::tier_e::general), "general");
  EXPECT_STREQ(stream_quality::tier_name(stream_quality::tier_e::hd), "hd");
  EXPECT_STREQ(stream_quality::tier_name(stream_quality::tier_e::bluray), "bluray");
  EXPECT_STREQ(stream_quality::availability_name(stream_quality::availability_e::high), "high");
  EXPECT_STREQ(stream_quality::availability_name(stream_quality::availability_e::low), "low");
  EXPECT_STREQ(stream_quality::availability_name(stream_quality::availability_e::probing), "probing");
  EXPECT_STREQ(stream_quality::availability_name(stream_quality::availability_e::recovering), "recovering");
}

TEST(StreamQualityControllerTests, UuComparableCleanP2pMotionKeepsHighQualityEvidence) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 70000,
    .ceiling_total_bitrate_kbps = 150000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 126000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 0,
      .received_packets = 0,
      .video_bytes = 1400 * 1024,
      .rtt_ms = 16,
      .rtt_variance_ms = 2,
      .displayed_frames = 60,
      .visual_stale_frames = 0,
      .duplicate_frames = 0,
      .local_display_pressure = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::random_loss);
  EXPECT_NE(action.scenario, stream_quality::scenario_e::local_render);
  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_NE(action.reason, stream_quality::reason_e::render_deadline);
  EXPECT_GE(action.target_bitrate_kbps, 12000)
    << "Clean P2P-like evidence must not collapse to mosaic bitrate";
  EXPECT_GE(action.target_fps, 60)
    << "Clean P2P-like evidence must keep an interactive cadence";
  EXPECT_LE(action.fec_percentage, 10)
    << "Clean P2P-like evidence must not raise FEC as if media packets were missing";
  EXPECT_EQ(action.resolution_scale_percent, 100)
    << "Clean P2P-like evidence should keep native resolution unless real pressure appears";
}

TEST(StreamQualityControllerTests, CleanHighMotionDemandRaisesReadableFloorWithoutUnsafeLearning) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 30000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 12000,
    .ceiling_total_bitrate_kbps = 50000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 60,
    .startup_fps = 60,
    .min_fps = 30,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 30000,
    .ideal_demand_kbps = 30000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2000,
      .received_packets = 2000,
      .video_bytes = 1100 * 1024,
      .rtt_ms = 45,
      .rtt_variance_ms = 4,
      .decode_queue_depth = 1,
      .render_queue_depth = 2,
      .displayed_frames = 60,
      .visual_stale_frames = 0,
      .duplicate_frames = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::random_loss);
  EXPECT_GE(action.target_bitrate_kbps, 24000)
    << "Clean high-motion cadence needs readable 4K60 bitrate instead of staying at the startup floor";
  EXPECT_GE(action.target_fps, 60);
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_FALSE(action.unsafe_ceiling_active)
    << "Clean app-limited high-motion samples are demand evidence, not capacity proof";
  EXPECT_EQ(action.high_motion_unsafe_ceiling_kbps, 0);
}

TEST(StreamQualityControllerTests, PublicForwardNonDestructivePacketAccountingKeepsVideoReadable) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 70000,
    .ceiling_total_bitrate_kbps = 150000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 126000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 10; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1060,
      .frames_seen = 62,
      .complete_frames = 62,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 64086,
      .total_packets = 2000000,
      .received_packets = 1935914,
      .video_bytes = 150 * 1024,
      .rtt_ms = 21,
      .rtt_variance_ms = 8,
      .displayed_frames = 69,
      .visual_stale_frames = 0,
      .duplicate_frames = 8,
      .local_display_pressure = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 1166400,
      .full_frame_dirty = false,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::random_loss)
    << "Packet accounting conflicts with complete/displayed video must stay diagnostic";
  EXPECT_NE(action.reason, stream_quality::reason_e::random_loss);
  EXPECT_GE(action.target_bitrate_kbps, 8000)
    << "Clean visible cadence must not collapse into mosaic bitrate";
  EXPECT_GE(action.target_fps, 60);
  EXPECT_LE(action.fec_percentage, 10)
    << "Non-destructive packet accounting must not open a long FEC floor";
  EXPECT_EQ(action.resolution_scale_percent, 100);
}

TEST(StreamQualityControllerTests, PublicForwardAudioLateDropsWithoutConcealmentDoNotCrushVideo) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 70000,
    .ceiling_total_bitrate_kbps = 150000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 126000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  for (int i = 0; i < 10; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1060,
      .frames_seen = 62,
      .complete_frames = 62,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 2000000,
      .received_packets = 2000000,
      .video_bytes = 150 * 1024,
      .rtt_ms = 21,
      .rtt_variance_ms = 8,
      .displayed_frames = 69,
      .visual_stale_frames = 0,
      .duplicate_frames = 8,
      .local_display_pressure = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 1166400,
      .full_frame_dirty = false,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
      .late_audio_drops = 216,
      .audio_buffer_depth_ms = 140,
    });
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::audio_pressure)
    << "Late-drop-only audio accounting is not video congestion when audio has no underrun/conceal/PLC/fade";
  EXPECT_NE(action.reason, stream_quality::reason_e::audio_pressure);
  EXPECT_GE(action.target_bitrate_kbps, 8000);
  EXPECT_GE(action.target_fps, 60);
  EXPECT_LE(action.fec_percentage, 10);
  EXPECT_EQ(action.resolution_scale_percent, 100);
}

TEST(StreamQualityControllerTests, LowFpsLoadingScreenDoesNotPoisonFollowingGameCadence) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 70000,
    .ceiling_total_bitrate_kbps = 150000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 90,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 126000,
    .fps_needed_kbps = 30000,
  });

  for (int i = 0; i < 3; ++i) {
    controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 90,
      .video_bytes = 1500 * 1024,
      .rtt_ms = 18,
      .rtt_variance_ms = 4,
      .displayed_frames = 90,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  stream_quality::action_t loading_action {};
  for (int i = 0; i < 6; ++i) {
    loading_action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 1,
      .complete_frames = 1,
      .video_bytes = 64 * 1024,
      .rtt_ms = 18,
      .rtt_variance_ms = 4,
      .displayed_frames = 1,
      .duplicate_frames = 89,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 0,
      .full_frame_dirty = false,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }
  EXPECT_GE(loading_action.target_fps, 60)
    << "A source-limited loading screen must not be turned into a 1fps transport target";

  stream_quality::action_t action {};
  for (int i = 0; i < 4; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1000,
      .frames_seen = 90,
      .complete_frames = 90,
      .video_bytes = 1800 * 1024,
      .rtt_ms = 18,
      .rtt_variance_ms = 4,
      .displayed_frames = 90,
      .visual_stale_frames = 0,
      .duplicate_frames = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 3840ULL * 2160ULL,
      .full_frame_dirty = true,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::no_video_delivery);
  EXPECT_GE(action.target_bitrate_kbps, 12000)
    << "Clean game motion after loading must recover without requiring reconnect";
  EXPECT_GE(action.target_fps, 90);
  EXPECT_GE(action.resolution_scale_percent, 90);
}

TEST(StreamQualityControllerTests, PublicForwardMotionBurstDoesNotStayAtMosaicFloorAfterCleanRecovery) {
  stream_quality::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 126000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 35,
    .startup_bitrate_kbps = 11000,
    .ceiling_total_bitrate_kbps = 150000,
    .min_bitrate_kbps = 500,
    .baseline_fps = 150,
    .startup_fps = 60,
    .min_fps = 60,
    .frame_width = 3840,
    .frame_height = 2160,
    .chroma_sampling_type = 0,
    .runtime_profile_tier_supported = true,
    .user_quality_kbps = 126000,
    .ideal_demand_kbps = 126000,
    .fps_needed_kbps = 30000,
  });

  stream_quality::action_t action {};
  action = controller.on_feedback({
    .duration_ms = 1000,
    .frames_seen = 60,
    .complete_frames = 60,
    .video_bytes = 1200 * 1024,
    .rtt_ms = 54,
    .rtt_variance_ms = 9,
    .displayed_frames = 60,
    .duplicate_frames = 4,
    .frame_area = 3840ULL * 2160ULL,
    .dirty_area = 3840ULL * 2160ULL,
    .full_frame_dirty = true,
    .user_input_active = true,
    .path_lan_direct = false,
    .path_identity_confident = true,
  });

  for (int i = 0; i < 5; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1075,
      .frames_seen = 45,
      .complete_frames = 45,
      .recovered_frames = 18,
      .unrecoverable_frames = 0,
      .missing_packets = 22,
      .total_packets = 190,
      .received_packets = 168,
      .video_bytes = 260 * 1024,
      .rtt_ms = 52,
      .rtt_variance_ms = 12,
      .displayed_frames = 52,
      .duplicate_frames = 30,
      .decode_queue_depth = 1,
      .render_queue_depth = 0,
      .local_display_pressure = 350,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 1600000,
      .full_frame_dirty = false,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  action = controller.on_feedback({
    .duration_ms = 1060,
    .frames_seen = 44,
    .complete_frames = 40,
    .recovered_frames = 15,
    .unrecoverable_frames = 2,
    .missing_packets = 24,
    .total_packets = 172,
    .received_packets = 148,
    .video_bytes = 120 * 1024,
    .rtt_ms = 45,
    .rtt_variance_ms = 8,
    .displayed_frames = 52,
    .duplicate_frames = 29,
    .decode_queue_depth = 1,
    .render_queue_depth = 0,
    .rfi_requests = 1,
    .local_display_pressure = 350,
    .frame_area = 3840ULL * 2160ULL,
    .dirty_area = 1679616,
    .full_frame_dirty = false,
    .user_input_active = true,
    .path_lan_direct = false,
    .path_identity_confident = true,
  });

  EXPECT_GE(action.target_bitrate_kbps, 7000)
    << "A short public-forward media burst should rescue into a readable floor, not a 500 Kbps mosaic floor";
  EXPECT_GE(action.resolution_scale_percent, 90)
    << "Transient media damage should not immediately churn the encoder down to a visibly different cursor scale";

  for (int i = 0; i < 8; ++i) {
    action = controller.on_feedback({
      .duration_ms = 1075,
      .frames_seen = 60,
      .complete_frames = 60,
      .recovered_frames = 0,
      .unrecoverable_frames = 0,
      .missing_packets = 0,
      .total_packets = 320,
      .received_packets = 320,
      .video_bytes = 820 * 1024,
      .rtt_ms = 52,
      .rtt_variance_ms = 10,
      .displayed_frames = 60,
      .visual_stale_frames = 0,
      .duplicate_frames = 0,
      .decode_queue_depth = 1,
      .render_queue_depth = 0,
      .frame_area = 3840ULL * 2160ULL,
      .dirty_area = 1600000,
      .full_frame_dirty = false,
      .user_input_active = true,
      .path_lan_direct = false,
      .path_identity_confident = true,
    });
  }

  EXPECT_NE(action.scenario, stream_quality::scenario_e::random_loss);
  EXPECT_GE(action.target_bitrate_kbps, 12000)
    << "After clean motion cadence returns, the controller must climb out of mosaic bitrate without reconnecting";
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_FALSE(action.unsafe_ceiling_active)
    << "Clean post-burst cadence should release the stale unsafe ceiling instead of pinning dynamic scenes blurry";
}
