#include "src/weak_net_controller.h"

#include <gtest/gtest.h>

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
  controller.configure({ .baseline_bitrate_kbps = 10000, .baseline_fec_percentage = 80 });

  EXPECT_LE(controller.current_fec_percentage(), weak_net::controller_t::max_fec_percentage);
  EXPECT_LE(controller.current_fec_percentage(), 35);
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
  EXPECT_GE(controller.pacing_bitrate_kbps(), 15000);
  EXPECT_LE(controller.pacing_bitrate_kbps(), 16200);
  EXPECT_GE(action.pacing_bitrate_kbps, 15000);
  EXPECT_LE(action.pacing_bitrate_kbps, 16200);
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
  EXPECT_GT(action.target_bitrate_kbps, 90000);
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

TEST(WeakNetControllerTests, HighRefreshCrisisCanDropBelowSixtyAndRecoverGradually) {
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
    });
  }

  const auto crisis_fps = action.target_fps;
  EXPECT_LT(crisis_fps, 60);
  EXPECT_GE(crisis_fps, 24);

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

  auto action = controller.on_feedback({
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
  });

  EXPECT_EQ(action.state, weak_net::state_e::constrained);
  EXPECT_EQ(action.target_bitrate_kbps, 20000);
  EXPECT_EQ(action.fec_percentage, 10);
  EXPECT_LT(action.target_fps, 120);
  EXPECT_GE(action.target_fps, 110);
}

TEST(WeakNetControllerTests, HighRefreshDefaultCanRecoverFromEmergencyLowFps) {
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

  EXPECT_LT(action.target_fps, 60);
  EXPECT_GE(action.target_fps, 24);

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

  EXPECT_EQ(action.fec_percentage, weak_net::controller_t::max_fec_percentage);

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
