/**
 * @file src/file_mapping/config_http.cpp
 * @brief Web UI file_mapping handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "config_http.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/file_mapping/file_mapping_store.h"
#include <mutex>

using namespace std::literals;
using json = nlohmann::json;

namespace file_mapping::config_http {

  using namespace confighttp;

  namespace {
    std::mutex file_mapping_store_transaction_mutex;

    bool
    persistFileMappingStoreOrRestore(std::vector<file_mapping::mapping_t> previous_mappings) {
      if (file_mapping_store::persist_to_config(file_mapping_store::global())) {
        return true;
      }

      file_mapping_store::global().replace(std::move(previous_mappings));
      return false;
    }

    void
    listFileMappings(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      json body;
      body["ok"] = true;
      body["mappings"] = file_mapping_store::global().to_json();
      write_json(std::move(response), web::StatusCode::ok, body);
    }

    void
    createFileMapping(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);

      json input;
      if (!read_json_body(response, request, input)) return;
      if (!input.is_object() || !input.contains("path") || !input["path"].is_string()) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "missing string field: path");
        return;
      }

      file_mapping_store::mutation_result_t result;
      {
        std::scoped_lock transaction_lock { file_mapping_store_transaction_mutex };
        auto previous_mappings = file_mapping_store::global().snapshot();
        result = file_mapping_store::global().add_quick_share(input["path"].get<std::string>());
        if (!result.ok) {
          write_json_error(std::move(response), web::StatusCode::bad_request, result.error);
          return;
        }
        if (!persistFileMappingStoreOrRestore(std::move(previous_mappings))) {
          write_json_error(std::move(response), web::StatusCode::internal_server_error, "failed to persist mapping configuration");
          return;
        }
      }

      json body;
      body["ok"] = true;
      body["mapping"] = file_mapping_store::mapping_to_config_json(result.mapping);
      write_json(std::move(response), web::StatusCode::ok, body);
    }

    void
    updateFileMapping(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      print_req(request);

      const auto id = request->path_match.size() > 1 ? request->path_match[1].str() : std::string {};
      if (!file_mapping::is_valid_mapping_id(id)) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "invalid mapping id");
        return;
      }

      json input;
      if (!read_json_body(response, request, input)) return;
      file_mapping_store::mutation_result_t result;
      {
        std::scoped_lock transaction_lock { file_mapping_store_transaction_mutex };
        auto previous_mappings = file_mapping_store::global().snapshot();
        result = file_mapping_store::global().update(id, input);
        if (!result.ok) {
          const auto status = result.error == "mapping was not found" ? web::StatusCode::not_found : web::StatusCode::bad_request;
          write_json_error(std::move(response), status, result.error);
          return;
        }
        if (!persistFileMappingStoreOrRestore(std::move(previous_mappings))) {
          write_json_error(std::move(response), web::StatusCode::internal_server_error, "failed to persist mapping configuration");
          return;
        }
      }

      json body;
      body["ok"] = true;
      body["mapping"] = file_mapping_store::mapping_to_config_json(result.mapping);
      write_json(std::move(response), web::StatusCode::ok, body);
    }

    void
    deleteFileMapping(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      const auto id = request->path_match.size() > 1 ? request->path_match[1].str() : std::string {};
      if (!file_mapping::is_valid_mapping_id(id)) {
        write_json_error(std::move(response), web::StatusCode::bad_request, "invalid mapping id");
        return;
      }
      {
        std::scoped_lock transaction_lock { file_mapping_store_transaction_mutex };
        auto previous_mappings = file_mapping_store::global().snapshot();
        if (!file_mapping_store::global().remove(id)) {
          write_json_error(std::move(response), web::StatusCode::not_found, "mapping was not found");
          return;
        }
        if (!persistFileMappingStoreOrRestore(std::move(previous_mappings))) {
          write_json_error(std::move(response), web::StatusCode::internal_server_error, "failed to persist mapping configuration");
          return;
        }
      }

      json body;
      body["ok"] = true;
      body["id"] = id;
      write_json(std::move(response), web::StatusCode::ok, body);
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/v1/file-mapping/mappings$"]["GET"] = listFileMappings;
    server.resource["^/api/v1/file-mapping/mappings$"]["POST"] = createFileMapping;
    server.resource["^/api/v1/file-mapping/mappings/([A-Za-z0-9_\\-]{1,64})$"]["PATCH"] = updateFileMapping;
    server.resource["^/api/v1/file-mapping/mappings/([A-Za-z0-9_\\-]{1,64})$"]["DELETE"] = deleteFileMapping;
  }

}  // namespace file_mapping::config_http
