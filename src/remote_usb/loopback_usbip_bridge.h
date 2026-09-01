/**
 * @file src/remote_usb/loopback_usbip_bridge.h
 * @brief An authenticated-session-facing USB/IP endpoint for usbip-win2.
 *
 * The bridge owns only a loopback TCP listener.  USB/IP requests received from
 * the local vhci client are handed to the caller as complete PDUs; replies
 * from the caller are queued back to that same client.  Authentication and the
 * actual USB transport deliberately live outside this class.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/system/error_code.hpp>

namespace remote_usb {

/** USB interface class tuple advertised by USB/IP control-plane replies. */
struct interface_info {
  std::uint8_t interface_class { 0 };
  std::uint8_t interface_subclass { 0 };
  std::uint8_t interface_protocol { 0 };
};

/** Value-only device metadata; no platform USB handles cross this boundary. */
struct device_info {
  std::string busid;
  std::string path;
  std::uint32_t busnum { 1 };
  std::uint32_t devnum { 1 };
  /** USB/IP speed: 0 unknown, 1 low, 2 full, 3 high, 4 variable, 5 super. */
  std::uint32_t speed { 3 };
  std::uint16_t vendor_id { 0 };
  std::uint16_t product_id { 0 };
  std::uint16_t device_bcd { 0 };
  std::uint8_t device_class { 0 };
  std::uint8_t device_subclass { 0 };
  std::uint8_t device_protocol { 0 };
  std::uint8_t configuration_value { 1 };
  std::uint8_t num_configurations { 1 };
  std::vector<interface_info> interfaces;
};

enum class close_reason {
  stopped,
  peer_disconnected,
  protocol_error,
  transport_rejected,
  queue_limit,
  internal_error,
};

/**
 * Callbacks connecting the local USB/IP socket to the authenticated broker.
 *
 * `on_request` is called on the bridge I/O thread with ownership of one
 * complete USB/IP request PDU.  It must either enqueue/copy the bytes and
 * return true, or return false to terminate the local connection.  Replies
 * arrive through send_reply(), which is thread-safe and may be called from a
 * broker worker thread.
 */
struct callbacks {
  std::function<bool(std::vector<std::uint8_t>)> on_request;
  std::function<void()> on_imported;
  std::function<void(close_reason)> on_closed;
};

struct endpoint {
  std::string address { "127.0.0.1" };
  std::uint16_t port { 0 };
  std::string busid;
};

/**
 * A single-device, single-client USB/IP server bound to an ephemeral
 * loopback port.  It is intentionally not a general LAN USB/IP daemon.
 */
class loopback_usbip_bridge final {
public:
  loopback_usbip_bridge();
  ~loopback_usbip_bridge();

  loopback_usbip_bridge(const loopback_usbip_bridge &) = delete;
  loopback_usbip_bridge &operator=(const loopback_usbip_bridge &) = delete;

  /**
   * Start listening on 127.0.0.1 and return the endpoint to advertise to the
   * local usbip-win2 helper.  `device.busid` must be non-empty and at most 31
   * bytes, but is used only for validation and replaced by an opaque value.
   * Use the returned `endpoint.busid` as the advertised ID.
   * `callbacks.on_request` is required.
   */
  std::optional<endpoint> start(device_info device,
                                callbacks callbacks,
                                boost::system::error_code &error);

  /** Stop the listener and active client synchronously.  Safe to repeat. */
  void stop();

  /**
   * Queue one complete USB/IP RET_* PDU for the imported client.  The bytes
   * are copied/moved before this function returns.  Linux-canonical replies
   * (devid/direction/endpoint all zero) and legacy echo-header replies are
   * accepted.  A valid RET_SUBMIT racing a completed UNLINK is consumed and
   * not sent a second time.  Returns false when the bridge is stopped, not
   * imported, malformed, or over its queue limit.
   */
  bool send_reply(std::vector<std::uint8_t> wire);

  /** Snapshot listener state. */
  bool running() const noexcept;
  std::optional<endpoint> local_endpoint() const;

private:
  struct impl;
  mutable std::mutex mutex_;
  std::shared_ptr<impl> state_;
};

} // namespace remote_usb
