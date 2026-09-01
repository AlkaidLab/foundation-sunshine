/**
 * @file src/remote_usb/remote_usb_broker_adapter.cpp
 * @brief Typed session glue for the loopback USB/IP bridge.
 */

#include "remote_usb_broker_adapter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include <boost/system/errc.hpp>

namespace remote_usb {
namespace {

boost::system::error_code
invalid_argument_error() {
  return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
}

boost::system::error_code
busy_error() {
  return boost::system::errc::make_error_code(boost::system::errc::device_or_resource_busy);
}

bool
all_zero(const std::array<std::uint8_t, 16> &value) noexcept {
  return std::all_of(value.begin(), value.end(), [](std::uint8_t byte) {
    return byte == 0;
  });
}

bool
valid_fixed_string(const std::string &value, std::size_t width, bool allow_empty) {
  if (value.size() >= width || value.find('\0') != std::string::npos) {
    return false;
  }
  return allow_empty || !value.empty();
}

bool
valid_device(const device_info &device) {
  return valid_fixed_string(device.busid, 32, false) &&
         (device.path.empty() || valid_fixed_string(device.path, 256, false)) &&
         device.interfaces.size() <= 255;
}

bool
valid_binding(const session_binding &binding) noexcept {
  return !all_zero(binding.client_uuid) && binding.stream_generation != 0 &&
         binding.session_token != 0 && binding.attachment_token != 0 &&
         binding.lease_token != 0 && !all_zero(binding.capability_nonce) &&
         binding.max_urb >= broker_min_urb && binding.max_urb <= broker_max_pdu_size &&
         binding.max_inflight != 0 && binding.max_inflight <= broker_max_inflight &&
         !binding.isochronous;
}

/* Only command/sequence are needed for UNLINK bookkeeping.  Reply device,
 * direction and endpoint fields intentionally stay opaque to this layer;
 * loopback_usbip_bridge validates both canonical-zero and echoed forms. */
struct pdu_identity {
  std::uint32_t command { 0 };
  std::uint32_t seqnum { 0 };
  std::uint32_t unlink_seqnum { 0 };
};

bool
decode_pdu_identity(const std::vector<std::uint8_t> &pdu,
                    pdu_identity &identity) noexcept {
  if (pdu.size() < broker_usbip_header_size) {
    return false;
  }
  const auto read_u32_be = [&pdu](std::size_t offset) {
    return (static_cast<std::uint32_t>(pdu[offset]) << 24) |
           (static_cast<std::uint32_t>(pdu[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(pdu[offset + 2]) << 8) |
           static_cast<std::uint32_t>(pdu[offset + 3]);
  };
  identity.command = read_u32_be(0);
  identity.seqnum = read_u32_be(4);
  identity.unlink_seqnum = read_u32_be(20);
  return identity.seqnum != 0;
}

}  // namespace

broker_adapter::~broker_adapter() {
  stop();
}

bool
broker_adapter::start(session_binding binding,
                       loopback_usbip_bridge &bridge,
                       broker_transport_callbacks callbacks,
                       boost::system::error_code &error) {
  error.clear();
  if (!valid_binding(binding) || !callbacks.send_frame) {
    error = invalid_argument_error();
    std::lock_guard lock(mutex_);
    last_status_ = adapter_status::invalid_argument;
    return false;
  }

  /* A bridge must be quiescent before a binding is reused. */
  if (bridge.running()) {
    error = busy_error();
    std::lock_guard lock(mutex_);
    last_status_ = adapter_status::invalid_state;
    return false;
  }

  std::lock_guard lock(mutex_);
  if (state_ != adapter_state::idle && state_ != adapter_state::closed) {
    error = busy_error();
    last_status_ = adapter_status::invalid_state;
    return false;
  }

  binding_ = std::move(binding);
  callbacks_ = std::move(callbacks);
  bridge_ = &bridge;
  device_ = {};
  state_ = adapter_state::awaiting_capability;
  last_status_ = adapter_status::ok;
  capability_seen_ = false;
  open_sent_ = false;
  imported_ = false;
  endpoint_ready_pending_ = false;
  open_reply_required_ = false;
  endpoint_ready_generation_ = 0;
  pending_.clear();
  cancelled_.clear();
  next_pdu_id_ = 1;
  ++generation_;
  if (generation_ == 0) {
    generation_ = 1;
  }
  return true;
}

bool
broker_adapter::accept_capability(const capability_event &event) {
  if (!valid_device(event.device)) {
    close_session(adapter_status::invalid_argument, close_reason::protocol_error, false);
    return false;
  }

  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::awaiting_capability || capability_seen_) {
      last_status_ = adapter_status::invalid_state;
      /* State errors are terminal for this authenticated binding. */
    } else if (event.session_token != binding_.session_token ||
               event.lease_token != binding_.lease_token ||
               event.attachment_token != binding_.attachment_token) {
      last_status_ = adapter_status::token_mismatch;
    } else {
      device_ = event.device;
      capability_seen_ = true;
      state_ = adapter_state::awaiting_open;
      last_status_ = adapter_status::ok;
      return true;
    }
  }

