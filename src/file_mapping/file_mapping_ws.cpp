/**
 * @file src/file_mapping_ws.cpp
 * @brief Boost.Beast based WebSocket transport scaffolding for file mapping.
 */
#include "file_mapping_ws.h"

#include <algorithm>
#include <exception>
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

    std::string
    require_job_id(const nlohmann::json &body) {
      if (body.contains("job_id") && body["job_id"].is_string()) {
        return body["job_id"].get<std::string>();
      }
      return {};
    }

    std::uint64_t
    request_id(const nlohmann::json &body) {
      if (!body.contains("id")) {
        return 0;
      }
      if (body["id"].is_number_unsigned()) {
        return body["id"].get<std::uint64_t>();
      }
      if (body["id"].is_number_integer()) {
        const auto id = body["id"].get<std::int64_t>();
        return id < 0 ? 0 : static_cast<std::uint64_t>(id);
      }
      return 0;
    }

    std::string
    optional_string(const nlohmann::json &body, const char *name) {
      if (body.contains(name) && body[name].is_string()) {
        return body[name].get<std::string>();
      }
      return {};
    }

    file_mapping::rpc::operation_e
    operation_from_message_type(file_mapping::rpc::message_type_e type) {
      switch (type) {
        case file_mapping::rpc::message_type_e::list:
          return file_mapping::rpc::operation_e::list;
        case file_mapping::rpc::message_type_e::stat:
          return file_mapping::rpc::operation_e::stat;
        case file_mapping::rpc::message_type_e::read:
          return file_mapping::rpc::operation_e::read;
        default:
          return file_mapping::rpc::operation_e::download;
      }
    }

    std::vector<file_mapping::mapping_t>
    current_mappings(const file_mapping::operations::execution_context_t &context) {
      return context.mapping_provider ? context.mapping_provider() : context.mappings;
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

    if (parsed.type == file_mapping::rpc::message_type_e::job_status) {
      return handle_job_status(parsed.body);
    }
    if (parsed.type == file_mapping::rpc::message_type_e::cancel) {
      return handle_cancel(parsed.body);
    }

    return handle_operation(std::move(parsed));
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
    if (size - file_mapping::rpc::kBinaryHeaderSize != header.payload_length) {
      return inbound_fail("binary frame payload length does not match declared length", true);
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
  session_core_t::handle_job_status(const nlohmann::json &body) {
    const auto job_id = require_job_id(body);
    if (job_id.empty()) {
      return { false, false, {}, text_frame(file_mapping::rpc::make_error(request_id(body), "bad_request", "missing string field: job_id")) };
    }

    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
      return { false, false, {}, text_frame(file_mapping::rpc::make_error(request_id(body), "job_not_found", "job was not found")) };
    }

    nlohmann::json reply;
    reply["type"] = "result";
    reply["id"] = request_id(body);
    reply["ok"] = true;
    reply["job_id"] = job_id;
    reply["job"] = file_mapping::rpc::job_to_json(it->second);
    return { true, false, {}, text_frame(std::move(reply)) };
  }

  inbound_result_t
  session_core_t::handle_cancel(const nlohmann::json &body) {
    const auto job_id = require_job_id(body);
    if (job_id.empty()) {
      return { false, false, {}, text_frame(file_mapping::rpc::make_error(request_id(body), "bad_request", "missing string field: job_id")) };
    }

    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
      return { false, false, {}, text_frame(file_mapping::rpc::make_error(request_id(body), "job_not_found", "job was not found")) };
    }
    if (it->second.state == file_mapping::rpc::job_state_e::queued ||
        it->second.state == file_mapping::rpc::job_state_e::running) {
      it->second.state = file_mapping::rpc::job_state_e::cancelled;
    }

    nlohmann::json reply;
    reply["type"] = "result";
    reply["id"] = request_id(body);
    reply["ok"] = true;
    reply["job_id"] = job_id;
    reply["job"] = file_mapping::rpc::job_to_json(it->second);
    return { true, false, {}, text_frame(std::move(reply)) };
  }

  inbound_result_t
  session_core_t::handle_operation(file_mapping::rpc::parse_result_t parsed) {
    auto job = make_job(parsed);
    job.state = file_mapping::rpc::job_state_e::running;
    auto reply = file_mapping::operations::execute_control_message(parsed, operations_context_);
    if (reply.value("type", std::string {}) == "error") {
      job.state = file_mapping::rpc::job_state_e::failed;
      job.error_code = reply.value("code", std::string {});
      job.error_message = reply.value("message", std::string {});
    }
    else {
      job.state = file_mapping::rpc::job_state_e::completed;
      if (reply.contains("total_size") && reply["total_size"].is_number_unsigned()) {
        job.total_bytes = reply["total_size"].get<std::uint64_t>();
      }
      if (reply.contains("bytes_read") && reply["bytes_read"].is_number_unsigned()) {
        job.transferred_bytes = reply["bytes_read"].get<std::uint64_t>();
      }
    }

    reply["job_id"] = job.job_id;
    reply["job"] = file_mapping::rpc::job_to_json(job);
    const auto ok = reply.value("type", std::string {}) != "error";
    remember_job(std::move(job));
    return { ok, false, {}, text_frame(std::move(reply)) };
  }

  file_mapping::rpc::transfer_job_t
  session_core_t::make_job(const file_mapping::rpc::parse_result_t &parsed) {
    file_mapping::rpc::transfer_job_t job;
    job.job_id = "job-" + std::to_string(next_job_id_++);
    job.operation = operation_from_message_type(parsed.type);
    job.source.side = "host";
    job.source.mapping = optional_string(parsed.body, "mapping");
    job.source.path = optional_string(parsed.body, "path");
    job.destination.side = "client";
    return job;
  }

  void
  session_core_t::remember_job(file_mapping::rpc::transfer_job_t job) {
    if (jobs_.size() >= kDefaultMaxSessionJobs) {
      jobs_.erase(jobs_.begin());
    }
    jobs_[job.job_id] = std::move(job);
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
    if (*endpoint != file_mapping::rpc::endpoint_e::client) {
      return inbound_fail("hello endpoint must be client", true);
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

    try {
      nlohmann::json reply;
      reply["type"] = "hello";
      reply["version"] = file_mapping::rpc::kProtocolVersion;
      reply["endpoint"] = endpoint_name_;
      reply["peer_accepted"] = true;
      reply["mappings"] = nlohmann::json::array();
      for (const auto &mapping : current_mappings(operations_context_)) {
        if (mapping.clients.empty() || std::find(mapping.clients.begin(), mapping.clients.end(), peer_uuid_) != mapping.clients.end()) {
          reply["mappings"].push_back(file_mapping::operations::mapping_to_json(mapping));
        }
      }
      return { true, false, {}, text_frame(std::move(reply)) };
    }
    catch (const std::exception &err) {
      return inbound_fail(std::string { "hello mapping provider failed: " } + err.what(), true);
    }
    catch (...) {
      return inbound_fail("hello mapping provider failed with an unknown error", true);
    }
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
