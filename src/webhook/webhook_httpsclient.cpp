/**
 * @file src/webhook/webhook_httpsclient.cpp
 * @brief HTTPS client for Webhook certificate verification control.
 */
#include "webhook_httpsclient.h"

#include <cstddef>
#include <memory>
#include <stdexcept>

#include <boost/asio/ip/address.hpp>

#ifdef _WIN32
  #include <windows.h>
  #include <wincrypt.h>
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifdef _WIN32
  #include <openssl/x509.h>
  #include <openssl/x509err.h>
#endif

namespace webhook {
  namespace {
    bool
    is_ip_literal(const std::string &value) noexcept {
      SimpleWeb::error_code error;
      (void) boost::asio::ip::make_address(value, error);
      return !error;
    }

#ifdef _WIN32
    class certificate_store_t {
    public:
      explicit certificate_store_t(HCERTSTORE handle) noexcept:
          handle_(handle) {
      }

      certificate_store_t(const certificate_store_t &) = delete;
      certificate_store_t &operator=(const certificate_store_t &) = delete;

      ~certificate_store_t() {
        if (handle_) {
          CertCloseStore(handle_, 0);
        }
      }

      explicit operator bool() const noexcept {
        return handle_ != nullptr;
      }

      HCERTSTORE get() const noexcept {
        return handle_;
      }

    private:
      HCERTSTORE handle_;
    };

    struct root_import_result_t {
      bool opened = false;
      std::size_t usable_certificates = 0;
    };

    root_import_result_t
    import_windows_root_store(
      boost::asio::ssl::context &context,
      DWORD store_location
    ) noexcept {
      certificate_store_t windows_store {
        CertOpenStore(
          CERT_STORE_PROV_SYSTEM_W,
          0,
          0,
          store_location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
          L"ROOT"
        )
      };
      if (!windows_store) {
        return {};
      }

      auto *openssl_store = SSL_CTX_get_cert_store(context.native_handle());
      if (!openssl_store) {
        return {true, 0};
      }

      root_import_result_t result {true, 0};
      PCCERT_CONTEXT windows_certificate = nullptr;
      while ((windows_certificate = CertEnumCertificatesInStore(
                windows_store.get(),
                windows_certificate
              ))) {
        const unsigned char *encoded = windows_certificate->pbCertEncoded;
        std::unique_ptr<X509, decltype(&X509_free)> certificate {
          d2i_X509(nullptr, &encoded, static_cast<long>(windows_certificate->cbCertEncoded)),
          X509_free
        };
        if (!certificate) {
          ERR_clear_error();
          continue;
        }

        if (X509_STORE_add_cert(openssl_store, certificate.get()) == 1) {
          ++result.usable_certificates;
          continue;
        }

        // A certificate already loaded through OpenSSL's default paths is
        // still usable. Other individual malformed entries are ignored; the
        // store is accepted only if at least one usable root was observed.
        const auto error = ERR_peek_last_error();
        if (ERR_GET_LIB(error) == ERR_LIB_X509 &&
            ERR_GET_REASON(error) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
          ++result.usable_certificates;
        }
        ERR_clear_error();
      }
      return result;
    }

    void
    import_windows_root_certificates(boost::asio::ssl::context &context) {
      const auto local_machine = import_windows_root_store(
        context,
        CERT_SYSTEM_STORE_LOCAL_MACHINE
      );
      const auto current_user = import_windows_root_store(
        context,
        CERT_SYSTEM_STORE_CURRENT_USER
      );

      if ((!local_machine.opened && !current_user.opened) ||
          local_machine.usable_certificates + current_user.usable_certificates == 0) {
        throw std::runtime_error("Windows root certificate store is unavailable");
      }
    }
#endif
  }  // namespace

