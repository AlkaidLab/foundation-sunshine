#include "src/streaming/display_recovery.h"

#include <gtest/gtest.h>
#include <memory>

TEST(DisplayRecovery, VirtualTargetWaitsUntilItsNewDisplayAppears) {
  const std::vector<std::string> absent { "physical" };
  EXPECT_FALSE(streaming::select_recovery_display(absent, "", "physical", true));
  EXPECT_FALSE(streaming::select_recovery_display(absent, "old-virtual", "physical", true));
  const std::vector<std::string> restored { "physical", "new-virtual" };
  EXPECT_EQ(streaming::select_recovery_display(restored, "new-virtual", "physical", true), "new-virtual");
}

TEST(DisplayRecovery, PhysicalFallbackAndManualSwitchRemainAvailable) {
  const std::vector<std::string> displays { "physical", "other" };
  EXPECT_EQ(streaming::select_recovery_display(displays, "missing", "physical", false), "physical");
  EXPECT_EQ(streaming::select_recovery_display(displays, "other", "physical", false), "other");
}

TEST(DisplayRecovery, FailedOrSkippedConfigurationDoesNotCommit) {
  int width = 100;
  EXPECT_FALSE(streaming::commit_display_resolution(true, false, [&] { width = 200; }));
  EXPECT_FALSE(streaming::commit_display_resolution(false, true, [&] { width = 200; }));
  EXPECT_EQ(width, 100);
  EXPECT_TRUE(streaming::commit_display_resolution(true, true, [&] { width = 200; }));
  EXPECT_EQ(width, 200);
}

TEST(DisplayRecovery, StoppedSessionsAreJoinedWhileActiveSessionsKeepWaiting) {
  struct session_t {
    bool stopped;
    int *joins;
  };
  int joins_a = 0;
  int joins_b = 0;
  std::vector<std::unique_ptr<session_t>> sessions;
  sessions.push_back(std::make_unique<session_t>(true, &joins_a));
  sessions.push_back(std::make_unique<session_t>(false, &joins_b));
  auto prune = [&] {
    return streaming::retain_active_recovery_sessions(sessions, [](const auto &session) { return session.stopped; }, [](auto &session) { ++*session.joins; });
  };
  EXPECT_TRUE(prune());
  ASSERT_EQ(sessions.size(), 1u);
  EXPECT_EQ(joins_a, 1);
  EXPECT_EQ(joins_b, 0);
  sessions.front()->stopped = true;
  EXPECT_FALSE(prune());
  EXPECT_TRUE(sessions.empty());
  EXPECT_EQ(joins_a, 1);
  EXPECT_EQ(joins_b, 1);
}

TEST(DisplayRecovery, NewSessionInheritsOnlyFromActiveSession) {
  struct session_t {
    bool stopped;
    std::string target;
  };
  std::vector<std::unique_ptr<session_t>> sessions;
  sessions.push_back(std::make_unique<session_t>(true, "vdd"));
  auto choose = [&] {
    return streaming::recovery_session_target(sessions, "physical", [](const auto &session) { return session.stopped; }, [](const auto &session) { return session.target; });
  };
  EXPECT_EQ(choose(), "physical");
  sessions.push_back(std::make_unique<session_t>(false, "active"));
  EXPECT_EQ(choose(), "active");
  sessions.clear();
  EXPECT_EQ(choose(), "physical");
}
