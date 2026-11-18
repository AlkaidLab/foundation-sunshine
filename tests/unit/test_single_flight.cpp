#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "src/single_flight.h"

using namespace std::chrono_literals;

namespace {
  constexpr std::int64_t TTL = 60;
  constexpr std::int64_t REUSE = 2;
}  // namespace

TEST(SingleFlight, FourCallersAtOnceShareOneRunAndAllGetItsAnswer) {
  single_flight::cell_t<int> cell;
  std::atomic<int> runs { 0 };
  std::vector<std::thread> threads;
  std::vector<int> answers(4, 0);
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&, i]() {
      answers[i] = cell.get("k", true, 100, TTL, REUSE, [&]() {
        ++runs;
        std::this_thread::sleep_for(300ms);
        return 7;
      });
    });
    std::this_thread::sleep_for(30ms);  // each one comes in while the first still runs
  }
  for (auto &thread : threads) thread.join();
  const auto took = std::chrono::steady_clock::now() - started;
  EXPECT_EQ(runs.load(), 1);
  for (const auto answer : answers) EXPECT_EQ(answer, 7);
  EXPECT_LT(took, 700ms);  // not four runs one behind the other (1200 ms)
}

TEST(SingleFlight, ADifferentKeyIsADifferentProbe) {
  single_flight::cell_t<int> cell;
  std::atomic<int> runs { 0 };
  std::thread other([&]() { cell.get("b", true, 100, TTL, REUSE, [&]() { ++runs; std::this_thread::sleep_for(150ms); return 2; }); });
  std::this_thread::sleep_for(30ms);
  const auto value = cell.get("a", true, 100, TTL, REUSE, [&]() { ++runs; return 1; });
  other.join();
  EXPECT_EQ(value, 1);
  EXPECT_EQ(runs.load(), 2);
}

TEST(SingleFlight, AResultAnswersCallersForTheTtlAndARefreshOnlyRightAfterIt) {
  single_flight::cell_t<int> cell;
  int runs = 0;
  const auto run = [&]() { return ++runs; };
  EXPECT_EQ(cell.get("k", false, 100, TTL, REUSE, run), 1);
  EXPECT_EQ(cell.get("k", false, 130, TTL, REUSE, run), 1);  // inside the ttl
  EXPECT_EQ(cell.get("k", true, 101, TTL, REUSE, run), 1);  // a refresh right behind a finished run: that run is the answer
  EXPECT_EQ(cell.get("k", true, 103, TTL, REUSE, run), 2);  // a refresh after that asks again
  EXPECT_EQ(cell.get("k", false, 170, TTL, REUSE, run), 3);  // the ttl is over
  EXPECT_EQ(cell.get("other", false, 171, TTL, REUSE, run), 4);  // another key never reads this result
}

TEST(SingleFlight, AFailureReachesEveryWaiterAndTheNextCallTriesAgain) {
  single_flight::cell_t<int> cell;
  std::atomic<int> thrown { 0 };
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back([&]() {
      try {
        cell.get("k", true, 100, TTL, REUSE, []() -> int { std::this_thread::sleep_for(200ms); throw std::runtime_error("probe failed"); });
      }
      catch (const std::runtime_error &) {
        ++thrown;
      }
    });
    std::this_thread::sleep_for(30ms);
  }
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(thrown.load(), 3);
  EXPECT_EQ(cell.get("k", true, 110, TTL, REUSE, []() { return 9; }), 9);
}

TEST(SingleFlight, ASlowProbeOfAnotherKeyDoesNotHideARunningOneFromASecondCaller) {
  single_flight::cell_t<int> cell;
  std::atomic<int> runs_a { 0 };
  std::atomic<int> runs_b { 0 };
  std::thread first([&]() { cell.get("a", true, 100, TTL, REUSE, [&]() { ++runs_a; std::this_thread::sleep_for(400ms); return 1; }); });
  std::this_thread::sleep_for(40ms);
  std::thread other([&]() { cell.get("b", true, 100, TTL, REUSE, [&]() { ++runs_b; std::this_thread::sleep_for(100ms); return 2; }); });
  std::this_thread::sleep_for(40ms);
  const auto value = cell.get("a", true, 100, TTL, REUSE, [&]() { ++runs_a; return 3; });  // joins the first run of "a", it does not start a second
  first.join();
  other.join();
  EXPECT_EQ(value, 1);
  EXPECT_EQ(runs_a.load(), 1);
  EXPECT_EQ(runs_b.load(), 1);
}

TEST(SingleFlight, TheReuseWindowCountsFromTheEndOfAProbeNotItsStart) {
  single_flight::cell_t<int> cell;
  int runs = 0;
  // a probe that began at second 100 and took a bit over a second: it ended at about 101, so a refresh at 102 is inside the two seconds
  EXPECT_EQ(cell.get("k", true, 100, TTL, REUSE, [&]() { std::this_thread::sleep_for(1100ms); return ++runs; }), 1);
  EXPECT_EQ(cell.get("k", true, 102, TTL, REUSE, [&]() { return ++runs; }), 1);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(cell.get("k", true, 104, TTL, REUSE, [&]() { return ++runs; }), 2);
}
