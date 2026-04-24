/**
 * @file src/clipboard_http.cpp
 * @brief See clipboard_http.h.
 */
#include "clipboard_http.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include <nlohmann/json.hpp>

#include "clipboard_bridge.h"
#include "config.h"
#include "logging.h"

namespace clipboard_http {
  using namespace std::literals;
  namespace {
    struct subscriber_t {
      resp_https_t resp;
      std::atomic_bool alive { true };
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
    fanout_frame(const std::string &frame) {
      std::vector<std::shared_ptr<subscriber_t>> snapshot;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        snapshot = g_subs;
      }

      for (auto &sub : snapshot) {
        if (!sub->alive.load(std::memory_order_acquire)) {
          continue;
        }
        try {
          *sub->resp << frame;
          sub->resp->send([sub](const SimpleWeb::error_code &ec) {
            if (ec) {
              sub->alive.store(false, std::memory_order_release);
            }
          });
        } catch (const std::exception &e) {
          BOOST_LOG(debug) << "clipboard SSE write failed: "sv << e.what();
          sub->alive.store(false, std::memory_order_release);
        }
      }

      // Prune dead subscribers (cheap; clipboard events are infrequent).
      std::lock_guard<std::mutex> lk(g_mu);
      g_subs.erase(std::remove_if(g_subs.begin(), g_subs.end(),
                     [](const std::shared_ptr<subscriber_t> &s) {
                       return !s->alive.load(std::memory_order_acquire);
                     }),
        g_subs.end());
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
    ensure_inbound_sink() {
      // Idempotent: install once on first /events subscription.
      std::lock_guard<std::mutex> lk(g_mu);
      if (g_inbound_sink_installed) {
        return;
      }
      clipboard_bridge::bridge_t::instance().set_inbound_sink(&fanout_inbound);
      g_inbound_sink_installed = true;
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

      clipboard_bridge::payload_t bytes(body.begin(), body.end());
      bridge.enqueue_outbound(target, std::move(bytes));

      resp->write(SimpleWeb::StatusCode::success_accepted, R"({"ok":true})");
    }

    void
    handle_events(const auth_fn &auth, resp_https_t resp, req_https_t req) {
      if (!auth(resp, req)) {
        return;
      }

      ensure_inbound_sink();
      clipboard_bridge::bridge_t::instance().notify_gui_alive();

      resp->close_connection_after_response = true;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/event-stream");
      headers.emplace("Cache-Control", "no-cache");
      headers.emplace("Connection", "keep-alive");
      headers.emplace("X-Accel-Buffering", "no");
      resp->write(headers);
      resp->send();

      // Initial comment frame to flush headers through any intermediaries.
      *resp << ": clipboard-stream-ready\n\n";
      resp->send();

      auto sub = std::make_shared<subscriber_t>();
      sub->resp = std::move(resp);

      std::lock_guard<std::mutex> lk(g_mu);
      g_subs.push_back(std::move(sub));
    }
  }  // namespace

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
  }
}  // namespace clipboard_http
