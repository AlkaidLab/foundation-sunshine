/**
 * @file src/file_mapping_ws.h
 * @brief Boost.Beast based WebSocket transport scaffolding for file mapping.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "file_mapping_operations.h"

namespace file_mapping_ws {
  static constexpr std::uint16_t kDefaultPort = 0;
  static constexpr std::uint32_t kDefaultMaxControlFrameBytes = 1024 * 1024;
  static constexpr std::uint32_t kDefaultMaxBinaryFrameBytes = 1024 * 1024;

  enum class transport_state_e {
    stopped,
    starting,
    listening,
    stopping
  };

  enum class session_state_e {
    awaiting_hello,
    ready,
    closed
  };

  enum class frame_kind_e {
    text,
    binary
  };

  struct transport_config_t {
    std::string bind_address = "0.0.0.0";
    std::uint16_t port = kDefaultPort;
    std::string certificate_file;
    std::string private_key_file;
    bool require_client_certificate = true;
    std::uint32_t max_control_frame_bytes = kDefaultMaxControlFrameBytes;
    std::uint32_t max_binary_frame_bytes = kDefaultMaxBinaryFrameBytes;
  };

  struct validation_result_t {
    bool ok = false;
    std::string error;
  };

  struct outbound_frame_t {
    frame_kind_e kind = frame_kind_e::text;
    std::string text;
    std::vector<std::uint8_t> binary;
  };

  struct inbound_result_t {
    bool ok = false;
    bool close = false;
    std::string error;
    std::optional<outbound_frame_t> reply;
  };

  using session_token_validator_t = std::function<std::optional<std::string>(std::string_view)>;
  using client_uuid_authorizer_t = std::function<bool(std::string_view)>;

  struct session_auth_result_t {
    bool ok = false;
    std::string error;
    std::string client_uuid;
  };

  class session_core_t {
  public:
    explicit session_core_t(
      std::string endpoint_name = "host",
      std::string expected_peer_uuid = {},
      client_uuid_authorizer_t authorize_peer_uuid = {},
      file_mapping::operations::execution_context_t operations_context = {});

    session_state_e state() const;
    const std::string &peer_uuid() const;

    inbound_result_t handle_text(std::string_view text);
    inbound_result_t handle_binary(const std::uint8_t *data, std::size_t size);

  private:
    inbound_result_t handle_hello(const nlohmann::json &body);

    std::string endpoint_name_;
    std::string expected_peer_uuid_;
    std::string peer_uuid_;
    client_uuid_authorizer_t authorize_peer_uuid_;
    file_mapping::operations::execution_context_t operations_context_;
    session_state_e state_ = session_state_e::awaiting_hello;
  };

  validation_result_t
  validate_config(const transport_config_t &config);

  session_auth_result_t
  validate_session_target(
    std::string_view target,
    std::string_view expected_path,
    const session_token_validator_t &validate_token);

  std::string
  make_session_target(std::string_view host, std::uint16_t port, std::string_view path = "/api/v1/file-mapping/session");
}  // namespace file_mapping_ws
