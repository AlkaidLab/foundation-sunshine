/**
 * @file tests/unit/test_session_cleanup_gate.cpp
 * @brief Tests for ordered session cleanup and launch handoff.
 */

#include <future>

#include <gtest/gtest.h>

#include "src/session_cleanup_gate.h"

using namespace std::chrono_literals;

TEST(SessionCleanupGate, DefersCleanupUntilLaunchOperationCompletes) {
  rtsp_stream::session_cleanup_gate_t gate;

  gate.begin_launch();
  EXPECT_FALSE(gate.request_cleanup());
  EXPECT_EQ(gate.claim_cleanup(true), rtsp_stream::session_cleanup_gate_t::cleanup_claim_e::none);

  EXPECT_TRUE(gate.end_launch());
  EXPECT_EQ(gate.claim_cleanup(true), rtsp_stream::session_cleanup_gate_t::cleanup_claim_e::acquired);
  gate.finish_cleanup();
}

TEST(SessionCleanupGate, PublishedSessionCancelsDeferredCleanup) {
  rtsp_stream::session_cleanup_gate_t gate;

  gate.begin_launch();
  EXPECT_FALSE(gate.request_cleanup());
  gate.cancel_cleanup();
  EXPECT_FALSE(gate.end_launch());
  EXPECT_EQ(gate.claim_cleanup(true), rtsp_stream::session_cleanup_gate_t::cleanup_claim_e::none);
}

TEST(SessionCleanupGate, BlocksNewLaunchWhileCleanupRuns) {
  rtsp_stream::session_cleanup_gate_t gate;
  ASSERT_TRUE(gate.request_cleanup());
  ASSERT_EQ(gate.claim_cleanup(true), rtsp_stream::session_cleanup_gate_t::cleanup_claim_e::acquired);

  auto launch_started = std::async(std::launch::async, [&gate]() {
    gate.begin_launch();
    gate.end_launch();
  });
  EXPECT_EQ(launch_started.wait_for(50ms), std::future_status::timeout);

  gate.finish_cleanup();
  EXPECT_EQ(launch_started.wait_for(1s), std::future_status::ready);
}

TEST(SessionCleanupGate, AbandonsCleanupWhenHostIsNoLongerIdle) {
  rtsp_stream::session_cleanup_gate_t gate;

  ASSERT_TRUE(gate.request_cleanup());
  EXPECT_EQ(gate.claim_cleanup(false), rtsp_stream::session_cleanup_gate_t::cleanup_claim_e::cancelled);

  gate.begin_launch();
  EXPECT_FALSE(gate.end_launch());
}
