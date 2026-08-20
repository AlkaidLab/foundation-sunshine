/**
 * @file src/clipboard_http.h
 * @brief HTTP/SSE bridge between the user-session GUI agent and the
 *        clipboard_bridge (in-process control-stream forwarder).
 *
 * Endpoints (registered on the existing confighttp HTTPS server, so they
 * inherit its auth and TLS):
 *
 *   POST /api/v1/clipboard/capability
 *     GUI heartbeat. Marks the bridge as "GUI alive" so rtsp.cpp will
 *     advertise clipboard caps. Optional JSON body is currently ignored.
 *     Response: 200 {"ok": true, "sessions": [<sid>...]}.
 *
 *   POST /api/v1/clipboard/item
 *     Body: raw opaque bytes (the clipboard protocol payload constructed by
 *     the GUI). Optional header `X-Clipboard-Target-Sid` (decimal); 0 or
 *     missing means broadcast to every active session.
 *     Response: 202 Accepted.
 *
 *   GET /api/v1/clipboard/events
 *     Server-Sent Events stream. Long-lived. Each inbound clipboard packet
 *     from any client is delivered as:
 *         event: clipboard
 *         id: <sid>
 *         data: <base64(payload)>
 *
 *   POST /api/v1/clipboard/blob
 *     Out-of-band upload for clipboard payloads larger than the encrypted
 *     control-stream's per-frame limit (`clipboard_bridge::kMaxPayloadBytes`).
 *     Body: raw bytes. Required header `X-Clipboard-Mime` (e.g. image/png).
 *     Response: 200 {"id": "<uuid>", "size": N, "expires_in": seconds}.
 *     The id is then advertised to peers via a small KIND_REF wire frame on
 *     the existing /api/v1/clipboard/item endpoint; peers fetch the actual
 *     bytes via GET /api/v1/clipboard/blob/<id>.
 *
 *   GET /api/v1/clipboard/blob/<id>
 *     Fetch a previously-uploaded blob by id. Body: raw bytes with the
 *     stored MIME echoed in Content-Type. 404 if missing or expired. The
 *     blob is NOT consumed on read so transient retries work; TTL cleanup
 *     reclaims memory eventually.
 *
 * Auth is delegated to the caller via the function passed to register_routes,
 * so we don't duplicate confighttp's basic-auth logic here.
 */
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include <Simple-Web-Server/server_https.hpp>

namespace clipboard_http {
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  struct blob_response_t {
    SimpleWeb::StatusCode status;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap headers;
  };

  /// Build the HTTP response for POST /api/v1/clipboard/blob.
  ///
  /// The caller provides request headers and the raw body bytes. This helper is
  /// transport-agnostic so Sunshine can expose the same blob store on multiple
  /// HTTPS servers (e.g. confighttp for the local GUI agent and nvhttp for
  /// paired Moonlight clients) without duplicating validation or storage logic.
  blob_response_t make_blob_upload_response(
    const SimpleWeb::CaseInsensitiveMultimap &request_headers,
    const std::string &body);

  /// Validate upload headers that must be checked before reading the request
  /// body. Returns a response to send immediately when the request is rejected.
  std::optional<blob_response_t> make_blob_upload_preflight_response(
    const SimpleWeb::CaseInsensitiveMultimap &request_headers);

  /// Build the HTTP response for GET /api/v1/clipboard/blob/<id>.
  blob_response_t make_blob_get_response(const std::string &id);

  /// End-to-end blob upload handler: runs preflight, reads body iff preflight
  /// passes, then invokes `make_blob_upload_response`. Templated on the
  /// request type so confighttp's `SimpleWeb::HTTPS` and nvhttp's bespoke
  /// `SunshineHTTPS` Request types can share one definition.
  template <typename Request>
  inline blob_response_t process_blob_upload(const Request &req) {
    if (auto out = make_blob_upload_preflight_response(req->header)) {
      return *out;
    }
    std::stringstream ss;
    ss << req->content.rdbuf();
    return make_blob_upload_response(req->header, ss.str());
  }

  /// End-to-end blob fetch handler: extracts the id from `req->path_match[1]`
  /// and invokes `make_blob_get_response`.
  template <typename Request>
  inline blob_response_t process_blob_get(const Request &req) {
    const std::string id = req->path_match.size() >= 2 ? req->path_match[1].str() : std::string {};
    return make_blob_get_response(id);
  }

  /// Register the /api/v1/clipboard/* routes on `server`. `auth` is invoked
  /// at the start of every handler; it must return true for authorised
  /// requests (and is responsible for sending the 401 response itself when
  /// returning false).
  void register_routes(https_server_t &server, auth_fn auth);
}  // namespace clipboard_http
