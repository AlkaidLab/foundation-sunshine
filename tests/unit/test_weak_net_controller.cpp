#include "src/weak_net_controller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

TEST(WeakNetControllerTests, DropsBitrateAndRaisesFecWhenFramesAreUnrecoverable) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_TRUE(action.changed);
  EXPECT_LT(action.target_bitrate_kbps, 50000);
  EXPECT_GT(action.fec_percentage, 20);
  EXPECT_TRUE(action.request_idr);
}

TEST(WeakNetControllerTests, RecoversTowardBaselineWithoutExceedingUserSpec) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::recovering);
  EXPECT_TRUE(action.changed);
  EXPECT_LE(action.target_bitrate_kbps, 50000);
  EXPECT_LE(action.fec_percentage, weak_net::controller_t::max_fec_percentage);
}

TEST(WeakNetControllerTests, CapsHighConfiguredFecToSafeBudget) {
  weak_net::controller_t controller;
  controller.configure({ .baseline_bitrate_kbps = 10000, .baseline_fec_percentage = 100 });

  EXPECT_LE(controller.current_fec_percentage(), weak_net::controller_t::max_fec_percentage);
  EXPECT_LE(controller.current_fec_percentage(), 100);
}

TEST(WeakNetControllerTests, RaisesFecTowardConfiguredMaximumOnlyWhenNeeded) {
  weak_net::controller_t controller;
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
  EXPECT_LE(action.fec_percentage, weak_net::controller_t::max_fec_percentage);
  EXPECT_LE(action.fec_percentage, 80);
}

TEST(WeakNetControllerTests, AppliesFecOverheadOnceToPacingBudget) {
  weak_net::controller_t controller;
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

TEST(WeakNetControllerTests, ReducesFpsBeforeCrushingBitrateWhenQueuesMissDeadline) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_LT(action.target_fps, 60);
  EXPECT_GE(action.target_fps, 24);
  EXPECT_GE(action.target_bitrate_kbps, 16000);
  EXPECT_LE(action.fec_percentage, weak_net::controller_t::max_fec_percentage);
}

TEST(WeakNetControllerTests, RecoversFpsAndBitrateAfterStableWindows) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::recovering);
  EXPECT_LE(action.target_bitrate_kbps, 20000);
  EXPECT_LE(action.target_fps, 60);
  EXPECT_GT(action.target_fps, 24);
}

TEST(WeakNetControllerTests, TreatsConfiguredBitrateAsCeilingAndStartsFromSafeWorkingPoint) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::recovering);
  EXPECT_GT(action.target_bitrate_kbps, 20000);
  EXPECT_LT(action.target_bitrate_kbps, 120000);
  EXPECT_LE(action.target_bitrate_kbps, 35000);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(WeakNetControllerTests, ContinuouslyProbesTowardCeilingWhenNetworkAndQueuesStayHealthy) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 120000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 24,
  });

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::recovering);
  EXPECT_GT(action.target_bitrate_kbps, 70000);
  EXPECT_LT(action.target_bitrate_kbps, 90000);
  EXPECT_LE(action.target_bitrate_kbps, 120000);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(WeakNetControllerTests, LowBitrateCeilingDoesNotLockHighRefreshFpsWhenFeedbackIsHealthy) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::healthy);
  EXPECT_EQ(action.target_bitrate_kbps, 3250);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(WeakNetControllerTests, HighRefreshCrisisKeepsEmergencyInteractiveFloorAndRecoversGradually) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
  });

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::recovering);
  EXPECT_GT(action.target_fps, crisis_fps);
  EXPECT_LT(action.target_fps, crisis_fps + 20);
}

TEST(WeakNetControllerTests, KeepsFpsStableForTransientNetworkLossWithoutRenderPressure) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_LT(action.target_bitrate_kbps, 60000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_LE(action.fec_percentage, weak_net::controller_t::max_fec_percentage);
}

