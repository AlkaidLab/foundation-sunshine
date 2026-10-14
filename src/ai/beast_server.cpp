/**
 * @file src/ai/beast_server.cpp
 * @brief Loopback pilot policy over the production HTTP and SSE transports.
 */
#include "beast_server.h"
#include "http_stream.h"
#include "src/http/server.h"
#include <boost/asio/ip/address.hpp>
#include <future>
#include <limits>
#include <nlohmann/json.hpp>
#include <thread>

namespace ai::beast_http {
  namespace {
    constexpr char route[] = "/api/ai/chat/completions";
    Options
    validated(Options options) {
      if (!options.max_streams || options.max_streams > 16 || !options.io_threads || options.io_threads > 16 ||
          options.io_timeout.count() <= 0 || !options.body_limit || options.body_limit > std::numeric_limits<size_t>::max() - 8192 || options.send_buffer_bytes < 0)
        throw std::invalid_argument("Invalid Beast pilot resource limits");
      if (!boost::asio::ip::make_address(options.address).is_loopback()) throw std::invalid_argument("Beast pilot binds only to loopback");
      return options;
    }
  }  // namespace
  struct Server::Impl {
    Options options;
    Authorize authorize;
    Backend backend;
    web::Server<web::HTTPS> server;
    ai::http::StreamExecutor workers;
    std::thread listener;
    bool started = false;
    bool stopped = false;
    Impl(Options opts, Authorize auth, Backend service):
        options(validated(std::move(opts))), authorize(std::move(auth)), backend(std::move(service)),
        server(options.certificate_file, options.private_key_file, options.client_ca_file), workers(options.max_streams) {
      if (!authorize || !backend) throw std::invalid_argument("Missing Beast pilot policy/backend");
      server.config.address = options.address;
      server.config.port = options.port;
      server.config.thread_pool_size = options.io_threads;
      server.config.header_limit = 8192;
      // The shared parser bounds the complete request. The route also applies
      // the pilot's smaller body-only limit before authorization or upstream IO.
      server.config.max_request_streambuf_size = options.body_limit + 8192;
      server.config.max_response_buffer_size = 32 * 1024;
      server.config.timeout_request = std::chrono::ceil<std::chrono::seconds>(options.io_timeout).count();
      server.config.timeout_content = server.config.timeout_request;
      server.config.timeout_stream = options.io_timeout;
      server.config.send_buffer_bytes = options.send_buffer_bytes;
      auto handle = [this](auto response, auto request) {
        auto reply = [&](web::StatusCode status, std::string body = "") {
          response->close_connection_after_response = true;
          web::CaseInsensitiveMultimap headers { { "Content-Type", "application/json" } };
          if (status == web::StatusCode::unauthorized) headers.emplace("WWW-Authenticate", "Bearer");
          if (status == web::StatusCode::no_content) {
            headers.emplace("Access-Control-Allow-Methods", "POST, OPTIONS");
            headers.emplace("Access-Control-Allow-Headers", "Content-Type, Authorization");
          }
          response->write(status, body, headers);
        };
        if (request->version != 11) return reply(web::StatusCode::http_version_not_supported);
        if (request->path != route || !request->query_string.empty()) return reply(web::StatusCode::not_found);
        if (request->method == "OPTIONS") return reply(web::StatusCode::no_content);
        if (request->method != "POST") return reply(web::StatusCode::method_not_allowed);
        if (request->content.size() > options.body_limit) return reply(web::StatusCode::payload_too_large);
        Request policy_request;
        policy_request.version(request->version);
        policy_request.method_string(request->method);
        policy_request.target(request->path);
        policy_request.body() = request->content.string();
        for (const auto &[name, value] : request->header) policy_request.insert(name, value);
        bool allowed = false;
        try {
          allowed = policy_request.count(boost::beast::http::field::authorization) <= 1 &&
                    authorize(policy_request, { request->remote_endpoint().address().to_string(), request->certificate_sha256 });
        }
        catch (...) {
          return reply(web::StatusCode::internal_server_error);
        }
        if (!allowed) return reply(web::StatusCode::unauthorized);
        const auto type = policy_request[boost::beast::http::field::content_type];
        if (type != "application/json" && !type.starts_with("application/json;")) return reply(web::StatusCode::bad_request);
        try {
          if (!nlohmann::json::parse(policy_request.body()).value("stream", false)) return reply(web::StatusCode::bad_request);
        }
        catch (...) {
          return reply(web::StatusCode::bad_request);
        }
        ai::http::stream_response(response, std::move(policy_request.body()), workers, backend, options.io_timeout);
      };
      for (const auto *method : { "GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH", "TRACE", "CONNECT" })
        server.default_resource[method] = handle;
    }
    void
    stop() {
      if (stopped) return;
      stopped = true;
      // Upstream cancellation and downstream completions need the listener
      // alive until every blocking worker has returned.
      workers.stop();
      server.stop();
      if (listener.joinable()) listener.join();
    }
  };
  Server::Server(Options options, Authorize authorize, Backend backend):
      impl_(std::make_shared<Impl>(std::move(options), std::move(authorize), std::move(backend))) {}
  Server::~Server() { stop(); }
  unsigned short
  Server::start() {
    if (impl_->started || impl_->stopped) throw std::logic_error("Beast pilot cannot restart");
    impl_->started = true;
    auto ready = std::make_shared<std::promise<unsigned short>>();
    auto port = ready->get_future();
    impl_->listener = std::thread([impl = impl_, ready] {
      bool announced = false;
      try {
        impl->server.start([&](unsigned short port) { announced = true; ready->set_value(port); });
      }
      catch (...) {
        if (!announced) ready->set_exception(std::current_exception());
      }
    });
    try {
      return port.get();
    }
    catch (...) {
      impl_->stop();
      throw;
    }
  }
  void
  Server::stop() { impl_->stop(); }
  unsigned
  Server::active_streams() const { return impl_->workers.active(); }
}  // namespace ai::beast_http
