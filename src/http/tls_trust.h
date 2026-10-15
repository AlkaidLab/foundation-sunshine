/**
 * @file src/http/tls_trust.h
 * @brief Native trust anchors for verified Beast TLS clients.
 */
#pragma once
#include <boost/asio/ssl/context.hpp>
namespace web::detail {
  void
  load_system_trust(boost::asio::ssl::context &context);
}
