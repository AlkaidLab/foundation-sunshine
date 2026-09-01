/**
 * @file src/remote_usb/remote_usb_broker_adapter.h
 * @brief Typed glue between an authenticated RUSB session and USB/IP.
 *
 * This class deliberately stops at the transport boundary.  The caller owns
 * RUSB framing, authentication, sequence validation and fragment reassembly.
 * The adapter owns session/lease checks and connects complete USB/IP PDUs to
 * a loopback_usbip_bridge.
 */
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/system/error_code.hpp>

#include "loopback_usbip_bridge.h"

namespace remote_usb {

/** Limits shared by the v1 RUSB broker contract.
 *
 * `max_urb` is the maximum size of one complete USB/IP PDU, including its
 * fixed 48-byte header.  Keep the old `broker_max_urb` spelling as a source
 * compatibility alias for callers that already consume the v1 API.
 */
inline constexpr std::size_t broker_usbip_header_size = 48u;
inline constexpr std::uint32_t broker_max_pdu_size = 1024u * 1024u;
inline constexpr std::uint32_t broker_min_pdu_size =
  static_cast<std::uint32_t>(broker_usbip_header_size + 1u);
inline constexpr std::uint32_t broker_min_urb = broker_min_pdu_size;
inline constexpr std::uint32_t broker_max_transfer_size =
  broker_max_pdu_size - static_cast<std::uint32_t>(broker_usbip_header_size);
inline constexpr std::uint32_t broker_max_urb = broker_max_pdu_size;
inline constexpr std::uint32_t broker_max_inflight = 4096u;

/** Immutable identity and negotiated limits for one authenticated session. */
struct session_binding {
  std::array<std::uint8_t, 16> client_uuid {};
  std::uint64_t stream_generation { 0 };
  std::uint64_t session_token { 0 };
  std::uint64_t attachment_token { 0 };
  std::uint64_t lease_token { 0 };
  std::array<std::uint8_t, 16> capability_nonce {};
  std::uint32_t max_urb { 0 };
  std::uint32_t max_inflight { 0 };
  bool isochronous { false };
};

/** Event kinds understood by the adapter's typed ingress methods. */
enum class session_event_kind : std::uint8_t {
  capability,
  open,
  open_ok,
  open_reject,
  usbip_pdu,
  close,
};

enum class adapter_state : std::uint8_t {
  idle,
  awaiting_capability,
  awaiting_open,
  starting_bridge,
  open,
  closed,
};

/** Stable diagnostics for a rejected adapter operation. */
enum class adapter_status : std::uint8_t {
  ok,
  invalid_argument,
  invalid_state,
  token_mismatch,
  limit_exceeded,
  fragment_error,
  transport_rejected,
  bridge_failure,
  already_closed,
  /** The host platform has no compatible USB/IP attach backend. */
  unsupported,
};

/** Capability metadata after the outer RUSB decoder has done its validation. */
struct capability_event {
  std::uint64_t session_token { 0 };
  std::uint64_t lease_token { 0 };
  std::uint64_t attachment_token { 0 };
  device_info device;
};

struct open_event {
  std::uint64_t session_token { 0 };
  std::uint64_t lease_token { 0 };
  std::uint64_t attachment_token { 0 };
};

struct open_ok_event {
  std::uint64_t session_token { 0 };
};

struct open_reject_event {
  std::uint64_t session_token { 0 };
  std::uint32_t status { 0 };
};

/** A complete USB/IP PDU returned by the outer RUSB reassembler. */
struct usbip_pdu_event {
  std::uint64_t session_token { 0 };
  std::uint64_t lease_token { 0 };
  std::uint64_t pdu_id { 0 };
  std::vector<std::uint8_t> pdu;
};

struct close_event {
  std::uint64_t session_token { 0 };
  std::uint64_t lease_token { 0 };
};

/**
 * One typed message to the authenticated outer transport.
 *
 * `pdu` is populated only for USBIP_DATA and is the complete USB/IP PDU.  The
 * outer transport fragments it, assigns the RUSB sequence, and encodes the
 * common header.  Unused fields are zero for the other message kinds.
 */
enum class transport_message_kind : std::uint8_t {
  open,
  open_ok,
  open_reject,
  usbip_data,
  close,
};

struct transport_message {
  transport_message_kind kind { transport_message_kind::open };
  std::uint64_t session_token { 0 };
  std::uint64_t lease_token { 0 };
  std::uint64_t attachment_token { 0 };
  std::uint64_t pdu_id { 0 };
  std::uint32_t status { 0 };
  std::vector<std::uint8_t> pdu;
};

/** Completion supplied by the host-side endpoint integration. */
using local_endpoint_ready_completion = std::function<void(bool, adapter_status)>;

/**
 * Called after the loopback listener is bound, before USB/IP traffic is
 * admitted.  The callback may complete synchronously or retain the completion
 * for an asynchronous host attach.  It must copy the endpoint and binding if
 * it keeps them beyond the callback invocation.
 */
using local_endpoint_ready_callback = std::function<void(
  const endpoint &,
  const session_binding &,
  local_endpoint_ready_completion)>;

/**
 * Callbacks owned by the authenticated Moonlight/RUSB transport.
 *
 * `send_frame` receives a typed message, not a wire frame.  It must copy or
 * consume the message before returning.  Authentication, framing, sequence
 * numbers and fragmentation remain entirely outside this adapter.
 */
struct broker_transport_callbacks {
  std::function<bool(transport_message)> send_frame;
  std::function<void(close_reason)> on_closed;
  local_endpoint_ready_callback on_local_endpoint_ready;
};

/**
 * Binds one authenticated remote USB session to one loopback USB/IP bridge.
 * The bridge is started only after OPEN/OPEN_OK has completed, so a local
 * usbip-win2 helper cannot submit URBs before the remote lease is authorized.
 */
class broker_adapter final {
public:
  broker_adapter() = default;
  ~broker_adapter();

