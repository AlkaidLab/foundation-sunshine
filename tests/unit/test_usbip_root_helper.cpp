/**
 * @file tests/unit/test_usbip_root_helper.cpp
 * @brief Budgets, one repair at a time, status reuse, and a bounded child read.
 */
#include "src/confighttp/usbip_root_helper.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <unistd.h>
#ifdef _WIN32
  // MinGW has read and close in unistd.h, but the pipe comes from the CRT.
  #include <fcntl.h>
  #include <io.h>
  #define pipe(fds) _pipe((fds), 4096, _O_BINARY)
#endif

namespace {
  namespace ur = confighttp::usbip_root;

  TEST(UsbipRootHelper, BudgetsStayFinite) {
    EXPECT_EQ(ur::budget_for(ur::action::status).count(), 15000);
    EXPECT_EQ(ur::budget_for(ur::action::repair).count(), 60000);
    EXPECT_EQ(ur::status_reuse.count(), 5000);
    EXPECT_EQ(ur::budget_for(ur::action::unknown).count(), 0);
    EXPECT_EQ(ur::wait_milliseconds(std::chrono::milliseconds(-5)), 0ul);
    EXPECT_EQ(ur::wait_milliseconds(std::chrono::milliseconds(0)), 0ul);
    EXPECT_EQ(ur::wait_milliseconds(ur::status_budget), 15000ul);
    EXPECT_EQ(ur::wait_milliseconds(ur::repair_budget), 60000ul);
    EXPECT_EQ(ur::wait_milliseconds(std::chrono::milliseconds(0x1ffffffffLL)), ur::finite_wait_limit);
    EXPECT_NE(ur::finite_wait_limit, 0xfffffffful);
    EXPECT_FALSE(ur::should_terminate(ur::wait_outcome::exited));
    EXPECT_TRUE(ur::should_terminate(ur::wait_outcome::timed_out));
    EXPECT_TRUE(ur::should_terminate(ur::wait_outcome::failed));
  }

  TEST(UsbipRootHelper, OnlyTheTwoActionsAreAccepted) {
    EXPECT_EQ(ur::parse_action("status"), ur::action::status);
    EXPECT_EQ(ur::parse_action("repair"), ur::action::repair);
    EXPECT_EQ(ur::parse_action("repair;calc"), ur::action::unknown);
    EXPECT_EQ(ur::parse_action("status && reboot"), ur::action::unknown);
    EXPECT_EQ(ur::parse_action(""), ur::action::unknown);
    EXPECT_STREQ(ur::action_argument(ur::action::status), "status");
    EXPECT_STREQ(ur::action_argument(ur::action::repair), "repair");
    for (const char *argument : ur::fixed_powershell_arguments) {
      const std::string text(argument);
      EXPECT_EQ(text.find_first_of(" ;|&<>$"), std::string::npos) << text;
    }
  }

  TEST(UsbipRootHelper, FailuresAddOneFlagAndTheFallbackNote) {
    const auto timed = ur::timeout_failure();
    const auto busy = ur::busy_failure();
    EXPECT_STREQ(ur::extra_flag(timed), "timed_out");
    EXPECT_FALSE(timed.busy);
    EXPECT_TRUE(timed.timed_out);
    EXPECT_STREQ(ur::extra_flag(busy), "busy");
    EXPECT_TRUE(busy.busy);
    EXPECT_FALSE(busy.timed_out);
    EXPECT_NE(std::string(timed.ds4_fallback).find("DS4 fallback remains active"), std::string::npos);
    EXPECT_NE(std::string(busy.ds4_fallback).find("DS4 fallback remains active"), std::string::npos);
    EXPECT_NE(std::string(timed.error).find("timed out"), std::string::npos);
  }

  TEST(UsbipRootHelper, AnExitedChildIsNotKilled) {
    std::istringstream input("noise\r\n{\"root_present\":true}\r\n");
    unsigned long seen = 0;
    int kills = 0;
    const auto ran = ur::read_bounded(
      input,
      {},
      ur::status_budget,
      [&](unsigned long milliseconds) {
        seen = milliseconds;
        return ur::wait_outcome::exited;
      },
      [&] { ++kills; },
      ur::reader_grace);
    EXPECT_EQ(seen, 15000ul);
    EXPECT_EQ(kills, 0);
    EXPECT_EQ(ran.kill_calls, 0);
    EXPECT_EQ(ran.outcome, ur::wait_outcome::exited);
    EXPECT_EQ(ran.last_line, "{\"root_present\":true}");
    EXPECT_TRUE(ran.reader_finished);
    EXPECT_FALSE(ran.handed_off);
  }

  class pipe_buf: public std::streambuf {
  public:
    explicit pipe_buf(int fd):
        fd_(fd) {}

  protected:
    int_type
    underflow() override {
      if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
      const auto n = ::read(fd_, buffer_, sizeof buffer_);
      if (n <= 0) return traits_type::eof();
      setg(buffer_, buffer_, buffer_ + n);
      return traits_type::to_int_type(*gptr());
    }

  private:
    int fd_;
    char buffer_[32];
  };

  struct pipe_hold {
    int read_fd;
    pipe_buf buf;
    std::istream in;
    explicit pipe_hold(int fd):
        read_fd(fd), buf(fd), in(&buf) {}
  };

