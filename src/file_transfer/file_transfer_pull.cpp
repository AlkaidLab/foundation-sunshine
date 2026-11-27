/**
 * @file src/file_transfer/file_transfer_pull.cpp
 * @brief See file_transfer_pull.h.
 */
#include "file_transfer_pull.h"

#include <charconv>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "src/clipboard_bridge.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/logging.h"
#include "src/session_info.h"

using namespace std::literals;

namespace stream::session {
  // Declared in stream.h, which pulls in the whole platform layer.
  std::vector<session_info_t>
  get_all_sessions_info();
}  // namespace stream::session

namespace file_transfer_pull {
  namespace {
    using clock_t = std::chrono::steady_clock;
    using response_ptr = std::shared_ptr<web::detail::ResponseBuffer>;

    struct pending_t {
      response_ptr response;
      std::uint64_t sid = 0;  ///< the streaming session the chunk was asked of
      std::string client_uuid;
      std::uint64_t length = 0;
      clock_t::time_point deadline;
    };

    std::mutex g_mu;
    std::unordered_map<std::string, pending_t> g_pending;
    hooks_t g_hooks;

    web::CaseInsensitiveMultimap
    json_headers() {
      return { { "Content-Type", "application/json" }, { "Cache-Control", "no-store" } };
    }

    void
    reply_error(web::detail::ResponseBuffer &response, web::StatusCode status, std::string_view word) {
      response.write(status, nlohmann::json { { "error", word } }.dump(), json_headers());
    }

    std::optional<std::string>
    real_session_client(std::uint64_t sid) {
      for (const auto &info : stream::session::get_all_sessions_info()) {
        if (info.session_id == sid && info.state == "RUNNING" && !info.client_uuid.empty()) {
          return info.client_uuid;
        }
      }
      return std::nullopt;
    }

    void
    real_send(std::uint64_t sid, std::vector<std::uint8_t> payload) {
      clipboard_bridge::bridge_t::instance().enqueue_outbound(sid, std::move(payload));
    }

    /// Caller holds g_mu.
    hooks_t
    hooks_locked() {
      hooks_t out = g_hooks;
      if (!out.session_client) out.session_client = real_session_client;
      if (!out.send) out.send = real_send;
      return out;
    }

    std::string
    make_ticket() {
      const auto random = crypto::rand(16);
      if (random.size() != 16) {
        throw std::runtime_error("file_transfer_pull: CSPRNG failed");
      }
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out;
      out.reserve(32);
      for (const auto byte : random) {
        const auto b = static_cast<std::uint8_t>(byte);
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
      }
      return out;
    }

    template <class T>
    bool
    parse_number(std::string_view text, T &out) {
      if (text.empty() || text.size() > 20) return false;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
      return error == std::errc {} && end == text.data() + text.size();
    }

    /// Answers the agent's requests whose client did not post in time.
    class reaper_t {
    public:
      reaper_t():
          thread_([this]() { run(); }) {}

      ~reaper_t() {
        {
          std::lock_guard lk(mu_);
          stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
      }

      void
      wake() { cv_.notify_all(); }

    private:
      void
      run() {
        std::unique_lock lk(mu_);
        while (!stop_) {
          cv_.wait_for(lk, 500ms);
          if (stop_) break;
          lk.unlock();
          expire(clock_t::now());
          lk.lock();
        }
      }

      static void
      expire(clock_t::time_point now) {
        std::vector<pending_t> expired;
        {
          std::lock_guard lk(g_mu);
          if (!g_hooks.reap) return;
          for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (it->second.deadline <= now) {
              expired.push_back(std::move(it->second));
              it = g_pending.erase(it);
            }
            else {
              ++it;
            }
          }
        }
        for (auto &p : expired) {
          BOOST_LOG(warning) << "File transfer: the client did not send a requested chunk in time"sv;
          reply_error(*p.response, web::StatusCode::gateway_timeout, "timeout");
        }
      }

      std::mutex mu_;
      std::condition_variable cv_;
      bool stop_ = false;
      std::thread thread_;
    };

    void
    ensure_reaper() {
      static reaper_t reaper;
      reaper.wake();
    }
  }  // namespace

  void
  set_hooks(hooks_t hooks) {
    std::lock_guard lk(g_mu);
    g_hooks = std::move(hooks);
  }

  void
  clear() {
    std::unordered_map<std::string, pending_t> pending;
    {
      std::lock_guard lk(g_mu);
      pending.swap(g_pending);
    }
    for (auto &[ticket, p] : pending) {
      reply_error(*p.response, web::StatusCode::service_unavailable, "cancelled");
    }
  }

