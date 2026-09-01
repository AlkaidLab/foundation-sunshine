/**
 * @file src/remote_usb/remote_usb_broker_server.cpp
 * @brief Certificate-authenticated RUSB broker listener.
 */

#include "remote_usb_broker_server.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/async_result.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace remote_usb {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using error_code = boost::system::error_code;
using namespace std::chrono_literals;

constexpr std::size_t kHelloSize = 84;
constexpr std::size_t kFrameHeaderSize = 32;
constexpr std::size_t kCapabilityPrefixSize = 34;
constexpr std::size_t kOpenPayloadSize = 16;
constexpr std::size_t kOpenRejectPayloadSize = 4;
constexpr std::size_t kFragmentPrefixSize = 32;
constexpr std::size_t kClosePayloadSize = 8;
constexpr std::uint32_t kMagic = 0x42535552u;  // RUSB, little endian.
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kFlagMore = 1;
constexpr std::uint32_t kMaxPayload = 128u * 1024u;
constexpr std::uint32_t kMaxReassembly = 1024u * 1024u;
constexpr std::uint32_t kMaxFragments = 4096;
constexpr std::uint32_t kMaxOpenRejectStatus = 10;
constexpr std::uint32_t kMaxWindowPdus = 4096;
constexpr std::uint64_t kMaxWindowBytes = 16u * 1024u * 1024u;
/* A queued USB/IP PDU carries one RUSB header and one fragment prefix per
 * fragment.  The logical window is charged by complete PDU bytes; this
 * overhead bound keeps the physical write queue bounded without making a
 * PDU that exactly fills the logical window impossible to send. */
constexpr std::uint64_t kFrameQueueOverhead =
  static_cast<std::uint64_t>(kFrameHeaderSize + kFragmentPrefixSize);

/* A session close callback may run on the loopback bridge thread while the
 * broker worker is synchronously waiting for that bridge to stop.  Keep a
 * tiny, thread-local callback marker so a callback that requests broker stop
 * can initiate shutdown without waiting on the worker it is helping unwind. */
thread_local const void *broker_callback_owner = nullptr;

class broker_callback_scope final {
public:
  explicit broker_callback_scope(const void *owner):
      previous_(broker_callback_owner) {
    broker_callback_owner = owner;
  }

  ~broker_callback_scope() {
    broker_callback_owner = previous_;
  }

  broker_callback_scope(const broker_callback_scope &) = delete;
  broker_callback_scope &operator=(const broker_callback_scope &) = delete;

private:
  const void *previous_;
};