  const auto status = last_status();
  close_session(status,
                status == adapter_status::token_mismatch ? close_reason::protocol_error :
                                                            close_reason::transport_rejected,
                false);
  return false;
}

bool
broker_adapter::request_open() {
  transport_message message;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::awaiting_open || !capability_seen_ || open_sent_) {
      last_status_ = adapter_status::invalid_state;
      return false;
    }
    open_sent_ = true;
    message.kind = transport_message_kind::open;
    message.session_token = binding_.session_token;
    message.lease_token = binding_.lease_token;
    message.attachment_token = binding_.attachment_token;
  }

  if (emit(std::move(message))) {
    return true;
  }
  close_session(adapter_status::transport_rejected,
                close_reason::transport_rejected,
                false);
  return false;
}

bool
broker_adapter::accept_open(const open_event &event) {
  adapter_status status = adapter_status::ok;
  bool activate = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::awaiting_open || !capability_seen_ || open_sent_) {
      last_status_ = adapter_status::invalid_state;
      status = last_status_;
    } else if (event.session_token != binding_.session_token ||
               event.lease_token != binding_.lease_token ||
               event.attachment_token != binding_.attachment_token) {
      last_status_ = adapter_status::token_mismatch;
      status = last_status_;
    } else {
      last_status_ = adapter_status::ok;
      activate = true;
    }
  }

  if (activate) {
    /* activate_bridge() transitions starting_bridge -> open. */
    return activate_bridge(true);
  }

  close_session(status,
                status == adapter_status::token_mismatch ? close_reason::protocol_error :
                                                            close_reason::transport_rejected,
                false);
  return false;
}

bool
broker_adapter::accept_open_ok(const open_ok_event &event) {
  adapter_status status = adapter_status::ok;
  bool activate = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::awaiting_open || !capability_seen_ || !open_sent_) {
      last_status_ = adapter_status::invalid_state;
      status = last_status_;
    } else if (event.session_token != binding_.session_token) {
      last_status_ = adapter_status::token_mismatch;
      status = last_status_;
    } else {
      last_status_ = adapter_status::ok;
      activate = true;
    }
  }

  if (activate) {
    return activate_bridge(false);
  }

  close_session(status,
                status == adapter_status::token_mismatch ? close_reason::protocol_error :
                                                            close_reason::transport_rejected,
                false);
  return false;
}

bool
broker_adapter::accept_open_ok(std::uint64_t session_token) {
  return accept_open_ok(open_ok_event { session_token });
}

bool
broker_adapter::accept_open_reject(const open_reject_event &event) {
  adapter_status status = adapter_status::ok;
  bool accepted_reject = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::awaiting_open || !capability_seen_ || !open_sent_) {
      last_status_ = adapter_status::invalid_state;
      status = last_status_;
    } else if (event.session_token != binding_.session_token) {
      last_status_ = adapter_status::token_mismatch;
      status = last_status_;
    } else if (event.status == 0) {
      last_status_ = adapter_status::invalid_argument;
      status = last_status_;
    } else {
      last_status_ = adapter_status::transport_rejected;
      accepted_reject = true;
    }
  }

  if (accepted_reject) {
    /* The peer's rejection is already the terminal response. */
    close_session(adapter_status::transport_rejected,
                  close_reason::transport_rejected,
                  false);
    return true;
  }

  close_session(status,
                status == adapter_status::token_mismatch ? close_reason::protocol_error :
                                                            close_reason::transport_rejected,
                false);
  return false;
}