TEST(WeakNetControllerTests, PreservesBitrateForRenderPressureWithoutNetworkLoss) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  const weak_net::feedback_t feedback {
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LT(action.target_fps, 120);
  EXPECT_GE(action.target_fps, 110);
}

TEST(WeakNetControllerTests, InputOnlyPressureDoesNotReconfigureVideoTransport) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(WeakNetControllerTests, DoesNotRaiseFecForDelayOnlyCongestion) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LT(action.target_bitrate_kbps, 20000);
}

TEST(WeakNetControllerTests, DoesNotRequestIdrForDelayOnlyQueueingCrisis) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_FALSE(action.request_idr);
  EXPECT_EQ(action.fec_percentage, 10);
}

TEST(WeakNetControllerTests, ScalesFecLinearlyWithObservedRandomLoss) {
  weak_net::controller_t mild_controller;
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

  weak_net::controller_t severe_controller;
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

TEST(WeakNetControllerTests, AudioUnderrunsConstrainVideoWithoutRaisingFecOrIdr) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_LT(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(WeakNetControllerTests, AudioContinuityPressureConstrainVideoWithoutUnderruns) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_EQ(action.reason, weak_net::reason_e::audio_pressure);
  EXPECT_LT(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(WeakNetControllerTests, SustainedAudioUnderrunsApplyGentleLinearBitratePressure) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 20000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 30,
    .startup_bitrate_kbps = 20000,
    .baseline_fps = 120,
    .min_fps = 45,
  });

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_LT(action.target_bitrate_kbps, 20000);
  EXPECT_GE(action.target_bitrate_kbps, 17000);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_FALSE(action.request_idr);
}

TEST(WeakNetControllerTests, AudioOnlyPressureCannotCollapseVideoWhenNetworkIsClean) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.reason, weak_net::reason_e::audio_pressure);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_GE(action.target_bitrate_kbps, 15300);
}

TEST(WeakNetControllerTests, AudioOnlyPressureDoesNotBlockVisualRecoveryAfterLoss) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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
  ASSERT_EQ(action.state, weak_net::state_e::crisis);
  ASSERT_LT(action.target_bitrate_kbps, 10000);
  ASSERT_LT(action.resolution_scale_percent, 100);

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

  EXPECT_EQ(action.reason, weak_net::reason_e::audio_pressure);
  EXPECT_GE(action.target_bitrate_kbps, 15000);
  EXPECT_GE(action.target_fps, 90);
  EXPECT_GE(action.resolution_scale_percent, 90);
}

TEST(WeakNetControllerTests, RaisesFecForRandomLossWithAdaptiveHeadroom) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_GT(action.fec_percentage, 10);
  EXPECT_LE(action.fec_percentage, 30);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(WeakNetControllerTests, HighRefreshDefaultUsesSixtyFpsEmergencyFloorAndRecovers) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 5000,
    .baseline_fec_percentage = 10,
    .startup_bitrate_kbps = 5000,
    .baseline_fps = 120,
  });

  weak_net::action_t action {};
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

TEST(WeakNetControllerTests, RecoveredOnlyRandomLossRaisesFecWithoutCrushingInteractiveVideo) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 18000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 18000,
    .ceiling_total_bitrate_kbps = 22000,
    .baseline_fps = 120,
    .startup_fps = 120,
  });

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_GE(action.fec_percentage, 35);
  EXPECT_GE(action.target_bitrate_kbps, 12000);
  EXPECT_GE(action.target_fps, 90);
  EXPECT_GT(action.fec_budget_kbps, 0);
  EXPECT_GT(action.encoding_budget_kbps, action.fec_budget_kbps);
  EXPECT_GT(action.recovered_loss, 0.20);
  EXPECT_EQ(action.unrecoverable_loss, 0.0);
}