std::uint16_t
read_u16_le(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t
read_u32_le(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t
read_u64_le(const std::uint8_t *bytes) noexcept {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

void
write_u16_le(std::uint8_t *bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value);
  bytes[1] = static_cast<std::uint8_t>(value >> 8);
}

void
write_u32_le(std::uint8_t *bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value);
  bytes[1] = static_cast<std::uint8_t>(value >> 8);
  bytes[2] = static_cast<std::uint8_t>(value >> 16);
  bytes[3] = static_cast<std::uint8_t>(value >> 24);
}

void
write_u64_le(std::uint8_t *bytes, std::uint64_t value) noexcept {
  for (unsigned index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
}

bool
all_zero(const std::uint8_t *bytes, std::size_t size) noexcept {
  for (std::size_t index = 0; index < size; ++index) {
    if (bytes[index] != 0) {
      return false;
    }
  }
  return true;
}

bool
valid_nonzero_uuid(const std::array<std::uint8_t, 16> &bytes) noexcept {
  return !all_zero(bytes.data(), bytes.size());
}

bool
valid_hello(const broker_hello &hello) noexcept {
  return valid_nonzero_uuid(hello.wire_client_uuid) &&
         hello.stream_generation != 0 && hello.session_token != 0 &&
         hello.attachment_token != 0 && hello.lease_token != 0 &&
         valid_nonzero_uuid(hello.capability_nonce) &&
         hello.max_urb >= broker_min_urb && hello.max_urb <= broker_max_pdu_size &&
         hello.max_inflight != 0 &&
         hello.max_inflight <= broker_max_inflight && !hello.isochronous;
}

bool
decode_hello(const std::array<std::uint8_t, kHelloSize> &wire,
             broker_hello &hello) noexcept {
  if (read_u32_le(wire.data()) != kMagic || read_u16_le(wire.data() + 4) != kVersion ||
      read_u16_le(wire.data() + 6) != kHelloSize || !all_zero(wire.data() + 81, 3)) {
    return false;
  }

  hello = {};
  std::copy_n(wire.data() + 8, hello.wire_client_uuid.size(), hello.wire_client_uuid.data());
  hello.stream_generation = read_u64_le(wire.data() + 24);
  hello.session_token = read_u64_le(wire.data() + 32);
  hello.attachment_token = read_u64_le(wire.data() + 40);
  hello.lease_token = read_u64_le(wire.data() + 48);
  std::copy_n(wire.data() + 56, hello.capability_nonce.size(), hello.capability_nonce.data());
  hello.max_urb = read_u32_le(wire.data() + 72);
  hello.max_inflight = read_u32_le(wire.data() + 76);
  hello.isochronous = wire[80] != 0;
  return valid_hello(hello);
}

struct frame_header {
  std::uint8_t type { 0 };
  std::uint32_t flags { 0 };
  std::uint32_t payload_length { 0 };
  std::uint64_t session_token { 0 };
  std::uint64_t sequence { 0 };
};

bool
decode_frame_header(const std::array<std::uint8_t, kFrameHeaderSize> &wire,
                    frame_header &header,
                    std::uint32_t max_payload) noexcept {
  if (read_u32_le(wire.data()) != kMagic || wire[4] != kVersion ||
      wire[5] < 1 || wire[5] > 6 || read_u16_le(wire.data() + 6) != kFrameHeaderSize) {
    return false;
  }
  header.type = wire[5];
  header.flags = read_u32_le(wire.data() + 8);
  header.payload_length = read_u32_le(wire.data() + 12);
  header.session_token = read_u64_le(wire.data() + 16);
  header.sequence = read_u64_le(wire.data() + 24);
  if (header.session_token == 0 || header.sequence == 0 ||
      header.sequence == std::numeric_limits<std::uint64_t>::max() ||
      header.payload_length > std::min(max_payload, kMaxPayload)) {
    return false;
  }
  if (header.type != 5 && header.flags != 0) {
    return false;
  }
  if (header.type == 5 && (header.flags & ~kFlagMore) != 0) {
    return false;
  }
  return true;
}

void
encode_frame_header(std::array<std::uint8_t, kFrameHeaderSize> &wire,
                    std::uint8_t type,
                    std::uint32_t flags,
                    std::uint32_t payload_length,
                    std::uint64_t session_token,
                    std::uint64_t sequence) noexcept {
  wire.fill(0);
  write_u32_le(wire.data(), kMagic);
  wire[4] = static_cast<std::uint8_t>(kVersion);
  wire[5] = type;
  write_u16_le(wire.data() + 6, kFrameHeaderSize);
  write_u32_le(wire.data() + 8, flags);
  write_u32_le(wire.data() + 12, payload_length);
  write_u64_le(wire.data() + 16, session_token);
  write_u64_le(wire.data() + 24, sequence);
}

bool
valid_payload_shape(const frame_header &header,
                    const std::vector<std::uint8_t> &payload) noexcept {
  if (header.payload_length != payload.size()) {
    return false;
  }
  switch (header.type) {
    case 1:
      return payload.size() >= kCapabilityPrefixSize;
    case 2:
      return payload.size() == kOpenPayloadSize;
    case 3:
      return payload.empty();
    case 4:
      return payload.size() == kOpenRejectPayloadSize &&
             read_u32_le(payload.data()) != 0 &&
             read_u32_le(payload.data()) <= kMaxOpenRejectStatus;
    case 5:
      return payload.size() >= kFragmentPrefixSize;
    case 6:
      return payload.size() == kClosePayloadSize;
    default:
      return false;
  }
}

bool
parse_device_capability(const std::vector<std::uint8_t> &payload,
                        device_info &device,
                        std::uint64_t &lease_token,
                        std::uint64_t &attachment_token) {
  if (payload.size() < kCapabilityPrefixSize) {
    return false;
  }
  lease_token = read_u64_le(payload.data());
  attachment_token = read_u64_le(payload.data() + 8);
  const auto busid_size = static_cast<std::size_t>(payload[25]);
  const auto endpoint_count = static_cast<std::size_t>(read_u16_le(payload.data() + 26));
  const auto raw_size = static_cast<std::size_t>(read_u32_le(payload.data() + 30));
  if (lease_token == 0 || attachment_token == 0 || busid_size == 0 ||
      busid_size > 31 || endpoint_count > 256 || raw_size == 0 || raw_size > 64u * 1024u ||
      read_u16_le(payload.data() + 28) != 0) {
    return false;
  }
  const auto endpoint_bytes = endpoint_count * std::size_t { 8 };
  if (kCapabilityPrefixSize > payload.size() ||
      busid_size > payload.size() - kCapabilityPrefixSize ||
      raw_size > payload.size() - kCapabilityPrefixSize - busid_size ||
      endpoint_bytes != payload.size() - kCapabilityPrefixSize - busid_size - raw_size) {
    return false;
  }
  const auto busid_offset = kCapabilityPrefixSize;
  const auto raw_offset = busid_offset + busid_size;
  const auto endpoint_offset = raw_offset + raw_size;
  for (std::size_t index = 0; index < busid_size; ++index) {
    if (payload[busid_offset + index] == 0 || payload[busid_offset + index] < 0x20) {
      return false;
    }
  }
  for (std::size_t index = 0; index < endpoint_count; ++index) {
    if (payload[endpoint_offset + index * 8 + 7] != 0) {
      return false;
    }
  }

  /* Validate descriptor boundaries and collect interface class tuples. */
  std::vector<interface_info> interfaces;
  std::size_t descriptor_offset = raw_offset;
  const auto raw_end = raw_offset + raw_size;
  while (descriptor_offset < raw_end) {
    const auto length = static_cast<std::size_t>(payload[descriptor_offset]);
    if (length < 2 || length > raw_end - descriptor_offset) {
      return false;
    }
    if (payload[descriptor_offset + 1] == 4 && length >= 9) {
      interface_info info {
        payload[descriptor_offset + 5],
        payload[descriptor_offset + 6],
        payload[descriptor_offset + 7]
      };
      const auto duplicate = std::find_if(interfaces.begin(), interfaces.end(),
        [&info](const interface_info &candidate) {
          return candidate.interface_class == info.interface_class &&
                 candidate.interface_subclass == info.interface_subclass &&
                 candidate.interface_protocol == info.interface_protocol;
        });
      if (duplicate == interfaces.end()) {
        interfaces.push_back(info);
      }
    }
    descriptor_offset += length;
  }

  device = {};
  device.busid.assign(reinterpret_cast<const char *>(payload.data() + busid_offset), busid_size);
  /* loopback_usbip_bridge intentionally never forwards a host path. */
  device.path = device.busid;
  device.busnum = 1;
  device.devnum = 1;
  device.speed = 3;
  device.vendor_id = read_u16_le(payload.data() + 16);
  device.product_id = read_u16_le(payload.data() + 18);
  device.device_bcd = read_u16_le(payload.data() + 20);
  device.device_class = payload[22];
  device.device_subclass = payload[23];
  device.device_protocol = payload[24];
  device.interfaces = std::move(interfaces);
  return true;
}

}  // namespace

bool
decode_broker_hello(const std::array<std::uint8_t, broker_hello_size> &wire,
                    broker_hello &hello) noexcept {
  return decode_hello(wire, hello);
}

bool
decode_broker_frame_header(
  const std::array<std::uint8_t, broker_frame_header_size> &wire,
  broker_frame_header &header,
  std::uint32_t max_payload) noexcept {
  frame_header decoded;
  if (!decode_frame_header(wire, decoded, max_payload)) {
    return false;
  }
  header.type = decoded.type;
  header.flags = decoded.flags;
  header.payload_length = decoded.payload_length;
  header.session_token = decoded.session_token;
  header.sequence = decoded.sequence;
  return true;
}

void
encode_broker_frame_header(
  std::array<std::uint8_t, broker_frame_header_size> &wire,
  std::uint8_t type,
  std::uint32_t flags,
  std::uint32_t payload_length,
  std::uint64_t session_token,
  std::uint64_t sequence) noexcept {
  encode_frame_header(wire, type, flags, payload_length, session_token, sequence);
}

bool
decode_broker_capability_payload(const std::vector<std::uint8_t> &payload,
                                  device_info &device,
                                  std::uint64_t &lease_token,
                                  std::uint64_t &attachment_token) {
  return parse_device_capability(payload, device, lease_token, attachment_token);
}

class broker_session final : public std::enable_shared_from_this<broker_session> {
public:
  broker_session(std::shared_ptr<broker_server::impl> owner,
                 tcp::socket socket,
                 ssl::context &ssl_context);
  ~broker_session();

  void start();
  void stop(close_reason reason);

private:
  friend struct broker_server::impl;

  struct queued_frame {
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    /* Only the final fragment of a USBIP_DATA message carries these values.
     * Keeping the reservation on one queue item makes release atomic with the
     * final write while allowing any number of fragments per logical PDU. */
    std::size_t logical_pdu_bytes { 0 };
    std::uint32_t logical_pdu_count { 0 };
  };

  void arm_timer(std::uint32_t timeout_ms);
  void cancel_timer();
  void begin_hello_read();
  void handle_handshake(const error_code &error);
  void handle_hello(const error_code &error);
  void begin_frame_read();
  void handle_frame_header(const error_code &error);
  void handle_frame_payload(const error_code &error);
  bool process_frame(const frame_header &header, const std::vector<std::uint8_t> &payload);

  bool initialize_adapter(const std::array<std::uint8_t, kHelloSize> &wire);
  bool send_message(transport_message message);
  bool enqueue_wire(std::shared_ptr<std::vector<std::uint8_t>> wire);
  std::uint64_t wire_queue_limit() const noexcept;
  void begin_write();
  void handle_write(const error_code &error,
                   const std::shared_ptr<std::vector<std::uint8_t>> &wire);
  void close_on_io(close_reason reason);
  void post_close(close_reason reason);

  std::shared_ptr<broker_server::impl> owner_;
  ssl::stream<tcp::socket> stream_;
  asio::steady_timer timer_;
  std::mutex mutex_;
  /* `broker_server::stop()` may close a session from a thread other than the
   * io worker.  Serialize timer cancellation/arming and generation checks so
   * that a shutdown racing a read handler cannot access the Asio timer or its
   * generation counter concurrently. */
  std::mutex timer_mutex_;
  std::condition_variable close_condition_;
  std::atomic_bool closed_ { false };
  bool close_complete_ { false };
  /* Set by the one thread that claimed teardown.  Re-entrant close requests
   * from that thread must return instead of waiting on its own completion. */
  std::thread::id close_owner_id_;
  bool handshake_done_ { false };
  bool hello_done_ { false };
  bool write_in_progress_ { false };
  std::uint64_t timer_generation_ { 0 };
  std::uint64_t rx_sequence_ { 0 };
  std::uint64_t tx_sequence_ { 0 };
  broker_hello hello_;
  capability capability_;
  std::string cert_uuid_;
  std::array<std::uint8_t, kHelloSize> hello_wire_ {};
  std::array<std::uint8_t, kFrameHeaderSize> frame_header_wire_ {};
  std::vector<std::uint8_t> payload_;
  std::deque<queued_frame> write_queue_;
  std::size_t queued_bytes_ { 0 };
  std::uint64_t queued_logical_pdu_bytes_ { 0 };
  std::uint32_t queued_logical_pdu_count_ { 0 };
  bool reassembly_active_ { false };
  std::uint64_t reassembly_pdu_id_ { 0 };
  std::uint32_t reassembly_total_ { 0 };
  std::uint32_t reassembly_offset_ { 0 };
  std::uint32_t reassembly_fragments_ { 0 };
  std::vector<std::uint8_t> reassembly_buffer_;
  loopback_usbip_bridge bridge_;
  broker_adapter adapter_;
};

struct broker_server::impl : public std::enable_shared_from_this<broker_server::impl> {
  using work_guard_t = asio::executor_work_guard<asio::io_context::executor_type>;

  explicit impl(broker_server_config value):
      config(std::move(value)),
      ssl_context(ssl::context::tls_server),
      acceptor(io) {
  }

  broker_server_config config;
  asio::io_context io;
  std::unique_ptr<work_guard_t> work_guard;
  ssl::context ssl_context;
  tcp::acceptor acceptor;
  std::thread worker;
  mutable std::mutex worker_state_mutex;
  std::condition_variable worker_condition;
  std::thread::id worker_id;
  bool worker_alive { false };
  bool worker_finished { true };
  /* Set by a worker/callback that requests stop from inside the worker
   * lifecycle.  The worker detaches itself at the very end when no external
   * caller has claimed the thread for joining. */
  std::atomic_bool worker_detach_requested { false };
  /* Serializes start/stop and protects the std::thread lifecycle.  Socket
   * handlers use mutex below; they must not race a second stop() joining the
   * same worker. */
  std::mutex lifecycle_mutex;
  std::mutex mutex;
  std::set<std::shared_ptr<broker_session>> sessions;
  std::atomic_bool running { false };
  std::atomic_bool stopping { false };
  std::atomic<std::size_t> active_sessions { 0 };
  std::atomic<std::uint16_t> bound_port { 0 };

  bool on_io_thread() const noexcept {
    std::lock_guard lock(worker_state_mutex);
    return worker_alive && worker_id == std::this_thread::get_id();
  }

  bool worker_is_alive() const noexcept {
    std::lock_guard lock(worker_state_mutex);
    return worker_alive;
  }

  void wait_worker_finished() {
    std::unique_lock lock(worker_state_mutex);
    worker_condition.wait(lock, [this]() { return worker_finished; });
  }

  void reset_work_guard() noexcept {
    try {
      std::lock_guard lock(lifecycle_mutex);
      work_guard.reset();
    }
    catch (...) {
      /* io.stop() still releases run(); cleanup must remain noexcept. */
    }
  }

  bool post_session_close(const std::shared_ptr<broker_session> &session,
                          close_reason reason) {
    {
      std::lock_guard lock(worker_state_mutex);
      if (!worker_alive || io.stopped()) {
        return false;
      }
    }
    try {
      asio::post(io, [session, reason]() { session->close_on_io(reason); });
      return true;
    }
    catch (...) {
      return false;
    }
  }

  bool remove_noexcept(const std::shared_ptr<broker_session> &session) noexcept {
    try {
      std::lock_guard lock(mutex);
      if (sessions.erase(session) != 0) {
        active_sessions.fetch_sub(1);
        return true;
      }
      return false;
    }
    catch (...) {
      /* A best-effort emergency sweep must never unwind the worker. */
      return false;
    }
  }

  void accept_next() {
    if (stopping.load() || !acceptor.is_open()) {
      return;
    }
    auto self = shared_from_this();
    acceptor.async_accept([self](const error_code &error, tcp::socket socket) {
      if (!self->stopping.load() && !error) {
        const auto max_sessions = self->config.max_active_sessions == 0
                                    ? std::size_t { 1 }
                                    : self->config.max_active_sessions;
        if (self->active_sessions.load() < max_sessions) {
          auto session = std::make_shared<broker_session>(self, std::move(socket),
                                                            self->ssl_context);
          {
            std::lock_guard lock(self->mutex);
            self->sessions.insert(session);
          }
          self->active_sessions.fetch_add(1);
          session->start();
        }
        else {
          boost::system::error_code ignored;
          socket.shutdown(tcp::socket::shutdown_both, ignored);
          socket.close(ignored);
        }
      }
      if (!self->stopping.load()) {
        self->accept_next();
      }
    });
  }

  void remove(const std::shared_ptr<broker_session> &session) {
    std::lock_guard lock(mutex);
    if (sessions.erase(session) != 0) {
      active_sessions.fetch_sub(1);
    }
  }

  /* Close one session at a time so an allocation failure while taking a
   * snapshot cannot strand the remaining sessions.  The normal stop path is
   * already idempotent; the extra catches are for the worker's last-resort
   * exception path where no exception may escape into std::thread. */
  void close_sessions_noexcept(close_reason reason) noexcept {
    for (;;) {
      std::shared_ptr<broker_session> session;
      try {
        std::lock_guard lock(mutex);
        if (sessions.empty()) {
          return;
        }
        session = *sessions.begin();
      }
      catch (...) {
        return;
      }

      try {
        session->stop(reason);
      }
      catch (...) {
        /* Continue with an explicit erase below. */
      }
      /* `close_on_io()` normally erases itself.  Repeat the erase here so an
       * exceptional owner callback or a failed mutex operation cannot leave
       * this emergency loop spinning on a completed session. */
      if (!remove_noexcept(session)) {
        /* It was already erased by a concurrent close, or the owner mutex is
         * unavailable.  Avoid spinning forever in the latter case. */
        try {
          std::lock_guard lock(mutex);
          if (sessions.contains(session)) {
            return;
          }
        }
        catch (...) {
          return;
        }
      }
    }
  }
};

broker_session::broker_session(std::shared_ptr<broker_server::impl> owner,
                               tcp::socket socket,
                               ssl::context &ssl_context):
    owner_(std::move(owner)),
    stream_(std::move(socket), ssl_context),
    timer_(owner_->io) {
}

broker_session::~broker_session() {
  try {
    stop(close_reason::stopped);
  }
  catch (...) {
    /* Destruction is a last-resort cleanup path.  Never let an exceptional
     * platform socket/adapter teardown call std::terminate. */
  }
}

void
broker_session::start() {
  auto self = shared_from_this();
  error_code ignored;
  stream_.lowest_layer().set_option(tcp::no_delay(true), ignored);
  arm_timer(owner_->config.handshake_timeout_ms);
  stream_.async_handshake(ssl::stream_base::server,
    [self](const error_code &error) { self->handle_handshake(error); });
}

void
broker_session::stop(close_reason reason) {
  /* All Asio socket/timer operations belong to the broker worker.  A caller
   * outside that worker posts the close and waits until the teardown callback
   * (including adapter/host cleanup) has completed. */
  if (owner_->on_io_thread()) {
    close_on_io(reason);
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (close_complete_) {
      return;
    }
  }

  if (owner_->worker_is_alive()) {
    if (!owner_->post_session_close(shared_from_this(), reason)) {
      /* The worker may have stopped between the state check and post (or the
       * io context may already be stopping).  Wait for its completion barrier
       * before touching the stream from this caller; otherwise an in-flight
       * handler could race direct teardown. */
      if (owner_->worker_is_alive()) {
        owner_->wait_worker_finished();
      }
      close_on_io(reason);
      return;
    }
    std::unique_lock lock(mutex_);
    while (!close_complete_) {
      if (close_condition_.wait_for(lock, 100ms, [this]() {
            return close_complete_;
          })) {
        return;
      }
      /* If the worker exited before running the posted handler, no Asio
       * operation can still touch this session and direct teardown is safe. */
      if (!owner_->worker_is_alive()) {
        break;
      }
    }
    if (close_complete_) {
      return;
    }
  }

  close_on_io(reason);
}

void
broker_session::arm_timer(std::uint32_t timeout_ms) {
  const auto timeout = timeout_ms == 0 ? 1u : timeout_ms;
  auto self = shared_from_this();
  std::lock_guard timer_lock(timer_mutex_);
  const auto generation = ++timer_generation_;
  timer_.expires_after(std::chrono::milliseconds(timeout));
  timer_.async_wait([self, generation](const error_code &error) {
    bool current = false;
    {
      std::lock_guard timer_lock(self->timer_mutex_);
      current = generation == self->timer_generation_;
    }
    if (!error && current) {
      self->close_on_io(close_reason::transport_rejected);
    }
  });
}

void
broker_session::cancel_timer() {
  std::lock_guard timer_lock(timer_mutex_);
  ++timer_generation_;
  timer_.cancel();
}

void
broker_session::handle_handshake(const error_code &error) {
  if (closed_) {
    return;
  }
  cancel_timer();
  if (error) {
    close_on_io(close_reason::transport_rejected);
    return;
  }
  handshake_done_ = true;
  if (!owner_->config.client_certificate_uuid) {
    close_on_io(close_reason::transport_rejected);
    return;
  }
  try {
    broker_callback_scope callback_scope(owner_.get());
    cert_uuid_ = owner_->config.client_certificate_uuid(stream_.native_handle());
  }
  catch (const std::exception &exception) {
    (void)exception;
    cert_uuid_.clear();
  }
  if (cert_uuid_.empty()) {
    close_on_io(close_reason::transport_rejected);
    return;
  }
  begin_hello_read();
}

void
broker_session::begin_hello_read() {
  arm_timer(owner_->config.handshake_timeout_ms);
  auto self = shared_from_this();
  asio::async_read(stream_, asio::buffer(hello_wire_),
    [self](const error_code &error, std::size_t) { self->handle_hello(error); });
}

void
broker_session::handle_hello(const error_code &error) {
  if (closed_) {
    return;
  }
  cancel_timer();
  broker_hello decoded_hello;
  if (error || !decode_hello(hello_wire_, decoded_hello)) {
    close_on_io(close_reason::protocol_error);
    return;
  }
  if (owner_->config.capabilities == nullptr) {
    close_on_io(close_reason::transport_rejected);
    return;
  }
  const auto capability = owner_->config.capabilities->consume(
    cert_uuid_,
    decoded_hello.stream_generation,
    decoded_hello.session_token,
    decoded_hello.attachment_token,
    decoded_hello.lease_token,
    decoded_hello.capability_nonce);
  if (!capability || !capability_store::matches_wire_identity(
                         *capability, decoded_hello.wire_client_uuid)) {
    close_on_io(close_reason::transport_rejected);
    return;
  }
  if (owner_->config.authorize_client) {
    broker_callback_scope callback_scope(owner_.get());
    if (!owner_->config.authorize_client(cert_uuid_, decoded_hello, *capability)) {
      close_on_io(close_reason::transport_rejected);
      return;
    }
  }

  /* Publish the authenticated HELLO only after every validation step has
   * passed.  send_message() may run on a bridge worker thread, so its first
   * read of these fields must synchronize with this write. */
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    hello_ = decoded_hello;
    capability_ = *capability;
  }
  if (!initialize_adapter(hello_wire_)) {
    close_on_io(close_reason::internal_error);
    return;
  }

  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    hello_done_ = true;
    tx_sequence_ = 1;
  }
  /* The broker HELLO is an exact echo of the authenticated client config. */
  auto response = std::make_shared<std::vector<std::uint8_t>>(
    hello_wire_.begin(), hello_wire_.end());
  if (!enqueue_wire(std::move(response))) {
    close_on_io(close_reason::transport_rejected);
    return;
  }
  begin_frame_read();
}

