/**
 * @file src/file_transfer/file_transfer_http.h
 * @brief HTTP endpoints of file_transfer (see file_transfer.h for the flow).
 *
 *   POST /api/v1/file-transfer/offers           confighttp, for the GUI agent
 *     Body: {"paths": ["<absolute path>", ...], "type": "clipboard" | "send"}
 *     200: the offer (file_transfer::offer_json); 4xx: {"error": "<word>", "path": "..."}
 *
 *   GET /api/v1/file-transfer/inbound/<sid>/<id>/<index>?offset=&length=
 *     confighttp, for the GUI agent: a chunk of a file a client copied (file_transfer_pull.h)
 *
 *   GET /api/v1/file-transfer/<id>/<index>       nvhttp, for paired clients
 *     The file's bytes; "Range: bytes=N-" answers 206 from N on.
 *   GET /api/v1/file-transfer/<id>               nvhttp, the single-file route of the first version
 *     The same for an offer of one file (its download_url); 404 for any other offer.
 *   POST /api/v1/file-transfer/chunk/<ticket>    nvhttp, a client's answer to a chunk request (file_transfer_pull.h)
 *     403 clipboard_sync_disabled, 404 not_found, 409 changed, 410 gone, 416, 503 no_user.
 *
 * Both are off while `clipboard_sync` is off.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "src/http/server.h"

namespace file_transfer_http {
  using https_server_t = web::Server<web::HTTPS>;
  using resp_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename web::ServerBase<web::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  /// Largest registration body accepted.
  constexpr std::size_t kMaxRegisterBody = 1024 * 1024;

  struct json_response_t {
    web::StatusCode status;
    std::string body;
  };

  /// Registration, independent of the transport (tests call it directly).
  json_response_t
  make_register_response(const std::string &body);

  /// Register POST /api/v1/file-transfer/offers and GET .../inbound/... on confighttp.
  void
  register_routes(https_server_t &server, auth_fn auth);

  /// Route patterns, shared with the tests.
  inline constexpr const char *kDownloadRoute = "^/api/v1/file-transfer/([0-9a-f]{64})/([0-9]{1,6})$";
  inline constexpr const char *kSingleFileRoute = "^/api/v1/file-transfer/([0-9a-f]{64})$";
  inline constexpr const char *kChunkRoute = "^/api/v1/file-transfer/chunk/([0-9a-f]{32})$";
  inline constexpr const char *kInboundRoute = "^/api/v1/file-transfer/inbound/([0-9]{1,20})/([0-9a-f]{64})/([0-9]{1,6})$";

  /// Answer GET /api/v1/file-transfer/<id>/<index> (path_match[1] the id, [2] the index), or the
  /// single-file GET /api/v1/file-transfer/<id> (no [2]). Takes the shared base of every server's
  /// response, so nvhttp's paired server can call it.
  void
  handle_download(web::detail::ResponseBuffer &response, const web::Request &request);

  /// Answer POST /api/v1/file-transfer/chunk/<ticket> (file_transfer_pull::handle_chunk).
  void
  handle_chunk(web::detail::ResponseBuffer &response, const web::Request &request);

  /// Register the routes paired clients use on nvhttp (downloads and chunk answers).
  template <class Server>
  void
  register_paired_routes(Server &server) {
    server.resource[kDownloadRoute]["GET"] = [](auto resp, auto req) { handle_download(*resp, *req); };
    server.resource[kSingleFileRoute]["GET"] = [](auto resp, auto req) { handle_download(*resp, *req); };
    server.resource[kChunkRoute]["POST"] = [](auto resp, auto req) { handle_chunk(*resp, *req); };
  }
}  // namespace file_transfer_http
