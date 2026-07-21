/**
 * @file src/file_mapping_operations.cpp
 * @brief Read-only file mapping RPC execution helpers.
 */
#include "file_mapping_operations.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>

namespace file_mapping::operations {
  namespace {
    namespace fs = std::filesystem;

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

    nlohmann::json
    error_response(const nlohmann::json &body, std::string code, std::string message) {
      return rpc::make_error(request_id(body), std::move(code), std::move(message));
    }

    nlohmann::json
    result_response(const nlohmann::json &body) {
      return {
        { "type", "result" },
        { "id", request_id(body) },
        { "ok", true }
      };
    }

    std::vector<mapping_t>
    current_mappings(const execution_context_t &context) {
      return context.mapping_provider ? context.mapping_provider() : context.mappings;
    }

    const mapping_t *
    find_mapping(const std::vector<mapping_t> &mappings, const std::string &id) {
      auto it = std::find_if(mappings.begin(), mappings.end(), [&](const mapping_t &mapping) {
        return mapping.id == id;
      });
      return it == mappings.end() ? nullptr : &*it;
    }

    bool
    client_allowed(const mapping_t &mapping, const std::string &peer_uuid) {
      return mapping.clients.empty() || std::find(mapping.clients.begin(), mapping.clients.end(), peer_uuid) != mapping.clients.end();
    }

    std::string
    resolve_error_code(resolve_error_e error) {
      switch (error) {
        case resolve_error_e::invalid_mapping_id:
          return "invalid_mapping_id";
        case resolve_error_e::invalid_root:
          return "invalid_root";
        case resolve_error_e::absolute_path:
          return "absolute_path";
        case resolve_error_e::invalid_relative_path:
          return "invalid_path";
        case resolve_error_e::reserved_name:
          return "reserved_name";
        case resolve_error_e::path_escape:
          return "path_escape";
        case resolve_error_e::reparse_point_blocked:
          return "reparse_point_blocked";
        case resolve_error_e::not_found:
          return "not_found";
        case resolve_error_e::filesystem_error:
          return "filesystem_error";
        case resolve_error_e::none:
          break;
      }
      return "resolve_error";
    }

    std::string
    file_kind(const fs::directory_entry &entry, std::error_code &ec) {
      if (entry.is_directory(ec)) {
        return "directory";
      }
      ec.clear();
      if (entry.is_regular_file(ec)) {
        return "file";
      }
      ec.clear();
      return "other";
    }

    std::string
    file_kind(const fs::path &path, std::error_code &ec) {
      if (fs::is_directory(path, ec)) {
        return "directory";
      }
      ec.clear();
      if (fs::is_regular_file(path, ec)) {
        return "file";
      }
      ec.clear();
      return "other";
    }

    std::uint64_t
    file_size_or_zero(const fs::path &path, std::error_code &ec) {
      auto size = fs::file_size(path, ec);
      if (ec) {
        ec.clear();
        return 0;
      }
      return static_cast<std::uint64_t>(size);
    }

    std::string
    base64_encode(const std::vector<unsigned char> &bytes) {
      static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string out;
      out.reserve(((bytes.size() + 2) / 3) * 4);

      for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto b0 = bytes[i];
        const auto b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const auto b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;

        out.push_back(alphabet[(b0 >> 2) & 0x3f]);
        out.push_back(alphabet[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0f)]);
        out.push_back(i + 1 < bytes.size() ? alphabet[((b1 & 0x0f) << 2) | ((b2 >> 6) & 0x03)] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[b2 & 0x3f] : '=');
      }

