/**
 * @file src/file_transfer/file_transfer_http.cpp
 * @brief See file_transfer_http.h.
 */
#include "file_transfer_http.h"

#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/config.h"
#include "file_transfer.h"
#include "file_transfer_pull.h"
#include "src/logging.h"

using namespace std::literals;

namespace file_transfer_http {
  namespace {
    json_response_t
    error(web::StatusCode status, std::string_view word, const std::string &path = {}) {
      nlohmann::json out { { "error", word } };
      if (!path.empty()) out["path"] = path;
      return { status, out.dump() };
    }

    web::CaseInsensitiveMultimap
    json_headers() {
      return { { "Content-Type", "application/json" }, { "Cache-Control", "no-store" } };
    }

    void
    handle_register(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }
      if (const auto length = req->header.find("Content-Length"); length != req->header.end()) {
        std::uint64_t value = 0;
        try {
          value = std::stoull(length->second);
        }
        catch (...) {
          value = kMaxRegisterBody + 1;
        }
        if (value > kMaxRegisterBody) {
          const auto out = error(web::StatusCode::payload_too_large, "payload_too_large");
          resp->write(out.status, out.body, json_headers());
          return;
        }
      }
      std::stringstream ss;
      ss << req->content.rdbuf();
      const auto out = make_register_response(ss.str());
      resp->write(out.status, out.body, json_headers());
    }
  }  // namespace

  json_response_t
  make_register_response(const std::string &body) {
    if (!config::input.clipboard_sync) {
      return error(web::StatusCode::forbidden, "clipboard_sync_disabled");
    }
    if (body.size() > kMaxRegisterBody) {
      return error(web::StatusCode::payload_too_large, "payload_too_large");
    }

    std::vector<std::string> paths;
    std::string type = "clipboard";
    try {
      const auto json = nlohmann::json::parse(body);
      if (!json.is_object()) {
        return error(web::StatusCode::bad_request, "bad_json");
      }
      if (const auto it = json.find("paths"); it != json.end()) {
        if (!it->is_array()) {
          return error(web::StatusCode::bad_request, "bad_json");
        }
        for (const auto &p : *it) {
          if (!p.is_string()) {
            return error(web::StatusCode::bad_request, "bad_json");
          }
          paths.push_back(p.get<std::string>());
        }
      }
      else if (const auto single = json.find("path"); single != json.end() && single->is_string()) {
        // The first agent release sent one path for Explorer's "send to client".
        paths.push_back(single->get<std::string>());
        type = "send";
      }
      if (const auto t = json.find("type"); t != json.end() && t->is_string()) {
        type = t->get<std::string>();
      }
    }
    catch (const nlohmann::json::exception &) {
      return error(web::StatusCode::bad_request, "bad_json");
    }

    const auto result = file_transfer::register_paths(paths, type);
    if (!result.offer) {
      BOOST_LOG(warning) << "File transfer: registration refused: "sv << result.error;
      const auto status = result.error == "no_user"   ? web::StatusCode::service_unavailable :
                          result.error == "not_found" ? web::StatusCode::not_found :
                          result.error == "internal"  ? web::StatusCode::internal_server_error :
                                                        web::StatusCode::bad_request;
      return error(status, result.error, result.detail);
    }
    return { web::StatusCode::ok, file_transfer::offer_json(*result.offer).dump() };
  }

  void
  register_routes(https_server_t &server, auth_fn auth) {
    auto auth_cap = std::make_shared<auth_fn>(std::move(auth));
    server.resource["^/api/v1/file-transfer/offers$"]["POST"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_register(*auth_cap, std::move(resp), std::move(req));
      };
    // Files copied on a client: the agent pulls a chunk while the host pastes them.
    server.resource[kInboundRoute]["GET"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        if (!(*auth_cap)(resp, req)) {
          return;
        }
        file_transfer_pull::handle_pull(std::move(resp), *req);
      };
  }

  void
  handle_chunk(web::detail::ResponseBuffer &response, const web::Request &request) {
    file_transfer_pull::handle_chunk(response, request);
  }

  void
  handle_download(web::detail::ResponseBuffer &response, const web::Request &request) {
    const auto reply_error = [&response](web::StatusCode status, std::string_view word, web::CaseInsensitiveMultimap headers = json_headers()) {
      response.write(status, nlohmann::json { { "error", word } }.dump(), headers);
    };

    if (!config::input.clipboard_sync) {
      reply_error(web::StatusCode::forbidden, "clipboard_sync_disabled");
      return;
    }
    if (request.path_match.size() < 2) {
      reply_error(web::StatusCode::not_found, "not_found");
      return;
    }
    const auto id = request.path_match[1].str();
    // The single-file route (no index) serves an offer of one file only.
    const bool single_route = request.path_match.size() < 3 || !request.path_match[2].matched;
    if (single_route && !file_transfer::is_single_file(id)) {
      reply_error(web::StatusCode::not_found, "not_found");
      return;
    }

    auto opened = file_transfer::open_entry(id, single_route ? "0" : request.path_match[2].str());
    if (!opened.file.valid()) {
      reply_error(static_cast<web::StatusCode>(opened.status), opened.error);
      return;
    }

    const auto size = opened.file.size();
    std::uint64_t offset = 0;
    if (const auto range = request.header.find("Range"); range != request.header.end()) {
      bool unsatisfiable = false;
      if (const auto start = file_transfer::parse_range(range->second, size, unsatisfiable)) {
        offset = *start;
      }
      else if (unsatisfiable) {
        auto headers = json_headers();
        headers.emplace("Content-Range", "bytes */" + std::to_string(size));
        reply_error(web::StatusCode::range_not_satisfiable, "range_not_satisfiable", headers);
        return;
      }
    }

    web::CaseInsensitiveMultimap headers {
      { "Content-Type", "application/octet-stream" },
      { "X-Content-Type-Options", "nosniff" },
      { "Cache-Control", "no-store" },
      { "Accept-Ranges", "bytes" },
    };
    auto status = web::StatusCode::ok;
    if (offset != 0) {
      status = web::StatusCode::partial_content;
      headers.emplace("Content-Range", "bytes " + std::to_string(offset) + "-" + std::to_string(size - 1) + "/" + std::to_string(size));
    }
    if (!response.write_native_file(status, opened.file.release(), offset, headers)) {
      reply_error(web::StatusCode::gone, "gone");
    }
  }
}  // namespace file_transfer_http