  void
  session_stopped(std::uint64_t sid) {
    std::vector<pending_t> ended;
    {
      std::lock_guard lk(g_mu);
      for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (it->second.sid == sid) {
          ended.push_back(std::move(it->second));
          it = g_pending.erase(it);
        }
        else {
          ++it;
        }
      }
    }
    if (!ended.empty()) {
      BOOST_LOG(info) << "File transfer: session "sv << sid << " ended with "sv << ended.size() << " chunk request(s) waiting"sv;
    }
    for (auto &p : ended) {
      reply_error(*p.response, web::StatusCode::gone, "no_session");
    }
  }

  std::size_t
  pending_count() {
    std::lock_guard lk(g_mu);
    return g_pending.size();
  }

  std::chrono::milliseconds
  chunk_deadline(std::uint64_t length, std::chrono::milliseconds base) {
    // Rounded up to whole milliseconds; length is at most kMaxChunk, so no overflow.
    return base + std::chrono::milliseconds((length * 1000 + kMinUploadBytesPerSecond - 1) / kMinUploadBytesPerSecond);
  }

  std::vector<std::uint8_t>
  make_pull_frame(std::string_view ticket, std::string_view id, std::uint32_t index, std::uint64_t offset, std::uint64_t length) {
    const auto json = nlohmann::json {
      { "ticket", ticket },
      { "id", id },
      { "index", index },
      { "offset", offset },
      { "length", length },
    }
                        .dump();
    std::vector<std::uint8_t> frame;
    frame.reserve(10 + json.size());
    frame.push_back(1);  // wire version
    frame.push_back(kKindPull);
    for (int i = 0; i < 4; ++i) frame.push_back(0);  // token
    const auto size = static_cast<std::uint32_t>(json.size());
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
    frame.insert(frame.end(), json.begin(), json.end());
    return frame;
  }

  void
  handle_pull(response_ptr response, const web::Request &request) {
    if (!config::input.clipboard_sync) {
      reply_error(*response, web::StatusCode::forbidden, "clipboard_sync_disabled");
      return;
    }
    if (request.path_match.size() < 4) {
      reply_error(*response, web::StatusCode::not_found, "not_found");
      return;
    }

    std::uint64_t sid = 0;
    std::uint32_t index = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    const auto id = request.path_match[2].str();
    const auto query = request.parse_query_string();
    const auto offset_it = query.find("offset");
    const auto length_it = query.find("length");
    if (!parse_number(request.path_match[1].str(), sid) ||
        !parse_number(request.path_match[3].str(), index) ||
        offset_it == query.end() || !parse_number(offset_it->second, offset) ||
        length_it == query.end() || !parse_number(length_it->second, length) ||
        length == 0 || length > kMaxChunk) {
      reply_error(*response, web::StatusCode::bad_request, "bad_request");
      return;
    }

    std::string ticket;
    hooks_t hooks;
    {
      std::lock_guard lk(g_mu);
      hooks = hooks_locked();
    }
    const auto client = hooks.session_client(sid);
    if (!client) {
      reply_error(*response, web::StatusCode::gone, "no_session");
      return;
    }
    {
      std::lock_guard lk(g_mu);
      if (g_pending.size() >= kMaxPending) {
        reply_error(*response, web::StatusCode::service_unavailable, "busy");
        return;
      }
      ticket = make_ticket();
      const auto timeout = hooks.chunk_timeout.count() > 0 ? chunk_deadline(length, hooks.chunk_timeout) : chunk_deadline(length);
      g_pending.emplace(ticket, pending_t { response, sid, *client, length, clock_t::now() + timeout });
    }
    ensure_reaper();
    hooks.send(sid, make_pull_frame(ticket, id, index, offset, length));
  }

  void
  handle_chunk(web::detail::ResponseBuffer &response, const web::Request &request) {
    if (request.path_match.size() < 2) {
      reply_error(response, web::StatusCode::not_found, "not_found");
      return;
    }
    const auto ticket = request.path_match[1].str();

    pending_t pending;
    {
      std::lock_guard lk(g_mu);
      const auto it = g_pending.find(ticket);
      // Another client's chunk leaves the ticket for its own client.
      if (it == g_pending.end() || it->second.client_uuid != request.client_uuid) {
        reply_error(response, web::StatusCode::not_found, "not_found");
        return;
      }
      pending = std::move(it->second);
      g_pending.erase(it);
    }
    // The ticket's time ran out before the reaper got to it.
    if (pending.deadline <= clock_t::now()) {
      reply_error(*pending.response, web::StatusCode::gateway_timeout, "timeout");
      reply_error(response, web::StatusCode::gone, "expired");
      return;
    }
    // Clipboard sync was turned off while the chunk was on its way: it is not handed over.
    if (!config::input.clipboard_sync) {
      reply_error(*pending.response, web::StatusCode::service_unavailable, "clipboard_sync_disabled");
      reply_error(response, web::StatusCode::forbidden, "clipboard_sync_disabled");
      return;
    }

    if (const auto error = request.header.find("X-File-Transfer-Error"); error != request.header.end()) {
      // The client could not read the file; its word goes to the agent.
      std::string word = error->second.substr(0, 32);
      for (auto &c : word) {
        if (!((c >= 'a' && c <= 'z') || c == '_')) c = '_';
      }
      reply_error(*pending.response, web::StatusCode::bad_gateway, word.empty() ? "client_error" : word);
      response.write(web::StatusCode::no_content);
      return;
    }

    const auto body = request.content.string();
    if (body.size() > pending.length) {
      reply_error(*pending.response, web::StatusCode::bad_gateway, "too_long");
      reply_error(response, web::StatusCode::payload_too_large, "too_long");
      return;
    }
    pending.response->write(web::StatusCode::ok, body, {
                                                         { "Content-Type", "application/octet-stream" },
                                                         { "Cache-Control", "no-store" },
                                                       });
    response.write(web::StatusCode::no_content);
  }
}  // namespace file_transfer_pull