TEST(WeakNetControllerTests, SustainedUnrecoverableBurstsCanOpenFecTowardFullParity) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 50000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 100,
    .startup_bitrate_kbps = 50000,
    .ceiling_total_bitrate_kbps = 100000,
    .baseline_fps = 120,
    .startup_fps = 120,
  });

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_EQ(action.reason, weak_net::reason_e::random_loss);
  EXPECT_GE(action.fec_percentage, 80);
  EXPECT_LE(action.fec_percentage, 100);
  EXPECT_GE(action.target_fps, 90);
  EXPECT_GE(action.target_bitrate_kbps, 9000);
  EXPECT_GT(action.fec_efficiency, 0.0);
}

TEST(WeakNetControllerTests, RecoveryDoesNotExceedTotalCeilingWhileFecIsElevated) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

TEST(WeakNetControllerTests, ManualHighCeilingRecoversTowardSustainableEstimateAfterCongestion) {
  weak_net::controller_t controller;
  controller.configure({
    .baseline_bitrate_kbps = 1000000,
    .baseline_fec_percentage = 10,
    .max_fec_percentage = 80,
    .startup_bitrate_kbps = 30000,
    .baseline_fps = 120,
    .startup_fps = 120,
    .min_fps = 45,
  });

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_EQ(action.reason, weak_net::reason_e::random_loss);

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
  EXPECT_EQ(action.reason, weak_net::reason_e::recovering);
}

TEST(WeakNetControllerTests, FullFrameMotionPressurePrefersScaleBeforePptFps) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.reason, weak_net::reason_e::motion_pressure);
  EXPECT_GE(action.pressures.motion, 0.85);
  EXPECT_GE(action.target_fps, 90);
  EXPECT_LE(action.resolution_scale_percent, 85);
  EXPECT_EQ(action.chroma_sampling_type, 0);
  EXPECT_TRUE(action.profile_tier_changed);
  EXPECT_TRUE(action.profile_tier_deferred);
  EXPECT_FALSE(action.profile_tier_supported);
}

TEST(WeakNetControllerTests, ProfileTierFallbackLowersFpsSmoothlyWhenRuntimeScaleCannotApply) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.reason, weak_net::reason_e::motion_pressure);
  EXPECT_TRUE(action.profile_tier_changed);
  EXPECT_TRUE(action.profile_tier_deferred);
  EXPECT_FALSE(action.profile_tier_supported);
  EXPECT_LT(action.target_fps, 120);
  EXPECT_GE(action.target_fps, 72);
  EXPECT_EQ(action.fec_percentage, 10);
}

TEST(WeakNetControllerTests, DelayOnlyCongestionReducesPacingWithoutOpeningFullFec) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_GE(action.pressures.delay_congestion, 0.80);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LT(action.pacing_bitrate_kbps, 25000);
  EXPECT_GE(action.target_fps, 60);
}


TEST(WeakNetControllerTests, QueueBackpressureWithJitterCapsTotalAndDoesNotRaiseFec) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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
  EXPECT_EQ(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LE(total_kbps, 13500);
  EXPECT_LE(action.pacing_bitrate_kbps, 14000);
  EXPECT_LT(action.target_bitrate_kbps, 18000);
}

TEST(WeakNetControllerTests, RenderOnlyBackpressureDoesNotMasqueradeAsNetworkDelayCongestion) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.reason, weak_net::reason_e::render_deadline);
  EXPECT_NE(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_GE(action.pacing_bitrate_kbps, 15000);
}

TEST(WeakNetControllerTests, RfiStormIsCooledDownAndForcesCrisisTier) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(first.state, weak_net::state_e::crisis);
  EXPECT_TRUE(first.request_idr);
  EXPECT_EQ(second.state, weak_net::state_e::crisis);
  EXPECT_FALSE(second.request_idr);
  EXPECT_GE(second.pressures.burst_loss, 0.95);
  EXPECT_LE(second.resolution_scale_percent, 75);
}

