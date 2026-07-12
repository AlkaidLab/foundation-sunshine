/**
 * @file tests/unit/test_tray_state.cpp
 * @brief Tests for the tray state published to the GUI user-session agent.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>

#include <src/tray/tray_state.h>

class TrayStateTest: public ::testing::Test {
protected:
  void
  SetUp() override {
    tray_state::set_change_sink({});
    tray_state::clear_pairing_required();
    tray_state::clear_notification();
    tray_state::set_vdd_state(false, false, false, false);
    tray_state::set_vdd_confirmation(false);
    const auto operation_id = tray_state::begin_operation("test_reset");
    tray_state::complete_operation(operation_id, true);
    tray_state::set_idle();
  }

  void
  TearDown() override {
    tray_state::set_change_sink({});
  }
};

TEST_F(TrayStateTest, PublishesStreamingLifecycle) {
  const auto initial_revision = tray_state::get().revision;

  tray_state::set_streaming("Desktop");
  auto state = tray_state::get();
  EXPECT_EQ(state.status, tray_state::status_e::streaming);
  EXPECT_EQ(state.icon, "playing");
  EXPECT_EQ(state.tooltip, "Streaming Desktop");
  EXPECT_EQ(state.app_name, "Desktop");
  EXPECT_GT(state.revision, initial_revision);
  EXPECT_GT(state.updated_at_ms, 0);

  tray_state::set_paused("Desktop");
  state = tray_state::get();
  EXPECT_EQ(state.status, tray_state::status_e::paused);
  EXPECT_EQ(state.icon, "pausing");
  EXPECT_EQ(state.tooltip, "Stream paused: Desktop");

  tray_state::set_idle("Desktop");
  state = tray_state::get();
  EXPECT_EQ(state.status, tray_state::status_e::idle);
  EXPECT_EQ(state.icon, "default");
  EXPECT_EQ(state.tooltip, "Sunshine");
  EXPECT_EQ(state.app_name, "Desktop");
}

TEST_F(TrayStateTest, SerializesPairingAndVddState) {
  tray_state::set_vdd_state(true, true, false, true);
  tray_state::set_vdd_confirmation(true, 42);
  tray_state::set_pairing_required("Moonlight Client");

  const auto json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "pairing");
  EXPECT_EQ(json.at("icon"), "locked");
  EXPECT_EQ(json.at("pairing_client_name"), "Moonlight Client");
  EXPECT_TRUE(json.at("notification").at("active"));
  EXPECT_GT(json.at("notification").at("id").get<std::uint64_t>(), 0);
  EXPECT_EQ(json.at("notification").at("action"), "open_pin");
  EXPECT_TRUE(json.at("vdd").at("active"));
  EXPECT_TRUE(json.at("vdd").at("keep_enabled"));
  EXPECT_FALSE(json.at("vdd").at("headless_create_enabled"));
  EXPECT_TRUE(json.at("vdd").at("cooldown"));
  EXPECT_TRUE(json.at("vdd").at("awaiting_confirmation"));
  EXPECT_EQ(json.at("vdd").at("confirmation_operation_id"), 42);

  tray_state::set_vdd_confirmation(false);
  const auto cleared_vdd = tray_state::to_json().at("vdd");
  EXPECT_FALSE(cleared_vdd.at("awaiting_confirmation"));
  EXPECT_EQ(cleared_vdd.at("confirmation_operation_id"), 0);

  tray_state::clear_pairing_required();
  const auto restored = tray_state::to_json();
  EXPECT_EQ(restored.at("status"), "idle");
  EXPECT_EQ(restored.at("icon"), "default");
  EXPECT_FALSE(restored.at("notification").at("active"));
}

TEST_F(TrayStateTest, PairingClearsPreviousNotificationContent) {
  tray_state::set_notification("Audio warning", "Audio device unavailable", "default");
  tray_state::set_pairing_required("Moonlight Client");

  const auto notification = tray_state::to_json().at("notification");
  EXPECT_TRUE(notification.at("title").get<std::string>().empty());
  EXPECT_TRUE(notification.at("message").get<std::string>().empty());
  EXPECT_EQ(notification.at("action"), "open_pin");
}

TEST_F(TrayStateTest, PairingDoesNotDestroyStreamingState) {
  tray_state::set_streaming("Desktop");
  tray_state::set_pairing_required("Moonlight Client");

  auto json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "pairing");
  EXPECT_EQ(json.at("icon"), "locked");

  tray_state::set_paused("Desktop");
  json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "pairing");

  tray_state::clear_pairing_required();
  json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "paused");
  EXPECT_EQ(json.at("icon"), "pausing");
  EXPECT_EQ(json.at("tooltip"), "Stream paused: Desktop");
}

TEST_F(TrayStateTest, NotificationDoesNotDestroyStreamingState) {
  tray_state::set_streaming("Desktop");
  tray_state::set_notification("Audio warning", "Audio device unavailable", "default");

  auto json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "notification");
  EXPECT_EQ(json.at("tooltip"), "Audio warning");

  tray_state::clear_notification();
  json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "streaming");
  EXPECT_EQ(json.at("icon"), "playing");
  EXPECT_EQ(json.at("tooltip"), "Streaming Desktop");
}

TEST_F(TrayStateTest, OldNotificationTimerCannotClearNewNotification) {
  const auto old_id = tray_state::set_notification("Old", "Old notification");
  const auto new_id = tray_state::set_notification("New", "New notification");

  tray_state::clear_notification_if(old_id);
  auto json = tray_state::to_json();
  EXPECT_EQ(json.at("status"), "notification");
  EXPECT_EQ(json.at("tooltip"), "New");

  tray_state::clear_notification_if(new_id);
  json = tray_state::to_json();
  EXPECT_NE(json.at("status"), "notification");
}

TEST_F(TrayStateTest, PublishesStableProtocolIdentity) {
  const auto first = tray_state::to_json();
  const auto second = tray_state::to_json();

  EXPECT_EQ(first.at("protocol_version"), tray_state::protocol_version);
  EXPECT_FALSE(first.at("instance_id").get<std::string>().empty());
  EXPECT_EQ(first.at("instance_id"), second.at("instance_id"));
  EXPECT_TRUE(first.at("owner") == "gui" || first.at("owner") == "core" || first.at("owner") == "disabled");
  EXPECT_TRUE(first.at("capabilities").is_array());
  EXPECT_NE(
    std::find(first.at("capabilities").begin(), first.at("capabilities").end(), "events-v1"),
    first.at("capabilities").end());
  EXPECT_NE(
    std::find(first.at("capabilities").begin(), first.at("capabilities").end(), "shutdown"),
    first.at("capabilities").end());
}

TEST_F(TrayStateTest, PublishesChangesToTheEventSink) {
  std::atomic<int> changes { 0 };
  tray_state::set_change_sink([&changes]() {
    ++changes;
  });

  tray_state::set_idle();
  const auto notification_id = tray_state::set_notification("Notice", "Changed");
  tray_state::clear_notification_if(notification_id + 1);
  EXPECT_EQ(changes.load(), 2);

  tray_state::clear_notification_if(notification_id);
  EXPECT_EQ(changes.load(), 3);
}

TEST_F(TrayStateTest, PublishesAsynchronousOperationLifecycle) {
  const auto operation_id = tray_state::begin_operation("vdd_create");
  auto operation = tray_state::to_json().at("operation");
  EXPECT_EQ(operation.at("id"), operation_id);
  EXPECT_EQ(operation.at("action"), "vdd_create");
  EXPECT_EQ(operation.at("state"), "running");

  tray_state::complete_operation(operation_id + 1, false, {}, "stale");
  EXPECT_EQ(tray_state::to_json().at("operation").at("state"), "running");

  tray_state::complete_operation(operation_id, false, {}, "create failed");
  operation = tray_state::to_json().at("operation");
  EXPECT_EQ(operation.at("state"), "failed");
  EXPECT_EQ(operation.at("error"), "create failed");
}

TEST_F(TrayStateTest, AcknowledgesOnlyCurrentNonPairingNotification) {
  const auto old_id = tray_state::set_notification("Old", "Old notification");
  const auto current_id = tray_state::set_notification("Current", "Current notification");

  EXPECT_FALSE(tray_state::acknowledge_notification(old_id));
  EXPECT_TRUE(tray_state::to_json().at("notification").at("active"));
  EXPECT_TRUE(tray_state::acknowledge_notification(current_id));
  EXPECT_FALSE(tray_state::to_json().at("notification").at("active"));

  tray_state::set_pairing_required("Moonlight Client");
  const auto pairing_id = tray_state::to_json().at("notification").at("id").get<std::uint64_t>();
  EXPECT_FALSE(tray_state::acknowledge_notification(pairing_id));
  EXPECT_TRUE(tray_state::to_json().at("notification").at("active"));
  tray_state::clear_pairing_required();
}