bool
broker_adapter::accept_open_reject(std::uint64_t session_token, std::uint32_t status) {
  return accept_open_reject(open_reject_event { session_token, status });
}

bool
broker_adapter::accept_usbip_pdu(const usbip_pdu_event &event) {
  loopback_usbip_bridge *bridge = nullptr;
  std::uint64_t generation = 0;
  adapter_status rejected = adapter_status::ok;
  bool cancelled = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::open) {
      last_status_ = adapter_status::invalid_state;
      rejected = last_status_;
    } else if (event.session_token != binding_.session_token ||
               event.lease_token != binding_.lease_token) {
      last_status_ = adapter_status::token_mismatch;
      rejected = last_status_;
    } else if (event.pdu_id == 0 || !valid_pdu_size(event.pdu.size())) {
      last_status_ = adapter_status::limit_exceeded;
      rejected = last_status_;
    } else {
      const auto pending = pending_.find(event.pdu_id);
      const auto cancelled_request = cancelled_.find(event.pdu_id);
      if (pending == pending_.end() && cancelled_request == cancelled_.end()) {
        last_status_ = adapter_status::fragment_error;
        rejected = last_status_;
      } else {
        bridge = bridge_;
        generation = generation_;
        cancelled = cancelled_request != cancelled_.end();
        last_status_ = adapter_status::ok;
      }
    }
  }

  if (bridge == nullptr) {
    close_session(rejected,
                  rejected == adapter_status::token_mismatch ||
                          rejected == adapter_status::fragment_error ?
                      close_reason::protocol_error : close_reason::transport_rejected,
                  true);
    return false;
  }

  /* Do not inspect reply fields here.  The bridge owns USB/IP validation and
   * routes the completion using the request's native sequence/devid table. */
  bool accepted = false;
  try {
    accepted = bridge->send_reply(event.pdu);
  } catch (...) {
    accepted = false;
  }
  if (!accepted) {
    close_session(adapter_status::transport_rejected,
                  close_reason::transport_rejected,
                  true);
    return false;
  }

  {
    std::lock_guard lock(mutex_);
    if (generation_ != generation || state_ != adapter_state::open) {
      last_status_ = adapter_status::already_closed;
      return false;
    }
    if (cancelled) {
      const auto cancelled_request = cancelled_.find(event.pdu_id);
      if (cancelled_request == cancelled_.end()) {
        last_status_ = adapter_status::fragment_error;
        return false;
      }
      cancelled_.erase(cancelled_request);
      last_status_ = adapter_status::ok;
      return true;
    }
    const auto pending = pending_.find(event.pdu_id);
    if (pending == pending_.end()) {
      last_status_ = adapter_status::fragment_error;
      return false;
    }

    pdu_identity reply_identity;
    if (decode_pdu_identity(event.pdu, reply_identity) &&
        reply_identity.command == 4u && pending->second.command == 2u) {
      for (auto target = pending_.begin(); target != pending_.end(); ++target) {
        if (target->first != event.pdu_id && target->second.command == 1u &&
            target->second.seqnum == pending->second.unlink_seqnum) {
          if (cancelled_.size() < static_cast<std::size_t>(binding_.max_inflight)) {
            cancelled_.emplace(target->first, target->second);
            pending_.erase(target);
          }
          break;
        }
      }
    }
    pending_.erase(pending);
    last_status_ = adapter_status::ok;
  }
  return true;
}

bool
broker_adapter::accept_usbip_pdu(std::uint64_t pdu_id,
                                  std::vector<std::uint8_t> pdu) {
  usbip_pdu_event event;
  {
    std::lock_guard lock(mutex_);
    event.session_token = binding_.session_token;
    event.lease_token = binding_.lease_token;
  }
  event.pdu_id = pdu_id;
  event.pdu = std::move(pdu);
  return accept_usbip_pdu(event);
}

