/**
 * @file src/file_mapping_rpc.cpp
 * @brief Protocol helpers for the directory mapping RPC channel.
 */
#include "file_mapping_rpc.h"

#include <array>
#include <cstring>
#include <limits>

namespace file_mapping::rpc {
  namespace {
    void
    write_u16(std::array<std::uint8_t, kBinaryHeaderSize> &out, std::size_t offset, std::uint16_t value) {
      out[offset + 0] = static_cast<std::uint8_t>(value & 0xff);
      out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    }

    void
    write_u32(std::array<std::uint8_t, kBinaryHeaderSize> &out, std::size_t offset, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
      }
    }

    void
    write_u64(std::array<std::uint8_t, kBinaryHeaderSize> &out, std::size_t offset, std::uint64_t value) {
      for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
      }
    }

    std::uint16_t
    read_u16(const std::uint8_t *data, std::size_t offset) {
      return static_cast<std::uint16_t>(data[offset]) |
             (static_cast<std::uint16_t>(data[offset + 1]) << 8);
    }

    std::uint32_t
    read_u32(const std::uint8_t *data, std::size_t offset) {
      std::uint32_t value = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[offset + i]) << (i * 8);
      }
      return value;
    }

    std::uint64_t
    read_u64(const std::uint8_t *data, std::size_t offset) {
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (i * 8);
      }
      return value;
    }

    bool
    require_string(const nlohmann::json &json, const char *key, std::string &out, std::string &error) {
      if (!json.contains(key) || !json.at(key).is_string()) {
        error = std::string("missing string field: ") + key;
        return false;
      }
      out = json.at(key).get<std::string>();
      return true;
    }

    bool
    require_file_ref(const nlohmann::json &json, const char *key, file_ref_t &out, std::string &error) {
      if (!json.contains(key) || !json.at(key).is_object()) {
        error = std::string("missing object field: ") + key;
        return false;
      }
      const auto &ref = json.at(key);
      return require_string(ref, "side", out.side, error) &&
             require_string(ref, "mapping", out.mapping, error) &&
             require_string(ref, "path", out.path, error);
    }
  }  // namespace

  std::string_view
  to_string(endpoint_e value) {
    switch (value) {
      case endpoint_e::host:
        return "host";
      case endpoint_e::client:
        return "client";
    }
    return "unknown";
  }

  std::string_view
  to_string(message_type_e value) {
    switch (value) {
      case message_type_e::hello:
        return "hello";
      case message_type_e::list:
        return "list";
      case message_type_e::stat:
        return "stat";
      case message_type_e::open:
        return "open";
      case message_type_e::read:
        return "read";
      case message_type_e::close:
        return "close";
      case message_type_e::mkdir:
        return "mkdir";
      case message_type_e::rename:
        return "rename";
      case message_type_e::remove:
        return "delete";
      case message_type_e::job_start:
        return "job_start";
      case message_type_e::job_status:
        return "job_status";
      case message_type_e::cancel:
        return "cancel";
      case message_type_e::result:
        return "result";
      case message_type_e::error:
        return "error";
      case message_type_e::unknown:
        break;
    }
    return "unknown";
  }

  std::string_view
  to_string(job_state_e value) {
    switch (value) {
      case job_state_e::queued:
        return "queued";
      case job_state_e::running:
        return "running";
      case job_state_e::paused:
        return "paused";
      case job_state_e::completed:
        return "completed";
      case job_state_e::failed:
        return "failed";
      case job_state_e::cancelled:
        return "cancelled";
    }
    return "unknown";
  }

  std::string_view
  to_string(operation_e value) {
    switch (value) {
      case operation_e::download:
        return "download";
      case operation_e::list:
        return "list";
      case operation_e::stat:
        return "stat";
      case operation_e::read:
        return "read";
      case operation_e::upload:
        return "upload";
      case operation_e::copy:
        return "copy";
    }
    return "unknown";
  }

  std::optional<endpoint_e>
  endpoint_from_string(std::string_view value) {
    if (value == "host") {
      return endpoint_e::host;
    }
    if (value == "client") {
      return endpoint_e::client;
    }
    return std::nullopt;
  }

  message_type_e
  message_type_from_string(std::string_view value) {
    if (value == "hello") return message_type_e::hello;
    if (value == "list") return message_type_e::list;
    if (value == "stat") return message_type_e::stat;
    if (value == "open") return message_type_e::open;
    if (value == "read" || value == "read_chunk") return message_type_e::read;
    if (value == "close") return message_type_e::close;
    if (value == "mkdir") return message_type_e::mkdir;
    if (value == "rename") return message_type_e::rename;
    if (value == "delete") return message_type_e::remove;
    if (value == "job_start") return message_type_e::job_start;
    if (value == "job_status") return message_type_e::job_status;
    if (value == "cancel" || value == "cancel_job") return message_type_e::cancel;
    if (value == "result") return message_type_e::result;
    if (value == "error") return message_type_e::error;
    return message_type_e::unknown;
  }

  std::optional<job_state_e>
  job_state_from_string(std::string_view value) {
    if (value == "queued") return job_state_e::queued;
    if (value == "running") return job_state_e::running;
    if (value == "paused") return job_state_e::paused;
    if (value == "completed") return job_state_e::completed;
    if (value == "failed") return job_state_e::failed;
    if (value == "cancelled") return job_state_e::cancelled;
    return std::nullopt;
  }

  std::optional<operation_e>
  operation_from_string(std::string_view value) {
    if (value == "list") return operation_e::list;
    if (value == "stat") return operation_e::stat;
    if (value == "read") return operation_e::read;
    if (value == "download") return operation_e::download;
    if (value == "upload") return operation_e::upload;
    if (value == "copy") return operation_e::copy;
    return std::nullopt;
  }

  nlohmann::json
  make_hello(endpoint_e endpoint, std::string client_uuid, std::vector<exposed_mapping_t> mappings) {
    nlohmann::json out;
    out["type"] = to_string(message_type_e::hello);
    out["version"] = kProtocolVersion;
    out["endpoint"] = to_string(endpoint);
    out["client_uuid"] = std::move(client_uuid);
    out["mappings"] = nlohmann::json::array();

    for (const auto &mapping : mappings) {
      out["mappings"].push_back({
        { "id", mapping.id },
        { "name", mapping.name },
        { "side", mapping.side },
        { "mode", mapping.mode },
        { "capabilities", mapping.capabilities }
      });
    }
    return out;
  }

  nlohmann::json
  make_error(std::uint64_t id, std::string code, std::string message) {
    return {
      { "type", to_string(message_type_e::error) },
      { "id", id },
      { "code", std::move(code) },
      { "message", std::move(message) }
    };
  }

  nlohmann::json
  job_to_json(const transfer_job_t &job) {
    return {
      { "job_id", job.job_id },
      { "operation", to_string(job.operation) },
      { "source", {
          { "side", job.source.side },
          { "mapping", job.source.mapping },
          { "path", job.source.path }
        } },
      { "destination", {
          { "side", job.destination.side },
          { "mapping", job.destination.mapping },
          { "path", job.destination.path }
        } },
      { "total_bytes", job.total_bytes },
      { "transferred_bytes", job.transferred_bytes },
      { "state", to_string(job.state) },
      { "error_code", job.error_code },
      { "error_message", job.error_message }
    };
  }

  bool
  job_from_json(const nlohmann::json &json, transfer_job_t &out, std::string &error) {
    if (!json.is_object()) {
      error = "job must be an object";
      return false;
    }

    std::string operation;
    std::string state;
    if (!require_string(json, "job_id", out.job_id, error) ||
        !require_string(json, "operation", operation, error) ||
        !require_file_ref(json, "source", out.source, error) ||
        !require_file_ref(json, "destination", out.destination, error) ||
        !require_string(json, "state", state, error)) {
      return false;
    }

    auto operation_value = operation_from_string(operation);
    if (!operation_value) {
      error = "invalid job operation";
      return false;
    }
    out.operation = *operation_value;

    auto state_value = job_state_from_string(state);
    if (!state_value) {
      error = "invalid job state";
      return false;
    }
    out.state = *state_value;

    out.total_bytes = json.value("total_bytes", std::uint64_t { 0 });
    out.transferred_bytes = json.value("transferred_bytes", std::uint64_t { 0 });
    out.error_code = json.value("error_code", std::string {});
    out.error_message = json.value("error_message", std::string {});
    return true;
  }

  parse_result_t
  parse_control_message(std::string_view text) {
    parse_result_t result;
    try {
      result.body = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception &e) {
      result.error = e.what();
      return result;
    }

    if (!result.body.is_object()) {
      result.error = "control message must be a JSON object";
      return result;
    }

    if (!result.body.contains("type") || !result.body["type"].is_string()) {
      result.error = "control message missing type";
      return result;
    }

    result.type = message_type_from_string(result.body["type"].get<std::string>());
    if (result.type == message_type_e::unknown) {
      result.error = "unknown control message type";
      return result;
    }

    if (result.type == message_type_e::hello) {
      if (!result.body.contains("version") || (!result.body["version"].is_number_unsigned() && !result.body["version"].is_number_integer())) {
        result.error = "hello missing version";
        return result;
      }
      std::uint64_t version = 0;
      if (result.body["version"].is_number_unsigned()) {
        version = result.body["version"].get<std::uint64_t>();
      }
      else {
        const auto signed_version = result.body["version"].get<std::int64_t>();
        if (signed_version < 0) {
          result.error = "unsupported protocol version";
          return result;
        }
        version = static_cast<std::uint64_t>(signed_version);
      }
      if (version != kProtocolVersion) {
        result.error = "unsupported protocol version";
        return result;
      }
    }

    result.ok = true;
    return result;
  }

  std::array<std::uint8_t, kBinaryHeaderSize>
  encode_binary_header(const binary_header_t &header) {
    std::array<std::uint8_t, kBinaryHeaderSize> out {};
    write_u32(out, 0, header.magic);
    write_u16(out, 4, header.version);
    write_u16(out, 6, header.flags);
    write_u64(out, 8, header.request_id);
    write_u64(out, 16, header.job_id_hash);
    write_u64(out, 24, header.handle_id);
    write_u64(out, 32, header.offset);
    write_u32(out, 40, header.payload_length);
    return out;
  }

  bool
  decode_binary_header(const std::uint8_t *data, std::size_t size, binary_header_t &out, std::string &error) {
    if (!data || size < kBinaryHeaderSize) {
      error = "binary frame header is too small";
      return false;
    }

    out.magic = read_u32(data, 0);
    out.version = read_u16(data, 4);
    out.flags = read_u16(data, 6);
    out.request_id = read_u64(data, 8);
    out.job_id_hash = read_u64(data, 16);
    out.handle_id = read_u64(data, 24);
    out.offset = read_u64(data, 32);
    out.payload_length = read_u32(data, 40);

    if (out.magic != kBinaryMagic) {
      error = "invalid binary frame magic";
      return false;
    }
    if (out.version != kProtocolVersion) {
      error = "unsupported binary frame version";
      return false;
    }
    return true;
  }
}  // namespace file_mapping::rpc
