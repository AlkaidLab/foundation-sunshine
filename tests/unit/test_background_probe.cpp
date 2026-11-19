/**
 * @file tests/unit/test_background_probe.cpp
 * @brief The encoder probe of the start on its own thread: the order of the states, the gate, the bound on a request that waits for it, and a probe that never returns.
 */
#include "src/background_probe.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using gpu_guard::probe_e;

namespace {
  struct states_t {
    std::mutex mutex;
    std::vector<probe_e> seen;
    std::function<void(probe_e)>
    fn() {
      return [this](probe_e p) {
        std::lock_guard lock(mutex);
        seen.push_back(p);
      };
    }
  };
}  // namespace

TEST(BackgroundProbe, StatesGoRunningThenDoneAndTheGateOpens) {
  startup::gate_t gate;
  states_t states;
  EXPECT_TRUE(startup::run_probe([] { return false; }, gate, states.fn()));
  EXPECT_EQ(states.seen, (std::vector<probe_e> { probe_e::running, probe_e::done }));
  EXPECT_TRUE(gate.is_open());
}

TEST(BackgroundProbe, AProbeThatFindsNothingIsFailedAndStillOpensTheGate) {
  startup::gate_t gate;
  states_t states;
  EXPECT_FALSE(startup::run_probe([] { return true; }, gate, states.fn()));
  EXPECT_EQ(states.seen.back(), probe_e::failed);
  EXPECT_TRUE(gate.is_open());
}

TEST(BackgroundProbe, AnExceptionIsAFailureNotACrash) {
  startup::gate_t gate;
  states_t states;
  EXPECT_FALSE(startup::run_probe([]() -> bool { throw std::runtime_error("driver"); }, gate, states.fn()));
  EXPECT_EQ(states.seen.back(), probe_e::failed);
  EXPECT_TRUE(gate.is_open());
}

TEST(BackgroundProbe, TheStartDoesNotWaitForTheProbe) {
  // what the thread touches is shared with it: a thread that is let go after a failed assertion must not read a finished test
  auto gate = std::make_shared<startup::gate_t>();
  auto states = std::make_shared<states_t>();
  auto release = std::make_shared<std::atomic<bool>>(false);
  startup::bounded_thread_t task;
  task.start([gate, states, release] { startup::run_probe([release] { while (!*release) std::this_thread::sleep_for(1ms); return false; }, *gate, states->fn()); });
  // start() returned with the probe still running: the tray and the ready state can come up
  EXPECT_FALSE(gate->is_open());
  *release = true;
  EXPECT_TRUE(task.finish(30s));
  EXPECT_TRUE(gate->is_open());
}

TEST(BackgroundProbe, ARequestWaitsForTheProbeButOnlyForItsBound) {
  auto gate = std::make_shared<startup::gate_t>();
  auto states = std::make_shared<states_t>();
  auto release = std::make_shared<std::atomic<bool>>(false);
  startup::bounded_thread_t task { 30s };
  task.start([gate, states, release] { startup::run_probe([release] { while (!*release) std::this_thread::sleep_for(1ms); return false; }, *gate, states->fn()); });
  EXPECT_FALSE(gate->wait_for(150ms));  // a probe that is not over in time: the request is told that the host is still starting
  *release = true;
  EXPECT_TRUE(gate->wait_for(30s));  // and a request that comes later gets the result
  EXPECT_TRUE(task.finish(30s));
}

TEST(BackgroundProbe, AProbeThatNeverReturnsDoesNotHoldTheEndOfTheProcess) {
  auto gate = std::make_shared<startup::gate_t>();
  auto states = std::make_shared<states_t>();
  auto stuck = std::make_shared<std::atomic<bool>>(true);  // outlives the test: the let-go thread still reads them
  {
    startup::bounded_thread_t task { 100ms };
    task.start([gate, stuck, states] { startup::run_probe([stuck] { while (*stuck) std::this_thread::sleep_for(1ms); return false; }, *gate, states->fn()); });
    EXPECT_FALSE(task.finish(100ms));  // let go
    EXPECT_FALSE(task.running());
    EXPECT_FALSE(gate->is_open());
    // the destructor of a thread that was let go waits for nothing
  }
  *stuck = false;  // the detached thread ends by itself
  EXPECT_TRUE(gate->wait_for(30s));
}

TEST(BackgroundProbe, TheDestructorJoinsAFinishedThreadAndLetsGoOfAStuckOne) {
  std::atomic<bool> done { false };
  {
    startup::bounded_thread_t task { 5s };
    task.start([&] { done = true; });
  }
  EXPECT_TRUE(done.load());
  auto stuck = std::make_shared<std::atomic<bool>>(true);
  const auto begin = std::chrono::steady_clock::now();
  {
    startup::bounded_thread_t task { 100ms };
    task.start([stuck] { while (*stuck) std::this_thread::sleep_for(1ms); });
  }
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
  *stuck = false;
}

TEST(BackgroundProbe, FinishWithoutAThreadIsHarmless) {
  startup::bounded_thread_t task;
  EXPECT_FALSE(task.finish(10ms));
  EXPECT_FALSE(task.running());
}