TEST(WeakNetControllerTests, IsolatedRfiWithoutLossDoesNotTriggerRandomLossCrisis) {
  weak_net::controller_t controller;
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

  EXPECT_NE(action.state, weak_net::state_e::crisis);
  EXPECT_NE(action.reason, weak_net::reason_e::random_loss);
  EXPECT_FALSE(action.request_idr);
  EXPECT_LT(action.pressures.burst_loss, 0.50);
  EXPECT_GE(action.target_bitrate_kbps, 7500);
  EXPECT_EQ(action.target_fps, 120);
}

TEST(WeakNetControllerTests, StaticIdleWithoutVideoSamplesDoesNotReduceForRttJitter) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_NE(action.reason, weak_net::reason_e::random_loss);
  EXPECT_NE(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_FALSE(action.request_idr);
  EXPECT_EQ(action.target_bitrate_kbps, 18182);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_EQ(action.pressures.delay_congestion, 0.0);
  EXPECT_EQ(action.pressures.motion, 0.0);
  EXPECT_EQ(action.pressures.render, 0.0);
}

TEST(WeakNetControllerTests, RecoveredOnlyLossDoesNotConsumeHalfOfManualCeilingWithFec) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.reason, weak_net::reason_e::random_loss);
  EXPECT_EQ(action.unrecoverable_loss, 0.0);
  EXPECT_LE(action.fec_percentage, 60);
  EXPECT_GE(action.target_bitrate_kbps, 11500);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(WeakNetControllerTests, ManualHighCeilingTreatsRecoveredLossAsFecSignalNotProbePermission) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.reason, weak_net::reason_e::random_loss);
  EXPECT_EQ(action.unrecoverable_loss, 0.0);
  EXPECT_LE(action.fec_percentage, 60);
  EXPECT_LE(action.target_bitrate_kbps, 20000);
  EXPECT_LE(action.pacing_bitrate_kbps, 32000);
}

TEST(WeakNetControllerTests, FullFrameMotionDoesNotCollapseToUnreadableBitrateWhenProfileFallbackIsDeferred) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

TEST(WeakNetControllerTests, DelayOnlyCongestionBleedsOffElevatedFecBeforeCrushingEncodingBudget) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_LT(action.fec_percentage, 35);
  EXPECT_GE(action.target_bitrate_kbps, 7200);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(WeakNetControllerTests, TotalCeilingBoundsEncodingAndFecForAnyConfiguredBitrate) {
  const int ceilings[] = { 1000, 5000, 20000, 100000, 1000000 };

  for (const auto ceiling_kbps : ceilings) {
    weak_net::controller_t controller;
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

    weak_net::action_t action {};
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

TEST(WeakNetControllerTests, IneffectiveLossRecoveryReducesFecInsteadOfBlindlyOpeningParity) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_EQ(action.reason, weak_net::reason_e::random_loss);
  EXPECT_EQ(action.fec_efficiency, 0.0);
  EXPECT_LE(action.fec_percentage, 25);
  EXPECT_LT(action.target_bitrate_kbps, 12000);
  EXPECT_LE(action.pacing_bitrate_kbps, 20000);
}

TEST(WeakNetControllerTests, BandwidthLimitedLinkCutsTotalBudgetRatherThanAddingFec) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_LT(action.fec_percentage, 30);
  EXPECT_LT(action.target_bitrate_kbps, 50000);
  EXPECT_LE(action.pacing_bitrate_kbps, 60000);
}

TEST(WeakNetControllerTests, GoodNetworkRecoversTowardCeilingWithoutQualityPenalty) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_EQ(action.target_fps, 120);
  EXPECT_EQ(action.resolution_scale_percent, 100);
  EXPECT_EQ(action.chroma_sampling_type, 1);
  EXPECT_GT(action.target_bitrate_kbps, 70000);
  EXPECT_LE(action.pacing_bitrate_kbps, 200000);
}

