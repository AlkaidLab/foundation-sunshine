/**
 * @file src/remote_usb/reverse_tunnel_service.cpp
 */

#include "src/remote_usb/reverse_tunnel_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>


#include <nlohmann/json.hpp>

#include "src/logging_severity.h"

#include "src/remote_usb/remote_usb_host_controller.h"

namespace remote_usb {

  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  namespace ssl = asio::ssl;

  namespace {

    /** Upper bound for the JSON handshake line, mirroring the client. */
    constexpr std::size_t kMaxHandshakeBytes = 4 * 1024;
    /** Deadline covering TLS, the JSON handshake, and the usbip-win2 attach. */
    constexpr auto kStartupTimeout = std::chrono::seconds(12);
    /** USB/IP busid: non-empty, short, printable. */
    constexpr std::size_t kMaxBusIdBytes = 31;

    bool
    valid_busid(const std::string &busid) {
      if (busid.empty() || busid.size() > kMaxBusIdBytes) {
        return false;
      }
      for (const char c: busid) {
        const bool allowed = (c >= '0' && c <= '9') ||
                             (c >= 'a' && c <= 'z') ||
                             (c >= 'A' && c <= 'Z') ||
                             c == '-' || c == '.';
        if (!allowed) {
          return false;
        }
      }
      return true;
    }

