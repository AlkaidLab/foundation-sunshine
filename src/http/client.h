/**
 * @file src/http/client.h
 * @brief Beast HTTP clients with explicit deadlines and no hidden retries.
 */
#pragma once
#include "types.h"
#include <boost/asio/io_context.hpp>
#include <boost/system/system_error.hpp>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>

namespace web {
  using io_context = boost::asio::io_context;
  struct ClientConfig {
    long timeout = 30;
    long timeout_connect = 5;
    size_t max_response_streambuf_size = 64 * 1024 * 1024;
    std::string proxy_server;
    // Webhook delivery depends only on the final status and headers. It must
    // not wait for a streaming/oversized response body or replay a POST.
    bool headers_only = false;
  };
  struct ClientResponse {
    std::string http_version, status_code;
    CaseInsensitiveMultimap header;
    Content content;
  };
  namespace detail {
    using ClientCompletion = std::function<void(std::shared_ptr<ClientResponse>, const error_code &)>;
    class ClientRuntime {
      struct Impl;
      std::shared_ptr<Impl> impl;

    public:
      ClientRuntime(bool tls, std::string authority, bool verify, std::string certificate, std::string key, std::string ca);
      ~ClientRuntime();
      void
      request(std::shared_ptr<io_context> io, ClientConfig config, std::string method, std::string target,
        std::string body, CaseInsensitiveMultimap headers, ClientCompletion completion);
      void
      stop();
    };
  }  // namespace detail
  template <class Transport>
  class Client {
    detail::ClientRuntime runtime;

  public:
    using Response = ClientResponse;
    ClientConfig config;
    std::shared_ptr<io_context> io_service;
    explicit Client(const std::string &authority, bool verify = true, const std::string &certificate = "", const std::string &key = "", const std::string &ca = ""):
        runtime(!std::is_same_v<Transport, HTTP>, authority, verify, certificate, key, ca) {}
    void
    request(const std::string &method, const std::string &target, const std::string &body, const CaseInsensitiveMultimap &headers, detail::ClientCompletion completion) {
      if (!io_service) throw std::logic_error("Async HTTP requests require an io_context");
      runtime.request(io_service, config, method, target, body, headers, std::move(completion));
    }
    std::shared_ptr<Response>
    request(const std::string &method, const std::string &target = "/", std::string_view body = "", const CaseInsensitiveMultimap &headers = {}) {
      if (io_service) throw std::logic_error("Use async requests with an externally driven io_context");
      auto io = std::make_shared<io_context>();
      auto completion = std::make_shared<std::promise<std::shared_ptr<Response>>>();
      auto finished = completion->get_future();
      runtime.request(io, config, method, target, std::string(body), headers, [completion](auto response, auto ec) {
        if (ec)
          completion->set_exception(std::make_exception_ptr(boost::system::system_error(ec)));
        else
          completion->set_value(std::move(response));
      });
      io->run();
      return finished.get();
    }
    void
    stop() { runtime.stop(); }
  };
}  // namespace web