bool
broker_adapter::accept_close(const close_event &event) {
  adapter_status status = adapter_status::ok;
  bool accepted_close = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ == adapter_state::idle) {
      last_status_ = adapter_status::invalid_state;
      status = last_status_;
    } else if (event.session_token != binding_.session_token ||
               event.lease_token != binding_.lease_token) {
      last_status_ = adapter_status::token_mismatch;
      status = last_status_;
    } else if (state_ == adapter_state::closed) {
      last_status_ = adapter_status::already_closed;
      return true;
    } else {
      last_status_ = adapter_status::ok;
      accepted_close = true;
    }
  }

  if (accepted_close) {
    close_session(adapter_status::ok, close_reason::stopped, false);
    return true;
  }

  close_session(status,
                status == adapter_status::token_mismatch ? close_reason::protocol_error :
                                                            close_reason::transport_rejected,
                false);
  return false;
}

bool
broker_adapter::accept_close(std::uint64_t session_token, std::uint64_t lease_token) {
  return accept_close(close_event { session_token, lease_token });
}

void
broker_adapter::stop() {
  /* `close_session()` is idempotent, but a bridge I/O thread may have
   * already transitioned the adapter to closed before the broker's teardown
   * path runs.  Always take the bridge stop path as well so an external
   * caller waits for a self-detached bridge thread and its on_closed callback
   * to finish. */
  loopback_usbip_bridge *bridge = nullptr;
  {
    std::lock_guard lock(mutex_);
    bridge = bridge_;
  }
  close_session(adapter_status::ok, close_reason::stopped, true);
  if (bridge != nullptr) {
    bridge->stop();
  }
}

adapter_state
broker_adapter::state() const noexcept {
  std::lock_guard lock(mutex_);
  return state_;
}

adapter_status
broker_adapter::last_status() const noexcept {
  std::lock_guard lock(mutex_);
  return last_status_;
}

bool
broker_adapter::running() const noexcept {
  loopback_usbip_bridge *bridge = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::open) {
      return false;
    }
    bridge = bridge_;
  }
  return bridge != nullptr && bridge->running();
}

bool
broker_adapter::imported() const noexcept {
  std::lock_guard lock(mutex_);
  return imported_;
}

std::size_t
broker_adapter::pending_count() const noexcept {
  std::lock_guard lock(mutex_);
  return pending_.size() + cancelled_.size();
}

std::optional<endpoint>
broker_adapter::local_endpoint() const {
  loopback_usbip_bridge *bridge = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::open) {
      return std::nullopt;
    }
    bridge = bridge_;
  }
  return bridge == nullptr ? std::nullopt : bridge->local_endpoint();
}

bool
broker_adapter::handle_local_request(std::vector<std::uint8_t> pdu) {
  transport_message message;
  std::uint64_t generation = 0;
  std::uint64_t pdu_id = 0;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::open || !valid_pdu_size(pdu.size())) {
      last_status_ = state_ == adapter_state::open ? adapter_status::limit_exceeded :
                                                      adapter_status::invalid_state;
    } else if (pending_.size() + cancelled_.size() >= binding_.max_inflight ||
               next_pdu_id_ == std::numeric_limits<std::uint64_t>::max()) {
      last_status_ = adapter_status::limit_exceeded;
    } else {
      pdu_identity identity;
      if (!decode_pdu_identity(pdu, identity) ||
          (identity.command != 1u && identity.command != 2u)) {
        last_status_ = adapter_status::invalid_argument;
      } else {
        pdu_id = next_pdu_id_++;
        if (pdu_id == 0) {
          pdu_id = next_pdu_id_++;
        }
        pending_.emplace(pdu_id,
                         pending_request { pdu.size(), identity.command,
                                            identity.seqnum, identity.unlink_seqnum });
        generation = generation_;
        message.kind = transport_message_kind::usbip_data;
        message.session_token = binding_.session_token;
        message.lease_token = binding_.lease_token;
        message.pdu_id = pdu_id;
        message.pdu = std::move(pdu);
        last_status_ = adapter_status::ok;
      }
    }
  }

  if (pdu_id == 0) {
    close_session(last_status(), close_reason::queue_limit, true);
    return false;
  }

  bool sent = false;
  try {
    sent = emit(std::move(message));
  } catch (...) {
    sent = false;
  }
  if (!sent) {
    {
      std::lock_guard lock(mutex_);
      if (generation_ == generation) {
        pending_.erase(pdu_id);
      }
    }
    close_session(adapter_status::transport_rejected,
                  close_reason::transport_rejected,
                  true);
    return false;
  }

  {
    std::lock_guard lock(mutex_);
    if (generation_ != generation || state_ != adapter_state::open) {
      return false;
    }
  }
  return true;
}

