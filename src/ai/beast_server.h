/**
 * @file src/ai/beast_server.h
 * @brief Opt-in loopback TLS/HTTP transport pilot for AI SSE.
 */
#pragma once

#include "proxy.h"
#include <boost/beast/http.hpp>
#include <chrono>
#include <functional>
#include <memory>

namespace ai::beast_http {
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  struct Peer {
    std::string address;
    std::string certificate_sha256;
  };
  struct Options {
    std::string certificate_file;
    std::string private_key_file;
    std::string client_ca_file;  // Non-empty enables mandatory client certificates.
    std::string address = "127.0.0.1";
    unsigned short port = 0;
    unsigned io_threads = 2;
    unsigned max_streams = 4;
    std::size_t body_limit = 1024 * 1024;
    std::chrono::milliseconds io_timeout { 5000 };
    int send_buffer_bytes = 0;  // Zero keeps the OS default.
  };
  using Authorize = std::function<bool(const Request &, const Peer &)>;
  using Backend = std::function<AiProxyResult(const std::string &, std::function<bool(const char *, size_t)>, std::function<bool()>)>;

  // Both callbacks are mandatory and may run concurrently. The backend's chunk
  // and cancellation callbacks must be invoked serially, as in processAiChatStream.
  // start/stop are called from the owning thread, never from a transport callback.
  class Server {
  public:
    Server(Options options, Authorize authorize, Backend backend);
    ~Server();
    Server(const Server &) = delete;
    Server &
    operator=(const Server &) = delete;
    unsigned short
    start();
    unsigned
    active_streams() const;
    void
    stop();

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
  };
}  // namespace ai::beast_http