bool
broker_session::initialize_adapter(const std::array<std::uint8_t, kHelloSize> & /*wire*/) {
  broker_hello hello;
  capability capability;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return false;
    }
    hello = hello_;
    capability = capability_;
  }

  session_binding binding;
  binding.client_uuid = hello.wire_client_uuid;
  binding.stream_generation = hello.stream_generation;
  binding.session_token = hello.session_token;
  binding.attachment_token = hello.attachment_token;
  binding.lease_token = hello.lease_token;
  binding.capability_nonce = hello.capability_nonce;
  binding.max_urb = std::min(hello.max_urb, capability.max_urb);
  binding.max_inflight = std::min(hello.max_inflight, capability.max_inflight);
  binding.isochronous = hello.isochronous;
  if (binding.max_urb == 0 || binding.max_inflight == 0) {
    return false;
  }

  auto weak = weak_from_this();
  broker_transport_callbacks callbacks;
  callbacks.send_frame = [weak](transport_message message) {
    if (auto self = weak.lock()) {
      return self->send_message(std::move(message));
    }
    return false;
  };
  callbacks.on_closed = [weak](close_reason reason) {
    if (auto self = weak.lock()) {
      self->post_close(reason);
    }
  };
  const auto endpoint_ready_handler = owner_->config.on_local_endpoint_ready;
  if (endpoint_ready_handler) {
    callbacks.on_local_endpoint_ready =
      [weak, endpoint_ready_handler](const endpoint &local_endpoint,
                                     const session_binding &session,
                                     local_endpoint_ready_completion completion) {
        auto self = weak.lock();
        if (!self) {
          /* The adapter owns the completion callback, so never invoke it
           * through a dead session.  The host integration is responsible for
           * cleaning up an attach that has no live peer. */
          return;
        }

        /* Keep the session alive until a retained host operation completes.
         * This makes the adapter callback target valid even when a client
         * disconnects while usbip-win2 is still in its attach ioctl. */
        const auto callback_owner = self->owner_.get();
        auto completion_holder =
          std::make_shared<local_endpoint_ready_completion>(std::move(completion));
        auto guarded_completion =
          [self = std::move(self), completion_holder, callback_owner](
            bool success, adapter_status status) mutable {
            if (!self->closed_.load(std::memory_order_acquire)) {
              broker_callback_scope callback_scope(callback_owner);
              (*completion_holder)(success, status);
            }
          };
        try {
          broker_callback_scope callback_scope(callback_owner);
          endpoint_ready_handler(local_endpoint, session,
                                 std::move(guarded_completion));
        } catch (...) {
          /* A host integration exception must fail the pending OPEN, but the
           * completion is still guarded by the session lifetime. */
          if (auto live = weak.lock(); live &&
              !live->closed_.load(std::memory_order_acquire)) {
            (*completion_holder)(false, adapter_status::bridge_failure);
          }
        }
      };
  }

  const auto session_closed_handler = owner_->config.on_session_closed;
  if (session_closed_handler) {
    callbacks.on_closed =
      [weak, session_closed_handler, binding, callback_owner = owner_.get()](close_reason reason) {
        try {
          broker_callback_scope callback_scope(callback_owner);
          session_closed_handler(binding, reason);
        } catch (...) {
          /* Cleanup callbacks are observational and must not unwind bridge
           * or TLS worker threads. */
        }
        if (auto self = weak.lock()) {
          self->post_close(reason);
        }
      };
  }

  error_code error;
  if (!adapter_.start(std::move(binding), bridge_, std::move(callbacks), error)) {
    (void)error;
    return false;
  }
  return true;
}