    /** Constant-time comparison for the shared session token. */
    bool
    tokens_equal(const std::string &a, const std::string &b) {
      return a.size() == b.size() &&
             CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    /** One accepted client connection and its attached usbip-win2 binding. */
    class tunnel_session : public std::enable_shared_from_this<tunnel_session> {
    public:
      tunnel_session(tcp::socket socket,
                     ssl::context &ssl_context,
                     usbip_host_controller &controller)
          : stream_(std::move(socket), ssl_context),
            controller_(controller),
            startup_timer_(stream_.get_executor()) {}

      void
      start(reverse_tunnel_config config) {
        config_ = std::move(config);
        arm_startup_deadline();

        auto self = shared_from_this();
        stream_.async_handshake(ssl::stream_base::server,
          [self](boost::system::error_code error) {
            if (error) {
              BOOST_LOG(info) << "Reverse USB tunnel TLS rejected: " << error.message();
              self->finish();
              return;
            }
            self->read_handshake();
          });
      }

      /** Close both sockets and release the device slot; any thread, idempotent. */
      void
      abort() {
        // Socket operations are serialized on the service thread.
        asio::post(stream_.get_executor(), [self = shared_from_this()] {
          self->finish();
        });
      }

      /** Whether the session has terminated (slot may be reclaimed). */
      bool
      done() const noexcept {
        return finished_.load();
      }

    private:
      /** Kill the session unless forwarding has been established in time. */
      void
      arm_startup_deadline() {
        auto self = shared_from_this();
        startup_timer_.expires_after(kStartupTimeout);
        startup_timer_.async_wait([self](boost::system::error_code error) {
          if (error) {
            return;  // cancelled: forwarding started or the session ended
          }
          BOOST_LOG(warning) << "Reverse USB tunnel startup timed out";
          self->abort();
        });
      }

      void
      read_handshake() {
        auto self = shared_from_this();
        asio::async_read_until(stream_,
          asio::dynamic_buffer(handshake_, kMaxHandshakeBytes), '\n',
          [self](boost::system::error_code error, std::size_t) {
            if (error) {
              // A client that vanishes before completing the handshake is
              // expected during scans; do not warn.
              self->finish();
              return;
            }
            self->handle_handshake_line();
          });
      }

      void
      handle_handshake_line() {
        const auto newline = handshake_.find('\n');
        if (newline == std::string::npos) {
          reply_error("handshake line too long");
          return;
        }

        std::string op, token, busid;
        try {
          const auto request = nlohmann::json::parse(handshake_.substr(0, newline));
          op = request.value("op", std::string {});
          token = request.value("token", std::string {});
          busid = request.value("busid", std::string {});
        }
        catch (const std::exception &) {
          reply_error("malformed handshake");
          return;
        }

        if (op != "forward") {
          reply_error("unknown operation");
          return;
        }
        if (!tokens_equal(token, config_.session_token)) {
          BOOST_LOG(warning) << "Reverse USB tunnel rejected a mismatched session token";
          reply_error("unauthorized");
          return;
        }
        if (!valid_busid(busid)) {
          reply_error("invalid busid");
          return;
        }

        if (config_.acquire_device_slot && !config_.acquire_device_slot(busid)) {
          reply_error("device already forwarded");
          return;
        }
        slot_busid_ = busid;

        handshake_.erase(0, newline + 1);

        open_local_endpoint(busid);
      }

      /** Bind an ephemeral loopback port for usbip-win2 to attach to. */
      void
      open_local_endpoint(const std::string &busid) {
        boost::system::error_code error;
        local_acceptor_.open(tcp::v4(), error);
        if (!error) {
          local_acceptor_.bind(
            tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0), error);
        }
        if (!error) {
          local_acceptor_.listen(1, error);
        }
        if (error) {
          reply_error("cannot open loopback endpoint");
          return;
        }

        request_local_ = usbip_host_request {
          .server_endpoint = endpoint { "127.0.0.1", local_acceptor_.local_endpoint().port(), busid },
        };

        // Arm the accept before launching the helper. Attach cannot complete
        // until its USB/IP import/enumeration traffic crosses the tunnel.
        accept_local();
        request_attach();
      }

      void
      request_attach() {
        auto self = shared_from_this();
        attach_operation_ = controller_.attach(request_local_,
          [self](usbip_host_result result) {
            // The completion fires on a controller worker thread; hop back
            // onto the service thread so all socket writes stay serialized.
            asio::post(self->stream_.get_executor(),
              [self, result = std::move(result)]() mutable {
                self->finish_attach(std::move(result));
              });
          });
        // Even a synchronous rejection invokes the completion above.
      }

      void
      finish_attach(usbip_host_result result) {
        attach_operation_ = 0;
        if (finished_.load()) {
          if (result.binding) {
            controller_.detach(*result.binding, [](usbip_host_result) {});
          }
          return;
        }
        if (!result.ok() || !result.binding) {
          BOOST_LOG(warning) << "Reverse USB tunnel usbip attach failed: " << result.detail;
          reply_error("usbip attach failed");
          return;
        }
        binding_ = *result.binding;
        if (ready_sent_) {
          startup_timer_.cancel();
        }
        BOOST_LOG(info) << "Remote USB tunnel attached busid "
                        << binding_->server_endpoint.busid
                        << " at hub port " << binding_->hub_port;
      }

      void
      accept_local() {
        auto self = shared_from_this();
        local_acceptor_.async_accept(
          [self](boost::system::error_code error, tcp::socket socket) {
            if (self->finished_.load() || self->error_pending_) {
              return;
            }
            if (error) {
              self->reply_error("usbip attach did not connect");
              return;
            }
            self->local_ = std::make_unique<tcp::socket>(std::move(socket));
            self->raw_stream_ = true;
            asio::async_write(self->stream_, asio::buffer(self->ready_line_),
              [self](boost::system::error_code error, std::size_t) {
                if (self->finished_.load()) return;
                if (error) {
                  self->finish();
                  return;
                }
                BOOST_LOG(info) << "Remote USB tunnel forwarding busid "
                                << self->request_local_.server_endpoint.busid;
                self->ready_sent_ = true;
                if (self->binding_) {
                  self->startup_timer_.cancel();
                }
                self->flush_handshake_tail();
                self->pump_local_to_remote();
              });
          });
      }

      void
      flush_handshake_tail() {
        if (handshake_.empty()) {
          pump_remote_to_local();
          return;
        }
        auto self = shared_from_this();
        asio::async_write(*local_, asio::buffer(handshake_),
          [self](boost::system::error_code error, std::size_t) {
            if (self->finished_.load()) return;
            if (error) {
              self->finish();
              return;
            }
            self->handshake_.clear();
            self->pump_remote_to_local();
          });
      }

      void
      pump_remote_to_local() {
        auto self = shared_from_this();
        stream_.async_read_some(asio::buffer(remote_buffer_),
          [self](boost::system::error_code error, std::size_t bytes) {
            if (self->finished_.load()) return;
            if (error) {
              self->finish();
              return;
            }
            asio::async_write(*self->local_,
              asio::buffer(self->remote_buffer_, bytes),
              [self](boost::system::error_code error, std::size_t) {
                if (self->finished_.load()) return;
                if (error) {
                  self->finish();
                  return;
                }
                self->pump_remote_to_local();
              });
          });
      }

      void
      pump_local_to_remote() {
        auto self = shared_from_this();
        local_->async_read_some(asio::buffer(local_buffer_),
          [self](boost::system::error_code error, std::size_t bytes) {
            if (self->finished_.load()) return;
            if (error) {
              self->finish();
              return;
            }
            asio::async_write(self->stream_,
              asio::buffer(self->local_buffer_, bytes),
              [self](boost::system::error_code error, std::size_t) {
                if (self->finished_.load()) return;
                if (error) {
                  self->finish();
                  return;
                }
                self->pump_local_to_remote();
              });
          });
      }

      void
      reply_error(const std::string &reason) {
        if (finished_.load() || error_pending_) return;
        // Once ready has begun, the peer treats every byte as USB/IP. A late
        // attach failure must close the stream, never inject another JSON line.
        if (raw_stream_) {
          finish();
          return;
        }
        error_pending_ = true;
        startup_timer_.cancel();
        auto self = shared_from_this();
        // The buffer must outlive async_write, which does not own it.
        const auto line = std::make_shared<const std::string>(
          (nlohmann::json { { "op", "error" }, { "reason", reason } }).dump() + "\n");
        asio::async_write(stream_, asio::buffer(*line),
          [self, line](boost::system::error_code, std::size_t) {
            boost::system::error_code ignored;
            self->stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ignored);
            self->finish();
          });
      }

