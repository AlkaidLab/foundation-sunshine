/**
 * @file src/http/server.cpp
 * @brief Shared Beast request parsing, TLS identity and connection lifecycle.
 */
#include "server.h"
#include "headers.h"
#include "ostream.h"
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
  #include <windows.h>
#endif

namespace web::detail {
  namespace net = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace ssl = net::ssl;
  using tcp = net::ip::tcp;

  namespace {
    template <class Message>
    void
    reason(Message &message) {
      if (message.result_int() == 444) message.reason("No Response");
    }
  }  // namespace

  struct FileResponse {
    http::response<http::file_body> message;
    // Reserve for the serializer's small body buffer and owned HTTP fields,
    // not the file's length. Large files never enter the string write queue.
    size_t queue_cost = 16 * 1024;
    FileResponse(StatusCode status, unsigned version): message(status, version) {}
  };

  bool
  ResponseBuffer::write_file(StatusCode status, const std::filesystem::path &path, const CaseInsensitiveMultimap &values) {
    if (started()) throw std::logic_error("File response must be the first response write");
    auto file = std::make_shared<FileResponse>(status, version);
    error_code ec;
    beast::file handle;
#ifdef _WIN32
    // Logs stay open for writing and may be rotated during a download. Beast's
    // default Windows scan mode denies writers; adopt a shareable native handle.
    const auto native = CreateFileW(path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (native == INVALID_HANDLE_VALUE) return false;
    handle.native_handle(native);
#else
    handle.open(path.c_str(), beast::file_mode::scan, ec);
    if (ec) return false;
#endif
    file->message.body().reset(std::move(handle), ec);
    if (ec) return false;
    apply_headers(file->message, values);
    file->message.keep_alive(keep_alive && !close_connection_after_response);
    // Capture the open handle's size. Appends are excluded from this response;
    // truncation is reported as a short transfer instead of a successful body.
    file->message.prepare_payload();
    reason(file->message);
    for (const auto &field : file->message) {
      const auto size = field.name_string().size() + field.value().size() + 4;
      if (size > buffer.max_size() - file->queue_cost) throw std::length_error("HTTP file response headers exceed buffer limit");
      file->queue_cost += size;
    }
    sent = true;
    channel.send_file(std::move(file));
    return true;
  }

  ResponseBuffer::ResponseBuffer(const Request &request, ResponseChannel channel):
      std::ostream(nullptr), buffer(64 * 1024 * 1024), channel(std::move(channel)), version(request.version), keep_alive(request.keep_alive) {
    rdbuf(&buffer);
    exceptions(std::ios::badbit | std::ios::failbit);
  }
  std::string
  ResponseBuffer::take() {
    auto result = beast::buffers_to_string(buffer.data());
    buffer.consume(buffer.size());
    return result;
  }
  ResponseBuffer::~ResponseBuffer() {
    try {
      channel.send(take(), {}, true, close_connection_after_response || !keep_alive);
    }
    catch (...) {
      channel.abort();
    }
  }
  void
  ResponseBuffer::send(Completion callback) {
    sent = true;
    channel.send(take(), std::move(callback), false, false);
  }
  void
  ResponseBuffer::write(StatusCode status, const CaseInsensitiveMultimap &values) {
    http::response<http::empty_body> message { status, version };
    apply_headers(message, values);
    const bool sse = message[http::field::content_type] == "text/event-stream";
    if (sse) begin_stream();
    message.keep_alive(keep_alive && !close_connection_after_response);
    if (!message.count(http::field::content_length) && !message.chunked() && !sse) message.prepare_payload();
    reason(message);
    http::response_serializer<http::empty_body> serializer { message };
    BufferedWriteStream stream { *this };
    http::write_header(stream, serializer);
  }
  void
  ResponseBuffer::write(StatusCode status, std::string_view body, const CaseInsensitiveMultimap &values) {
    if (const auto type = values.find("Content-Type"); type != values.end() && type->second == "text/event-stream") {
      write(status, values);
      std::ostream::write(body.data(), static_cast<std::streamsize>(body.size()));
      return;
    }
    http::response<http::string_body> message { status, version };
    apply_headers(message, values);
    message.keep_alive(keep_alive && !close_connection_after_response);
    message.body() = body;
    message.prepare_payload();
    reason(message);
    BufferedWriteStream stream { *this };
    http::write(stream, message);
  }
  void
  ResponseBuffer::write(StatusCode status, std::istream &body, const CaseInsensitiveMultimap &values) {
    // Existing static asset routes own their ifstream only for this call.
    std::string bytes;
    std::array<char, 64 * 1024> block;
    while (body) {
      body.read(block.data(), block.size());
      const auto length = static_cast<size_t>(body.gcount());
      if (length > buffer.max_size() - bytes.size()) throw std::length_error("HTTP asset exceeds response buffer limit");
      bytes.append(block.data(), length);
    }
    if (!body.eof()) throw std::runtime_error("HTTP asset read failed");
    write(status, bytes, values);
  }

  struct ServerRuntime::Impl: std::enable_shared_from_this<Impl> {
    struct SessionBase {
      virtual ~SessionBase() = default;
      virtual void
      cancel() = 0;
    };
    template <bool Tls>
    struct Session;
    bool tls_enabled;
    std::string certificate, key, ca;
    ServerConfig config;
    Dispatch dispatch;
    std::function<int(SSL *)> verify;
    std::function<std::string(X509 *)> identity;
    std::function<void(std::shared_ptr<Request>, const error_code &)> on_error;
    net::io_context io;
    ssl::context tls { ssl::context::tls_server };
    tcp::acceptor acceptor { net::make_strand(io) };
    net::steady_timer accept_retry { acceptor.get_executor() };
    std::optional<net::executor_work_guard<net::io_context::executor_type>> work;
    std::atomic_bool stopping { false };
    bool started = false;
    std::mutex setup_mutex, sessions_mutex;
    std::unordered_map<SessionBase *, std::weak_ptr<SessionBase>> sessions;

    Impl(bool encrypted, std::string certificate, std::string key, std::string ca):
        tls_enabled(encrypted), certificate(std::move(certificate)), key(std::move(key)), ca(std::move(ca)) {}
    void
    error(const std::shared_ptr<Request> &request, error_code ec) {
      if (on_error && ec) {
        try {
          on_error(request, ec);
        }
        catch (...) {
        }
      }
    }
    void
    stop() {
      std::lock_guard lock(setup_mutex);
      if (stopping.exchange(true) || !started) return;
      net::post(acceptor.get_executor(), [self = shared_from_this()] {
        error_code ignored;
        self->acceptor.close(ignored);
        self->accept_retry.cancel();
        {
          std::lock_guard lock(self->sessions_mutex);
          for (auto &[key, weak] : self->sessions)
            if (auto session = weak.lock()) session->cancel();
        }
        self->work.reset();
      });
    }
    void
    accept();
  };

  template <bool Tls>
  struct ServerRuntime::Impl::Session: SessionBase, std::enable_shared_from_this<Session<Tls>> {
    using Stream = std::conditional_t<Tls, beast::ssl_stream<beast::tcp_stream>, beast::tcp_stream>;
    std::shared_ptr<Impl> owner;
    Stream stream;
    beast::flat_buffer buffer;
    std::optional<http::request_parser<http::string_body>> parser;
    std::shared_ptr<Request> request = std::make_shared<Request>();
    std::string client_uuid;
    std::string certificate_sha256;
    tcp::endpoint remote, local;
    std::atomic_bool closed { false };
    bool streaming = false, probe_pending = false, finishing = false;
    net::steady_timer response_timer;
    std::array<char, 1> probe {};
    struct Write {
      std::string bytes;
      Completion callback;
      bool final = false, close = false, completed = false;
      std::shared_ptr<FileResponse> file;
      size_t
      cost() const { return file ? file->queue_cost : bytes.size(); }
      void
      complete(error_code ec) {
        if (completed) return;
        completed = true;
        if (callback) {
          try {
            callback(ec);
          }
          catch (...) {
          }
        }
      }
    };
    std::deque<std::shared_ptr<Write>> queue;
    size_t queued_bytes = 0;
    bool writing = false;
    static Stream
    make_stream(tcp::socket socket, ssl::context &context) {
      if constexpr (Tls)
        return Stream(std::move(socket), context);
      else
        return Stream(std::move(socket));
    }
    Session(std::shared_ptr<Impl> owner, tcp::socket socket): owner(std::move(owner)), stream(make_stream(std::move(socket), this->owner->tls)), response_timer(stream.get_executor()) {
      error_code ec;
      remote = beast::get_lowest_layer(stream).socket().remote_endpoint(ec);
      local = beast::get_lowest_layer(stream).socket().local_endpoint(ec);
      request->remote = remote;
      request->local = local;
      // Optional buffer tuning must not turn a socket error into listener shutdown.
      if (this->owner->config.send_buffer_bytes > 0)
        beast::get_lowest_layer(stream).socket().set_option(net::socket_base::send_buffer_size(this->owner->config.send_buffer_bytes), ec);
    }
    void
    cancel() override {
      net::post(stream.get_executor(), [self = this->shared_from_this()] { self->close(net::error::operation_aborted); });
    }
    void
    close(error_code ec = {}) {
      if (closed.exchange(true)) return;
      response_timer.cancel();
      beast::get_lowest_layer(stream).close();
      for (auto &write : queue) write->complete(ec ? ec : net::error::operation_aborted);
      queue.clear();
      queued_bytes = 0;
      owner->error(request, ec);
      std::lock_guard lock(owner->sessions_mutex);
      owner->sessions.erase(this);
    }
    bool
    verify_peer() {
      if constexpr (Tls) {
        if (owner->verify && !owner->verify(stream.native_handle())) return false;
        client_uuid.clear();
        certificate_sha256.clear();
        std::unique_ptr<X509, decltype(&X509_free)> cert(
#if OPENSSL_VERSION_MAJOR >= 3
          SSL_get1_peer_certificate(stream.native_handle()),
#else
          SSL_get_peer_certificate(stream.native_handle()),
#endif
          X509_free);
        if (cert) {
          if (owner->identity) client_uuid = owner->identity(cert.get());
          unsigned char digest[EVP_MAX_MD_SIZE];
          unsigned length = 0;
          if (X509_digest(cert.get(), EVP_sha256(), digest, &length)) {
            constexpr char hex[] = "0123456789abcdef";
            for (unsigned i = 0; i < length; ++i) {
              certificate_sha256 += hex[digest[i] >> 4];
              certificate_sha256 += hex[digest[i] & 15];
            }
          }
        }
        if (owner->verify && owner->identity && client_uuid.empty()) return false;
      }
      return true;
    }
    // Own the session in the coroutine frame, rather than capturing a raw
    // this pointer or the short-lived closure of a coroutine lambda.
    static net::awaitable<void>
    read_request(std::shared_ptr<Session> self, bool handshake) {
      error_code ec;
      const auto token = net::redirect_error(net::use_awaitable, ec);
      if (self->owner->stopping || self->closed) {
        self->close();
        co_return;
      }
      if constexpr (Tls) {
        if (handshake) {
          beast::get_lowest_layer(self->stream).expires_after(std::chrono::seconds(self->owner->config.timeout_request));
          co_await self->stream.async_handshake(ssl::stream_base::server, token);
          if (ec || self->owner->stopping) {
            self->close(ec);
            co_return;
          }
          if (!self->verify_peer()) {
            self->deliver(false);
            co_return;
          }
        }
      }
      self->request = std::make_shared<Request>();
      self->request->remote = self->remote;
      self->request->local = self->local;
      self->request->client_uuid = self->client_uuid;
      self->request->certificate_sha256 = self->certificate_sha256;
      self->parser.emplace();
      const auto limit = self->owner->config.max_request_streambuf_size;
      self->parser->body_limit(limit);
      self->parser->header_limit(static_cast<unsigned>(std::min<size_t>(self->owner->config.header_limit, limit)));
      beast::get_lowest_layer(self->stream).expires_after(std::chrono::seconds(self->owner->config.timeout_request));
      const auto header_bytes = co_await http::async_read_header(self->stream, self->buffer, *self->parser, token);
      if (ec) {
        self->parse_error(ec);
        co_return;
      }
      if (header_bytes > limit || (self->parser->content_length() && *self->parser->content_length() > limit - header_bytes)) {
        self->reject(413);
        co_return;
      }
      self->parser->body_limit(limit - header_bytes);
      self->request->header_read_time = std::chrono::system_clock::now();
      const auto expect = self->parser->get()[http::field::expect];
      if (!expect.empty() && !beast::iequals(expect, "100-continue")) {
        self->reject(417);
        co_return;
      }
      if (!expect.empty() && !self->parser->is_done()) {
        // The interim message stays alive in the frame until its write finishes.
        http::response<http::empty_body> interim { http::status::continue_, self->parser->get().version() };
        co_await http::async_write(self->stream, interim, token);
        if (ec) {
          self->close(ec);
          co_return;
        }
      }
      beast::get_lowest_layer(self->stream).expires_after(std::chrono::seconds(self->owner->config.timeout_content));
      co_await http::async_read(self->stream, self->buffer, *self->parser, token);
      if (ec) {
        self->parse_error(ec);
        co_return;
      }
      const auto &message = self->parser->get();
      self->request->method = message.method_string();
      const std::string target(message.target());
      if (target.empty() || target.front() != '/' || target.find('#') != std::string::npos) {
        self->reject(400);
        co_return;
      }
      const auto query = target.find('?');
      self->request->path = target.substr(0, query);
      self->request->query_string = query == std::string::npos ? "" : target.substr(query + 1);
      self->request->version = message.version();
      self->request->http_version = message.version() == 10 ? "1.0" : "1.1";
      self->request->keep_alive = message.keep_alive();
      self->request->content.str(message.body());
      for (const auto &field : message) self->request->header.emplace(field.name_string(), field.value());
      // A paired certificate may have been revoked since the TLS handshake.
      // Reuse the current pairing policy before admitting any new request.
      if constexpr (Tls) {
        if (self->owner->verify) {
          if (!self->verify_peer()) {
            self->request->client_uuid.clear();
            self->deliver(false);
            co_return;
          }
          self->request->client_uuid = self->client_uuid;
          self->request->certificate_sha256 = self->certificate_sha256;
        }
      }
      self->deliver(true);
    }
    void
    launch_request(bool handshake) {
      auto self = this->shared_from_this();
      net::co_spawn(stream.get_executor(), read_request(self, handshake), [self](std::exception_ptr error) {
        if (error) self->close(net::error::operation_aborted);
      });
    }
    void
    run() { launch_request(true); }
    void
    read() { launch_request(false); }
    void
    parse_error(error_code ec) {
      if (ec == http::error::body_limit) return reject(413);
      if (ec == http::error::header_limit) return reject(431);
      if (ec == http::error::end_of_stream) return finish();
      if (ec.category() == http::make_error_code(http::error::bad_method).category()) return reject(400);
      close(ec);
    }
    void
    reject(unsigned status) {
      auto response = std::make_shared<http::response<http::string_body>>(static_cast<StatusCode>(status), 11);
      response->keep_alive(false);
      response->set(http::field::content_type, "application/json");
      response->body() = R"({"error":"Invalid HTTP request"})";
      response->prepare_payload();
      beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(owner->config.timeout_request));
      auto self = this->shared_from_this();
      http::async_write(stream, *response, [self, response](error_code ec, auto) { if (ec) self->close(ec); else self->finish(); });
    }
    void
    deliver(bool verified) {
      if (owner->stopping || closed) return close();
      beast::get_lowest_layer(stream).expires_never();
      auto self = this->shared_from_this();
      response_timer.expires_after(std::chrono::seconds(owner->config.timeout_content));
      response_timer.async_wait([self](error_code ec) { if (!ec) self->close(net::error::timed_out); });
      ResponseChannel channel {
        [self](std::string bytes, Completion callback, bool final, bool close) {
          auto write = std::make_shared<Write>(Write { std::move(bytes), std::move(callback), final, close });
          bool accepted = false;
          {
            // Stop and task admission share one gate. A response retained by a
            // business subscriber must not enqueue a self-owning task after
            // the last io_context runner has exited.
            std::lock_guard lock(self->owner->setup_mutex);
            if (!self->closed && !self->owner->stopping) {
              net::post(self->stream.get_executor(), [self, write] { self->enqueue(write); });
              accepted = true;
            }
          }
          if (!accepted) write->complete(net::error::operation_aborted);
        },
        [self] {
          std::lock_guard lock(self->owner->setup_mutex);
          if (!self->closed && !self->owner->stopping) net::post(self->stream.get_executor(), [self] { self->begin_stream(); });
        },
        [self] { return bool(self->closed) || bool(self->owner->stopping); },
        [self] {
          std::lock_guard lock(self->owner->setup_mutex);
          if (!self->closed && !self->owner->stopping) self->cancel();
        },
        [self](std::shared_ptr<FileResponse> file) {
          auto write = std::make_shared<Write>();
          write->file = std::move(file);
          std::lock_guard lock(self->owner->setup_mutex);
          if (!self->closed && !self->owner->stopping)
            net::post(self->stream.get_executor(), [self, write] { self->enqueue(write); });
        }
      };
      try {
        owner->dispatch(request, std::move(channel), verified);
      }
      catch (...) {
        close(net::error::operation_aborted);
      }
    }
    void
    begin_stream() {
      if (closed || streaming) return;
      streaming = true;
      response_timer.cancel();
      if (buffer.size()) return close();  // SSE does not admit pipelined requests.
      beast::get_lowest_layer(stream).expires_never();
      probe_pending = true;
      auto self = this->shared_from_this();
      stream.async_read_some(net::buffer(probe), [self](error_code ec, auto) {
        self->probe_pending = false;
        if (self->finishing && !self->closed)
          self->shutdown();
        else
          self->close(ec ? ec : net::error::operation_aborted);
      });
    }
    void
    enqueue(const std::shared_ptr<Write> &write) {
      if (closed || owner->stopping) {
        write->complete(net::error::operation_aborted);
        return;
      }
      if (write->cost() > owner->config.max_response_buffer_size || queued_bytes > owner->config.max_response_buffer_size - write->cost()) {
        write->complete(net::error::no_buffer_space);
        return close(net::error::no_buffer_space);
      }
      queued_bytes += write->cost();
      queue.push_back(write);
      if (!writing) write_next();
    }
    void
    write_next() {
      if (closed || queue.empty()) return;
      writing = true;
      const auto write = queue.front();
      beast::get_lowest_layer(stream).expires_after(streaming ? owner->config.timeout_stream : std::chrono::milliseconds(std::chrono::seconds(owner->config.timeout_content)));
      auto self = this->shared_from_this();
      auto complete = [self, write](error_code ec, auto) {
        if (self->closed) {
          write->complete(net::error::operation_aborted);
          return;
        }
        if (ec) return self->close(ec);
        self->queue.pop_front();
        self->queued_bytes -= write->cost();
        self->writing = false;
        write->complete({});
        if (write->final) {
          self->response_timer.cancel();
          if (write->close || self->streaming)
            self->finish();
          else
            self->read();
        }
        else
          self->write_next();
      };
      if (write->file)
        http::async_write(stream, write->file->message, std::move(complete));
      else
        net::async_write(stream, net::buffer(write->bytes), std::move(complete));
    }
    void
    finish() {
      if (closed || finishing) return;
      finishing = true;
      if (probe_pending) {
        error_code ignored;
        beast::get_lowest_layer(stream).socket().cancel(ignored);
      }
      else
        shutdown();
    }
    void
    shutdown() {
      if (closed) return;
      if constexpr (Tls) {
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(owner->config.timeout_request));
        stream.async_shutdown([self = this->shared_from_this()](error_code) { self->close(); });
      }
      else
        close();
    }
  };

  void
  ServerRuntime::Impl::accept() {
    acceptor.async_accept(net::make_strand(io), [self = shared_from_this()](error_code ec, tcp::socket socket) {
      if (self->stopping) return;
      if (ec) {
        if (ec == net::error::operation_aborted) return;
        // Resource exhaustion must not spin on an immediately failing accept.
        self->accept_retry.expires_after(std::chrono::milliseconds(100));
        self->accept_retry.async_wait([self](error_code timer_error) {
          if (!timer_error && !self->stopping) self->accept();
        });
        return;
      }
      if (!ec) {
        error_code ignored;
        socket.set_option(tcp::no_delay(true), ignored);
        bool available;
        {
          std::lock_guard lock(self->sessions_mutex);
          available = self->sessions.size() < self->config.max_connections;
        }
        if (available) {
          if (self->tls_enabled) {
            auto session = std::make_shared<Session<true>>(self, std::move(socket));
            {
              std::lock_guard lock(self->sessions_mutex);
              self->sessions.emplace(session.get(), session);
            }
            session->run();
          }
          else {
            auto session = std::make_shared<Session<false>>(self, std::move(socket));
            {
              std::lock_guard lock(self->sessions_mutex);
              self->sessions.emplace(session.get(), session);
            }
            session->run();
          }
        }
      }
      self->accept();
    });
  }
  ServerRuntime::ServerRuntime(bool encrypted, std::string certificate, std::string key, std::string ca):
      impl(std::make_shared<Impl>(encrypted, std::move(certificate), std::move(key), std::move(ca))) {}
  ServerRuntime::~ServerRuntime() { stop(); }
  void
  ServerRuntime::stop() { impl->stop(); }
  void
  ServerRuntime::start(ServerConfig config, Dispatch dispatch, std::function<void(unsigned short)> ready,
    std::function<int(SSL *)> verify, std::function<std::string(X509 *)> identity,
    std::function<void(std::shared_ptr<Request>, const error_code &)> on_error) {
    {
      std::lock_guard lock(impl->setup_mutex);
      if (impl->started || impl->stopping) throw std::logic_error("HTTP server cannot restart");
      if (!config.thread_pool_size || config.thread_pool_size > 32 || !config.max_connections || !config.header_limit || !config.max_request_streambuf_size || !config.max_response_buffer_size || config.timeout_request <= 0 || config.timeout_content <= 0 || config.timeout_stream.count() <= 0 || config.send_buffer_bytes < 0)
        throw std::invalid_argument("Invalid HTTP server resource limits");
      impl->config = std::move(config);
      impl->dispatch = std::move(dispatch);
      impl->verify = std::move(verify);
      impl->identity = std::move(identity);
      impl->on_error = std::move(on_error);
      if (impl->tls_enabled) {
        if (impl->verify && !impl->ca.empty()) throw std::invalid_argument("Choose paired certificate verification or CA validation");
        impl->tls.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
        impl->tls.use_certificate_chain_file(impl->certificate);
        impl->tls.use_private_key_file(impl->key, ssl::context::pem);
        if (SSL_CTX_check_private_key(impl->tls.native_handle()) != 1) throw std::invalid_argument("TLS certificate and key do not match");
        if (!impl->ca.empty()) {
          impl->tls.load_verify_file(impl->ca);
          impl->tls.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
        }
        if (impl->verify) {
          impl->tls.set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
          impl->tls.set_verify_callback([](bool, ssl::verify_context &) { return true; });
        }
      }
      const auto address = net::ip::make_address(impl->config.address.empty() ? "0.0.0.0" : impl->config.address);
      impl->acceptor.open(address.is_v4() ? tcp::v4() : tcp::v6());
      if (address.is_v6()) impl->acceptor.set_option(net::ip::v6_only(false));
      impl->acceptor.set_option(net::socket_base::reuse_address(impl->config.reuse_address));
      impl->acceptor.bind({ address, impl->config.port });
      impl->acceptor.listen();
      impl->work.emplace(impl->io.get_executor());
      impl->started = true;
      impl->accept();
    }
    std::vector<std::thread> threads;
    const auto run = [self = impl] { try { self->io.run(); } catch (...) { self->stop(); self->io.run(); } };
    try {
      for (size_t i = 1; i < impl->config.thread_pool_size; ++i) threads.emplace_back(run);
      if (ready) ready(impl->acceptor.local_endpoint().port());
      run();
    }
    catch (...) {
      stop();
      run();
      for (auto &thread : threads) thread.join();
      throw;
    }
    for (auto &thread : threads) thread.join();
  }
}  // namespace web::detail
