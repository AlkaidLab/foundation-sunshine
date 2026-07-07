/**
 * @file src/plugin.h
 * @brief Lifecycle plugin host declarations.
 */
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace plugin {

  enum class lifecycle_event_e {
    sunshine_startup_recover,
    sunshine_shutdown_restoring,
    stream_first_session_starting,
    stream_last_session_stopping,
    stream_dynamic_params_changed,
  };

  std::string_view
  to_string(lifecycle_event_e event);

  void
  fire_lifecycle_event(lifecycle_event_e event, const nlohmann::json &context = nlohmann::json::object());

  nlohmann::json
  list_installed_plugins();

  nlohmann::json
  list_marketplace_plugins(std::string &error);

  bool
  set_plugin_enabled(std::string_view id, bool enabled, std::string &error);

  bool
  save_plugin_config(std::string_view id, const nlohmann::json &config, std::string &error);

  bool
  run_plugin_action(std::string_view id, std::string_view action_id, nlohmann::json &result, std::string &error);

}  // namespace plugin
