/**
 * @file src/file_mapping_rpc.h
 * @brief Protocol helpers for the directory mapping RPC channel.
 */
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace file_mapping::rpc {
  static constexpr std::uint16_t kProtocolVersion = 1;
  static constexpr std::uint32_t kBinaryMagic = 0x53464d50;  // "SFMP"
  static constexpr std::size_t kBinaryHeaderSize = 44;

  enum class endpoint_e {
    host,
    client
  };

  enum class message_type_e {
    unknown,
    hello,
    list,
    stat,
    open,
    read,
    close,
    mkdir,
    rename,
    remove,
    job_start,
    job_status,
    cancel,
    result,
    error
  };

  enum class job_state_e {
    queued,
    running,
    paused,
    completed,
    failed,
    cancelled
  };

  enum class operation_e {
    list,
    stat,
    read,
    download,
    upload,
    copy
  };

  struct exposed_mapping_t {
    std::string id;
    std::string name;
    std::string side;
    std::string mode;
    std::vector<std::string> capabilities;
  };

  struct file_ref_t {
    std::string side;
    std::string mapping;
    std::string path;
  };

  struct transfer_job_t {
    std::string job_id;
    operation_e operation = operation_e::download;
    file_ref_t source;
    file_ref_t destination;
    std::uint64_t total_bytes = 0;
    std::uint64_t transferred_bytes = 0;
    job_state_e state = job_state_e::queued;
    std::string error_code;
    std::string error_message;
  };

  struct binary_header_t {
    std::uint32_t magic = kBinaryMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t flags = 0;
    std::uint64_t request_id = 0;
    std::uint64_t job_id_hash = 0;
    std::uint64_t handle_id = 0;
    std::uint64_t offset = 0;
    std::uint32_t payload_length = 0;
  };

  struct parse_result_t {
    bool ok = false;
    message_type_e type = message_type_e::unknown;
    nlohmann::json body;
    std::string error;
  };

  std::string_view
  to_string(endpoint_e value);

  std::string_view
  to_string(message_type_e value);

  std::string_view
  to_string(job_state_e value);

  std::string_view
  to_string(operation_e value);

  std::optional<endpoint_e>
  endpoint_from_string(std::string_view value);

  message_type_e
  message_type_from_string(std::string_view value);

  std::optional<job_state_e>
  job_state_from_string(std::string_view value);

  std::optional<operation_e>
  operation_from_string(std::string_view value);

  nlohmann::json
  make_hello(endpoint_e endpoint, std::string client_uuid, std::vector<exposed_mapping_t> mappings);

  nlohmann::json
  make_error(std::uint64_t id, std::string code, std::string message);

  nlohmann::json
  job_to_json(const transfer_job_t &job);

  bool
  job_from_json(const nlohmann::json &json, transfer_job_t &out, std::string &error);

  parse_result_t
  parse_control_message(std::string_view text);

  std::array<std::uint8_t, kBinaryHeaderSize>
  encode_binary_header(const binary_header_t &header);

  bool
  decode_binary_header(const std::uint8_t *data, std::size_t size, binary_header_t &out, std::string &error);
}  // namespace file_mapping::rpc
