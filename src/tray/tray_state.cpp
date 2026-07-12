#include "tray_state.h"

#include <chrono>
#include <mutex>
#include <random>
#include <sstream>
#include <utility>

namespace tray_state {

  namespace {

    std::mutex state_mutex;
    state_t current_state;
    std::uint64_t next_notification_id = 0;
    std::uint64_t next_operation_id = 0;
    std::mutex change_sink_mutex;
    change_sink_t change_sink;
    std::int64_t
    now_ms() {
      const auto now = std::chrono::system_clock::now().time_since_epoch();
      return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    const std::string &
    instance_id() {
      static const auto value = []() {
        std::random_device random;
        std::ostringstream id;
        id << std::hex << now_ms() << '-' << random() << random();
        return id.str();
      }();
      return value;
    }

    const char *
    tray_owner() {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      return "core";
#elif defined SUNSHINE_GUI_TRAY && SUNSHINE_GUI_TRAY >= 1
      return "gui";
#else
      return "disabled";
#endif
    }

    const char *
    status_name(status_e status) {
      switch (status) {
        case status_e::idle:
          return "idle";
        case status_e::streaming:
          return "streaming";
        case status_e::paused:
          return "paused";
      }
      return "idle";
    }

    struct presentation_t {
      const char *status;
      std::string icon;
      std::string tooltip;
    };

    presentation_t
    presentation(const state_t &state) {
      if (state.pairing_pending) {
        return {
          "pairing",
          "locked",
          state.pairing_client_name.empty() ? "Pairing request" : "Pairing request: " + state.pairing_client_name,
        };
      }

      if (state.notification.active) {
        auto tooltip = state.notification.title;
        if (tooltip.empty()) {
          tooltip = state.notification.message;
        }
        if (tooltip.empty()) {
          tooltip = "Sunshine notification";
        }
        return {
          "notification",
          state.notification.icon.empty() ? "default" : state.notification.icon,
          std::move(tooltip),
        };
      }

      return { status_name(state.status), state.icon, state.tooltip };
    }

    void
    touch_locked() {
      ++current_state.revision;
      current_state.updated_at_ms = now_ms();
    }

    void
    notify_changed() {
      change_sink_t sink;
      {
        std::lock_guard lock { change_sink_mutex };
        sink = change_sink;
      }
      if (sink) {
        sink();
      }
    }

  }  // namespace

  state_t
  get() {
    std::lock_guard lock { state_mutex };
    return current_state;
  }

  void
  set_change_sink(change_sink_t sink) {
    std::lock_guard lock { change_sink_mutex };
    change_sink = std::move(sink);
  }

  void
  set_idle(const std::string &last_app_name) {
    {
      std::lock_guard lock { state_mutex };
      current_state.status = status_e::idle;
      current_state.icon = "default";
      current_state.tooltip = "Sunshine";
      current_state.app_name = last_app_name;
      touch_locked();
    }
    notify_changed();
  }

  void
  set_streaming(const std::string &app_name) {
    {
      std::lock_guard lock { state_mutex };
      current_state.status = status_e::streaming;
      current_state.icon = "playing";
      current_state.tooltip = app_name.empty() ? "Streaming" : "Streaming " + app_name;
      current_state.app_name = app_name;
      touch_locked();
    }
    notify_changed();
  }

  void
  set_paused(const std::string &app_name) {
    {
      std::lock_guard lock { state_mutex };
      current_state.status = status_e::paused;
      current_state.icon = "pausing";
      current_state.tooltip = app_name.empty() ? "Stream paused" : "Stream paused: " + app_name;
      current_state.app_name = app_name;
      touch_locked();
    }
    notify_changed();
  }

  void
  set_pairing_required(const std::string &client_name) {
    {
      std::lock_guard lock { state_mutex };
      current_state.pairing_pending = true;
      current_state.pairing_client_name = client_name;
      current_state.notification.id = ++next_notification_id;
      current_state.notification.active = true;
      current_state.notification.title.clear();
      current_state.notification.message.clear();
      current_state.notification.icon = "locked";
      current_state.notification.action = "open_pin";
      touch_locked();
    }
    notify_changed();
  }

  void
  clear_pairing_required() {
    {
      std::lock_guard lock { state_mutex };
      current_state.pairing_pending = false;
      current_state.pairing_client_name.clear();
      if (current_state.notification.action == "open_pin") {
        current_state.notification = {};
      }
      touch_locked();
    }
    notify_changed();
  }

  std::uint64_t
  set_notification(const std::string &title, const std::string &message, const std::string &icon, const std::string &action) {
    std::uint64_t notification_id;
    {
      std::lock_guard lock { state_mutex };
      current_state.notification.id = ++next_notification_id;
      current_state.notification.active = true;
      current_state.notification.title = title;
      current_state.notification.message = message;
      current_state.notification.icon = icon;
      current_state.notification.action = action;
      touch_locked();
      notification_id = current_state.notification.id;
    }
    notify_changed();
    return notification_id;
  }