      /** Tear both sockets down and detach the usbip-win2 binding. */
      void
      finish() {
        if (finished_.exchange(true)) {
          return;
        }
        boost::system::error_code ignored;
        startup_timer_.cancel();
        local_acceptor_.close(ignored);
        if (attach_operation_ != 0) {
          controller_.cancel(attach_operation_);
        }
        if (local_) {
          local_->close(ignored);
        }
        stream_.lowest_layer().close(ignored);

        if (binding_) {
          // Fire-and-forget: the controller completes detach on its worker.
          controller_.detach(*binding_, [](usbip_host_result result) {
            if (!result.ok()) {
              BOOST_LOG(warning) << "Remote USB tunnel detach reported: " << result.detail;
            }
          });
          binding_.reset();
        }
        if (!slot_busid_.empty()) {
          if (config_.release_device_slot) {
            config_.release_device_slot(slot_busid_);
          }
          slot_busid_.clear();
        }
      }

      ssl::stream<tcp::socket> stream_;
      usbip_host_controller &controller_;
      reverse_tunnel_config config_;

      tcp::acceptor local_acceptor_ { stream_.get_executor() };
      std::unique_ptr<tcp::socket> local_;
      usbip_host_request request_local_;
      usbip_host_controller::operation_id attach_operation_ { 0 };
      std::string slot_busid_;
      std::optional<usbip_host_binding> binding_;

      std::string handshake_;
      const std::string ready_line_ { "{\"op\":\"ready\"}\n" };
      asio::steady_timer startup_timer_;
      std::array<std::uint8_t, 64 * 1024> remote_buffer_ {};
      std::array<std::uint8_t, 64 * 1024> local_buffer_ {};
      std::atomic_bool finished_ { false };
      bool raw_stream_ { false };
      bool ready_sent_ { false };
      bool error_pending_ { false };
    };

