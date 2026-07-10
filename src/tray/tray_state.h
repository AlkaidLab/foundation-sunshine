#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace tray_state {

  using change_sink_t = std::function<void()>;

  inline constexpr std::uint32_t protocol_version = 1;

  enum class status_e {
    idle,
    streaming,
    paused
  };

  struct vdd_state_t {
    bool active = false;
    bool keep_enabled = false;
    bool headless_create_enabled = false;
    bool cooldown = false;
  };

  struct notification_t {
    std::uint64_t id = 0;
    bool active = false;
    std::string title;
    std::string message;
    std::string icon;
    std::string action;
  };

  struct state_t {
    status_e status = status_e::idle;
    std::string icon = "default";
    std::string tooltip = "Sunshine";
    std::string app_name;
    bool pairing_pending = false;
    std::string pairing_client_name;
    vdd_state_t vdd;
    notification_t notification;
    std::uint64_t revision = 0;
    std::int64_t updated_at_ms = 0;
  };

  state_t
  get();

  void
  set_change_sink(change_sink_t sink);

  void
  set_idle(const std::string &last_app_name = {});

  void
  set_streaming(const std::string &app_name);

  void
  set_paused(const std::string &app_name);

  void
  set_pairing_required(const std::string &client_name);

  void
  clear_pairing_required();

  std::uint64_t
  set_notification(const std::string &title, const std::string &message, const std::string &icon = "default", const std::string &action = {});

  void
  clear_notification();

  void
  clear_notification_if(std::uint64_t notification_id);

  bool
  acknowledge_notification(std::uint64_t notification_id);

  void
  set_vdd_state(bool active, bool keep_enabled, bool headless_create_enabled, bool cooldown);

  nlohmann::json
  to_json();

}  // namespace tray_state
