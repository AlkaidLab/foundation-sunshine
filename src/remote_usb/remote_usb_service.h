/**
 * @file src/remote_usb/remote_usb_service.h
 * @brief Long-lived Remote USB host service independent of HTTP routing.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <openssl/ssl.h>

#include "remote_usb_broker_server.h"

namespace remote_usb {

  struct service_config {
    std::string bind_address { "0.0.0.0" };
    std::string certificate_file;
    std::string private_key_file;
    /** Resolve the paired certificate UUID for an authenticated broker TLS socket. */
    std::function<std::string(SSL *)> client_certificate_uuid;
  };

  enum class capability_issue_status : std::uint8_t {
    ok,
    unsupported,
    unavailable,
    limit_exceeded,
    invalid_request,
  };

  struct capability_issue_request {
    std::string client_uuid;
    std::uint64_t stream_generation { 0 };
    std::string endpoint_host;
    std::string wire_client_uuid;
    std::uint64_t session_token { 0 };
    std::uint64_t attachment_token { 0 };
    std::uint64_t lease_token { 0 };
  };

  struct capability_issue_result {
    capability_issue_status status { capability_issue_status::invalid_request };
    std::optional<capability> value;

    explicit
    operator bool() const noexcept {
      return status == capability_issue_status::ok && value.has_value();
    }
  };

  /**
   * Owns the Remote USB broker, host attach state and shutdown cleanup.
   * HTTP, IPC and future control-plane adapters call this typed API only.
   */
  class remote_usb_service final {
  public:
    remote_usb_service();
    ~remote_usb_service();

    remote_usb_service(const remote_usb_service &) = delete;
    remote_usb_service &
    operator=(const remote_usb_service &) = delete;

    broker_server_result
    start(service_config config);
    capability_issue_result
    issue_capability(capability_issue_request request);
    void
    stop() noexcept;

    bool
    available() const noexcept;
    std::uint16_t
    bound_port() const noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
  };

}  // namespace remote_usb
