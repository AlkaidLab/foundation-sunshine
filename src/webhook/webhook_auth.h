/**
 * @file src/webhook/webhook_auth.h
 * @brief Complete Webhook configuration stored separately from sunshine.conf.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "webhook.h"

namespace webhook::auth {
  using settings_t = configuration_t;

  enum class load_status_t {
    LOADED,
    MISSING,
    INVALID
  };

  struct load_result_t {
    load_status_t status = load_status_t::MISSING;
    settings_t settings;
  };

  /**
   * Resolve webhook_auth.json beside the selected sunshine.conf.
   */
  std::filesystem::path
  path_for(const std::filesystem::path &sunshine_config_file);

  /**
   * Resolve the manual recovery backup beside webhook_auth.json.
   */
  std::filesystem::path
  backup_path_for(const std::filesystem::path &webhook_auth_file);

  /**
   * Load and strictly validate a Webhook settings file.
   *
   * A missing file returns disabled defaults with MISSING status. An invalid
   * file returns INVALID status. This function never throws and never logs
   * file contents or paths.
   */
  load_result_t
  load(const std::filesystem::path &path) noexcept;

  /**
   * Parse a comma-separated stable event ID list. "-1" means no events.
   */
  bool
  parse_event_ids(std::string_view value, std::vector<int> &events) noexcept;

  /**
   * Serialize normalized event IDs. An empty list is persisted as "-1".
   */
  std::string
  serialize_event_ids(const std::vector<int> &events);

  /**
   * Persist a complete Webhook settings file. Before replacing an existing
   * file, preserve its exact previous contents as webhook_auth.json.bak.
   *
   * The caller is responsible for reporting a stable, non-sensitive error.
   */
  bool
  save(const std::filesystem::path &path, const settings_t &settings) noexcept;
}  // namespace webhook::auth