  WebhookHttpsClient::WebhookHttpsClient(
    const std::string &server_port_path,
    bool verify_certificate
  ):
      base_t(server_port_path, 443),
#if BOOST_ASIO_VERSION >= 101300
      context_(boost::asio::ssl::context::tls_client) {
    context_.set_options(boost::asio::ssl::context::no_tlsv1);
    context_.set_options(boost::asio::ssl::context::no_tlsv1_1);
#else
      context_(boost::asio::ssl::context::tlsv12) {
#endif
    if (verify_certificate) {
#if BOOST_ASIO_VERSION >= 103300
      context_.set_verify_callback(boost::asio::ssl::host_name_verification(host));
#else
      context_.set_verify_callback(boost::asio::ssl::rfc2818_verification(host));
#endif

#ifdef _WIN32
      // The packaged OpenSSL build cannot be assumed to have a usable
      // OPENSSLDIR. Use the Windows machine and current-user trust anchors
      // in this isolated client context.
      import_windows_root_certificates(context_);
#else
      context_.set_default_verify_paths();
#endif
      context_.set_verify_mode(boost::asio::ssl::verify_peer);
    }
    else {
      context_.set_verify_mode(boost::asio::ssl::verify_none);
    }
  }

  WebhookHttpsClient::~WebhookHttpsClient() noexcept {
    // Release SSL streams before context_ is destroyed. ClientBase performs
    // the same idempotent cleanup again for its remaining base state.
    stop();
  }

  std::shared_ptr<WebhookHttpsClient::Connection>
  WebhookHttpsClient::create_connection() noexcept {
    return std::make_shared<Connection>(handler_runner, *io_service, context_);
  }

  void
  WebhookHttpsClient::connect(const std::shared_ptr<Session> &session) {
    // Keep retries in dispatcher_t. In particular, never replay a POST
    // invisibly after the peer read it but closed before returning headers.
    session->connection->attempt_reconnect = false;
    // Webhook configuration has no proxy contract. Reject an accidentally
    // supplied proxy before opening a connection.
    if (!config.proxy_server.empty()) {
      session->callback(SimpleWeb::make_error_code::make_error_code(
        SimpleWeb::errc::operation_not_supported
      ));
      return;
    }
    if (!session->connection->socket->lowest_layer().is_open()) {
      auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(*io_service);
      session->connection->set_timeout(config.timeout_connect);
      SimpleWeb::async_resolve(
        *resolver,
        *host_port,
        [this, session, resolver](
          const SimpleWeb::error_code &resolve_error,
          SimpleWeb::resolver_results results
        ) {
          session->connection->cancel_timeout();
          auto lock = session->connection->handler_runner->continue_lock();
          if (!lock) {
            return;
          }
          if (resolve_error) {
            session->callback(resolve_error);
            return;
          }

          session->connection->set_timeout(config.timeout_connect);
          boost::asio::async_connect(
            session->connection->socket->lowest_layer(),
            results,
            [this, session, resolver](
              const SimpleWeb::error_code &connect_error,
              SimpleWeb::async_connect_endpoint
            ) {
              session->connection->cancel_timeout();
              auto lock = session->connection->handler_runner->continue_lock();
              if (!lock) {
                return;
              }
              if (connect_error) {
                session->callback(connect_error);
                return;
              }

              boost::asio::ip::tcp::no_delay option(true);
              SimpleWeb::error_code option_error;
              session->connection->socket->lowest_layer().set_option(option, option_error);

              handshake(session);
            }
          );
        }
      );
      return;
    }

    write_request(session);
  }

  void
  WebhookHttpsClient::handshake(const std::shared_ptr<Session> &session) {
    // RFC 6066 permits DNS hostnames in SNI, but not literal IPv4 or IPv6
    // addresses. Some TLS endpoints reject a ClientHello containing an IP in
    // server_name, even when the certificate itself has a matching IP SAN.
    if (!is_ip_literal(host) &&
        SSL_set_tlsext_host_name(
          session->connection->socket->native_handle(),
          host.c_str()
        ) != 1) {
      ERR_clear_error();
      session->callback(SimpleWeb::make_error_code::make_error_code(
        SimpleWeb::errc::protocol_error
      ));
      return;
    }

    session->connection->set_timeout(config.timeout_connect);
    session->connection->socket->async_handshake(
      boost::asio::ssl::stream_base::client,
      [this, session](const SimpleWeb::error_code &handshake_error) {
        session->connection->cancel_timeout();
        auto lock = session->connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }
        if (handshake_error) {
          session->callback(handshake_error);
          return;
        }
        write_request(session);
      }
    );
  }

}  // namespace webhook
