/**
 * @file src/clipboard_http.cpp
 * @brief See clipboard_http.h.
 */
#include "clipboard_http.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include <nlohmann/json.hpp>

#include "clipboard_bridge.h"
#include "clipboard_blob_store.h"
#include "config.h"
#include "logging.h"

namespace clipboard_http {
  using namespace std::literals;
  namespace {
    struct subscriber_t {
      resp_https_t resp;
      std::atomic_bool alive { true };
      std::mutex send_mu;
    };

    std::mutex g_mu;
    std::vector<std::shared_ptr<subscriber_t>> g_subs;
    bool g_inbound_sink_installed = false;

    std::string
    base64_encode(const std::uint8_t *data, std::size_t len) {
      if (!len) {
        return {};
      }
      std::string out;
      out.resize(4 * ((len + 2) / 3));
      const int n = EVP_EncodeBlock(
        reinterpret_cast<unsigned char *>(out.data()),
        data,
        static_cast<int>(len));
      if (n < 0) {
        return {};
      }
      out.resize(static_cast<std::size_t>(n));
      return out;
    }

    /// Build the SSE frame for a single inbound payload.
    std::string
    build_event(clipboard_bridge::session_id sid, const clipboard_bridge::payload_t &bytes) {
      const std::string b64 = base64_encode(bytes.data(), bytes.size());
      std::string frame;
      frame.reserve(b64.size() + 64);
      frame += "event: clipboard\n";
      frame += "id: ";
      frame += std::to_string(sid);
      frame += "\n";
      frame += "data: ";
      frame += b64;
      frame += "\n\n";
      return frame;
    }

    void
    prune_dead_subscribers_locked() {
      g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(),
                     [](const std::shared_ptr<subscriber_t> &s) {
                       return !s->alive.load(std::memory_order_acquire);
                     }),
        g_subs.end());

