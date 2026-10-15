/**
 * @file src/http/server.h
 * @brief Beast listeners with typed application routes and shared sessions.
 */
#pragma once
#include "types.h"
#include <boost/asio/streambuf.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <openssl/ssl.h>
#include <type_traits>

namespace web {
  struct ServerConfig {
    unsigned short port = 0;
    std::string address;
    bool reuse_address = true;
    size_t thread_pool_size = 1;
    long timeout_request = 5;
    long timeout_content = 300;
    size_t max_request_streambuf_size = 16 * 1024 * 1024;
    size_t max_response_buffer_size = 64 * 1024 * 1024;
    size_t max_connections = 256;
    unsigned header_limit = 16 * 1024;
    std::chrono::milliseconds timeout_stream { 5000 };
    int send_buffer_bytes = 0;
  };
  namespace detail {
    struct FileResponse;
    using Completion = std::function<void(const error_code &)>;
    struct ResponseChannel {
      std::function<void(std::string, Completion, bool, bool)> send;
      std::function<void()> begin_stream;
      std::function<bool()> closed;
      std::function<void()> abort;
      std::function<void(std::shared_ptr<FileResponse>)> send_file;
    };
    class ResponseBuffer: public std::ostream {
      boost::asio::streambuf buffer;
      ResponseChannel channel;
      unsigned version;
      bool keep_alive;
      bool sent = false;
      std::string
      take();

    public:
      bool close_connection_after_response = false;
      ResponseBuffer(const Request &request, ResponseChannel channel);
      ~ResponseBuffer();
      ResponseBuffer(const ResponseBuffer &) = delete;
      ResponseBuffer &
      operator=(const ResponseBuffer &) = delete;
      size_t
      size() const { return buffer.size(); }
      bool
      closed() const { return channel.closed(); }
      bool
      started() const { return sent || buffer.size() != 0; }
      unsigned
      http_version() const { return version; }
      void
      abort() { channel.abort(); }
      void
      begin_stream() {
        close_connection_after_response = true;
        channel.begin_stream();
      }
      void
      send(Completion callback = {});
      void
      write(const char *data, std::streamsize length) { std::ostream::write(data, length); }
      void
      write(StatusCode status = StatusCode::ok, const CaseInsensitiveMultimap &headers = {});
      void
      write(StatusCode status, std::string_view body, const CaseInsensitiveMultimap &headers = {});
      void
      write(StatusCode status, std::istream &body, const CaseInsensitiveMultimap &headers = {});
      // Own an open file until asynchronous serialization finishes. Returns
      // false on open/stat failure, before any response has been admitted.
      bool
      write_file(StatusCode status, const std::filesystem::path &path, const CaseInsensitiveMultimap &headers = {});
      void
      write(std::string_view body, const CaseInsensitiveMultimap &headers = {}) { write(StatusCode::ok, body, headers); }
      void
      write(std::istream &body, const CaseInsensitiveMultimap &headers = {}) { write(StatusCode::ok, body, headers); }
      void
      write(const CaseInsensitiveMultimap &headers) { write(StatusCode::ok, headers); }
    };

    using Dispatch = std::function<void(std::shared_ptr<Request>, ResponseChannel, bool)>;
    class ServerRuntime {
      struct Impl;
      std::shared_ptr<Impl> impl;

    public:
      ServerRuntime(bool tls, std::string certificate, std::string key, std::string ca);
      ~ServerRuntime();
      void
      start(ServerConfig config, Dispatch dispatch, std::function<void(unsigned short)> ready,
        std::function<int(SSL *)> verify, std::function<std::string(X509 *)> identity,
        std::function<void(std::shared_ptr<Request>, const error_code &)> on_error);
      void
      stop();
    };
  }  // namespace detail

  template <class Transport>
  class ServerBase {
  public:
    using Request = web::Request;
    class Response: public detail::ResponseBuffer {
    public:
      using detail::ResponseBuffer::ResponseBuffer;
    };
  };

  struct RoutePattern {
    std::string str;
    std::regex regex;
    RoutePattern(const char *pattern): RoutePattern(std::string(pattern)) {}
    RoutePattern(std::string pattern): str(std::move(pattern)), regex(str) {}
    bool
    operator<(const RoutePattern &other) const { return str < other.str; }
  };

  template <class Transport>
  class Server: public ServerBase<Transport> {
    detail::ServerRuntime runtime;

  public:
    using typename ServerBase<Transport>::Request;
    using typename ServerBase<Transport>::Response;
    using Handler = std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)>;
    ServerConfig config;
    std::map<RoutePattern, std::map<std::string, Handler>> resource;
    std::map<std::string, Handler> default_resource;
    std::function<void(std::shared_ptr<Request>, const error_code &)> on_error;
    // Paired TLS accepts the handshake so it can send GameStream's XML
    // rejection. Verification and identity still happen before any route.
    std::function<int(SSL *)> verify;
    std::function<std::string(X509 *)> identify;
    Handler on_verify_failed;
    explicit Server(const std::string &cert = "", const std::string &key = "", const std::string &ca = ""):
        runtime(!std::is_same_v<Transport, HTTP>, cert, key, ca) {}
    void
    start(std::function<void(unsigned short)> ready = {}) {
      if constexpr (std::is_same_v<Transport, PairedHTTPS>) {
        if (!verify || !identify) throw std::invalid_argument("Paired TLS requires verification and identity policies");
      }
      auto routes = resource;
      auto defaults = default_resource;
      auto denied = on_verify_failed;
      detail::Dispatch dispatch = [routes = std::move(routes), defaults = std::move(defaults), denied = std::move(denied)](auto request, auto channel, bool verified) {
        auto response = std::make_shared<Response>(*request, std::move(channel));
        try {
          if (!verified) {
            response->close_connection_after_response = true;
            if (denied)
              denied(response, request);
            else
              response->write(StatusCode::forbidden);
            return;
          }
          for (const auto &[pattern, methods] : routes) {
            const auto handler = methods.find(request->method);
            if (handler != methods.end() && std::regex_match(request->path, request->path_match, pattern.regex)) {
              handler->second(response, request);
              return;
            }
          }
          if (const auto handler = defaults.find(request->method); handler != defaults.end())
            handler->second(response, request);
          else
            response->write(StatusCode::not_found);
        }
        catch (...) {
          if (response->started())
            response->abort();
          else
            response->write(StatusCode::internal_server_error, R"({"error":"Request handler failed"})", { { "Content-Type", "application/json" } });
        }
      };
      runtime.start(config, std::move(dispatch), std::move(ready), verify, identify, on_error);
    }
    void
    stop() { runtime.stop(); }
  };
}  // namespace web
