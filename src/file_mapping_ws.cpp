/**
 * @file src/file_mapping_ws.cpp
 * @brief Boost.Beast based WebSocket transport scaffolding for file mapping.
 */
#include "file_mapping_ws.h"

#include <algorithm>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "file_mapping_rpc.h"

namespace file_mapping_ws {
  namespace {
    validation_result_t
    fail(std::string error) {
      return { false, std::move(error) };
    }

    inbound_result_t
    inbound_fail(std::string error, bool close = false) {
      return { false, close, std::move(error), std::nullopt };
    }

    outbound_frame_t
    text_frame(nlohmann::json body) {
      outbound_frame_t out;
      out.kind = frame_kind_e::text;
      out.text = body.dump();
      return out;
    }

    std::optional<std::string_view>
    query_param(std::string_view query, std::string_view name) {
      while (!query.empty()) {
        auto part_end = query.find('&');
        auto part = query.substr(0, part_end);
        auto equals = part.find('=');
        if (equals != std::string_view::npos && part.substr(0, equals) == name) {
          return part.substr(equals + 1);
        }
        if (part_end == std::string_view::npos) {
          break;
        }
        query.remove_prefix(part_end + 1);
      }
      return std::nullopt;
    }
  }  // namespace

  session_core_t::session_core_t(
    std::string endpoint_name,
    std::string expected_peer_uuid,
    client_uuid_authorizer_t authorize_peer_uuid,
    file_mapping::operations::execution_context_t operations_context):
      endpoint_name_(std::move(endpoint_name)),
      expected_peer_uuid_(std::move(expected_peer_uuid)),
      authorize_peer_uuid_(std::move(authorize_peer_uuid)),
      operations_context_(std::move(operations_context)) {
  }

  session_state_e
  session_core_t::state() const {
    return state_;
  }

  const std::string &
  session_core_t::peer_uuid() const {
    return peer_uuid_;
  }

  inbound_result_t
  session_core_t::handle_text(std::string_view text) {
    if (state_ == session_state_e::closed) {
      return inbound_fail("session is closed", true);
    }

    auto parsed = file_mapping::rpc::parse_control_message(text);
    if (!parsed.ok) {
      return inbound_fail(parsed.error, true);
    }

    if (state_ == session_state_e::awaiting_hello) {
      if (parsed.type != file_mapping::rpc::message_type_e::hello) {
        return inbound_fail("first control message must be hello", true);
      }
      return handle_hello(parsed.body);
    }

    auto reply = file_mapping::operations::execute_control_message(parsed, operations_context_);
    return { reply.value("type", std::string {}) != "error", false, {}, text_frame(std::move(reply)) };
  }

  inbound_result_t
  session_core_t::handle_binary(const std::uint8_t *data, std::size_t size) {
    if (state_ == session_state_e::closed) {
      return inbound_fail("session is closed", true);
    }
    if (state_ != session_state_e::ready) {
      return inbound_fail("binary frame received before hello", true);
    }

    file_mapping::rpc::binary_header_t header;
    std::string error;
    if (!file_mapping::rpc::decode_binary_header(data, size, header, error)) {
      return inbound_fail(std::move(error), true);
    }
    if (size < file_mapping::rpc::kBinaryHeaderSize + header.payload_length) {
      return inbound_fail("binary frame payload is shorter than declared length", true);
    }

    nlohmann::json reply;
    reply["type"] = "result";
    reply["ok"] = true;
    reply["binary"] = true;
    reply["request_id"] = header.request_id;
    reply["payload_length"] = header.payload_length;
    return { true, false, {}, text_frame(std::move(reply)) };
  }

  inbound_result_t
  session_core_t::handle_hello(const nlohmann::json &body) {
    if (!body.contains("endpoint") || !body["endpoint"].is_string()) {
      return inbound_fail("hello missing endpoint", true);
    }

    auto endpoint = file_mapping::rpc::endpoint_from_string(body["endpoint"].get<std::string>());
    if (!endpoint) {
      return inbound_fail("hello contains invalid endpoint", true);
    }

    if (!body.contains("client_uuid") || !body["client_uuid"].is_string() || body["client_uuid"].get<std::string>().empty()) {
      return inbound_fail("hello missing client_uuid", true);
    }

    peer_uuid_ = body["client_uuid"].get<std::string>();
    if (!expected_peer_uuid_.empty() && peer_uuid_ != expected_peer_uuid_) {
      return inbound_fail("hello client_uuid does not match session token", true);
    }
    if (authorize_peer_uuid_ && !authorize_peer_uuid_(peer_uuid_)) {
      return inbound_fail("hello client_uuid is not paired", true);
    }

    state_ = session_state_e::ready;
    operations_context_.peer_uuid = peer_uuid_;

    nlohmann::json reply;
    reply["type"] = "hello";
    reply["version"] = file_mapping::rpc::kProtocolVersion;
    reply["endpoint"] = endpoint_name_;
    reply["peer_accepted"] = true;
    reply["mappings"] = nlohmann::json::array();
    for (const auto &mapping : operations_context_.mappings) {
      if (mapping.clients.empty() || std::find(mapping.clients.begin(), mapping.clients.end(), peer_uuid_) != mapping.clients.end()) {
        reply["mappings"].push_back(file_mapping::operations::mapping_to_json(mapping));
      }
    }
    return { true, false, {}, text_frame(std::move(reply)) };
  }

  validation_result_t
  validate_config(const transport_config_t &config) {
    if (config.bind_address.empty()) {
      return fail("bind address is required");
    }
    if (config.certificate_file.empty()) {
      return fail("certificate file is required");
    }
    if (config.private_key_file.empty()) {
      return fail("private key file is required");
    }
    if (config.max_control_frame_bytes == 0) {
      return fail("max control frame size must be non-zero");
    }
    if (config.max_binary_frame_bytes == 0) {
      return fail("max binary frame size must be non-zero");
    }
    if (config.max_write_queue_frames == 0) {
      return fail("max write queue frame count must be non-zero");
    }

    return { true, {} };
  }

  session_auth_result_t
  validate_session_target(
    std::string_view target,
    std::string_view expected_path,
    const session_token_validator_t &validate_token) {
    const auto query_start = target.find('?');
    const auto path = query_start == std::string_view::npos ? target : target.substr(0, query_start);
    const auto query = query_start == std::string_view::npos ? std::string_view {} : target.substr(query_start + 1);

    if (path != expected_path) {
      return { false, "invalid file mapping websocket path", {} };
    }
    if (!validate_token) {
      return { false, "session token validator is unavailable", {} };
    }

    const auto token = query_param(query, "token");
    if (!token || token->empty()) {
      return { false, "missing file mapping session token", {} };
    }
    auto client_uuid = validate_token(*token);
    if (!client_uuid || client_uuid->empty()) {
      return { false, "invalid or expired file mapping session token", {} };
    }

    return { true, {}, std::move(*client_uuid) };
  }

  std::string
  make_session_target(std::string_view host, std::uint16_t port, std::string_view path) {
    std::ostringstream url;
    url << "wss://" << host;
    if (port != 0) {
      url << ':' << port;
    }
    if (path.empty() || path.front() != '/') {
      url << '/';
    }
    url << path;
    return url.str();
  }
}  // namespace file_mapping_ws
