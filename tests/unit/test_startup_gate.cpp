/**
 * @file tests/unit/test_startup_gate.cpp
 * @brief The gates the servers wait on while the host finishes starting.
 */
#include <src/startup_gate.h>

#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(StartupGate, WaitsUntilOpenedAndReleasesEveryWaiter) {
  startup::gate_t gate;
  EXPECT_FALSE(gate.is_open());
  EXPECT_FALSE(gate.wait_for(20ms)) << "a closed gate runs out its bound";
  std::atomic<int> released { 0 };
  std::vector<std::thread> waiters;
  for (int i = 0; i < 4; ++i) {
    waiters.emplace_back([&] { if (gate.wait_for(5s)) ++released; });
  }
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(released.load(), 0);
  gate.open();
  for (auto &t : waiters) t.join();
  EXPECT_EQ(released.load(), 4);
  EXPECT_TRUE(gate.wait_for(0ms)) << "an open gate does not wait";
  gate.open();
  EXPECT_TRUE(gate.is_open()) << "opening twice changes nothing";
}

TEST(StartupGate, TheBoundIsRespectedAndAnOpenGateAnswersAtOnce) {
  startup::gate_t gate;
  const auto before = std::chrono::steady_clock::now();
  EXPECT_FALSE(gate.wait_for(50ms));
  EXPECT_GE(std::chrono::steady_clock::now() - before, 45ms);
  gate.open();
  const auto open_before = std::chrono::steady_clock::now();
  EXPECT_TRUE(gate.wait_for(5s));
  EXPECT_LT(std::chrono::steady_clock::now() - open_before, 500ms);
}

TEST(StartupGate, AStreamNeedsBothTheProbeAndTheInputDevices) {
  // the shared gates are process-wide: this test is the only one that opens them
  EXPECT_FALSE(startup::await_stream_ready(20ms));
  startup::probe_gate().open();
  EXPECT_FALSE(startup::await_stream_ready(20ms)) << "the probe alone is not enough";
  std::thread input([] { std::this_thread::sleep_for(60ms); startup::input_gate().open(); });
  EXPECT_TRUE(startup::await_stream_ready(5s));
  input.join();
  EXPECT_TRUE(startup::await_stream_ready(0ms));
}

TEST(StartupPhase, WritesOneLineWithTheDurationAndTheTimeline) {
  std::vector<std::string> lines;
  {
    startup::phase_t phase("probe", [&](const std::string &line) { lines.push_back(line); });
    std::this_thread::sleep_for(30ms);
  }
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].rfind("[startup] probe ", 0), 0u);
  EXPECT_NE(lines[0].find(" ms (at +"), std::string::npos);
  EXPECT_GE(std::stoi(lines[0].substr(std::string("[startup] probe ").size())), 25);
}
