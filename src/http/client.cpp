/**
 * @file src/http/client.cpp
 * @brief Beast HTTP/TLS transfers with bounded parsing and explicit cancellation.
 */
#include "client.h"
#include "headers.h"
#include "tls_trust.h"
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <curl/curl.h>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace web::detail {
  namespace net = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ssl = net::ssl;
  using tcp = net::ip::tcp;
  struct ClientRuntime::Impl: std::enable_shared_from_this<Impl> {
    struct OperationBase {
      virtual ~OperationBase() = default;
      virtual void
      cancel() = 0;
    };
    template <bool Tls>
    struct Operation;
    bool tls_enabled;
    std::string host, port, authority, prefix;
    ssl::context tls { ssl::context::tls_client };
    std::mutex mutex;
    bool stopped = false;
    std::unordered_map<OperationBase *, std::weak_ptr<OperationBase>> active;
    Impl(bool encrypted, std::string server, bool verify, const std::string &cert, const std::string &key, const std::string &ca): tls_enabled(encrypted) {
      if (server.empty() || std::any_of(server.begin(), server.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) throw std::invalid_argument("Invalid HTTP authority");
      std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
      const auto address = std::string(encrypted ? "https://" : "http://") + server;
      if (!url || curl_url_set(url.get(), CURLUPART_URL, address.c_str(), 0) != CURLUE_OK) throw std::invalid_argument("Invalid HTTP authority");
      auto part = [&](CURLUPart part, unsigned flags = 0) {
        char *raw = nullptr;
        if (curl_url_get(url.get(), part, &raw, flags) != CURLUE_OK) return std::string {};
        std::unique_ptr<char, decltype(&curl_free)> value(raw, curl_free);
        return std::string(value.get());
      };
      if (!part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() || !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty()) throw std::invalid_argument("HTTP authority contains unsupported components");
      host = part(CURLUPART_HOST);
      port = part(CURLUPART_PORT, CURLU_DEFAULT_PORT);
      prefix = part(CURLUPART_PATH);
      if (host.empty() || port.empty()) throw std::invalid_argument("Missing HTTP host or port");
      authority = host + ':' + port;
      if (host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
      if (prefix == "/") prefix.clear();
      if (!encrypted) return;
      tls.set_options(ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
      if (verify) {
        if (ca.empty())
          load_system_trust(tls);
        else
          tls.load_verify_file(ca);
        tls.set_verify_mode(ssl::verify_peer);
        tls.set_verify_callback(ssl::host_name_verification(host));
      }
      else
        tls.set_verify_mode(ssl::verify_none);
      if (!cert.empty() || !key.empty()) {
        if (cert.empty() || key.empty()) throw std::invalid_argument("Both TLS certificate and key are required");
        tls.use_certificate_chain_file(cert);
        tls.use_private_key_file(key, ssl::context::pem);
        if (SSL_CTX_check_private_key(tls.native_handle()) != 1) throw std::invalid_argument("TLS certificate and key do not match");
      }
    }
  };
  template <bool Tls>
  struct ClientRuntime::Impl::Operation: OperationBase, std::enable_shared_from_this<Operation<Tls>> {
    using Stream = std::conditional_t<Tls, beast::ssl_stream<beast::tcp_stream>, beast::tcp_stream>;
    std::shared_ptr<Impl> owner;
    net::strand<io_context::executor_type> executor;
    Stream stream;
    tcp::resolver resolver;
    net::steady_timer deadline, connect_deadline;
    net::executor_work_guard<io_context::executor_type> work;
    ClientConfig config;
    ClientCompletion completion;
    http::request<http::string_body> message;
    std::optional<http::response_parser<http::string_body>> parser;
    beast::flat_buffer buffer;
    std::shared_ptr<ClientResponse> response = std::make_shared<ClientResponse>();
    std::atomic_bool completed { false }, cancellation_requested { false };
    unsigned interim = 0;
    static Stream
    make_stream(const net::strand<io_context::executor_type> &executor, ssl::context &tls) {
      if constexpr (Tls)
        return Stream(executor, tls);
      else
        return Stream(executor);
    }
    Operation(std::shared_ptr<Impl> owner, io_context &io, ClientConfig config, std::string method, std::string target, std::string body, const CaseInsensitiveMultimap &headers, ClientCompletion completion):
        owner(std::move(owner)), executor(net::make_strand(io)), stream(make_stream(executor, this->owner->tls)), resolver(executor), deadline(executor), connect_deadline(executor), work(io.get_executor()), config(std::move(config)), completion(std::move(completion)) {
      if (target.empty()) target = "/";
      if (target.front() != '/' || std::any_of(target.begin(), target.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) throw std::invalid_argument("Invalid HTTP target");
      if (method.empty() || !std::all_of(method.begin(), method.end(), [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) throw std::invalid_argument("Invalid HTTP method");
      message.version(11);
      message.method_string(method);
      message.target(this->owner->prefix + target);
      apply_headers(message, headers);
      message.set(http::field::host, this->owner->authority);
      message.keep_alive(false);
      message.body() = std::move(body);
      message.prepare_payload();
    }
    void
    cancel() override {
      if (completed) return;
      cancellation_requested = true;
      net::dispatch(executor, [self = this->shared_from_this()] { self->finish(net::error::operation_aborted); });
    }
    void
    finish(error_code ec) {
      if (completed.exchange(true)) return;
      deadline.cancel();
      connect_deadline.cancel();
      resolver.cancel();
      beast::get_lowest_layer(stream).close();
      {
        std::lock_guard lock(owner->mutex);
        owner->active.erase(this);
      }
      auto callback = std::move(completion);
      work.reset();
      if (callback) {
        try {
          callback(response, ec);
        }
        catch (...) {
        }
      }
    }
    void
    start() {
      if (completed) return;
      if (cancellation_requested) return finish(net::error::operation_aborted);
      auto self = this->shared_from_this();
      deadline.expires_after(std::chrono::seconds(config.timeout));
      deadline.async_wait([self](error_code ec) { if (!ec) self->finish(net::error::timed_out); });
      connect_deadline.expires_after(std::chrono::seconds(std::min(config.timeout, config.timeout_connect)));
      connect_deadline.async_wait([self](error_code ec) { if (!ec) self->finish(net::error::timed_out); });
      if constexpr (Tls) {
        error_code address_error;
        net::ip::make_address(owner->host, address_error);
        if (address_error && SSL_set_tlsext_host_name(stream.native_handle(), owner->host.c_str()) != 1) return finish(net::error::invalid_argument);
      }
      resolver.async_resolve(owner->host, owner->port, [self](error_code ec, auto endpoints) {
        if (self->completed) return;
        if (ec) return self->finish(ec);
        beast::get_lowest_layer(self->stream).async_connect(endpoints, [self](error_code ec, auto) {
          if (self->completed) return;
          if (ec) return self->finish(ec);
          if constexpr (Tls)
            self->stream.async_handshake(ssl::stream_base::client, [self](error_code ec) {
              if (self->completed) return;
              if (ec)
                self->finish(ec);
              else
                self->write();
            });
          else
            self->write();
        });
      });
    }
    void
    write() {
      if (completed) return;
      connect_deadline.cancel();
      auto self = this->shared_from_this();
      http::async_write(stream, message, [self](error_code ec, auto) {
        if (self->completed) return;
        if (ec)
          self->finish(ec);
        else
          self->read_header();
      });
    }
    void
    read_header() {
      if (completed) return;
      parser.emplace();
      parser->body_limit(config.max_response_streambuf_size);
      parser->header_limit(static_cast<unsigned>(std::min<size_t>(16 * 1024, config.max_response_streambuf_size)));
      if (config.headers_only || message.method() == http::verb::head) parser->skip(true);
      auto self = this->shared_from_this();
      http::async_read_header(stream, buffer, *parser, [self](error_code ec, auto) {
        if (self->completed) return;
        if (ec) return self->finish(ec);
        const auto &message = self->parser->get();
        if (message.result_int() < 200 && message.result_int() != 101) {
          if (++self->interim > 8) return self->finish(make_error_code(boost::system::errc::protocol_error));
          return self->read_header();
        }
        self->response->http_version = message.version() == 10 ? "1.0" : "1.1";
        self->response->status_code = std::to_string(message.result_int()) + ' ' + std::string(message.reason());
        for (const auto &field : message) self->response->header.emplace(field.name_string(), field.value());
        if (self->config.headers_only || self->parser->is_done()) return self->finish({});
        http::async_read(self->stream, self->buffer, *self->parser, [self](error_code ec, auto) {
          if (self->completed) return;
          self->response->content.str(self->parser->get().body());
          self->finish(ec);
        });
      });
    }
  };
  ClientRuntime::ClientRuntime(bool tls, std::string authority, bool verify, std::string certificate, std::string key, std::string ca):
      impl(std::make_shared<Impl>(tls, std::move(authority), verify, certificate, key, ca)) {}
  ClientRuntime::~ClientRuntime() { stop(); }
  void
  ClientRuntime::stop() {
    std::vector<std::shared_ptr<Impl::OperationBase>> active;
    {
      std::lock_guard lock(impl->mutex);
      if (impl->stopped) return;
      impl->stopped = true;
      for (const auto &[key, weak] : impl->active)
        if (auto operation = weak.lock()) active.push_back(std::move(operation));
    }
    for (auto &operation : active) operation->cancel();
  }
  void
  ClientRuntime::request(std::shared_ptr<io_context> io, ClientConfig config, std::string method, std::string target, std::string body, CaseInsensitiveMultimap headers, ClientCompletion completion) {
    if (!io || !completion || config.timeout <= 0 || config.timeout_connect <= 0 || !config.max_response_streambuf_size) throw std::invalid_argument("Invalid HTTP client options");
    if (!config.proxy_server.empty()) throw std::invalid_argument("HTTP proxy mode is not supported");
    std::lock_guard lock(impl->mutex);
    if (impl->stopped) throw std::logic_error("HTTP client has stopped");
    if (impl->tls_enabled) {
      auto operation = std::make_shared<Impl::Operation<true>>(impl, *io, std::move(config), std::move(method), std::move(target), std::move(body), headers, std::move(completion));
      impl->active.emplace(operation.get(), operation);
      net::post(operation->executor, [operation] { operation->start(); });
    }
    else {
      auto operation = std::make_shared<Impl::Operation<false>>(impl, *io, std::move(config), std::move(method), std::move(target), std::move(body), headers, std::move(completion));
      impl->active.emplace(operation.get(), operation);
      net::post(operation->executor, [operation] { operation->start(); });
    }
  }
}  // namespace web::detail
