/**
 * @file src/ai/http_stream.h
 * @brief Bounded SSE execution shared by Web UI and GameStream.
 */
#pragma once

#include "beast_response.h"
#include "proxy.h"
#include "src/http/server.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace ai::http {
  using StreamBackend = std::function<AiProxyResult(const std::string &, std::function<bool(const char *, size_t)>, std::function<bool()>)>;
  // No waiting requests: admission fails when every worker is occupied.
  // Each owner drains its workers while network completions can still run.
  class StreamExecutor {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    explicit StreamExecutor(unsigned capacity = 4);
    ~StreamExecutor();
    StreamExecutor(const StreamExecutor &) = delete;
    StreamExecutor &
    operator=(const StreamExecutor &) = delete;
    bool
    submit(std::function<void()> task);
    bool
    stopping() const;
    unsigned
    active() const;
    void
    stop();
  };
  StreamExecutor &
  production_executor();
  inline bool
  submit_stream(std::function<void()> task) { return production_executor().submit(std::move(task)); }
  inline bool
  stream_shutdown_requested() { return production_executor().stopping(); }
  // Call before either HTTPS listener is destroyed. Cancels and drains jobs
  // while their asynchronous send completions can still run.
  void
  shutdown_streams();

  template <class ResponsePtr>
  void
  stream_response(ResponsePtr response, std::string body, StreamExecutor &pool, StreamBackend backend,
    std::chrono::milliseconds send_timeout = std::chrono::seconds(5)) {
    // Chunked SSE requires HTTP/1.1. Reject before admission or an upstream call.
    if (response->http_version() != 11) {
      response->close_connection_after_response = true;
      response->write(web::StatusCode::http_version_not_supported);
      return;
    }
    // Observe a downstream disconnect while curl is still awaiting its first
    // upstream byte. This belongs to the native connection, not a heartbeat.
    response->begin_stream();
    auto task = [response, body = std::move(body), &pool, backend = std::move(backend), send_timeout] {
      using clock = std::chrono::steady_clock;
      bool header_sent = false;
      auto last_send = clock::now();
      bool cancelled = false;
      response->close_connection_after_response = true;

      auto flush = [&] {
        if (cancelled || pool.stopping() || response->closed()) return false;
        struct Completion {
          std::mutex mutex;
          std::condition_variable ready;
          bool done = false;
          bool failed = false;
        };
        auto completion = std::make_shared<Completion>();
        response->send([completion](const web::error_code &ec) {
          std::lock_guard lock(completion->mutex);
          completion->failed = bool(ec);
          completion->done = true;
          completion->ready.notify_one();
        });
        // Only one chunk is queued at a time. A slow client or stopped server
        // cannot accumulate unbounded response buffers or hold a worker forever.
        std::unique_lock lock(completion->mutex);
        cancelled = !completion->ready.wait_for(lock, send_timeout, [&] { return completion->done; }) || completion->failed;
        if (cancelled) response->abort();
        last_send = clock::now();
        return !cancelled;
      };

      auto send_chunk = [&](const char *data, size_t length) {
        if (cancelled || pool.stopping() || response->closed()) return false;
        if (!length) return true;
        if (!header_sent) {
          header_sent = true;
          framing::sse_header(*response);
        }
        // Injected backends may emit larger blocks than curl. Split without
        // retaining the block, waiting for each write before admitting another.
        constexpr size_t max_chunk = 16 * 1024;
        while (length) {
          const auto part = length < max_chunk ? length : max_chunk;
          framing::sse_chunk(*response, data, part);
          if (!flush()) return false;
          data += part;
          length -= part;
        }
        return true;
      };

      try {
        const auto result = backend(body, send_chunk, [&] {
          if (cancelled || pool.stopping() || response->closed()) return true;
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
        else if (!cancelled && !pool.stopping() && result.httpCode == 200) {
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
    if (!pool.submit(std::move(task))) {
      response->close_connection_after_response = true;
      framing::reply(*response, 503,
        R"({"error":{"message":"Too many AI streams","type":"capacity_error"}})",
        "application/json", true);
    }
  }
  template <class ResponsePtr>
  void
  stream_response(ResponsePtr response, std::string body) {
    stream_response(std::move(response), std::move(body), production_executor(), processAiChatStream);
  }
}  // namespace ai::http