std::uint64_t
broker_session::wire_queue_limit() const noexcept {
  const auto logical_bytes = owner_->config.tx_window_bytes;
  const auto logical_pdus = static_cast<std::uint64_t>(std::max<std::uint32_t>(
    1, owner_->config.tx_window_pdus));

  /* The configuration is validated at server start, but keep this helper
   * overflow-safe because it is also used from teardown/error paths. */
  if (logical_pdus > (std::numeric_limits<std::uint64_t>::max() - logical_bytes) /
                      kFrameQueueOverhead) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return logical_bytes + logical_pdus * kFrameQueueOverhead + kFrameHeaderSize;
}

void
broker_session::begin_frame_read() {
  if (closed_) {
    return;
  }
  arm_timer(owner_->config.idle_timeout_ms);
  auto self = shared_from_this();
  asio::async_read(stream_, asio::buffer(frame_header_wire_),
    [self](const error_code &error, std::size_t) { self->handle_frame_header(error); });
}

void
broker_session::handle_frame_header(const error_code &error) {
  if (closed_) {
    return;
  }
  cancel_timer();
  frame_header header;
  std::uint64_t session_token = 0;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    session_token = hello_.session_token;
  }
  if (error || !decode_frame_header(frame_header_wire_, header,
                                    owner_->config.max_frame_payload) ||
      header.session_token != session_token ||
      (rx_sequence_ == std::numeric_limits<std::uint64_t>::max()) ||
      header.sequence != rx_sequence_ + 1) {
    close_on_io(error == asio::error::eof ? close_reason::peer_disconnected
                                          : close_reason::protocol_error);
    return;
  }
  try {
    payload_.assign(header.payload_length, 0);
  }
  catch (...) {
    close_on_io(close_reason::internal_error);
    return;
  }
  if (payload_.empty()) {
    rx_sequence_ = header.sequence;
    if (!process_frame(header, payload_)) {
      close_on_io(close_reason::protocol_error);
      return;
    }
    begin_frame_read();
    return;
  }
  arm_timer(owner_->config.idle_timeout_ms);
  auto self = shared_from_this();
  asio::async_read(stream_, asio::buffer(payload_),
    [self, header](const error_code &payload_error, std::size_t) {
      if (self->closed_) {
        return;
      }
      self->cancel_timer();
      if (payload_error || !valid_payload_shape(header, self->payload_)) {
        self->close_on_io(payload_error == asio::error::eof
                            ? close_reason::peer_disconnected
                            : close_reason::protocol_error);
        return;
      }
      self->rx_sequence_ = header.sequence;
      if (!self->process_frame(header, self->payload_)) {
        self->close_on_io(close_reason::protocol_error);
        return;
      }
      self->begin_frame_read();
    });
}

