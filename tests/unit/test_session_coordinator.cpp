/**
 * @file tests/unit/test_session_coordinator.cpp
 * @brief Tests for the serialized active-session lifecycle coordinator.
 */

#include <algorithm>
#include <array>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "src/session_coordinator.h"

#include "../tests_common.h"

namespace {
  const session_coord::record_t *
  find_record(const session_coord::snapshot_t &snapshot, std::uint32_t session_id) {
    auto record = std::ranges::find(snapshot.records, session_id, &session_coord::record_t::session_id);
    return record == snapshot.records.end() ? nullptr : &*record;
  }
}  // namespace

TEST(SessionCoordinator, KeepsLifecycleMonotonicAndClosed) {
  boost::asio::io_context context;
  session_coord::coordinator_t coordinator { context.get_executor() };

  coordinator.observe({ 1, "cert-a", session_coord::state_e::starting, {} });
  coordinator.observe({ 1, "cert-a", session_coord::state_e::active, {} });
  coordinator.observe({ 1, "cert-a", session_coord::state_e::draining, "control_timeout" });
  coordinator.observe({ 1, "cert-a", session_coord::state_e::closed, "host_terminate" });
  coordinator.observe({ 1, "cert-a", session_coord::state_e::active, {} });
  context.run();

  const auto snapshot = coordinator.snapshot();
  const auto *record = find_record(*snapshot, 1);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->state, session_coord::state_e::closed);
  EXPECT_EQ(record->stop_reason, "control_timeout");
  EXPECT_EQ(snapshot->version, 4U);
  EXPECT_EQ(snapshot->rejected_events, 0U);
}

TEST(SessionCoordinator, RejectsAuthenticatedOwnerChanges) {
  boost::asio::io_context context;
  session_coord::coordinator_t coordinator { context.get_executor() };

  coordinator.observe({ 2, "cert-a", session_coord::state_e::starting, {} });
  coordinator.observe({ 2, "cert-b", session_coord::state_e::active, {} });
  coordinator.observe({ 2, "cert-a", session_coord::state_e::active, {} });
  context.run();

  const auto snapshot = coordinator.snapshot();
  const auto *record = find_record(*snapshot, 2);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->client_uuid, "cert-a");
  EXPECT_EQ(record->state, session_coord::state_e::active);
  EXPECT_EQ(snapshot->rejected_events, 1U);
}

TEST(SessionCoordinator, SerializesConcurrentObservers) {
  boost::asio::io_context context;
  session_coord::coordinator_t coordinator { context.get_executor() };

  std::array<std::thread, 4> producers {
    std::thread { [&] { coordinator.observe({ 3, "cert-c", session_coord::state_e::starting, {} }); } },
    std::thread { [&] { coordinator.observe({ 3, "cert-c", session_coord::state_e::active, {} }); } },
    std::thread { [&] { coordinator.observe({ 3, "cert-c", session_coord::state_e::draining, "client_cancel" }); } },
    std::thread { [&] { coordinator.observe({ 3, "cert-c", session_coord::state_e::closed, "client_cancel" }); } },
  };
  for (auto &producer : producers) {
    producer.join();
  }

  std::array<std::thread, 4> runners {
    std::thread { [&] { context.run(); } },
    std::thread { [&] { context.run(); } },
    std::thread { [&] { context.run(); } },
    std::thread { [&] { context.run(); } },
  };
  for (auto &runner : runners) {
    runner.join();
  }

  const auto snapshot = coordinator.snapshot();
  const auto *record = find_record(*snapshot, 3);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->state, session_coord::state_e::closed);
  EXPECT_EQ(record->client_uuid, "cert-c");
  EXPECT_EQ(snapshot->rejected_events, 0U);
}

TEST(SessionCoordinator, BoundsClosedTombstones) {
  boost::asio::io_context context;
  session_coord::coordinator_t coordinator { context.get_executor() };

  constexpr auto session_count = static_cast<std::uint32_t>(session_coord::MAX_CLOSED_TOMBSTONES + 32);
  for (std::uint32_t id = 1; id <= session_count; ++id) {
    coordinator.observe({ id, "cert", session_coord::state_e::closed, "host_terminate" });
  }
  context.run();

  const auto snapshot = coordinator.snapshot();
  EXPECT_EQ(snapshot->records.size(), session_coord::MAX_CLOSED_TOMBSTONES);
  EXPECT_EQ(find_record(*snapshot, 1), nullptr);
  EXPECT_NE(find_record(*snapshot, session_count), nullptr);
}