  TEST(UsbipRootHelper, TimeoutStopsOnlyThatChildAndUnblocksTheReader) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::pipe(fds), 0);
    auto hold = std::make_shared<pipe_hold>(fds[0]);
    const auto started = std::chrono::steady_clock::now();
    const auto ran = ur::read_bounded(
      hold->in,
      hold,
      ur::repair_budget,
      [](unsigned long milliseconds) {
        EXPECT_EQ(milliseconds, 60000ul);
        return ur::wait_outcome::timed_out;
      },
      [&] {
        if (fds[1] >= 0) {
          ::close(fds[1]);
          fds[1] = -1;
        }
      },
      std::chrono::milliseconds(2000));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(ran.kill_calls, 1);
    EXPECT_EQ(ran.outcome, ur::wait_outcome::timed_out);
    EXPECT_TRUE(ran.reader_finished);
    EXPECT_FALSE(ran.handed_off);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    if (fds[0] >= 0) ::close(fds[0]);
    if (fds[1] >= 0) ::close(fds[1]);
  }

  TEST(UsbipRootHelper, ABlockedReaderIsHandedOffInsideTheGrace) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::pipe(fds), 0);
    auto hold = std::make_shared<pipe_hold>(fds[0]);
    const auto started = std::chrono::steady_clock::now();
    const auto ran = ur::read_bounded(
      hold->in,
      hold,
      ur::status_budget,
      [](unsigned long) { return ur::wait_outcome::failed; },
      [] {},
      std::chrono::milliseconds(200));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(ran.kill_calls, 1);
    EXPECT_FALSE(ran.reader_finished);
    EXPECT_TRUE(ran.handed_off);
    EXPECT_GE(elapsed, std::chrono::milliseconds(150));
    EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
    if (fds[1] >= 0) ::close(fds[1]);
    if (fds[0] >= 0) ::close(fds[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  struct manual_now {
    std::chrono::steady_clock::time_point *current = nullptr;
    std::chrono::steady_clock::time_point
    operator()() const {
      return *current;
    }
  };

  TEST(UsbipRootHelper, StatusIsReusedForAFewSeconds) {
    auto now = std::chrono::steady_clock::now();
    ur::gate<manual_now> gate { manual_now { &now } };
    const auto first = gate.acquire_status(std::chrono::milliseconds(0));
    EXPECT_EQ(first.take, ur::gate<manual_now>::status_take::start);
    gate.finish_status("{\"root_present\":true}");
    now += std::chrono::milliseconds(4999);
    const auto again = gate.acquire_status(std::chrono::milliseconds(0));
    EXPECT_EQ(again.take, ur::gate<manual_now>::status_take::reuse);
    EXPECT_EQ(again.body, "{\"root_present\":true}");
    now += std::chrono::milliseconds(1);
    const auto stale = gate.acquire_status(std::chrono::milliseconds(0));
    EXPECT_EQ(stale.take, ur::gate<manual_now>::status_take::start);
    gate.cancel_status();
  }

  TEST(UsbipRootHelper, ASecondRepairIsBusyAtOnce) {
    ur::gate<> gate;
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(gate.begin_repair(std::chrono::seconds(5)), ur::gate<>::repair_take::granted);
    EXPECT_EQ(gate.begin_repair(std::chrono::seconds(5)), ur::gate<>::repair_take::busy);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
    gate.finish_repair("{\"root_present\":true}");
    const auto status = gate.acquire_status(std::chrono::milliseconds(0));
    EXPECT_EQ(status.take, ur::gate<>::status_take::reuse);
    EXPECT_EQ(status.body, "{\"root_present\":true}");
  }

  TEST(UsbipRootHelper, StatusDoesNotStartWhileARepairRuns) {
    ur::gate<> gate;
    ASSERT_EQ(gate.begin_repair(std::chrono::milliseconds(0)), ur::gate<>::repair_take::granted);
    auto waiter = std::async(std::launch::async, [&] {
      return gate.acquire_status(std::chrono::seconds(2));
    });
    gate.finish_repair("{\"cached\":1}");
    const auto ticket = waiter.get();
    EXPECT_EQ(ticket.take, ur::gate<>::status_take::reuse);
    EXPECT_EQ(ticket.body, "{\"cached\":1}");
  }

  TEST(UsbipRootHelper, ASecondStatusWaitsForTheInFlightResult) {
    ur::gate<> gate;
    const auto first = gate.acquire_status(std::chrono::milliseconds(0));
    ASSERT_EQ(first.take, ur::gate<>::status_take::start);
    auto waiter = std::async(std::launch::async, [&] {
      return gate.acquire_status(std::chrono::seconds(2));
    });
    gate.finish_status("{\"n\":2}");
    const auto second = waiter.get();
    EXPECT_EQ(second.take, ur::gate<>::status_take::reuse);
    EXPECT_EQ(second.body, "{\"n\":2}");
  }

  TEST(UsbipRootHelper, RepairStartsAfterTheStatusFinishes) {
    ur::gate<> gate;
    const auto status = gate.acquire_status(std::chrono::milliseconds(0));
    ASSERT_EQ(status.take, ur::gate<>::status_take::start);
    std::thread finisher([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      gate.finish_status("{\"from\":\"status\"}");
    });
    const auto take = gate.begin_repair(std::chrono::seconds(2));
    finisher.join();
    EXPECT_EQ(take, ur::gate<>::repair_take::granted);
    gate.cancel_repair();
  }

  TEST(UsbipRootHelper, RepairIsBusyWhenStatusDoesNotFinish) {
    ur::gate<> gate;
    const auto status = gate.acquire_status(std::chrono::milliseconds(0));
    ASSERT_EQ(status.take, ur::gate<>::status_take::start);
    const auto started = std::chrono::steady_clock::now();
    const auto take = gate.begin_repair(std::chrono::milliseconds(80));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(take, ur::gate<>::repair_take::busy);
    EXPECT_GE(elapsed, std::chrono::milliseconds(60));
    EXPECT_LT(elapsed, std::chrono::milliseconds(1000));
    gate.cancel_status();
  }
}  // namespace