bool
broker_session::process_frame(const frame_header &header,
                              const std::vector<std::uint8_t> &payload) {
  if (!valid_payload_shape(header, payload)) {
    return false;
  }

  /* Keep the authenticated session metadata stable while adapter callbacks
   * run.  Do not hold mutex_ across those callbacks: request_open() can call
   * back into send_message(), which uses the same mutex to serialize TX. */
  broker_hello hello;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return false;
    }
    hello = hello_;
  }

  switch (header.type) {
    case 1: {
      device_info device;
      std::uint64_t lease_token = 0;
      std::uint64_t attachment_token = 0;
      if (!parse_device_capability(payload, device, lease_token, attachment_token) ||
          lease_token != hello.lease_token ||
          attachment_token != hello.attachment_token) {
        return false;
      }
      capability_event event;
      event.session_token = hello.session_token;
      event.lease_token = lease_token;
      event.attachment_token = attachment_token;
      event.device = std::move(device);
      return adapter_.accept_capability(event) && adapter_.request_open();
    }
    case 2: {
      if (read_u64_le(payload.data()) != hello.lease_token ||
          read_u64_le(payload.data() + 8) != hello.attachment_token) {
        return false;
      }
      open_event event;
      event.session_token = hello.session_token;
      event.lease_token = hello.lease_token;
      event.attachment_token = hello.attachment_token;
      return adapter_.accept_open(event);
    }
    case 3:
      return adapter_.accept_open_ok(hello.session_token);
    case 4:
      return adapter_.accept_open_reject(hello.session_token,
                                         read_u32_le(payload.data()));
    case 5: {
      const auto lease = read_u64_le(payload.data());
      const auto pdu_id = read_u64_le(payload.data() + 8);
      const auto total = read_u32_le(payload.data() + 16);
      const auto offset = read_u32_le(payload.data() + 20);
      const auto chunk = read_u32_le(payload.data() + 24);
      if (lease != hello.lease_token || read_u32_le(payload.data() + 28) != 0 ||
          pdu_id == 0 || total < broker_usbip_header_size ||
          total > std::min({ hello.max_urb,
                             owner_->config.max_reassembly_size,
                             kMaxReassembly }) ||
          chunk == 0 || chunk != payload.size() - kFragmentPrefixSize ||
          offset > total || chunk > total - offset) {
        return false;
      }
      const auto end = static_cast<std::uint64_t>(offset) + chunk;
      const bool more = (header.flags & kFlagMore) != 0;
      if ((more && end >= total) || (!more && end != total)) {
        return false;
      }
      if (!reassembly_active_) {
        if (offset != 0) {
          return false;
        }
        reassembly_active_ = true;
        reassembly_pdu_id_ = pdu_id;
        reassembly_total_ = total;
        reassembly_offset_ = 0;
        reassembly_fragments_ = 0;
        try {
          reassembly_buffer_.assign(total, 0);
        }
        catch (...) {
          return false;
        }
      }
      if (!reassembly_active_ || pdu_id != reassembly_pdu_id_ ||
          total != reassembly_total_ || offset != reassembly_offset_ ||
          reassembly_fragments_ >= std::min(owner_->config.max_fragments, kMaxFragments)) {
        return false;
      }
      std::copy_n(payload.data() + kFragmentPrefixSize, chunk,
                  reassembly_buffer_.data() + offset);
      reassembly_offset_ += chunk;
      ++reassembly_fragments_;
      if (more) {
        return true;
      }
      auto pdu = std::move(reassembly_buffer_);
      const auto completed_id = reassembly_pdu_id_;
      reassembly_active_ = false;
      reassembly_pdu_id_ = 0;
      reassembly_total_ = 0;
      reassembly_offset_ = 0;
      reassembly_fragments_ = 0;
      usbip_pdu_event event;
      event.session_token = hello.session_token;
      event.lease_token = hello.lease_token;
      event.pdu_id = completed_id;
      event.pdu = std::move(pdu);
      return adapter_.accept_usbip_pdu(event);
    }
    case 6:
      if (read_u64_le(payload.data()) != hello.lease_token) {
        return false;
      }
      return adapter_.accept_close(hello.session_token, hello.lease_token);
    default:
      return false;
  }
}

