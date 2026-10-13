/**
 * @file src/ai/http_stream.h
 * @brief Bounded SSE execution shared by Web UI and GameStream.
 */
#pragma once

#include "proxy.h"
#include <Simple-Web-Server/server_http.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace ai::http {
  // A shared pool with no waiting requests: admission fails when every worker
  // is occupied. HTTPS io_context threads remain available for send callbacks.
  bool
  submit_stream(std::function<void()> task);

  bool
  stream_shutdown_requested();
  // Call before either HTTPS listener is destroyed. Cancels and drains jobs
  // while their asynchronous send completions can still run.
  void
  shutdown_streams();

  template <class ResponsePtr>
  void
  stream_response(ResponsePtr response, std::string body) {
    auto task = [response, body = std::move(body)] {
      using clock = std::chrono::steady_clock;
      bool header_sent = false;
      auto last_send = clock::now();
      bool cancelled = false;
      response->close_connection_after_response = true;

      auto send_chunk = [&](const char *data, size_t length) {
        if (cancelled || stream_shutdown_requested()) return false;
        if (!header_sent) {
          *response << "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                       "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
          header_sent = true;
        }
        response->write(data, static_cast<std::streamsize>(length));
        struct Completion {
          std::mutex mutex;
          std::condition_variable ready;
          bool done = false;
          bool failed = false;
        };
        auto completion = std::make_shared<Completion>();
        response->send([completion](const SimpleWeb::error_code &ec) {
          std::lock_guard lock(completion->mutex);
          completion->failed = bool(ec);
          completion->done = true;
          completion->ready.notify_one();
        });
        // Only one chunk is queued at a time. A slow client or stopped server
        // cannot accumulate unbounded response buffers or hold a worker forever.
        std::unique_lock lock(completion->mutex);
        cancelled = !completion->ready.wait_for(lock, std::chrono::seconds(5), [&] { return completion->done; }) || completion->failed;
        last_send = clock::now();
        return !cancelled;
      };

      try {
        const auto result = processAiChatStream(body, send_chunk, [&] {
          if (cancelled || stream_shutdown_requested()) return true;
          // Probe an established stream during upstream silence, so a dropped
          // downstream connection also cancels an idle upstream request.
          if (header_sent && clock::now() - last_send >= std::chrono::seconds(1)) {
            constexpr char heartbeat[] = ": keep-alive\n\n";
            return !send_chunk(heartbeat, sizeof(heartbeat) - 1);
          }
          return false;
        });
        if (!header_sent) {
          const auto status = result.httpCode == 400 ? SimpleWeb::StatusCode::client_error_bad_request :
                              result.httpCode == 401 ? SimpleWeb::StatusCode::client_error_unauthorized :
                              result.httpCode == 403 ? SimpleWeb::StatusCode::client_error_forbidden :
                              result.httpCode == 429 ? SimpleWeb::StatusCode::client_error_too_many_requests :
                              result.httpCode == 200 ? SimpleWeb::StatusCode::success_ok :
                                                       SimpleWeb::StatusCode::server_error_bad_gateway;
          response->write(status, result.body, { { "Content-Type", result.contentType } });
        }
      }
      catch (...) {
        if (!header_sent) {
          response->write(SimpleWeb::StatusCode::server_error_internal_server_error,
            R"({"error":{"message":"AI stream failed","type":"internal_error"}})",
            { { "Content-Type", "application/json" } });
        }
      }
    };
    if (!submit_stream(std::move(task))) {
      response->write(SimpleWeb::StatusCode::server_error_service_unavailable,
        R"({"error":{"message":"Too many AI streams","type":"capacity_error"}})",
        { { "Content-Type", "application/json" }, { "Retry-After", "1" } });
    }
  }
}  // namespace ai::http