      if (g_subs.empty() && g_inbound_sink_installed) {
        clipboard_bridge::bridge_t::instance().set_inbound_sink({});
        g_inbound_sink_installed = false;
      }
    }

    void
    send_frame(const std::shared_ptr<subscriber_t> &sub, const std::string &frame) {
      if (!sub->alive.load(std::memory_order_acquire)) {
        return;
      }

      try {
        std::lock_guard<std::mutex> lk(sub->send_mu);
        if (sub->alive.load(std::memory_order_acquire)) {
          *sub->resp << frame;
          sub->resp->send([sub](const SimpleWeb::error_code &ec) {
            if (ec) {
              sub->alive.store(false, std::memory_order_release);
            }
          });
        }
      } catch (const std::exception &e) {
        BOOST_LOG(debug) << "clipboard SSE write failed: "sv << e.what();
        sub->alive.store(false, std::memory_order_release);
      }
    }

    void
    fanout_frame(const std::string &frame) {
      std::vector<std::shared_ptr<subscriber_t>> snapshot;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        snapshot = g_subs;
      }

      for (auto &sub : snapshot) {
        send_frame(sub, frame);
      }

      // Prune dead subscribers (cheap; clipboard events are infrequent).
      std::lock_guard<std::mutex> lk(g_mu);
      prune_dead_subscribers_locked();
    }

    void
    fanout_inbound(clipboard_bridge::session_id sid, const clipboard_bridge::payload_t &bytes) {
      fanout_frame(build_event(sid, bytes));
    }

    void
    fanout_keepalive() {
      fanout_frame(": clipboard-keepalive\n\n");
    }

    void
    ensure_inbound_sink_locked() {
      if (!g_inbound_sink_installed) {
        clipboard_bridge::bridge_t::instance().set_inbound_sink(&fanout_inbound);
        g_inbound_sink_installed = true;
      }
    }

    // ---- Endpoint handlers ----

    void
    handle_capability(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }

      auto &bridge = clipboard_bridge::bridge_t::instance();
      bridge.notify_gui_alive();
      fanout_keepalive();

      nlohmann::json out;
      out["ok"] = true;
      out["clipboard_sync"] = config::input.clipboard_sync;
      out["session_count"] = bridge.session_count();
      out["gui_alive"] = bridge.gui_alive();

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      resp->write(SimpleWeb::StatusCode::success_ok, out.dump(), headers);
    }

    void
    handle_item(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }
      if (!config::input.clipboard_sync) {
        resp->write(SimpleWeb::StatusCode::client_error_forbidden,
          R"({"error":"clipboard_sync_disabled"})");
        return;
      }

      // Refresh GUI heartbeat — any /item POST is also implicit liveness.
      auto &bridge = clipboard_bridge::bridge_t::instance();
      bridge.notify_gui_alive();

      auto content_length = req->header.find("Content-Length");
      if (content_length != req->header.end() && !content_length->second.empty()) {
        try {
          if (std::stoull(content_length->second) > clipboard_bridge::kMaxPayloadBytes) {
            resp->write(SimpleWeb::StatusCode::client_error_payload_too_large,
              R"({"error":"payload_too_large"})");
            return;
          }
        } catch (...) {
          resp->write(SimpleWeb::StatusCode::client_error_bad_request,
            R"({"error":"bad_content_length"})");
          return;
        }
      }

      // Parse optional target sid from header.
      clipboard_bridge::session_id target = clipboard_bridge::kBroadcast;
      auto it = req->header.find("X-Clipboard-Target-Sid");
      if (it != req->header.end() && !it->second.empty()) {
        try {
          target = static_cast<clipboard_bridge::session_id>(std::stoull(it->second));
        } catch (...) {
          resp->write(SimpleWeb::StatusCode::client_error_bad_request,
            R"({"error":"bad_target_sid"})");
          return;
        }
      }

      // Read raw body bytes.
      std::stringstream ss;
      ss << req->content.rdbuf();
      const std::string body = ss.str();
      if (body.empty()) {
        resp->write(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"empty_body"})");
        return;
      }
      if (body.size() > clipboard_bridge::kMaxPayloadBytes) {
        resp->write(SimpleWeb::StatusCode::client_error_payload_too_large,
          R"({"error":"payload_too_large"})");
        return;
      }

      clipboard_bridge::payload_t bytes(body.begin(), body.end());
      bridge.enqueue_outbound(target, std::move(bytes));

      resp->write(SimpleWeb::StatusCode::success_accepted, R"({"ok":true})");
    }

    void
    handle_events(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }

      clipboard_bridge::bridge_t::instance().notify_gui_alive();

      auto sub = std::make_shared<subscriber_t>();
      sub->resp = std::move(resp);
      sub->resp->close_connection_after_response = true;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/event-stream");
      headers.emplace("Cache-Control", "no-cache");
      headers.emplace("Connection", "keep-alive");
      headers.emplace("X-Accel-Buffering", "no");
      sub->resp->write(headers);
      sub->resp->send();

      // Initial comment frame to flush headers through any intermediaries.
      send_frame(sub, ": clipboard-stream-ready\n\n");

      std::lock_guard<std::mutex> lk(g_mu);
      g_subs.push_back(std::move(sub));
      ensure_inbound_sink_locked();
    }

    // ---- Out-of-band blob endpoints ----
    //
    // Wire-frame KIND_REF carries metadata only; the actual bytes are pushed
    // / pulled here. See clipboard_blob_store.h for storage policy.

    // Strict RFC 6838-ish MIME validator. Accepts a single "type/subtype"
    // pair; both halves must be non-empty and use only token characters
    // [A-Za-z0-9!#$&^_.+-]. Total length capped at 128 chars. Parameters
    // (e.g. "; charset=utf-8") are intentionally rejected to keep the value
    // safe to reflect into a response Content-Type header.
    static bool
    is_valid_mime(const std::string &s) {
      if (s.empty() || s.size() > 128) {
        return false;
      }
      auto is_token_char = [](unsigned char c) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
          return true;
        }
        switch (c) {
          case '!': case '#': case '$': case '&': case '^':
          case '_': case '.': case '+': case '-':
            return true;
          default:
            return false;
        }
      };
      auto slash = s.find('/');
      if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) {
        return false;
      }
      // Exactly one '/'.
      if (s.find('/', slash + 1) != std::string::npos) {
        return false;
      }
      for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == slash) {
          continue;
        }
        if (!is_token_char(static_cast<unsigned char>(s[i]))) {
          return false;
        }
      }
      return true;
    }

    blob_response_t
    json_blob_response(SimpleWeb::StatusCode status, std::string body) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return { status, std::move(body), std::move(headers) };
    }

    bool
    parse_content_length(const std::string &value, std::uint64_t &out) {
      if (value.empty()) {
        return false;
      }
      if (!std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return false;
      }

      try {
        std::size_t consumed = 0;
        out = std::stoull(value, &consumed);
        return consumed == value.size();
      } catch (...) {
        return false;
      }
    }

    void
    handle_blob_upload(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }

      if (auto out = make_blob_upload_preflight_response(req->header)) {
        resp->write(out->status, out->body, out->headers);
        return;
      }

      std::stringstream ss;
      ss << req->content.rdbuf();
      auto out = make_blob_upload_response(req->header, ss.str());
      resp->write(out.status, out.body, out.headers);
    }

    void
    handle_blob_get(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }

      const std::string id = req->path_match.size() >= 2 ? req->path_match[1].str() : std::string {};
      auto out = make_blob_get_response(id);
      resp->write(out.status, out.body, out.headers);
    }
  }  // namespace

  blob_response_t
  make_blob_upload_response(
    const SimpleWeb::CaseInsensitiveMultimap &request_headers,
    const std::string &body) {
      if (auto out = make_blob_upload_preflight_response(request_headers)) {
        return *out;
      }

      if (!config::input.clipboard_sync) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_forbidden,
          R"({"error":"clipboard_sync_disabled"})");
      }

      auto mime_it = request_headers.find("X-Clipboard-Mime");
      if (mime_it == request_headers.end() || mime_it->second.empty()) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"missing_mime"})");
      }
      // Strict RFC 6838 token shape: type "/" subtype, each non-empty and made
      // of [A-Za-z0-9!#$&^_.+-]. Length capped to keep logs/json bounded and
      // to prevent the value being abused as an XSS vector when it is later
      // echoed verbatim into the Content-Type header on the GET path.
      if (!is_valid_mime(mime_it->second)) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"bad_mime"})");
      }

      if (body.empty()) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"empty_body"})");
      }
      if (body.size() > clipboard_blob_store::kMaxBlobBytes) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_payload_too_large,
          R"({"error":"payload_too_large"})");
      }

      clipboard_blob_store::payload_t bytes(body.begin(), body.end());
      const std::size_t size = bytes.size();
      auto put = clipboard_blob_store::put(std::move(bytes), mime_it->second);
      if (!put.ok) {
        nlohmann::json err;
        err["error"] = put.err.empty() ? std::string { "put_failed" } : put.err;
        return json_blob_response(SimpleWeb::StatusCode::client_error_payload_too_large, err.dump());
      }

      nlohmann::json out;
      out["id"] = put.id;
      out["size"] = size;
      out["expires_in"] = clipboard_blob_store::kBlobTtlSeconds;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      return { SimpleWeb::StatusCode::success_ok, out.dump(), std::move(headers) };
    }

  std::optional<blob_response_t>
  make_blob_upload_preflight_response(const SimpleWeb::CaseInsensitiveMultimap &request_headers) {
      auto content_length = request_headers.find("Content-Length");
      if (content_length == request_headers.end()) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"bad_content_length"})");
      }

      std::uint64_t length = 0;
      if (!parse_content_length(content_length->second, length)) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"bad_content_length"})");
      }

      if (length > clipboard_blob_store::kMaxBlobBytes) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_payload_too_large,
          R"({"error":"payload_too_large"})");
      }

      return std::nullopt;
  }

  blob_response_t
  make_blob_get_response(const std::string &id) {
      if (!config::input.clipboard_sync) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_forbidden,
          R"({"error":"clipboard_sync_disabled"})");
      }

      if (id.empty()) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_bad_request,
          R"({"error":"bad_id"})");
      }

      // Constrain to canonical UUID-v4 shape (36 chars, hex + dashes) to
      // avoid the path regex accidentally matching weird inputs.
      if (id.size() != 36) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_not_found,
          R"({"error":"not_found"})");
      }

      auto got = clipboard_blob_store::get(id, /*consume=*/false);
      if (!got.found) {
        return json_blob_response(SimpleWeb::StatusCode::client_error_not_found,
          R"({"error":"not_found"})");
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      // The mime was validated at upload time (is_valid_mime) so reflecting it
      // here is safe; nosniff defends against any UA that might still try to
      // override the declared type when displaying the response inline.
      headers.emplace("Content-Type", got.mime.empty() ? std::string { "application/octet-stream" } : got.mime);
      headers.emplace("X-Content-Type-Options", "nosniff");
      headers.emplace("Cache-Control", "no-store");
      // Bytes-as-string copy: SimpleWebServer's `write(string)` is binary-safe
      // (string isn't NUL-terminated-sensitive).
      std::string body(reinterpret_cast<const char *>(got.bytes.data()), got.bytes.size());
      return { SimpleWeb::StatusCode::success_ok, std::move(body), std::move(headers) };
  }

  void
  register_routes(https_server_t &server, auth_fn auth) {
    auto auth_cap = std::make_shared<auth_fn>(std::move(auth));

    server.resource["^/api/v1/clipboard/capability$"]["POST"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_capability(*auth_cap, std::move(resp), std::move(req));
      };

    server.resource["^/api/v1/clipboard/item$"]["POST"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_item(*auth_cap, std::move(resp), std::move(req));
      };

    server.resource["^/api/v1/clipboard/events$"]["GET"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_events(*auth_cap, std::move(resp), std::move(req));
      };

    server.resource["^/api/v1/clipboard/blob$"]["POST"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_blob_upload(*auth_cap, std::move(resp), std::move(req));
      };

    // Capture group is the blob id. UUID-v4 canonical shape is 36 chars
    // (hex+dashes); the regex is loose to allow other id schemes later.
    server.resource["^/api/v1/clipboard/blob/([A-Za-z0-9_\\-]{1,128})$"]["GET"] =
      [auth_cap](resp_https_t resp, req_https_t req) {
        handle_blob_get(*auth_cap, std::move(resp), std::move(req));
      };
  }
}  // namespace clipboard_http