bool
broker_session::send_message(transport_message message) {
  std::lock_guard lock(mutex_);
  if (closed_ || !hello_done_) {
    return false;
  }
  if (message.session_token != 0 && message.session_token != hello_.session_token) {
    return false;
  }
  if (message.lease_token != 0 && message.lease_token != hello_.lease_token) {
    return false;
  }

  const bool is_usbip = message.kind == transport_message_kind::usbip_data;
  const auto logical_pdu_bytes = is_usbip ? message.pdu.size() : std::size_t { 0 };
  const auto logical_pdu_count = is_usbip ? std::uint32_t { 1 } : std::uint32_t { 0 };

  std::vector<std::shared_ptr<std::vector<std::uint8_t>>> frames;
  auto make_frame = [this](std::uint8_t type, std::uint32_t flags,
                           const std::vector<std::uint8_t> &payload) {
    if (payload.size() > std::min(owner_->config.max_frame_payload, kMaxPayload) ||
        tx_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      return std::shared_ptr<std::vector<std::uint8_t>> {};
    }
    std::array<std::uint8_t, kFrameHeaderSize> header;
    encode_frame_header(header, type, flags, static_cast<std::uint32_t>(payload.size()),
                        hello_.session_token, tx_sequence_++);
    auto wire = std::make_shared<std::vector<std::uint8_t>>();
    try {
      wire->reserve(kFrameHeaderSize + payload.size());
      wire->insert(wire->end(), header.begin(), header.end());
      wire->insert(wire->end(), payload.begin(), payload.end());
    }
    catch (...) {
      return std::shared_ptr<std::vector<std::uint8_t>> {};
    }
    return wire;
  };

  switch (message.kind) {
    case transport_message_kind::open: {
      std::vector<std::uint8_t> payload(kOpenPayloadSize, 0);
      write_u64_le(payload.data(), hello_.lease_token);
      write_u64_le(payload.data() + 8, hello_.attachment_token);
      auto frame = make_frame(2, 0, payload);
      if (!frame) return false;
      frames.push_back(std::move(frame));
      break;
    }
    case transport_message_kind::open_ok: {
      std::vector<std::uint8_t> payload;
      auto frame = make_frame(3, 0, payload);
      if (!frame) return false;
      frames.push_back(std::move(frame));
      break;
    }
    case transport_message_kind::open_reject: {
      if (message.status == 0 || message.status > kMaxOpenRejectStatus) return false;
      std::vector<std::uint8_t> payload(kOpenRejectPayloadSize, 0);
      write_u32_le(payload.data(), message.status);
      auto frame = make_frame(4, 0, payload);
      if (!frame) return false;
      frames.push_back(std::move(frame));
      break;
    }
    case transport_message_kind::close: {
      std::vector<std::uint8_t> payload(kClosePayloadSize, 0);
      write_u64_le(payload.data(), hello_.lease_token);
      auto frame = make_frame(6, 0, payload);
      if (!frame) return false;
      frames.push_back(std::move(frame));
      break;
    }
    case transport_message_kind::usbip_data: {
      if (message.pdu_id == 0 || message.pdu.empty() ||
          message.pdu.size() > std::min<std::size_t>(hello_.max_urb, broker_max_pdu_size) ||
          message.pdu.size() > kMaxReassembly) {
        return false;
      }
      const auto max_payload = std::min(owner_->config.max_frame_payload, kMaxPayload);
      if (max_payload <= kFragmentPrefixSize) {
        return false;
      }
      const auto max_chunk = static_cast<std::size_t>(max_payload - kFragmentPrefixSize);
      const auto fragment_count = (message.pdu.size() + max_chunk - 1) / max_chunk;
      if (fragment_count == 0 ||
          fragment_count > std::min(owner_->config.max_fragments, kMaxFragments)) {
        return false;
      }
      std::size_t offset = 0;
      while (offset < message.pdu.size()) {
        const auto chunk = std::min(max_chunk, message.pdu.size() - offset);
        const auto more = offset + chunk < message.pdu.size();
        std::vector<std::uint8_t> payload(kFragmentPrefixSize + chunk, 0);
        write_u64_le(payload.data(), hello_.lease_token);
        write_u64_le(payload.data() + 8, message.pdu_id);
        write_u32_le(payload.data() + 16, static_cast<std::uint32_t>(message.pdu.size()));
        write_u32_le(payload.data() + 20, static_cast<std::uint32_t>(offset));
        write_u32_le(payload.data() + 24, static_cast<std::uint32_t>(chunk));
        std::copy_n(message.pdu.data() + offset, chunk,
                    payload.data() + kFragmentPrefixSize);
        auto frame = make_frame(5, more ? kFlagMore : 0, payload);
        if (!frame) return false;
        frames.push_back(std::move(frame));
        offset += chunk;
      }
      break;
    }
    default:
      return false;
  }

  std::size_t added_bytes = 0;
  for (const auto &frame : frames) {
    if (!frame || added_bytes > std::numeric_limits<std::size_t>::max() - frame->size()) {
      return false;
    }
    added_bytes += frame->size();
  }
  const auto queue_limit = wire_queue_limit();
  if (added_bytes > queue_limit ||
      queued_bytes_ > queue_limit - added_bytes ||
      (logical_pdu_count != 0 &&
       (logical_pdu_bytes > owner_->config.tx_window_bytes ||
        queued_logical_pdu_bytes_ > owner_->config.tx_window_bytes - logical_pdu_bytes ||
        queued_logical_pdu_count_ >
          std::max<std::uint32_t>(1, owner_->config.tx_window_pdus) - logical_pdu_count)) ||
      closed_) {
    return false;
  }
  const auto was_empty = write_queue_.empty();
  for (std::size_t index = 0; index < frames.size(); ++index) {
    auto &frame = frames[index];
    queued_bytes_ += frame->size();
    queued_frame queued { std::move(frame) };
    if (index + 1 == frames.size()) {
      queued.logical_pdu_bytes = logical_pdu_bytes;
      queued.logical_pdu_count = logical_pdu_count;
    }
    write_queue_.push_back(std::move(queued));
  }
  queued_logical_pdu_bytes_ += logical_pdu_bytes;
  queued_logical_pdu_count_ += logical_pdu_count;
  if (was_empty && !write_in_progress_) {
    asio::post(owner_->io, [self = shared_from_this()]() { self->begin_write(); });
  }
  return true;
}

