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
 * Auth is delegated to the caller via the function passed to register_routes,
 * so we don't duplicate confighttp's basic-auth logic here.
 */
#pragma once

#include <functional>
#include <memory>

#include <Simple-Web-Server/server_https.hpp>

namespace clipboard_http {
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using auth_fn = std::function<bool(resp_https_t, req_https_t)>;

  /// Register the /api/v1/clipboard/* routes on `server`. `auth` is invoked
  /// at the start of every handler; it must return true for authorised
  /// requests (and is responsible for sending the 401 response itself when
  /// returning false).
  void register_routes(https_server_t &server, auth_fn auth);
}  // namespace clipboard_http