TEST(WeakNetControllerTests, MotionReadableFloorCannotOverrideWeakRouteSustainableLimit) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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
  ASSERT_EQ(action.state, weak_net::state_e::crisis);
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

TEST(WeakNetControllerTests, SustainedRfiAudioCrisisUsesTightSustainableBudget) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.state, weak_net::state_e::crisis);
  EXPECT_TRUE(action.rfi_limited);
  EXPECT_GE(action.pressures.audio, 0.95);
  EXPECT_LE(action.fec_percentage, 70);
  EXPECT_LE(action.pacing_bitrate_kbps, 10000);
  EXPECT_LE(action.effective_ceiling_kbps, action.sustainable_estimate_kbps + 3000);
  EXPECT_GE(action.target_fps, 60);
}

TEST(WeakNetControllerTests, StartupAudioUnderrunsWithoutVideoSamplesDoNotProbeToCeiling) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.reason, weak_net::reason_e::audio_pressure);
  EXPECT_EQ(action.packet_loss, 0.0);
  EXPECT_LE(action.target_bitrate_kbps, 15000);
  EXPECT_LE(action.pacing_bitrate_kbps, 18000);
  EXPECT_LT(action.target_bitrate_kbps, action.requested_ceiling_kbps / 2);
}

TEST(WeakNetControllerTests, StrongCleanNetworkCanExceedUserQualityBudgetToProtectHighRefreshFps) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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
  EXPECT_EQ(action.fec_percentage, 10);
}

TEST(WeakNetControllerTests, CongestionImmediatelyReturnsOvershootTowardUserQualityBudget) {
  weak_net::controller_t controller;
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

  weak_net::action_t action {};
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

  EXPECT_EQ(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_LE(action.target_bitrate_kbps, 6500);
  EXPECT_LE(action.fec_percentage, 10);
}

TEST(WeakNetControllerTests, RfiRecoveryRampsMediaParametersWithoutSawtoothJumps) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(first_crisis.state, weak_net::state_e::crisis);
  EXPECT_GE(first_crisis.unrecoverable_loss, 0.5);
  EXPECT_EQ(second_crisis.state, weak_net::state_e::crisis);
  EXPECT_LE(recovery_probe.target_bitrate_kbps,
            second_crisis.target_bitrate_kbps + std::max(1200, second_crisis.target_bitrate_kbps / 5));
  EXPECT_LE(std::abs(recovery_probe.fec_percentage - second_crisis.fec_percentage), 8);
  EXPECT_LE(std::abs(recovery_probe.target_fps - second_crisis.target_fps), 8);
  EXPECT_LE(recovery_probe.pacing_bitrate_kbps,
            second_crisis.pacing_bitrate_kbps + std::max(1600, second_crisis.pacing_bitrate_kbps / 4));
  EXPECT_GT(clean_probe.target_bitrate_kbps, first_crisis.target_bitrate_kbps);
}

TEST(WeakNetControllerTests, DelayOnlyCongestionCapsTotalNearUserBudget) {
  weak_net::controller_t controller;
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

  weak_net::action_t action;
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
  EXPECT_EQ(action.reason, weak_net::reason_e::delay_congestion);
  EXPECT_LE(total_kbps, 13000);
  EXPECT_LE(action.pacing_bitrate_kbps, 13500);
}

TEST(WeakNetControllerTests, RandomRecoveredLossCanRaiseFecWithoutDelayOvershoot) {
  weak_net::controller_t controller;
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

  EXPECT_EQ(action.reason, weak_net::reason_e::random_loss);
  EXPECT_GT(action.fec_percentage, 10);
  EXPECT_GE(action.target_fps, 72);
}

TEST(WeakNetControllerTests, FullResFallbackDoesNotStayAtMosaicBitrateWhenNetworkIsClean) {
  weak_net::controller_t controller;
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

  weak_net::action_t action;
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
  EXPECT_TRUE(action.profile_tier_deferred);
}
