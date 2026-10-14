/**
 * @file src/ai/http_stream.h
 * @brief Bounded SSE execution shared by Web UI and GameStream.
 */
#pragma once

#include "beast_response.h"
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

      auto flush = [&] {
        if (cancelled || stream_shutdown_requested()) return false;
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

      auto send_chunk = [&](const char *data, size_t length) {
        if (cancelled || stream_shutdown_requested()) return false;
        if (!length) return true;
        if (!header_sent) {
          header_sent = true;
          framing::sse_header(*response);
        }
        framing::sse_chunk(*response, data, length);
        return flush();
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
          const auto status = result.httpCode == 400 || result.httpCode == 401 || result.httpCode == 403 || result.httpCode == 429 || result.httpCode == 200 ? result.httpCode : 502;
          framing::reply(*response, status, result.body, result.contentType);
        }
        else if (!cancelled && !stream_shutdown_requested() && result.httpCode == 200) {
          // Only a successful upstream transfer receives the terminating chunk.
          // A cancelled or failed SSE remains visibly truncated to the client.
          framing::sse_end(*response);
          flush();
        }
      }
      catch (...) {
        if (!header_sent) {
          framing::reply(*response, 500,
            R"({"error":{"message":"AI stream failed","type":"internal_error"}})",
            "application/json");
        }
      }
    };
    if (!submit_stream(std::move(task))) {
      response->close_connection_after_response = true;
      framing::reply(*response, 503,
        R"({"error":{"message":"Too many AI streams","type":"capacity_error"}})",
        "application/json", true);
    }
  }
}  // namespace ai::http
