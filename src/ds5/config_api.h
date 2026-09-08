/**
 * @file src/ds5/config_api.h
 * @brief Authenticated HTTP handlers and conditional transactions for DualSense settings.
 */
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <Simple-Web-Server/server_https.hpp>

#include "config.h"

namespace ds5_config::api {
  enum class update_status_t {
    APPLIED,
    UNCHANGED,
    PRECONDITION_REQUIRED,
    PRECONDITION_FAILED,
    INVALID_PRECONDITION,
    INVALID_SETTINGS,
    INVALID_STORE,
    SAVE_FAILED,
    APPLY_FAILED,
  };

  struct config_state_t {
    load_status_t disk_status = load_status_t::MISSING;
    settings_t settings;
    bool persisted = false;
    std::string entity_tag;
  };

  struct update_result_t {
    update_status_t status = update_status_t::INVALID_SETTINGS;
    config_state_t state;
  };

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  /** Query one serialized disk/runtime view and its strong validator. */
  config_state_t query_state(const std::filesystem::path &path);

  /**
   * Conditionally persist and publish a complete settings document.
   * The implementation owns revision assignment; requested.revision is ignored.
   */
  update_result_t update_state(
    const std::filesystem::path &path,
    settings_t requested,
    std::optional<std::string_view> if_match
  );

  void get_config(resp_https_t response, const std::string &sunshine_config_file) noexcept;
  void save_config(
    resp_https_t response,
    req_https_t request,
    const std::string &sunshine_config_file
  ) noexcept;
}  // namespace ds5_config::api