    using session_ptr = std::shared_ptr<tunnel_session>;

  }  // namespace

  struct reverse_tunnel_service::impl {
    explicit impl(usbip_host_controller_config controller_config)
        : controller(std::move(controller_config)) {}

    asio::io_context io;
    tcp::acceptor acceptor { io };
    std::unique_ptr<ssl::context> ssl_context;
    usbip_host_controller controller;
    reverse_tunnel_config config;
    std::atomic_bool stopping { false };
    std::mutex session_mutex;
    /** Live sessions, for stop() broadcast; finished ones are reaped lazily. */
    std::vector<session_ptr> sessions;
    /** busids with an established tunnel; one tunnel per device. */
    std::unordered_set<std::string> claimed_busids;
    std::thread thread;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;

    ~impl() {
      stop();
    }

    void
    stop() noexcept {
      if (stopping.exchange(true)) {
        return;
      }
      asio::post(io, [this] {
        boost::system::error_code ignored;
        acceptor.close(ignored);
        const std::lock_guard lock(session_mutex);
        for (const auto &session: sessions) {
          session->abort();
        }
      });
      // Keep io alive while controller completions post their final cleanup.
      controller.stop();
      asio::post(io, [this] { work.reset(); });
      if (thread.joinable()) {
        thread.join();
      }
    }

    void
    accept_next() {
      acceptor.async_accept(
        [this](boost::system::error_code error, tcp::socket socket) {
          if (stopping.load() || error == asio::error::operation_aborted) {
            return;
          }
          if (error) {
            BOOST_LOG(warning) << "Remote USB tunnel accept failed: " << error.message();
          }
          else {
            const std::lock_guard lock(session_mutex);
            // Reap finished sessions so stop() only has live handles.
            sessions.erase(
              std::remove_if(sessions.begin(), sessions.end(),
                [](const session_ptr &session) { return session->done(); }),
              sessions.end());
            const auto session = std::make_shared<tunnel_session>(
              std::move(socket), *ssl_context, controller);
            sessions.push_back(session);
            session->start(config);
          }
          if (!stopping.load()) {
            accept_next();
          }
        });
    }
  };

  reverse_tunnel_service::reverse_tunnel_service(usbip_host_controller_config controller_config)
      : impl_(std::make_shared<impl>(std::move(controller_config))) {}

  reverse_tunnel_service::~reverse_tunnel_service() {
    if (impl_) {
      impl_->stop();
    }
  }

  bool
  reverse_tunnel_service::start(reverse_tunnel_config config) {
    if (config.session_token.empty()) {
      BOOST_LOG(info) << "Remote USB tunnel disabled: no session token configured";
      return false;
    }
    if (!impl_->controller.backend_supported()) {
      BOOST_LOG(info) << "Remote USB tunnel disabled: no host USB/IP backend on this platform";
      return false;
    }

    impl_->config = config;
    impl_->config.acquire_device_slot =
      [impl = impl_.get()](const std::string &busid) {
        const std::lock_guard lock(impl->session_mutex);
        return impl->claimed_busids.insert(busid).second;
      };
    impl_->config.release_device_slot =
      [impl = impl_.get()](const std::string &busid) {
        const std::lock_guard lock(impl->session_mutex);
        impl->claimed_busids.erase(busid);
      };

    impl_->ssl_context = std::make_unique<ssl::context>(ssl::context::tls_server);
    boost::system::error_code error;
    impl_->ssl_context->use_certificate_chain_file(config.certificate_file, error);
    if (!error) {
      impl_->ssl_context->use_private_key_file(
        config.private_key_file, ssl::context::pem, error);
    }
    if (error) {
      BOOST_LOG(warning) << "Remote USB tunnel cannot load credentials: " << error.message();
      impl_->ssl_context.reset();
      return false;
    }
    impl_->ssl_context->set_verify_mode(ssl::verify_peer | ssl::verify_fail_if_no_peer_cert);
    // Asio owns SSL_CTX app_data and deletes its callback there on destruction.
    // Use its callback API instead of replacing that slot with a foreign pointer.
    impl_->ssl_context->set_verify_callback(
      [verify = config.verify_client_cert](bool, ssl::verify_context &context) {
        X509 *peer = X509_STORE_CTX_get0_cert(context.native_handle());
        return peer != nullptr && verify && verify(peer);
      });

    const tcp::endpoint bound(
      asio::ip::make_address(
        config.bind_address.empty() ? "0.0.0.0" : config.bind_address),
      config.port);
    impl_->acceptor.open(bound.protocol(), error);
    if (!error) {
      impl_->acceptor.set_option(tcp::acceptor::reuse_address(true), error);
    }
    if (!error) {
      impl_->acceptor.bind(bound, error);
    }
    if (!error) {
      impl_->acceptor.listen(16, error);
    }
    if (error) {
      BOOST_LOG(warning) << "Remote USB tunnel cannot bind " << config.bind_address
                         << ':' << config.port << " — " << error.message();
      impl_->acceptor.close();
      impl_->ssl_context.reset();
      return false;
    }

    impl_->stopping.store(false);
    impl_->accept_next();
    impl_->work = std::make_unique<
      asio::executor_work_guard<asio::io_context::executor_type>>(
      impl_->io.get_executor());
    impl_->thread = std::thread([io = &impl_->io] { io->run(); });
    BOOST_LOG(info) << "Remote USB tunnel listening on "
                    << config.bind_address << ':' << config.port;
    return true;
  }

  void
  reverse_tunnel_service::stop() noexcept {
    if (impl_) {
      impl_->stop();
    }
  }

  bool
  reverse_tunnel_service::available() const noexcept {
    return impl_ && impl_->acceptor.is_open() && !impl_->stopping.load();
  }

  std::uint16_t
  reverse_tunnel_service::bound_port() const noexcept {
    if (!impl_ || !impl_->acceptor.is_open()) {
      return 0;
    }
    boost::system::error_code ignored;
    return impl_->acceptor.local_endpoint(ignored).port();
  }

}  // namespace remote_usb