void
broker_adapter::handle_local_imported() {
  std::lock_guard lock(mutex_);
  if (state_ == adapter_state::open || state_ == adapter_state::starting_bridge) {
    imported_ = true;
  }
}

void
broker_adapter::handle_bridge_closed(close_reason reason) {
  {
    std::lock_guard lock(mutex_);
    if (state_ == adapter_state::idle || state_ == adapter_state::closed) {
      return;
    }
  }
  /* The bridge reports the session closure before its I/O thread stops.  Stop
   * it here as part of terminating this binding; leaving its listener alive
   * would retain callbacks that capture this adapter and block later reuse. */
  close_session(adapter_status::transport_rejected, reason, true, false);
}

bool
broker_adapter::activate_bridge(bool reply_with_open_ok) {
  loopback_usbip_bridge *bridge = nullptr;
  device_info device;
  std::uint64_t generation = 0;
  {
    std::lock_guard lock(mutex_);
    if (state_ != adapter_state::awaiting_open || !capability_seen_ || bridge_ == nullptr) {
      last_status_ = adapter_status::invalid_state;
      return false;
    }
    state_ = adapter_state::starting_bridge;
    endpoint_ready_pending_ = true;
    open_reply_required_ = reply_with_open_ok;
    endpoint_ready_generation_ = generation_;
    bridge = bridge_;
    device = device_;
    generation = generation_;
  }

  callbacks bridge_callbacks;
  bridge_callbacks.on_request = [this](std::vector<std::uint8_t> pdu) {
    return handle_local_request(std::move(pdu));
  };
  bridge_callbacks.on_imported = [this]() { handle_local_imported(); };
  bridge_callbacks.on_closed = [this](close_reason reason) {
    handle_bridge_closed(reason);
  };

  boost::system::error_code error;
  try {
    if (!bridge->start(std::move(device), std::move(bridge_callbacks), error).has_value()) {
      {
        std::lock_guard lock(mutex_);
        if (generation_ == generation) {
          endpoint_ready_pending_ = false;
          endpoint_ready_generation_ = 0;
        }
      }
      close_session(adapter_status::bridge_failure, close_reason::internal_error, false);
      return false;
    }
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      if (generation_ == generation) {
        endpoint_ready_pending_ = false;
        endpoint_ready_generation_ = 0;
      }
    }
    close_session(adapter_status::bridge_failure, close_reason::internal_error, false);
    return false;
  }

  const auto local_endpoint = bridge->local_endpoint();
  if (!local_endpoint) {
    {
      std::lock_guard lock(mutex_);
      if (generation_ == generation) {
        endpoint_ready_pending_ = false;
        endpoint_ready_generation_ = 0;
      }
    }
    bridge->stop();
    close_session(adapter_status::bridge_failure, close_reason::internal_error, false);
    return false;
  }

  local_endpoint_ready_callback endpoint_callback;
  session_binding binding_snapshot;
  bool stale = false;
  {
    std::lock_guard lock(mutex_);
    stale = generation_ != generation || state_ != adapter_state::starting_bridge ||
            !endpoint_ready_pending_ || endpoint_ready_generation_ != generation;
    if (stale) {
      /* A concurrent CLOSE/STOP invalidated this bridge while it was binding. */
      endpoint_callback = {};
    } else {
      endpoint_callback = callbacks_.on_local_endpoint_ready;
      binding_snapshot = binding_;
    }
  }
  if (stale) {
    bridge->stop();
    return false;
  }

  if (endpoint_callback) {
    auto completion = [this, generation](bool success, adapter_status status) {
      complete_open(generation, success, status);
    };
    try {
      endpoint_callback(*local_endpoint, binding_snapshot, std::move(completion));
    } catch (...) {
      complete_open(generation, false, adapter_status::bridge_failure);
    }
    /* The endpoint callback owns the transition to OPEN.  It may complete
     * synchronously, but must also be allowed to retain the completion. */
    return true;
  }

  complete_open(generation, true, adapter_status::ok);
  return true;
}

