/**
 * @file src/remote_usb/remote_usb_broker_server.h
 * @brief Certificate-authenticated RUSB broker listener.
 *
 * The listener is deliberately separate from the USB/IP loopback listener.
 * It accepts one authenticated RUSB connection, binds it to a one-shot
 * capability, and routes complete USB/IP PDUs through broker_adapter.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/ssl.h>

#include "remote_usb_broker_adapter.h"
#include "remote_usb_capability.h"

namespace remote_usb {

/** The decoded 84-byte client HELLO, kept separate from certificate identity. */
struct broker_hello {
  std::array<std::uint8_t, 16> wire_client_uuid {};
  std::uint64_t stream_generation { 0 };
  std::uint64_t session_token { 0 };
  std::uint64_t attachment_token { 0 };
  std::uint64_t lease_token { 0 };
  std::array<std::uint8_t, capability_nonce_size> capability_nonce {};
  std::uint32_t max_urb { 0 };
  std::uint32_t max_inflight { 0 };
  bool isochronous { false };
};

/** Decoded RUSB frame header, kept independent from the transport socket. */
struct broker_frame_header {
  std::uint8_t type { 0 };
  std::uint32_t flags { 0 };
  std::uint32_t payload_length { 0 };
  std::uint64_t session_token { 0 };
  std::uint64_t sequence { 0 };
};

inline constexpr std::size_t broker_hello_size = 84;
inline constexpr std::size_t broker_frame_header_size = 32;
inline constexpr std::size_t broker_capability_prefix_size = 34;

/** Decode and validate the normative RUSB v1 HELLO wire value. */
bool decode_broker_hello(const std::array<std::uint8_t, broker_hello_size> &wire,
                         broker_hello &hello) noexcept;

/** Decode and validate a normative RUSB v1 frame header. */
bool decode_broker_frame_header(
  const std::array<std::uint8_t, broker_frame_header_size> &wire,
  broker_frame_header &header,
  std::uint32_t max_payload) noexcept;

/** Encode a normative RUSB v1 frame header. */
void encode_broker_frame_header(
  std::array<std::uint8_t, broker_frame_header_size> &wire,
  std::uint8_t type,
  std::uint32_t flags,
  std::uint32_t payload_length,
  std::uint64_t session_token,
  std::uint64_t sequence) noexcept;

/** Decode and validate a CAPABILITY payload and its USB descriptor metadata. */
bool decode_broker_capability_payload(const std::vector<std::uint8_t> &payload,
                                      device_info &device,
                                      std::uint64_t &lease_token,
                                      std::uint64_t &attachment_token);

/** Result returned by start/configuration without throwing from nvhttp. */
struct broker_server_result {
  bool ok { false };
  std::string error;

  explicit operator bool() const noexcept { return ok; }
};

/**
 * Configuration for one broker listener.
 *
 * `client_certificate_uuid` must perform the same paired-certificate check as
 * nvhttp and return the stable certificate UUID.  Returning an empty string
 * rejects the TLS connection.  `authorize_client` is optional policy after
 * nonce consumption; it receives both identities because they are not
 * required to be equal.
 */
struct broker_server_config {
  std::string bind_address { "0.0.0.0" };
  std::uint16_t port { 0 };
  std::string certificate_file;
  std::string private_key_file;
  capability_store *capabilities { nullptr };

  std::function<std::string(SSL *)> client_certificate_uuid;
  std::function<bool(std::string_view cert_uuid,
                     const broker_hello &hello,
                     const capability &capability)> authorize_client;

  /**
   * Called after the loopback USB/IP listener is bound and before the session
   * enters OPEN.  The callback may complete immediately or retain the
   * completion while a platform host helper (for example usbip-win2) attaches
   * the endpoint.
   */
  local_endpoint_ready_callback on_local_endpoint_ready;

  /** Called exactly once for an authenticated adapter binding at session end. */
  std::function<void(const session_binding &, close_reason)> on_session_closed;

  /** Maximum simultaneous broker connections. Zero means one for v1. */
  std::size_t max_active_sessions { 1 };
  std::uint32_t handshake_timeout_ms { 5000 };
  std::uint32_t idle_timeout_ms { 15000 };
  std::uint32_t max_frame_payload { 128u * 1024u };
  std::uint32_t max_reassembly_size { 1024u * 1024u };
  std::uint32_t max_fragments { 4096 };
  std::uint64_t tx_window_bytes { 16u * 1024u * 1024u };
  std::uint32_t tx_window_pdus { 4096 };
  std::uint64_t rx_window_bytes { 16u * 1024u * 1024u };
  std::uint32_t rx_window_pdus { 4096 };
};

/**
 * Standalone TLS RUSB broker.  `start()` owns its worker thread and returns
 * after the listening socket is bound.  `stop()` is synchronous and idempotent.
 */
class broker_server final {
public:
  struct impl;

  explicit broker_server(broker_server_config config);
  ~broker_server();

  broker_server(const broker_server &) = delete;
  broker_server &operator=(const broker_server &) = delete;

  broker_server_result start();
  void stop();

  bool running() const noexcept;
  std::uint16_t bound_port() const noexcept;
  std::string bind_address() const;

private:
  std::shared_ptr<impl> state_;
};

}  // namespace remote_usb
