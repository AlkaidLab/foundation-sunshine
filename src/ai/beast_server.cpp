/**
 * @file src/ai/beast_server.cpp
 * @brief Beast framing and TLS sessions, with bounded blocking upstream work.
 */
#include "beast_server.h"
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <openssl/x509.h>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ai::beast_http {
  namespace net = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ssl = net::ssl;
  using tcp = net::ip::tcp;
  using Error = boost::system::error_code;
  constexpr char route[] = "/api/ai/chat/completions";

  Options
  validated(Options options) {
    if (options.max_streams == 0 || options.max_streams > 16 || options.io_threads == 0 || options.io_threads > 16 ||
        options.io_timeout.count() <= 0 || options.body_limit == 0) {
      throw std::invalid_argument("Invalid Beast pilot resource limits");
    }
    if (!net::ip::make_address(options.address).is_loopback()) throw std::invalid_argument("Beast pilot binds only to loopback");
    return options;
  }

  struct Server::Impl: std::enable_shared_from_this<Impl> {
    struct Session;
    Options options;
    Authorize authorize;
    Backend backend;
    net::io_context io;
    ssl::context tls { ssl::context::tls_server };
    tcp::acceptor acceptor { net::make_strand(io) };
    net::executor_work_guard<net::io_context::executor_type> work { io.get_executor() };
    net::thread_pool workers;
    std::vector<std::thread> listeners;
    std::mutex admission;
    std::atomic_bool stopping { false };
    std::atomic_uint active { 0 };
    std::mutex sessions_mutex;
    std::unordered_map<Session *, std::weak_ptr<Session>> sessions;
    bool started = false;

    Impl(Options opts, Authorize auth, Backend service):
        options(validated(std::move(opts))), authorize(std::move(auth)), backend(std::move(service)), workers(options.max_streams) {
      if (!authorize || !backend) throw std::invalid_argument("Missing Beast pilot policy/backend");
      const auto address = net::ip::make_address(options.address);
      tls.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                      ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
      tls.use_certificate_chain_file(options.certificate_file);
      tls.use_private_key_file(options.private_key_file, ssl::context::pem);
      if (!options.client_ca_file.empty()) {
        tls.load_verify_file(options.client_ca_file);
        tls.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
      }
      acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6());
      acceptor.set_option(net::socket_base::reuse_address(true));
      acceptor.bind({ address, options.port });
      acceptor.listen();
    }
    void
    accept();
    bool
    submit(const std::shared_ptr<Session> &session, std::string body);
    void
    stop();
  };

  struct Server::Impl::Session: std::enable_shared_from_this<Session> {
    std::shared_ptr<Impl> owner;
    beast::ssl_stream<beast::tcp_stream> stream;
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    std::array<char, 1> probe {};
    std::atomic_bool cancelled { false };
    bool probe_pending = false;
    bool finishing = false;
    bool header_sent = false;
    bool closed = false;
    std::optional<http::response<http::string_body>> reply;
    http::response<http::empty_body> sse { http::status::ok, 11 };
    std::optional<http::response_serializer<http::empty_body>> serializer;
    std::string chunk;
    std::optional<http::chunk_body<net::const_buffer>> chunk_frame;
    decltype(http::make_chunk_last()) last_chunk;
    std::shared_ptr<std::promise<bool>> pending_send;

    Session(std::shared_ptr<Impl> impl, tcp::socket socket): owner(std::move(impl)), stream(std::move(socket), owner->tls) {
      parser.body_limit(owner->options.body_limit);
      parser.header_limit(8192);
      if (owner->options.send_buffer_bytes > 0) {
        beast::get_lowest_layer(stream).socket().set_option(net::socket_base::send_buffer_size(owner->options.send_buffer_bytes));
      }
    }
    void
    run() {
      auto self = shared_from_this();
      beast::get_lowest_layer(stream).expires_after(owner->options.io_timeout);
      stream.async_handshake(ssl::stream_base::server, [self](Error ec) {
        if (ec || self->owner->stopping) return self->close();
        beast::get_lowest_layer(self->stream).expires_after(self->owner->options.io_timeout);
        http::async_read(self->stream, self->buffer, self->parser, [self](Error ec, auto) {
          if (ec == http::error::body_limit) return self->respond(413, R"({"error":"Request too large"})");
          if (ec == http::error::header_limit) return self->respond(431, R"({"error":"Headers too large"})");
          if (ec || self->owner->stopping) return self->close();
          self->request();
        });
      });
    }
    Peer
    peer() {
      Peer result;
      Error ec;
      const auto remote = beast::get_lowest_layer(stream).socket().remote_endpoint(ec);
      if (!ec) result.address = remote.address().to_string();
      auto *cert = SSL_get_peer_certificate(stream.native_handle());
      if (cert) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned length = 0;
        if (X509_digest(cert, EVP_sha256(), digest, &length)) {
          constexpr char hex[] = "0123456789abcdef";
          for (unsigned i = 0; i < length; ++i) {
            result.certificate_sha256 += hex[digest[i] >> 4];
            result.certificate_sha256 += hex[digest[i] & 15];
          }
        }
        X509_free(cert);
      }
      return result;
    }
    void
    request() {
      const auto &request = parser.get();
      if (request.version() != 11) return respond(505, R"({"error":"This pilot requires HTTP/1.1"})");
      if (request.target() != route) return respond(404, R"({"error":"Not found"})");
      if (request.method() == http::verb::options) return respond(204, "");
      if (request.method() != http::verb::post) return respond(405, R"({"error":"Method not allowed"})");
      bool allowed = false;
      try {
        allowed = request.count(http::field::authorization) <= 1 && owner->authorize(request, peer());
      }
      catch (...) {
        return respond(500, R"({"error":"Authorization policy failed"})");
      }
      if (!allowed) return respond(401, R"({"error":"Unauthorized"})");
      const auto type = request[http::field::content_type];
      if (type != "application/json" && !type.starts_with("application/json;")) return respond(400, R"({"error":"JSON content type required"})");
      try {
        if (!nlohmann::json::parse(request.body()).value("stream", false)) return respond(400, R"({"error":"This pilot requires stream=true"})");
      }
      catch (...) {
        return respond(400, R"({"error":"Invalid JSON request"})");
      }
      if (!owner->submit(shared_from_this(), request.body())) return respond(503, R"({"error":"Too many AI streams"})");
      // One read and one write may coexist. A read left pending after the POST
      // observes FIN/RST even before upstream has produced its first chunk.
      beast::get_lowest_layer(stream).expires_never();
      probe_pending = true;
      auto self = shared_from_this();
      stream.async_read_some(net::buffer(probe), [self](Error, auto) {
        self->probe_pending = false;
        if (self->finishing && !self->closed)
          self->shutdown_tls();
        else
          self->close();  // Extra pipelined data is unsupported too.
      });
    }
    void
    finish_send(bool ok) {
      auto completion = std::move(pending_send);
      if (completion) completion->set_value(ok);
    }
    void
    close() {
      if (closed) return;
      closed = true;
      cancelled = true;
      beast::get_lowest_layer(stream).close();
      finish_send(false);
      std::lock_guard lock(owner->sessions_mutex);
      owner->sessions.erase(this);
    }
    void
    shutdown_tls() {
      if (closed) return;
      auto self = shared_from_this();
      beast::get_lowest_layer(stream).expires_after(owner->options.io_timeout);
      stream.async_shutdown([self](Error) { self->close(); });
    }
    void
    finish() {
      finishing = true;
      if (probe_pending) {
        Error ignored;
        beast::get_lowest_layer(stream).socket().cancel(ignored);
      }
      else
        shutdown_tls();
    }
    void
    respond(int status, std::string body, std::string type = "application/json") {
      if (closed || owner->stopping) return close();
      reply.emplace(static_cast<http::status>(status), 11);
      reply->keep_alive(false);
      reply->set(http::field::content_type, type);
      if (status == 401) reply->set(http::field::www_authenticate, "Bearer");
      if (status == 503) reply->set(http::field::retry_after, "1");
      if (status == 204) {
        reply->set(http::field::access_control_allow_methods, "POST, OPTIONS");
        reply->set(http::field::access_control_allow_headers, "Content-Type, Authorization");
      }
      reply->body() = std::move(body);
      reply->prepare_payload();
      auto self = shared_from_this();
      beast::get_lowest_layer(stream).expires_after(owner->options.io_timeout);
      http::async_write(stream, *reply, [self](Error ec, auto) {
        if (ec)
          self->close();
        else
          self->finish();
      });
    }
    void
    write_chunk() {
      if (closed || owner->stopping) return close();
      chunk_frame.emplace(net::const_buffer(chunk.data(), chunk.size()));
      beast::get_lowest_layer(stream).expires_after(owner->options.io_timeout);
      auto self = shared_from_this();
      net::async_write(stream, *chunk_frame, [self](Error ec, auto) {
        self->chunk_frame.reset();
        self->chunk.clear();
        if (ec)
          self->close();
        else
          self->finish_send(true);
      });
    }
    bool
    send_chunk(const char *data, size_t size) {
      if (cancelled || owner->stopping) return false;
      // curl delivers at most CURL_MAX_WRITE_SIZE (16 KiB) per write callback.
      // The pilot admits only one owned chunk per session at a time.
      if (size > 16 * 1024) return false;
      if (size == 0) return true;
      auto completion = std::make_shared<std::promise<bool>>();
      auto done = completion->get_future();
      auto self = shared_from_this();
      net::post(stream.get_executor(), [self, data = std::string(data, size), completion] {
        if (self->closed || self->owner->stopping) {
          completion->set_value(false);
          return;
        }
        self->pending_send = completion;
        self->chunk = data;
        if (self->header_sent) return self->write_chunk();
        self->sse.keep_alive(false);
        self->sse.chunked(true);
        self->sse.set(http::field::content_type, "text/event-stream");
        self->sse.set(http::field::cache_control, "no-cache");
        self->serializer.emplace(self->sse);
        beast::get_lowest_layer(self->stream).expires_after(self->owner->options.io_timeout);
        http::async_write_header(self->stream, *self->serializer, [self](Error ec, auto) {
          if (ec) return self->close();
          self->header_sent = true;
          self->write_chunk();
        });
      });
      if (done.wait_for(owner->options.io_timeout + std::chrono::milliseconds(100)) != std::future_status::ready) {
        cancelled = true;
        net::post(stream.get_executor(), [self] { self->close(); });
        return false;
      }
      return done.get();
    }
    void
    complete(AiProxyResult result) {
      if (closed || cancelled || owner->stopping) return close();
      if (!header_sent) return respond(result.httpCode, std::move(result.body), std::move(result.contentType));
      if (result.httpCode != 200) return close();  // A truncated SSE stream must not look successful.
      auto self = shared_from_this();
      beast::get_lowest_layer(stream).expires_after(owner->options.io_timeout);
      net::async_write(stream, last_chunk, [self](Error ec, auto) {
        if (ec)
          self->close();
        else
          self->finish();
      });
    }
  };

  void
  Server::Impl::accept() {
    auto self = shared_from_this();
    acceptor.async_accept(net::make_strand(io), [self](Error ec, tcp::socket socket) {
      if (self->stopping) return;
      if (!ec) {
        auto session = std::make_shared<Session>(self, std::move(socket));
        {
          std::lock_guard lock(self->sessions_mutex);
          self->sessions.emplace(session.get(), session);
        }
        session->run();
      }
      self->accept();
    });
  }
  bool
  Server::Impl::submit(const std::shared_ptr<Session> &session, std::string body) {
    std::lock_guard lock(admission);
    if (stopping || active >= options.max_streams) return false;
    ++active;
    try {
      auto self = shared_from_this();
      net::post(workers, [self, session, body = std::move(body)] {
        struct Release {
          std::atomic_uint &active;
          ~Release() { --active; }
        } release { self->active };
        bool streaming = false;
        auto last_send = std::chrono::steady_clock::now();
        auto sink = [&](const char *data, size_t size) {
          const bool ok = session->send_chunk(data, size);
          if (ok && size) {
            streaming = true;
            last_send = std::chrono::steady_clock::now();
          }
          return ok;
        };
        AiProxyResult result;
        try {
          result = self->backend(body, sink, [&] {
            if (session->cancelled || self->stopping) return true;
            if (streaming && std::chrono::steady_clock::now() - last_send >= std::chrono::seconds(1)) {
              constexpr char heartbeat[] = ": keep-alive\n\n";
              return !sink(heartbeat, sizeof(heartbeat) - 1);
            }
            return false;
          });
        }
        catch (...) {
          result = { 500, R"({"error":"AI stream failed"})", "application/json" };
        }
        net::post(session->stream.get_executor(), [session, result = std::move(result)]() mutable { session->complete(std::move(result)); });
      });
    }
    catch (...) {
      --active;
      throw;
    }
    return true;
  }
  void
  Server::Impl::stop() {
    {
      std::lock_guard lock(admission);
      if (stopping.exchange(true)) return;
    }
    // Cancel sessions while the io_context still delivers read/write completions.
    net::post(acceptor.get_executor(), [self = shared_from_this()] {
      Error ignored;
      self->acceptor.close(ignored);
      std::lock_guard lock(self->sessions_mutex);
      for (const auto &[key, weak] : self->sessions) {
        if (auto session = weak.lock()) net::post(session->stream.get_executor(), [session] { session->close(); });
      }
    });
    workers.join();
    work.reset();
    if (listeners.empty()) io.run();
    for (auto &thread : listeners) thread.join();
    listeners.clear();
  }
  Server::Server(Options options, Authorize authorize, Backend backend): impl_(std::make_shared<Impl>(std::move(options), std::move(authorize), std::move(backend))) {}
  Server::~Server() { stop(); }
  unsigned short
  Server::start() {
    if (impl_->started || impl_->stopping) throw std::logic_error("Beast pilot cannot restart");
    impl_->started = true;
    const auto port = impl_->acceptor.local_endpoint().port();
    impl_->accept();
    for (unsigned i = 0; i < impl_->options.io_threads; ++i) {
      impl_->listeners.emplace_back([impl = impl_] { impl->io.run(); });
    }
    return port;
  }
  void
  Server::stop() {
    if (impl_->started) impl_->stop();
  }
  unsigned
  Server::active_streams() const { return impl_->active; }
}  // namespace ai::beast_http