void
broker_adapter::complete_open(std::uint64_t generation,
                               bool success,
                               adapter_status status) {
  bool send_open_ok = false;
  transport_message open_ok;
  adapter_status failure_status = status;
  {
    std::lock_guard lock(mutex_);
    if (generation_ != generation || state_ != adapter_state::starting_bridge ||
        !endpoint_ready_pending_ || endpoint_ready_generation_ != generation) {
      return;
    }
    endpoint_ready_pending_ = false;
    endpoint_ready_generation_ = 0;
    if (!success) {
      failure_status = status == adapter_status::ok ? adapter_status::bridge_failure : status;
      last_status_ = failure_status;
    } else {
      state_ = adapter_state::open;
      last_status_ = adapter_status::ok;
      send_open_ok = open_reply_required_;
      open_reply_required_ = false;
      if (send_open_ok) {
        open_ok.kind = transport_message_kind::open_ok;
        open_ok.session_token = binding_.session_token;
      }
    }
  }

  if (!success) {
    close_session(failure_status,
                  failure_status == adapter_status::transport_rejected
                    ? close_reason::transport_rejected
                    : close_reason::internal_error,
                  true);
    return;
  }
  if (send_open_ok && !emit(std::move(open_ok))) {
    close_session(adapter_status::transport_rejected,
                  close_reason::transport_rejected,
                  true);
  }
}

bool
broker_adapter::emit(transport_message message) {
  std::function<bool(transport_message)> sender;
  {
    std::lock_guard lock(mutex_);
    if (state_ == adapter_state::idle || state_ == adapter_state::closed ||
        !callbacks_.send_frame) {
      return false;
    }
    sender = callbacks_.send_frame;
  }
  try {
    return sender(std::move(message));
  } catch (...) {
    return false;
  }
}

bool
broker_adapter::valid_session_token(std::uint64_t token) const noexcept {
  std::lock_guard lock(mutex_);
  return token != 0 && token == binding_.session_token;
}

bool
broker_adapter::valid_lease_tokens(std::uint64_t lease,
                                    std::uint64_t attachment) const noexcept {
  std::lock_guard lock(mutex_);
  return lease != 0 && attachment != 0 && lease == binding_.lease_token &&
         attachment == binding_.attachment_token;
}

bool
broker_adapter::valid_pdu_size(std::size_t size) const noexcept {
  const auto maximum = std::min<std::uint32_t>(binding_.max_urb, broker_max_pdu_size);
  return size >= broker_usbip_header_size && size <= static_cast<std::size_t>(maximum);
}

void
broker_adapter::close_session(adapter_status status,
                               close_reason reason,
                               bool send_close,
                               bool bridge_already_closed) {
  loopback_usbip_bridge *bridge = nullptr;
  broker_transport_callbacks callbacks;
  transport_message close_message;
  bool notify = false;
  bool should_send_close = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ == adapter_state::idle || state_ == adapter_state::closed) {
      if (state_ == adapter_state::closed && last_status_ == adapter_status::ok &&
          status != adapter_status::ok) {
        last_status_ = status;
      }
      return;
    }
    const auto was_open = state_ == adapter_state::open ||
                          state_ == adapter_state::starting_bridge;
    last_status_ = status;
    state_ = adapter_state::closed;
    ++generation_;
    pending_.clear();
    cancelled_.clear();
    imported_ = false;
    endpoint_ready_pending_ = false;
    open_reply_required_ = false;
    endpoint_ready_generation_ = 0;
    bridge = bridge_;
    callbacks = callbacks_;
    notify = true;
    should_send_close = send_close && was_open && static_cast<bool>(callbacks.send_frame);
    if (should_send_close) {
      close_message.kind = transport_message_kind::close;
      close_message.session_token = binding_.session_token;
      close_message.lease_token = binding_.lease_token;
    }
  }

  if (should_send_close) {
    try {
      (void)callbacks.send_frame(std::move(close_message));
    } catch (...) {
      /* Closing is best effort; the authenticated transport may already be gone. */
    }
  }
  if (bridge != nullptr && !bridge_already_closed) {
    bridge->stop();
  }
  if (notify && callbacks.on_closed) {
    try {
      callbacks.on_closed(reason);
    } catch (...) {
      /* A user callback must not escape a bridge or transport thread. */
    }
  }
}

}  // namespace remote_usb