bool
broker_session::enqueue_wire(std::shared_ptr<std::vector<std::uint8_t>> wire) {
  if (!wire || wire->empty()) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    const auto queue_limit = wire_queue_limit();
    if (closed_ || wire->size() > queue_limit ||
        queued_bytes_ > queue_limit - wire->size()) {
      return false;
    }
    const auto was_empty = write_queue_.empty();
    queued_bytes_ += wire->size();
    write_queue_.push_back(queued_frame { std::move(wire) });
    if (was_empty && !write_in_progress_) {
      asio::post(owner_->io, [self = shared_from_this()]() { self->begin_write(); });
    }
  }
  return true;
}

void
broker_session::begin_write() {
  std::shared_ptr<std::vector<std::uint8_t>> wire;
  {
    std::lock_guard lock(mutex_);
    if (closed_ || write_in_progress_ || write_queue_.empty()) {
      return;
    }
    write_in_progress_ = true;
    wire = write_queue_.front().bytes;
  }
  auto self = shared_from_this();
  asio::async_write(stream_, asio::buffer(*wire),
    [self, wire](const error_code &error, std::size_t) {
      self->handle_write(error, wire);
    });
}

void
broker_session::handle_write(const error_code &error,
                             const std::shared_ptr<std::vector<std::uint8_t>> &wire) {
  {
    std::lock_guard lock(mutex_);
    write_in_progress_ = false;
    if (!write_queue_.empty() && write_queue_.front().bytes == wire) {
      const auto completed = write_queue_.front();
      queued_bytes_ -= wire->size();
      write_queue_.pop_front();
      if (completed.logical_pdu_bytes != 0) {
        /* A logical reservation is attached to the final fragment, so the
         * peer's window is released only after every fragment is on the wire. */
        if (queued_logical_pdu_bytes_ >= completed.logical_pdu_bytes) {
          queued_logical_pdu_bytes_ -= completed.logical_pdu_bytes;
        }
      }
      if (completed.logical_pdu_count != 0 &&
          queued_logical_pdu_count_ >= completed.logical_pdu_count) {
        queued_logical_pdu_count_ -= completed.logical_pdu_count;
      }
    }
  }
  if (error) {
    close_on_io(error == asio::error::eof ? close_reason::peer_disconnected
                                          : close_reason::transport_rejected);
    return;
  }
  begin_write();
}

void
broker_session::post_close(close_reason reason) {
  auto self = shared_from_this();
  if (owner_->post_session_close(self, reason)) {
    return;
  }
  /* A stopped worker cannot execute a posted handler.  If this callback is
   * already on the worker, or the worker has exited, complete teardown here;
   * an active worker will be closed by broker_server::stop(). */
  if (owner_->on_io_thread() || !owner_->worker_is_alive()) {
    close_on_io(reason);
  }
}

void
broker_session::close_on_io(close_reason reason) {
  const auto caller = std::this_thread::get_id();
  std::shared_ptr<broker_session> self;
  try {
    self = shared_from_this();
  }
  catch (...) {
    /* A destructor can be the last owner.  The close gate below still needs
     * to publish completion even when no shared self-reference is available. */
  }
  {
    std::unique_lock lock(mutex_);
    if (closed_) {
      /* `closed_` is published before the potentially lengthy adapter and
       * user-callback teardown below.  A second close request must not treat
       * that publication as completion: doing so lets the owning session (and
       * its nvhttp callback captures) be destroyed while the first teardown
       * is still running.  The teardown owner may re-enter close_on_io through
       * adapter/bridge callbacks, so it is the one exception to the wait. */
      if (!close_complete_ && close_owner_id_ != caller) {
        close_condition_.wait(lock, [this]() { return close_complete_; });
      }
      return;
    }
    closed_ = true;
    close_owner_id_ = caller;
    write_queue_.clear();
    queued_bytes_ = 0;
    queued_logical_pdu_bytes_ = 0;
    queued_logical_pdu_count_ = 0;
    write_in_progress_ = false;
  }
  /* Keep each teardown step exception-contained.  In particular, an Asio
   * timer/socket operation can throw when a platform service has already been
   * torn down; close_complete_ must still be published so waiters cannot be
   * stranded indefinitely. */
  try {
    cancel_timer();
  }
  catch (...) {
  }
  error_code ignored;
  try {
    stream_.lowest_layer().cancel(ignored);
  }
  catch (...) {
  }
  try {
    stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ignored);
  }
  catch (...) {
  }
  try {
    stream_.lowest_layer().close(ignored);
  }
  catch (...) {
  }
  try {
    adapter_.stop();
  }
  catch (...) {
    /* Teardown is best effort; never strand the close waiter or worker. */
  }
  (void)reason;
  if (self) {
    try {
      owner_->remove(self);
    }
    catch (...) {
      /* The owner may already be in its final destruction sweep.  The session
       * still publishes completion below; retaining the shared owner reference
       * is preferable to exposing an exception from a teardown callback. */
    }
  }
  {
    std::lock_guard lock(mutex_);
    close_complete_ = true;
    close_owner_id_ = {};
  }
  close_condition_.notify_all();
}

broker_server::broker_server(broker_server_config config):
    state_(std::make_shared<impl>(std::move(config))) {
}

broker_server::~broker_server() {
  stop();
}