  void
  clear_notification() {
    {
      std::lock_guard lock { state_mutex };
      current_state.notification = {};
      touch_locked();
    }
    notify_changed();
  }

  void
  clear_notification_if(const std::uint64_t notification_id) {
    {
      std::lock_guard lock { state_mutex };
      if (current_state.notification.id != notification_id) {
        return;
      }
      current_state.notification = {};
      touch_locked();
    }
    notify_changed();
  }

  bool
  acknowledge_notification(const std::uint64_t notification_id) {
    {
      std::lock_guard lock { state_mutex };
      if (!current_state.notification.active || current_state.notification.id != notification_id) {
        return false;
      }

      // Pairing remains actionable until the pairing flow succeeds, expires, or
      // is cancelled. Clicking the menu must not make the PIN entry point vanish.
      if (current_state.pairing_pending || current_state.notification.action == "open_pin") {
        return false;
      }

      current_state.notification = {};
      touch_locked();
    }
    notify_changed();
    return true;
  }

  void
  set_vdd_state(bool active, bool keep_enabled, bool headless_create_enabled, bool cooldown) {
    {
      std::lock_guard lock { state_mutex };
      current_state.vdd.active = active;
      current_state.vdd.keep_enabled = keep_enabled;
      current_state.vdd.headless_create_enabled = headless_create_enabled;
      current_state.vdd.cooldown = cooldown;
      touch_locked();
    }
    notify_changed();
  }

  void
  set_vdd_confirmation(const bool awaiting_confirmation, const std::uint64_t operation_id) {
    {
      std::lock_guard lock { state_mutex };
      current_state.vdd.awaiting_confirmation = awaiting_confirmation;
      current_state.vdd.confirmation_operation_id = awaiting_confirmation ? operation_id : 0;
      touch_locked();
    }
    notify_changed();
  }

  std::uint64_t
  begin_operation(const std::string &action) {
    std::uint64_t operation_id;
    {
      std::lock_guard lock { state_mutex };
      current_state.operation = {
        ++next_operation_id,
        action,
        "running",
        {},
        {},
      };
      operation_id = current_state.operation.id;
      touch_locked();
    }
    notify_changed();
    return operation_id;
  }

  void
  complete_operation(const std::uint64_t operation_id, const bool success, const std::string &message, const std::string &error) {
    {
      std::lock_guard lock { state_mutex };
      if (current_state.operation.id != operation_id || current_state.operation.state != "running") {
        return;
      }
      current_state.operation.state = success ? "succeeded" : "failed";
      current_state.operation.message = message;
      current_state.operation.error = error;
      touch_locked();
    }
    notify_changed();
  }

  nlohmann::json
  to_json() {
    state_t snapshot;
    {
      std::lock_guard lock { state_mutex };
      snapshot = current_state;
    }

    const auto current_presentation = presentation(snapshot);

    return {
      { "protocol_version", protocol_version },
      { "instance_id", instance_id() },
      { "owner", tray_owner() },
      { "capabilities", nlohmann::json::array({ "state-v1", "events-v1", "actions-v1", "operations-v1", "notification-ack", "pairing", "vdd", "shutdown" }) },
      { "status", current_presentation.status },
      { "icon", current_presentation.icon },
      { "tooltip", current_presentation.tooltip },
      { "app_name", snapshot.app_name },
      { "pairing_client_name", snapshot.pairing_client_name },
      { "revision", snapshot.revision },
      { "updated_at_ms", snapshot.updated_at_ms },
      { "vdd",
        {
          { "active", snapshot.vdd.active },
          { "keep_enabled", snapshot.vdd.keep_enabled },
          { "headless_create_enabled", snapshot.vdd.headless_create_enabled },
          { "cooldown", snapshot.vdd.cooldown },
          { "awaiting_confirmation", snapshot.vdd.awaiting_confirmation },
          { "confirmation_operation_id", snapshot.vdd.confirmation_operation_id },
        } },
      { "notification",
        {
          { "id", snapshot.notification.id },
          { "active", snapshot.notification.active },
          { "title", snapshot.notification.title },
          { "message", snapshot.notification.message },
          { "icon", snapshot.notification.icon },
          { "action", snapshot.notification.action },
        } },
      { "operation",
        {
          { "id", snapshot.operation.id },
          { "action", snapshot.operation.action },
          { "state", snapshot.operation.state },
          { "message", snapshot.operation.message },
          { "error", snapshot.operation.error },
        } },
    };
  }

}  // namespace tray_state