      return out;
    }

    bool
    require_string_field(const nlohmann::json &body, const char *name, std::string &out) {
      if (!body.contains(name) || !body[name].is_string()) {
        return false;
      }
      out = body[name].get<std::string>();
      return true;
    }

    bool
    optional_string_field(const nlohmann::json &body, const char *name, std::string &out, nlohmann::json &error) {
      if (!body.contains(name)) {
        out.clear();
        return true;
      }
      if (!body[name].is_string()) {
        error = error_response(body, "bad_request", std::string("field must be a string: ") + name);
        return false;
      }
      out = body[name].get<std::string>();
      return true;
    }

    bool
    optional_uint64_field(const nlohmann::json &body, const char *name, std::uint64_t fallback, std::uint64_t &out, nlohmann::json &error) {
      if (!body.contains(name)) {
        out = fallback;
        return true;
      }
      if (body[name].is_number_unsigned()) {
        out = body[name].get<std::uint64_t>();
        return true;
      }
      if (body[name].is_number_integer()) {
        const auto value = body[name].get<std::int64_t>();
        if (value >= 0) {
          out = static_cast<std::uint64_t>(value);
          return true;
        }
      }
      error = error_response(body, "bad_request", std::string("field must be a non-negative integer: ") + name);
      return false;
    }

    std::optional<mapping_t>
    require_mapping(const nlohmann::json &body, const execution_context_t &context, nlohmann::json &error) {
      std::string mapping_id;
      if (!require_string_field(body, "mapping", mapping_id)) {
        error = error_response(body, "bad_request", "missing string field: mapping");
        return std::nullopt;
      }

      auto mappings = current_mappings(context);
      const auto *mapping = find_mapping(mappings, mapping_id);
      if (!mapping) {
        error = error_response(body, "mapping_not_found", "mapping was not found");
        return std::nullopt;
      }
      if (!client_allowed(*mapping, context.peer_uuid)) {
        error = error_response(body, "forbidden", "client is not allowed to access mapping");
        return std::nullopt;
      }
      return *mapping;
    }

    nlohmann::json
    execute_list(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }

      std::string remote_path;
      if (!optional_string_field(body, "path", remote_path, error)) {
        return error;
      }
      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      if (!fs::is_directory(resolved.resolved_path, ec)) {
        return error_response(body, "not_directory", "path is not a directory");
      }

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["entries"] = nlohmann::json::array();
      out["truncated"] = false;

      std::uint32_t count = 0;
      for (const auto &entry : fs::directory_iterator(resolved.resolved_path, ec)) {
        if (ec) {
          return error_response(body, "filesystem_error", ec.message());
        }
        if (context.max_list_entries != 0 && count >= context.max_list_entries) {
          out["truncated"] = true;
          break;
        }

        std::error_code entry_ec;
        const auto kind = file_kind(entry, entry_ec);
        const auto name = entry.path().filename().generic_string();
        nlohmann::json item {
          { "name", name },
          { "kind", kind }
        };
        if (kind == "file") {
          item["size"] = file_size_or_zero(entry.path(), entry_ec);
        }
        out["entries"].push_back(std::move(item));
        ++count;
      }
      if (ec) {
        return error_response(body, "filesystem_error", ec.message());
      }

      return out;
    }

    nlohmann::json
    execute_stat(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }

      std::string remote_path;
      if (!optional_string_field(body, "path", remote_path, error)) {
        return error;
      }
      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      const auto kind = file_kind(resolved.resolved_path, ec);
      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["kind"] = kind;
      if (kind == "file") {
        out["size"] = file_size_or_zero(resolved.resolved_path, ec);
      }
      return out;
    }

    nlohmann::json
    execute_read(const rpc::parse_result_t &message, const execution_context_t &context) {
      const auto &body = message.body;
      nlohmann::json error;
      const auto mapping = require_mapping(body, context, error);
      if (!mapping) {
        return error;
      }

      std::string remote_path;
      if (!optional_string_field(body, "path", remote_path, error)) {
        return error;
      }
      auto resolved = resolve_path(*mapping, remote_path, true);
      if (!resolved.ok) {
        return error_response(body, resolve_error_code(resolved.error), resolved.message);
      }

      std::error_code ec;
      if (!fs::is_regular_file(resolved.resolved_path, ec)) {
        return error_response(body, "not_file", "path is not a regular file");
      }

      const auto total_size = file_size_or_zero(resolved.resolved_path, ec);
      if (mapping->max_file_size != 0 && total_size > mapping->max_file_size) {
        return error_response(body, "file_too_large", "file exceeds mapping max_file_size");
      }

      std::uint64_t offset = 0;
      if (!optional_uint64_field(body, "offset", 0, offset, error)) {
        return error;
      }
      std::uint64_t requested_length = 64 * 1024;
      if (!optional_uint64_field(body, "length", requested_length, requested_length, error)) {
        return error;
      }
      if (requested_length > std::numeric_limits<std::uint32_t>::max()) {
        return error_response(body, "bad_request", "read length is too large");
      }
      auto length = static_cast<std::uint32_t>(requested_length);
      length = std::min(length, context.max_read_bytes);
      if (length == 0) {
        return error_response(body, "bad_request", "read length must be non-zero");
      }

      std::ifstream in(resolved.resolved_path, std::ios::binary);
      if (!in) {
        return error_response(body, "open_failed", "failed to open file for reading");
      }

      in.seekg(static_cast<std::streamoff>(offset));
      if (!in && !in.eof()) {
        return error_response(body, "seek_failed", "failed to seek file");
      }

      std::vector<unsigned char> bytes(length);
      in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      const auto bytes_read = static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount()));
      bytes.resize(bytes_read);

      auto out = result_response(body);
      out["mapping"] = mapping->id;
      out["path"] = remote_path;
      out["offset"] = offset;
      out["bytes_read"] = bytes_read;
      out["total_size"] = total_size;
      out["eof"] = offset + bytes_read >= total_size;
      out["encoding"] = "base64";
      out["data"] = base64_encode(bytes);
      return out;
    }
  }  // namespace

  nlohmann::json
  mapping_to_json(const mapping_t &mapping) {
    return {
      { "id", mapping.id },
      { "name", mapping.name },
      { "side", "host" },
      { "mode", mapping.mode == access_mode_e::read ? "read" : "readwrite" },
      { "capabilities", nlohmann::json::array({ "list", "stat", "read" }) }
    };
  }

  nlohmann::json
  execute_control_message(const rpc::parse_result_t &message, const execution_context_t &context) {
    switch (message.type) {
      case rpc::message_type_e::list:
        return execute_list(message, context);
      case rpc::message_type_e::stat:
        return execute_stat(message, context);
      case rpc::message_type_e::read:
        return execute_read(message, context);
      default:
        return error_response(message.body, "unsupported_operation", "operation is not supported by the read-only file mapping executor");
    }
  }
}  // namespace file_mapping::operations