broker_server_result
broker_server::start() {
  auto state = state_;
  if (!state) {
    return { false, "remote USB broker state is unavailable" };
  }

  std::lock_guard lifecycle_lock(state->lifecycle_mutex);
  if (state->running.load() || state->stopping.load()) {
    return { false, "remote USB broker is already started or stopped" };
  }
  auto &server = *state;

  /* A failed start must leave no accept operation or stopped io_context behind;
   * otherwise a subsequent start can observe stale handlers or an occupied
   * port.  Keep this cleanup local so every validation/bind failure uses it. */
  const auto fail_start = [&server](std::string message) {
    server.stopping.store(true);
    error_code ignored;
    server.acceptor.cancel(ignored);
    server.acceptor.close(ignored);
    server.bound_port.store(0);
    server.running.store(false);
    server.io.stop();
    server.work_guard.reset();
    server.io.restart();
    /* Dispatch cancellation handlers while stopping is still true so a stale
     * async_accept cannot schedule another accept if the caller retries start. */
    try {
      server.io.poll();
    }
    catch (...) {
      /* A failed start must remain non-throwing; the context is reset below. */
    }
    server.io.restart();
    {
      std::lock_guard lock(server.worker_state_mutex);
      server.worker_alive = false;
      server.worker_id = {};
      server.worker_finished = true;
      server.worker_detach_requested.store(false);
    }
    server.worker_condition.notify_all();
    server.stopping.store(false);
    return broker_server_result { false, std::move(message) };
  };

  if (server.config.capabilities == nullptr || server.config.certificate_file.empty() ||
      server.config.private_key_file.empty() || !server.config.client_certificate_uuid) {
    return fail_start("remote USB broker configuration is incomplete");
  }
  if (server.config.max_frame_payload <= kFragmentPrefixSize ||
      server.config.max_frame_payload > kMaxPayload ||
      server.config.max_reassembly_size < broker_usbip_header_size ||
      server.config.max_reassembly_size > kMaxReassembly || server.config.max_fragments == 0 ||
      server.config.max_fragments > kMaxFragments || server.config.tx_window_bytes == 0 ||
      server.config.tx_window_bytes > kMaxWindowBytes || server.config.tx_window_pdus == 0 ||
      server.config.tx_window_pdus > kMaxWindowPdus || server.config.rx_window_bytes == 0 ||
      server.config.rx_window_bytes > kMaxWindowBytes || server.config.rx_window_pdus == 0 ||
      server.config.rx_window_pdus > kMaxWindowPdus) {
    return fail_start("remote USB broker limits are invalid");
  }

  try {
    server.io.restart();
    server.work_guard = std::make_unique<broker_server::impl::work_guard_t>(
      asio::make_work_guard(server.io));
    server.bound_port.store(0);
    server.ssl_context.set_options(
      ssl::context::default_workarounds | ssl::context::no_sslv2 |
      ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
      ssl::context::single_dh_use);
    server.ssl_context.use_certificate_chain_file(server.config.certificate_file);
    server.ssl_context.use_private_key_file(server.config.private_key_file, ssl::context::pem);
    server.ssl_context.set_verify_mode(
      ssl::verify_peer | ssl::verify_fail_if_no_peer_cert | ssl::verify_client_once);
    /* Pairing verification is performed after the TLS handshake by the same
     * callback used by nvhttp.  The callback must therefore see the peer even
     * when the certificate is not anchored in OpenSSL's default CA store. */
    server.ssl_context.set_verify_callback([](bool, ssl::verify_context &) { return true; });

    error_code error;
    const auto address = asio::ip::make_address(server.config.bind_address, error);
    if (error) {
      return fail_start(error.message());
    }
    const tcp::endpoint endpoint { address, server.config.port };
    server.acceptor.open(endpoint.protocol(), error);
    if (error) return fail_start(error.message());
    server.acceptor.set_option(tcp::acceptor::reuse_address(true), error);
    if (error) return fail_start(error.message());
    server.acceptor.bind(endpoint, error);
    if (error) return fail_start(error.message());
    server.acceptor.listen(asio::socket_base::max_listen_connections, error);
    if (error) return fail_start(error.message());
    server.bound_port.store(server.acceptor.local_endpoint(error).port());
    if (error || server.bound_port.load() == 0) {
      return fail_start(error ? error.message() : "remote USB broker port is unavailable");
    }
    server.stopping.store(false);
    server.running.store(true);
    server.accept_next();
    {
      std::lock_guard lock(server.worker_state_mutex);
      server.worker_alive = true;
      server.worker_id = {};
      server.worker_finished = false;
    }
    server.worker_detach_requested.store(false);
    server.worker = std::thread([holder = state]() {
      {
        std::lock_guard lock(holder->worker_state_mutex);
        holder->worker_id = std::this_thread::get_id();
      }
      bool run_threw = false;
      try {
        holder->io.run();
      }
      catch (...) {
        run_threw = true;
      }

      /* `io.run()` can return because a handler threw before broker_server::stop
       * had a chance to run.  Keep the worker identity published while this
       * emergency sweep closes every session; this lets adapter/bridge
       * callbacks re-enter close_on_io without waiting on themselves. */
      holder->stopping.store(true);
      holder->running.store(false);
      error_code ignored;
      holder->acceptor.cancel(ignored);
      holder->acceptor.close(ignored);
      holder->close_sessions_noexcept(
        run_threw ? close_reason::internal_error : close_reason::stopped);
      holder->reset_work_guard();
      holder->io.stop();
      holder->bound_port.store(0);

      {
        std::lock_guard lock(holder->worker_state_mutex);
        holder->worker_alive = false;
        holder->worker_id = {};
        holder->worker_finished = true;
      }
      holder->worker_condition.notify_all();

      /* A stop requested from a callback running on this worker cannot join
       * itself.  Defer detach until all cleanup and the finished publication
       * are complete.  A concurrent external stop may have moved the thread
       * out for joining already; in that case `joinable()` is simply false. */
      if (holder->worker_detach_requested.load()) {
        std::lock_guard lifecycle_lock(holder->lifecycle_mutex);
        if (holder->worker.joinable() &&
            holder->worker.get_id() == std::this_thread::get_id()) {
          holder->worker.detach();
        }
      }
    });
    return { true, {} };
  }
  catch (const std::exception &exception) {
    {
      std::lock_guard lock(server.worker_state_mutex);
      server.worker_alive = false;
      server.worker_id = {};
      server.worker_finished = true;
      server.worker_detach_requested.store(false);
    }
    server.worker_condition.notify_all();
    return fail_start(exception.what());
  }
}

void
broker_server::stop() {
  auto state = state_;
  if (!state) {
    return;
  }

  /* Read the worker identity before taking the lifecycle lock.  This is
   * important when a bridge/user callback re-enters stop() while another
   * thread is already in its shutdown path: waiting for that thread from the
   * callback would form a cycle. */
  const bool self_worker = state->on_io_thread();
  const bool callback_thread = broker_callback_owner == state.get();
  bool first_stop = false;
  {
    std::lock_guard lifecycle_lock(state->lifecycle_mutex);
    first_stop = !state->stopping.exchange(true);
    if (first_stop) {
      state->running.store(false);
      error_code ignored;
      state->acceptor.cancel(ignored);
      state->acceptor.close(ignored);
    }
  }

  /* A callback running on a bridge thread must only publish the shutdown
   * request.  The broker worker is often blocked in bridge.stop() waiting for
   * this callback to return; synchronously closing sessions here would make
   * both threads wait on each other.  The worker's final sweep below performs
   * the actual teardown.  If the worker is already gone, direct cleanup is
   * safe and prevents a residual session from surviving forever. */
  if (callback_thread && !self_worker && state->worker_is_alive()) {
    state->worker_detach_requested.store(true);
    state->reset_work_guard();
    state->io.stop();
    return;
  }

  if (first_stop) {
    state->close_sessions_noexcept(close_reason::stopped);
    state->reset_work_guard();
    state->io.stop();
  }

  if (self_worker) {
    /* The worker cannot join itself.  Its exit lambda will detach after the
     * finished barrier is published, unless an external caller claims the
     * thread for joining first. */
    state->worker_detach_requested.store(true);
    return;
  }

  /* A callback on a dead worker reaches the branch above only when
   * worker_is_alive() is false; no Asio handler can touch the sessions now. */
  state->wait_worker_finished();

  std::thread join_target;
  {
    std::lock_guard lifecycle_lock(state->lifecycle_mutex);
    if (state->worker.joinable()) {
      if (state->worker.get_id() == std::this_thread::get_id()) {
        /* Defensive fallback for a worker whose alive marker was cleared just
         * before a re-entrant callback inspected it. */
        state->worker_detach_requested.store(true);
        state->worker.detach();
      } else {
        join_target = std::move(state->worker);
      }
    }
  }
  if (join_target.joinable()) {
    join_target.join();
  }

  /* An accept callback can pass its stopping check just before the flag is
   * observed and insert a session after the first snapshot.  At this point
   * the worker is quiescent, so this final sweep is race-free. */
  state->close_sessions_noexcept(close_reason::stopped);
  state->running.store(false);
  state->bound_port.store(0);
}

bool
broker_server::running() const noexcept {
  return state_ && state_->running.load();
}

std::uint16_t
broker_server::bound_port() const noexcept {
  return state_ ? state_->bound_port.load() : 0;
}

std::string
broker_server::bind_address() const {
  return state_ ? state_->config.bind_address : std::string {};
}

}  // namespace remote_usb
