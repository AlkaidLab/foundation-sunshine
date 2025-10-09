/**
 * @file src/webhook_httpsclient.cpp
 * @brief HTTPS client for webhook with certificate verification
 */
#include "webhook_httpsclient.h"
#include "logging.h"
#include <openssl/ssl.h>
#include <openssl/x509.h>

extern "C" {
  static int webhook_verify_cb(int ok, X509_STORE_CTX *ctx) {
    int err_code = X509_STORE_CTX_get_error(ctx);

    switch (err_code) {
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
      case X509_V_ERR_CERT_NOT_YET_VALID:
      case X509_V_ERR_CERT_HAS_EXPIRED:
        return 1;

      default:
        return ok;
    }
  }
}

namespace webhook {

  WebhookHttpsClient::WebhookHttpsClient(const std::string& server_port_path, 
                                        bool verify_certificate, 
                                        const std::string& expected_hostname)
      : SimpleWeb::Client<SimpleWeb::HTTPS>(server_port_path, false),
        expected_hostname_(expected_hostname.empty() ? server_port_path : expected_hostname) {
    
    if (verify_certificate) {
      context.set_default_verify_paths();
      context.set_verify_mode(boost::asio::ssl::verify_peer);
      context.set_verify_callback([](bool preverified, boost::asio::ssl::verify_context& ctx) {
        return webhook_verify_cb(preverified ? 1 : 0, ctx.native_handle()) == 1;
      });
      BOOST_LOG(debug) << "WebhookHttpsClient: SSL verification enabled for host: " << expected_hostname_;
    } else {
      context.set_verify_mode(boost::asio::ssl::verify_none);
      BOOST_LOG(debug) << "WebhookHttpsClient: SSL verification disabled";
    }
  }


}  // namespace webhook