  broker_adapter(const broker_adapter &) = delete;
  broker_adapter &operator=(const broker_adapter &) = delete;

  /** Bind a fresh authenticated session.  This does not open a listener. */
  bool start(session_binding binding,
             loopback_usbip_bridge &bridge,
             broker_transport_callbacks callbacks,
             boost::system::error_code &error);

  /** Ingress methods receive already authenticated/decoded typed events. */
  bool accept_capability(const capability_event &event);
  /** Request the peer to bind the capability (the host/client role). */
  bool request_open();
  bool accept_open(const open_event &event);
  bool accept_open_ok(const open_ok_event &event);
  bool accept_open_ok(std::uint64_t session_token);
  bool accept_open_reject(const open_reject_event &event);
  bool accept_open_reject(std::uint64_t session_token, std::uint32_t status);
  bool accept_usbip_pdu(const usbip_pdu_event &event);
  bool accept_usbip_pdu(std::uint64_t pdu_id, std::vector<std::uint8_t> pdu);
  bool accept_close(const close_event &event);
  bool accept_close(std::uint64_t session_token, std::uint64_t lease_token);

  /** Stop the bridge and make the binding terminal.  Safe to repeat. */
  void stop();

  adapter_state state() const noexcept;
  adapter_status last_status() const noexcept;
  bool running() const noexcept;
  bool imported() const noexcept;
  std::size_t pending_count() const noexcept;
  std::optional<endpoint> local_endpoint() const;

private:
  struct pending_request {
    std::size_t wire_size { 0 };
    std::uint32_t command { 0 };
    std::uint32_t seqnum { 0 };
    std::uint32_t unlink_seqnum { 0 };
  };

  bool handle_local_request(std::vector<std::uint8_t> pdu);
  void handle_local_imported();
  void handle_bridge_closed(close_reason reason);
  void complete_open(std::uint64_t generation,
                     bool success,
                     adapter_status status);

  bool activate_bridge(bool reply_with_open_ok);
  bool emit(transport_message message);
  bool valid_session_token(std::uint64_t token) const noexcept;
  bool valid_lease_tokens(std::uint64_t lease,
                          std::uint64_t attachment) const noexcept;
  bool valid_pdu_size(std::size_t size) const noexcept;
  void close_session(adapter_status status,
                     close_reason reason,
                     bool send_close,
                     bool bridge_already_closed = false);

  mutable std::mutex mutex_;
  session_binding binding_;
  broker_transport_callbacks callbacks_;
  loopback_usbip_bridge *bridge_ { nullptr };
  device_info device_;
  adapter_state state_ { adapter_state::idle };
  adapter_status last_status_ { adapter_status::ok };
  bool capability_seen_ { false };
  bool open_sent_ { false };
  bool imported_ { false };
  bool endpoint_ready_pending_ { false };
  bool open_reply_required_ { false };
  std::uint64_t endpoint_ready_generation_ { 0 };
  std::uint64_t generation_ { 0 };
  std::uint64_t next_pdu_id_ { 1 };
  std::unordered_map<std::uint64_t, pending_request> pending_;
  /* A RET_UNLINK retires its target, but a late RET_SUBMIT may still arrive.
   * Keep a bounded tombstone so the bridge can consume that completion. */
  std::unordered_map<std::uint64_t, pending_request> cancelled_;
};

}  // namespace remote_usb
