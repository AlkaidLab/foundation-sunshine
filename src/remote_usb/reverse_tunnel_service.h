/**
 * @file src/remote_usb/reverse_tunnel_service.h
 * @brief Reverse USB/IP tunnel endpoint for paired Moonlight clients.
 *
 * The Moonlight client connects a platform USB/IP server (usbipd-win,
 * usbip-host, USBIPServerForAndroid) through a client-initiated TLS tunnel.
 * This service terminates that tunnel: after a one-line JSON handshake it
 * attaches usbip-win2 to an ephemeral loopback port and pumps opaque bytes in
 * both directions.  No USB/IP parsing happens here — the protocol belongs to
 * the USB/IP server and client at the two ends of the tunnel.
 *
 * Wire contract (docs/remote-usb-reverse-tunnel.md in moonlight-qt):
 *   C → S: {"op":"forward","token":"<session-token>","busid":"1-2"}\n
 *   S → C: {"op":"ready"}\n                                  (then raw bytes)
 *   S → C: {"op":"error","reason":"..."}\n                   (then close)
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <openssl/ssl.h>

#include "remote_usb_host_controller.h"

namespace remote_usb {

  struct reverse_tunnel_config {
    std::string bind_address { "0.0.0.0" };
    std::uint16_t port { 47996 };
    std::string certificate_file;
    std::string private_key_file;
    /** Shared secret presented by the client in the JSON handshake. */
    std::string session_token;
    /** Decides whether a presented TLS client certificate is a paired
     *  Moonlight client; injected by the caller so this module never
     *  depends on the HTTP layer's pairing state. */
    std::function<bool(X509 *)> verify_client_cert;
    /** Claims the forwarding slot for one busid (one tunnel per device);
     *  wired by the service itself. Returns false when already claimed. */
    std::function<bool(const std::string &)> acquire_device_slot;
    std::function<void(const std::string &)> release_device_slot;
  };

  /**
   * Owns the TLS listener, one tunnel per forwarded busid, and the
   * usbip-win2 attach/detach lifecycle.  All callbacks run on the service's
   * own io_context thread; stop() joins it.
   */
  class reverse_tunnel_service final {
  public:
    explicit reverse_tunnel_service(usbip_host_controller_config controller_config = {});
    ~reverse_tunnel_service();

    reverse_tunnel_service(const reverse_tunnel_service &) = delete;
    reverse_tunnel_service &
    operator=(const reverse_tunnel_service &) = delete;

    /** Returns false (and logs) when the endpoint cannot be bound. */
    bool
    start(reverse_tunnel_config config);

    void
    stop() noexcept;

    bool
    available() const noexcept;

    std::uint16_t
    bound_port() const noexcept;

  private:
    struct impl;
    std::shared_ptr<impl> impl_;
  };

}  // namespace remote_usb
