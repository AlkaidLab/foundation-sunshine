/**
 * @file src/http/tls_trust.cpp
 * @brief OpenSSL trust context with the existing Windows root-store policy.
 */
#include "tls_trust.h"
#include <memory>
#include <openssl/err.h>
#include <stdexcept>
#ifdef _WIN32
  #include <openssl/x509.h>
  #include <openssl/x509err.h>
  #include <wincrypt.h>
  #include <windows.h>
#endif
namespace web::detail {
  namespace {
#ifdef _WIN32
    class certificate_store_t {
    public:
      explicit certificate_store_t(HCERTSTORE handle) noexcept:
          handle_(handle) {
      }

      certificate_store_t(const certificate_store_t &) = delete;
      certificate_store_t &
      operator=(const certificate_store_t &) = delete;

      ~certificate_store_t() {
        if (handle_) {
          CertCloseStore(handle_, 0);
        }
      }

      explicit
      operator bool() const noexcept {
        return handle_ != nullptr;
      }

      HCERTSTORE
      get() const noexcept {
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
      DWORD store_location) noexcept {
      certificate_store_t windows_store {
        CertOpenStore(
          CERT_STORE_PROV_SYSTEM_W,
          0,
          0,
          store_location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
          L"ROOT")
      };
      if (!windows_store) {
        return {};
      }

      auto *openssl_store = SSL_CTX_get_cert_store(context.native_handle());
      if (!openssl_store) {
        return { true, 0 };
      }

      root_import_result_t result { true, 0 };
      PCCERT_CONTEXT windows_certificate = nullptr;
      while ((windows_certificate = CertEnumCertificatesInStore(
                windows_store.get(),
                windows_certificate))) {
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
        CERT_SYSTEM_STORE_LOCAL_MACHINE);
      const auto current_user = import_windows_root_store(
        context,
        CERT_SYSTEM_STORE_CURRENT_USER);

      if ((!local_machine.opened && !current_user.opened) ||
          local_machine.usable_certificates + current_user.usable_certificates == 0) {
        throw std::runtime_error("Windows root certificate store is unavailable");
      }
    }
#endif
  }  // namespace
  void
  load_system_trust(boost::asio::ssl::context &context) {
#ifdef _WIN32
    import_windows_root_certificates(context);
#else
    context.set_default_verify_paths();
#endif
  }
}  // namespace web::detail
