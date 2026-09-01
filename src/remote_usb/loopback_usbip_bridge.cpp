/**
 * @file src/remote_usb/loopback_usbip_bridge.cpp
 * @brief Loopback USB/IP control and URB transport for one authenticated session.
 */

#include "loopback_usbip_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/errc.hpp>

namespace remote_usb {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr std::uint16_t kUsbipVersion = 0x0111;
constexpr std::uint16_t kOpRequestImport = 0x8003;
constexpr std::uint16_t kOpRequestDevlist = 0x8005;
constexpr std::uint16_t kOpReplyImport = 0x0003;
constexpr std::uint16_t kOpReplyDevlist = 0x0005;
constexpr std::uint32_t kOpStatusOk = 0;
constexpr std::uint32_t kOpStatusNodev = 4;

constexpr std::uint32_t kCmdSubmit = 1;
constexpr std::uint32_t kCmdUnlink = 2;
constexpr std::uint32_t kRetSubmit = 3;
constexpr std::uint32_t kRetUnlink = 4;

constexpr std::size_t kOpCommonSize = 8;
constexpr std::size_t kImportBodySize = 32;
constexpr std::size_t kDeviceSize = 312;
constexpr std::size_t kInterfaceSize = 4;
constexpr std::size_t kPduHeaderSize = 48;
constexpr std::size_t kBasicHeaderSize = 20;
constexpr std::size_t kSubmitTailSize = kPduHeaderSize - kBasicHeaderSize;
constexpr std::size_t kMaxTransferSize = (1024u * 1024u) - kPduHeaderSize;
constexpr std::size_t kMaxPduSize = kPduHeaderSize + kMaxTransferSize;
constexpr std::size_t kMaxQueuedReplies = 256;
constexpr std::size_t kMaxQueuedBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxInflight = 4096;
/* A cancelled submit may still race with a RET_SUBMIT on the broker path. */
constexpr std::size_t kMaxCancelledSubmits = kMaxInflight;

/*
 * The documented USB/IP wire value for a non-isochronous URB is -1.  Some
 * older peers (and the Linux kernel in a few paths) use zero instead, so the
 * bridge accepts both representations while continuing to reject a positive
 * packet count (isochronous transfers are deliberately unsupported here).
 */
constexpr std::int32_t kNonIsoNumberOfPackets = -1;

bool
is_non_iso_number_of_packets(std::int32_t value) noexcept {
  return value == kNonIsoNumberOfPackets || value == 0;
}

std::uint16_t
read_u16_be(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint16_t>(
    (static_cast<std::uint16_t>(bytes[0]) << 8) |
    static_cast<std::uint16_t>(bytes[1]));
}

std::uint16_t
read_u16_le(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint16_t>(
    static_cast<std::uint16_t>(bytes[0]) |
    (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t
read_u32_be(const std::uint8_t *bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::int32_t
read_i32_be(const std::uint8_t *bytes) noexcept {
  const auto value = read_u32_be(bytes);
  if ((value & 0x80000000u) == 0) {
    return static_cast<std::int32_t>(value);
  }
  if (value == 0x80000000u) {
    return std::numeric_limits<std::int32_t>::min();
  }
  return -static_cast<std::int32_t>(~value + 1u);
}

void
write_u16_be(std::uint8_t *bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 8);
  bytes[1] = static_cast<std::uint8_t>(value);
}

void
write_u32_be(std::uint8_t *bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 24);
  bytes[1] = static_cast<std::uint8_t>(value >> 16);
  bytes[2] = static_cast<std::uint8_t>(value >> 8);
  bytes[3] = static_cast<std::uint8_t>(value);
}

bool
all_zero(const std::uint8_t *bytes, std::size_t size) noexcept {
  return std::all_of(bytes, bytes + size, [](std::uint8_t value) { return value == 0; });
}

bool
valid_fixed_string(const std::string &value, std::size_t width, bool allow_empty) {
  if (value.size() >= width || value.find('\0') != std::string::npos) {
    return false;
  }
  return allow_empty || !value.empty();
}

void
copy_fixed_string(std::uint8_t *out, std::size_t width, const std::string &value) {
  std::memset(out, 0, width);
  const auto length = std::min(width - 1, value.size());
  std::memcpy(out, value.data(), length);
}

std::uint64_t
sequence_key(std::uint32_t devid, std::uint32_t seqnum) noexcept {
  return (static_cast<std::uint64_t>(devid) << 32) | seqnum;
}

boost::system::error_code
invalid_argument_error() {
  return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
}

boost::system::error_code
busy_error() {
  return boost::system::errc::make_error_code(boost::system::errc::device_or_resource_busy);
}

/* USB/IP has no authentication field.  Use an unguessable per-listener busid
 * so a local process that discovers the ephemeral port still cannot import the
 * device without the endpoint returned to the launching helper. */
std::string
make_opaque_busid() {
  constexpr char hex[] = "0123456789abcdef";
  static std::atomic<std::uint64_t> serial { 0 };
  std::random_device random;
  std::string value = "rusb-";
  value.reserve(29);
  for (std::size_t i = 0; i < 16; ++i) {
    value.push_back(hex[random() & 0x0fu]);
  }
  const auto suffix = serial.fetch_add(1, std::memory_order_relaxed);
  for (int shift = 28; shift >= 0; shift -= 4) {
    value.push_back(hex[(suffix >> shift) & 0x0fu]);
  }
  return value;
}

struct parsed_request {
  enum class kind {
    submit,
    unlink,
  };

  kind type { kind::submit };
  std::uint32_t seqnum { 0 };
  std::uint32_t devid { 0 };
  std::uint32_t direction { 0 };
  std::uint32_t endpoint { 0 };
  std::int32_t transfer_buffer_length { 0 };
  std::uint32_t unlink_seqnum { 0 };
};

struct request_meta {
  parsed_request::kind type { parsed_request::kind::submit };
  std::uint32_t devid { 0 };
  std::uint32_t direction { 0 };
  std::uint32_t endpoint { 0 };
  std::int32_t transfer_buffer_length { 0 };
  std::uint32_t unlink_seqnum { 0 };
};

bool
parse_request(const std::vector<std::uint8_t> &wire, parsed_request &out) {
  if (wire.size() < kPduHeaderSize || wire.size() > kMaxPduSize) {
    return false;
  }

  const auto command = read_u32_be(wire.data());
  out.seqnum = read_u32_be(wire.data() + 4);
  out.devid = read_u32_be(wire.data() + 8);
  out.direction = read_u32_be(wire.data() + 12);
  out.endpoint = read_u32_be(wire.data() + 16);
  if (out.seqnum == 0 || out.direction > 1 || out.endpoint > 15) {
    return false;
  }

  if (command == kCmdSubmit) {
    out.type = parsed_request::kind::submit;
    out.transfer_buffer_length = read_i32_be(wire.data() + 24);
    const auto start_frame = read_i32_be(wire.data() + 28);
    const auto number_of_packets = read_i32_be(wire.data() + 32);
    const auto interval = read_i32_be(wire.data() + 36);
    if (out.transfer_buffer_length < 0 ||
        static_cast<std::size_t>(out.transfer_buffer_length) > kMaxTransferSize ||
        start_frame != 0 || !is_non_iso_number_of_packets(number_of_packets) || interval < 0) {
      return false;
    }

    const auto transfer_length = static_cast<std::size_t>(out.transfer_buffer_length);
    if (out.direction == 0) {
      if (wire.size() != kPduHeaderSize + transfer_length) {
        return false;
      }
    } else if (wire.size() != kPduHeaderSize) {
      return false;
    }

    const auto *setup = wire.data() + 40;
    if (out.endpoint == 0) {
      if ((((setup[0] & 0x80u) != 0u) != (out.direction == 1)) ||
          read_u16_le(setup + 6) != static_cast<std::uint16_t>(transfer_length)) {
        return false;
      }
    } else if (!all_zero(setup, 8)) {
      return false;
    }
    return true;
  }

  if (command == kCmdUnlink) {
    out.type = parsed_request::kind::unlink;
    out.unlink_seqnum = read_u32_be(wire.data() + 20);
    /* USB/IP reserves direction and endpoint for UNLINK and requires zero. */
    if (out.direction != 0 || out.endpoint != 0 ||
        wire.size() != kPduHeaderSize || out.unlink_seqnum == 0 ||
        out.unlink_seqnum == out.seqnum ||
        !all_zero(wire.data() + 24, kPduHeaderSize - 24)) {
      return false;
    }
    return true;
  }

  return false;
}

/*
 * Linux specifies that a server response carries zeroes in the basic header
 * fields which identify the device, direction, and endpoint.  A few older
 * user-space servers echo those fields from CMD_SUBMIT/CMD_UNLINK instead.
 * Keep both forms interoperable, but never accept a partially mixed header.
 */
bool
reply_header_matches(const request_meta &request,
                     std::uint32_t devid,
                     std::uint32_t direction,
                     std::uint32_t endpoint,
                     bool canonical) noexcept {
  if (canonical) {
    return devid == 0 && direction == 0 && endpoint == 0;
  }
  return devid == request.devid && direction == request.direction && endpoint == request.endpoint;
}

bool
valid_ret_submit_wire(const std::vector<std::uint8_t> &wire,
                       const request_meta &request,
                       bool canonical) noexcept {
  if (wire.size() < kPduHeaderSize ||
      read_u32_be(wire.data()) != kRetSubmit ||
      !reply_header_matches(request,
                            read_u32_be(wire.data() + 8),
                            read_u32_be(wire.data() + 12),
                            read_u32_be(wire.data() + 16),
                            canonical)) {
    return false;
  }

  const auto actual_length = read_i32_be(wire.data() + 24);
  const auto start_frame = read_i32_be(wire.data() + 28);
  const auto number_of_packets = read_i32_be(wire.data() + 32);
  const auto error_count = read_i32_be(wire.data() + 36);
  if (actual_length < 0 || start_frame != 0 ||
      !is_non_iso_number_of_packets(number_of_packets) || error_count < 0 ||
      actual_length > request.transfer_buffer_length ||
      static_cast<std::size_t>(actual_length) > kMaxTransferSize ||
      !all_zero(wire.data() + 40, 8)) {
    return false;
  }

  const auto payload_size = request.direction == 1 ? static_cast<std::size_t>(actual_length) : 0;
  return wire.size() == kPduHeaderSize + payload_size;
}

bool
valid_ret_unlink_wire(const std::vector<std::uint8_t> &wire,
                      const request_meta &request,
                      bool canonical) noexcept {
  return wire.size() == kPduHeaderSize &&
         read_u32_be(wire.data()) == kRetUnlink &&
         reply_header_matches(request,
                              read_u32_be(wire.data() + 8),
                              read_u32_be(wire.data() + 12),
                              read_u32_be(wire.data() + 16),
                              canonical) &&
         all_zero(wire.data() + 24, kPduHeaderSize - 24);
}

}  // namespace

struct loopback_usbip_bridge::impl final : std::enable_shared_from_this<loopback_usbip_bridge::impl> {
  using packet_ptr = std::shared_ptr<std::vector<std::uint8_t>>;

  enum class phase {
    waiting_devlist,
    ready,
    imported,
  };

  impl():
      acceptor(io),
      socket(io) {
  }

  ~impl() {
    if (thread_is_joinable()) {
      /* The run lambda owns a shared_ptr while the I/O thread is active, so
       * reaching the implementation destructor means the run loop has
       * already returned (or thread creation failed).  Do not call
       * request_stop() here: shared_from_this() is no longer valid during the
       * final shared_ptr destruction. */
      reap_thread();
    }
  }

  bool
  thread_is_joinable() const {
    std::lock_guard lock(thread_mutex);
    return thread.joinable();
  }

  bool
  thread_finished() const {
    std::lock_guard lock(thread_mutex);
    return run_finished;
  }

  /*
   * Claim the std::thread exactly once.  A bridge close callback can call
   * loopback_usbip_bridge::stop() on the I/O thread while another caller is
   * already stopping it, so checking joinable() without serializing the
   * subsequent join/detach is not sufficient.  Moving a join target out of
   * the lock keeps the lock available to a self-stop while an external caller
   * waits for the I/O thread to finish.
   */
  void
  reap_thread() {
    std::thread join_target;
    std::unique_lock lifecycle_lock(thread_mutex);
    /* If another caller already claimed the thread, or a self-stop detached
     * it, an external caller still has to wait for io.run() to finish before
     * returning.  The I/O thread itself must never wait on this condition. */
    if (!thread.joinable()) {
      if (worker_id == std::this_thread::get_id() || run_finished) {
        return;
      }
      thread_condition.wait(lifecycle_lock, [this]() {
        return run_finished;
      });
      return;
    }

    if (thread.get_id() == std::this_thread::get_id()) {
      thread.detach();
      return;
    }
    join_target = std::move(thread);
    lifecycle_lock.unlock();
    join_target.join();
  }

  void
  run() noexcept {
    {
      std::lock_guard lock(thread_mutex);
      worker_id = std::this_thread::get_id();
    }
    try {
      io.run();
    } catch (...) {
      boost::system::error_code ignored;
      socket.close(ignored);
      acceptor.close(ignored);
      running_flag.store(false);
    }
    running_flag.store(false);
    {
      std::lock_guard lock(thread_mutex);
      worker_id = {};
      run_finished = true;
    }
    thread_condition.notify_all();
  }

  void
  request_stop() {
    bool expected = false;
    if (!stop_requested.compare_exchange_strong(expected, true)) {
      return;
    }
    running_flag.store(false);
    const auto self = shared_from_this();
    asio::post(io, [self]() { self->stop_on_io(); });
  }

  void
  stop_on_io() {
    running_flag.store(false);
    boost::system::error_code ignored;
    acceptor.cancel(ignored);
    acceptor.close(ignored);
    close_session(close_reason::stopped);
    io.stop();
  }

  void
  start_accept() {
    if (stop_requested.load() || !acceptor.is_open()) {
      return;
    }
    boost::system::error_code ignored;
    socket.close(ignored);
    const auto self = shared_from_this();
    acceptor.async_accept(socket, [self](const boost::system::error_code &error) {
      self->handle_accept(error);
    });
  }

  void
  handle_accept(const boost::system::error_code &error) {
    if (stop_requested.load()) {
      boost::system::error_code ignored;
      socket.close(ignored);
      return;
    }
    if (error) {
      if (error != asio::error::operation_aborted) {
        stop_on_io();
      }
      return;
    }

    boost::system::error_code endpoint_error;
    const auto peer = socket.remote_endpoint(endpoint_error);
    if (endpoint_error || !peer.address().is_loopback()) {
      boost::system::error_code ignored;
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
      start_accept();
      return;
    }

    boost::system::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
    std::uint64_t session_generation = 0;
    {
      std::lock_guard lock(mutex);
      ++generation;
      session_generation = generation;
      session_active = true;
      current_phase = phase::waiting_devlist;
      imported_callback_sent = false;
      close_when_drained = false;
      write_in_progress = false;
      queued_bytes = 0;
      write_queue.clear();
      outstanding.clear();
      cancelled_submits.clear();
    }
    begin_control_read(session_generation);
  }

  void
  begin_control_read(std::uint64_t session_generation) {
    if (!is_current_session(session_generation)) {
      return;
    }
    const auto self = shared_from_this();
    asio::async_read(
      socket,
      asio::buffer(control_header),
      [self, session_generation](const boost::system::error_code &error, std::size_t) {
        self->handle_control_header(session_generation, error);
      });
  }

  void
  handle_control_header(std::uint64_t session_generation,
                        const boost::system::error_code &error) {
    if (!is_current_session(session_generation)) {
      return;
    }
    if (error) {
      close_session(error == asio::error::eof ? close_reason::peer_disconnected : close_reason::internal_error);
      return;
    }

    const auto version = read_u16_be(control_header.data());
    const auto code = read_u16_be(control_header.data() + 2);
    const auto status = read_u32_be(control_header.data() + 4);
    if (version != kUsbipVersion || status != kOpStatusOk) {
      close_session(close_reason::protocol_error);
      return;
    }

    phase expected_phase;
    {
      std::lock_guard lock(mutex);
      expected_phase = current_phase;
    }
    if (code == kOpRequestDevlist && expected_phase == phase::waiting_devlist) {
      send_devlist_reply(session_generation);
      {
        std::lock_guard lock(mutex);
        if (session_active && generation == session_generation) {
          current_phase = phase::ready;
        }
      }
      begin_control_read(session_generation);
      return;
    }

    /* usbip-win2's UDE client imports the advertised bus directly and does
     * not issue OP_REQ_DEVLIST first.  Keep the traditional DEVLIST ->
     * IMPORT sequence, while allowing IMPORT as the first control request. */
    if (code == kOpRequestImport &&
        (expected_phase == phase::waiting_devlist || expected_phase == phase::ready)) {
      const auto self = shared_from_this();
      asio::async_read(
        socket,
        asio::buffer(import_body),
        [self, session_generation](const boost::system::error_code &read_error, std::size_t) {
          self->handle_import_body(session_generation, read_error);
        });
      return;
    }

    close_session(close_reason::protocol_error);
  }

  void
  handle_import_body(std::uint64_t session_generation,
                     const boost::system::error_code &error) {
    if (!is_current_session(session_generation)) {
      return;
    }
    if (error) {
      close_session(error == asio::error::eof ? close_reason::peer_disconnected : close_reason::internal_error);
      return;
    }

    std::string requested_busid;
    const auto *bytes = import_body.data();
    const auto terminator = std::find(bytes, bytes + import_body.size(), static_cast<std::uint8_t>(0));
    if (terminator == bytes || terminator == bytes + import_body.size() ||
        !all_zero(terminator + 1, static_cast<std::size_t>(bytes + import_body.size() - terminator - 1))) {
      close_session(close_reason::protocol_error);
      return;
    }
    requested_busid.assign(reinterpret_cast<const char *>(bytes),
                           static_cast<std::size_t>(terminator - bytes));

    if (requested_busid != device.busid) {
      send_import_reply(kOpStatusNodev, session_generation);
      {
        std::lock_guard lock(mutex);
        if (session_active && generation == session_generation) {
          close_when_drained = true;
        }
      }
      start_write(session_generation);
      return;
    }

    send_import_reply(kOpStatusOk, session_generation);
    {
      std::lock_guard lock(mutex);
      if (!session_active || generation != session_generation) {
        return;
      }
      current_phase = phase::imported;
      if (!imported_callback_sent) {
        imported_callback_sent = true;
      }
    }

    std::function<void()> imported_callback;
    {
      std::lock_guard lock(mutex);
      imported_callback = callback_set.on_imported;
    }
    if (imported_callback) {
      try {
        imported_callback();
      } catch (...) {
        close_session(close_reason::internal_error);
        return;
      }
    }
    begin_data_read(session_generation);
  }

  void
  begin_data_read(std::uint64_t session_generation) {
    if (!is_current_session(session_generation)) {
      return;
    }
    const auto self = shared_from_this();
    asio::async_read(
      socket,
      asio::buffer(data_header),
      [self, session_generation](const boost::system::error_code &error, std::size_t) {
        self->handle_data_header(session_generation, error);
      });
  }

  void
  handle_data_header(std::uint64_t session_generation,
                     const boost::system::error_code &error) {
    if (!is_current_session(session_generation)) {
      return;
    }
    if (error) {
      close_session(error == asio::error::eof ? close_reason::peer_disconnected : close_reason::internal_error);
      return;
    }

    const auto command = read_u32_be(data_header.data());
    if (command != kCmdSubmit && command != kCmdUnlink) {
      close_session(close_reason::protocol_error);
      return;
    }

    try {
      pending_pdu.assign(data_header.begin(), data_header.end());
      pending_pdu.resize(kPduHeaderSize);
    } catch (...) {
      close_session(close_reason::internal_error);
      return;
    }
    const auto self = shared_from_this();
    asio::async_read(
      socket,
      asio::buffer(pending_pdu.data() + kBasicHeaderSize, kSubmitTailSize),
      [self, session_generation, command](const boost::system::error_code &read_error, std::size_t) {
        self->handle_data_tail(session_generation, command, read_error);
      });
  }

  void
  handle_data_tail(std::uint64_t session_generation,
                   std::uint32_t command,
                   const boost::system::error_code &error) {
    if (!is_current_session(session_generation)) {
      return;
    }
    if (error) {
      close_session(error == asio::error::eof ? close_reason::peer_disconnected : close_reason::internal_error);
      return;
    }

    if (command == kCmdSubmit) {
      const auto direction = read_u32_be(pending_pdu.data() + 12);
      const auto length = read_i32_be(pending_pdu.data() + 24);
      if (direction > 1 || length < 0 || static_cast<std::size_t>(length) > kMaxTransferSize) {
        close_session(close_reason::protocol_error);
        return;
      }
      if (direction == 0 && length != 0) {
        try {
          pending_pdu.resize(kPduHeaderSize + static_cast<std::size_t>(length));
        } catch (...) {
          close_session(close_reason::internal_error);
          return;
        }
        const auto self = shared_from_this();
        asio::async_read(
          socket,
          asio::buffer(pending_pdu.data() + kPduHeaderSize, static_cast<std::size_t>(length)),
          [self, session_generation](const boost::system::error_code &read_error, std::size_t) {
            self->handle_complete_pdu(session_generation, read_error);
          });
        return;
      }
    }
    handle_complete_pdu(session_generation, {});
  }

  void
  handle_complete_pdu(std::uint64_t session_generation,
                      const boost::system::error_code &error) {
    if (!is_current_session(session_generation)) {
      return;
    }
    if (error) {
      close_session(error == asio::error::eof ? close_reason::peer_disconnected : close_reason::internal_error);
      return;
    }

    parsed_request request;
    if (!parse_request(pending_pdu, request)) {
      close_session(close_reason::protocol_error);
      return;
    }

    request_meta meta;
    meta.type = request.type;
    meta.devid = request.devid;
    meta.direction = request.direction;
    meta.endpoint = request.endpoint;
    meta.transfer_buffer_length = request.transfer_buffer_length;
    meta.unlink_seqnum = request.unlink_seqnum;
    bool reject_request = false;
    close_reason reject_reason = close_reason::queue_limit;
    {
      std::lock_guard lock(mutex);
      if (!session_active || generation != session_generation || current_phase != phase::imported) {
        return;
      }
      const auto request_key = sequence_key(request.devid, request.seqnum);
      bool sequence_in_use = outstanding.find(request_key) != outstanding.end() ||
                             cancelled_submits.find(request.seqnum) != cancelled_submits.end();
      /* seqnum is connection-scoped; do not permit the same number under a
       * different devid either.  This also makes canonical server replies
       * unambiguous when their devid field is zero. */
      if (!sequence_in_use) {
        for (const auto &entry : outstanding) {
          if (static_cast<std::uint32_t>(entry.first) == request.seqnum) {
            sequence_in_use = true;
            break;
          }
        }
      }
      if (outstanding.size() >= kMaxInflight || sequence_in_use) {
        reject_request = true;
        if (sequence_in_use) {
          reject_reason = close_reason::protocol_error;
        }
      } else {
        outstanding.emplace(request_key, meta);
      }
    }
    if (reject_request) {
      close_session(reject_reason);
      return;
    }

    std::function<bool(std::vector<std::uint8_t>)> request_callback;
    {
      std::lock_guard lock(mutex);
      request_callback = callback_set.on_request;
    }
    bool accepted = false;
    try {
      accepted = request_callback(std::move(pending_pdu));
    } catch (...) {
      close_session(close_reason::internal_error);
      return;
    }
    if (!accepted) {
      close_session(close_reason::transport_rejected);
      return;
    }
    begin_data_read(session_generation);
  }

  void
  send_devlist_reply(std::uint64_t session_generation) {
    const auto interface_count = device.interfaces.size();
    /*
     * USB/IP clients only need an opaque device path for the control reply.
     * Never forward the platform path supplied by the remote capability
     * (for example /sys/... or a Windows device node) to a local process.
     * The busid is stable for this bridge and is sufficient for usbip-win2.
     */
    const auto &path = device.busid;
    auto wire = std::make_shared<std::vector<std::uint8_t>>(
      kOpCommonSize + 4 + kDeviceSize + interface_count * kInterfaceSize, 0);
    write_u16_be(wire->data(), kUsbipVersion);
    write_u16_be(wire->data() + 2, kOpReplyDevlist);
    write_u32_be(wire->data() + 4, kOpStatusOk);
    write_u32_be(wire->data() + kOpCommonSize, 1);
    encode_device(wire->data() + kOpCommonSize + 4, path);
    auto offset = kOpCommonSize + 4 + kDeviceSize;
    for (const auto &interface_value : device.interfaces) {
      (*wire)[offset++] = interface_value.interface_class;
      (*wire)[offset++] = interface_value.interface_subclass;
      (*wire)[offset++] = interface_value.interface_protocol;
      (*wire)[offset++] = 0;
    }
    enqueue_packet(std::move(wire), session_generation);
  }

  void
  send_import_reply(std::uint32_t status, std::uint64_t session_generation) {
    const auto &path = device.busid;
    const auto size = status == kOpStatusOk ? kOpCommonSize + kDeviceSize : kOpCommonSize;
    auto wire = std::make_shared<std::vector<std::uint8_t>>(size, 0);
    write_u16_be(wire->data(), kUsbipVersion);
    write_u16_be(wire->data() + 2, kOpReplyImport);
    write_u32_be(wire->data() + 4, status);
    if (status == kOpStatusOk) {
      encode_device(wire->data() + kOpCommonSize, path);
    }
    enqueue_packet(std::move(wire), session_generation);
  }

  void
  encode_device(std::uint8_t *out, const std::string &path) const {
    std::memset(out, 0, kDeviceSize);
    copy_fixed_string(out, 256, path);
    copy_fixed_string(out + 256, 32, device.busid);
    write_u32_be(out + 288, device.busnum);
    write_u32_be(out + 292, device.devnum);
    write_u32_be(out + 296, device.speed);
    write_u16_be(out + 300, device.vendor_id);
    write_u16_be(out + 302, device.product_id);
    write_u16_be(out + 304, device.device_bcd);
    out[306] = device.device_class;
    out[307] = device.device_subclass;
    out[308] = device.device_protocol;
    out[309] = device.configuration_value;
    out[310] = device.num_configurations;
    out[311] = static_cast<std::uint8_t>(device.interfaces.size());
  }

  bool
  send_reply(std::vector<std::uint8_t> wire) {
    if (wire.size() < kPduHeaderSize || wire.size() > kMaxPduSize) {
      schedule_close(close_reason::protocol_error);
      return false;
    }

    const auto packet = std::make_shared<std::vector<std::uint8_t>>(std::move(wire));
    const auto command = read_u32_be(packet->data());
    const auto seqnum = read_u32_be(packet->data() + 4);
    const auto devid = read_u32_be(packet->data() + 8);
    const auto direction = read_u32_be(packet->data() + 12);
    const auto endpoint = read_u32_be(packet->data() + 16);
    const bool canonical_header = devid == 0 && direction == 0 && endpoint == 0;
    bool malformed = false;
    bool dropped_cancelled_reply = false;
    close_reason malformed_reason = close_reason::protocol_error;
    std::uint64_t session_generation = 0;
    {
      std::lock_guard lock(mutex);
      session_generation = generation;
      if (!session_active || current_phase != phase::imported || seqnum == 0 ||
          (!canonical_header && (direction > 1 || endpoint > 15))) {
        return false;
      }

      auto found = outstanding.find(sequence_key(devid, seqnum));
      /* Canonical server replies use devid=0, so fall back to the
       * connection-scoped seqnum when the exact (devid, seqnum) key is absent. */
      if (found == outstanding.end() && canonical_header) {
        for (auto candidate = outstanding.begin(); candidate != outstanding.end(); ++candidate) {
          if (static_cast<std::uint32_t>(candidate->first) == seqnum) {
            if (found != outstanding.end()) {
              /* A duplicate seqnum would make a canonical reply ambiguous. */
              malformed = true;
              break;
            }
            found = candidate;
          }
        }
      }

      if (found == outstanding.end()) {
        if (command == kRetSubmit) {
          /* usbip-win2 deliberately ignores a RET_SUBMIT which races with a
           * successful unlink after the client-side request was completed. */
          const auto cancelled = cancelled_submits.find(seqnum);
          if (cancelled == cancelled_submits.end()) {
            malformed = true;
          } else if (!valid_ret_submit_wire(*packet, cancelled->second, canonical_header)) {
            malformed = true;
          } else {
            cancelled_submits.erase(cancelled);
            dropped_cancelled_reply = true;
          }
        } else {
          malformed = true;
        }
      } else {
        const auto &request = found->second;
        if (command == kRetSubmit && request.type == parsed_request::kind::submit) {
          if (!valid_ret_submit_wire(*packet, request, canonical_header)) {
            malformed = true;
          } else {
            outstanding.erase(found);
          }
        } else if (command == kRetUnlink && request.type == parsed_request::kind::unlink) {
          if (!valid_ret_unlink_wire(*packet, request, canonical_header)) {
            malformed = true;
          } else {
            /* RET_UNLINK completes both the unlink command and its target.
             * Keep a bounded copy of the target metadata so a racing
             * RET_SUBMIT can be validated and ignored instead of poisoning
             * the session with an unknown-sequence protocol error. */
            const auto target_key = sequence_key(request.devid, request.unlink_seqnum);
            const auto target = outstanding.find(target_key);
            if (target != outstanding.end() && target->second.type == parsed_request::kind::submit) {
              if (cancelled_submits.size() >= kMaxCancelledSubmits) {
                cancelled_submits.clear();
              }
              cancelled_submits[request.unlink_seqnum] = target->second;
              outstanding.erase(target);
            }
            outstanding.erase(found);
          }
        } else {
          malformed = true;
        }
      }

      if (!malformed && !dropped_cancelled_reply && (write_queue.size() >= kMaxQueuedReplies ||
                         queued_bytes > kMaxQueuedBytes ||
                         packet->size() > kMaxQueuedBytes - queued_bytes)) {
        malformed = true;
        malformed_reason = close_reason::queue_limit;
      }
      if (!malformed && !dropped_cancelled_reply) {
        queued_bytes += packet->size();
        write_queue.push_back(packet);
      }
    }

    if (malformed) {
      schedule_close(malformed_reason);
      return false;
    }
    if (dropped_cancelled_reply) {
      return true;
    }
    asio::post(io, [self = shared_from_this(), session_generation]() {
      self->start_write(session_generation);
    });
    return true;
  }

  void
  enqueue_packet(packet_ptr packet, std::uint64_t session_generation) {
    if (!packet) {
      schedule_close(close_reason::internal_error);
      return;
    }
    bool accepted = false;
    {
      std::lock_guard lock(mutex);
      if (session_active && generation == session_generation &&
          write_queue.size() < kMaxQueuedReplies &&
          queued_bytes <= kMaxQueuedBytes &&
          packet->size() <= kMaxQueuedBytes - queued_bytes) {
        queued_bytes += packet->size();
        write_queue.push_back(std::move(packet));
        accepted = true;
      }
    }
    if (!accepted) {
      close_session(close_reason::queue_limit);
      return;
    }
    start_write(session_generation);
  }

  void
  start_write(std::uint64_t session_generation) {
    if (!is_current_session(session_generation)) {
      return;
    }
    packet_ptr packet;
    {
      std::lock_guard lock(mutex);
      if (!session_active || generation != session_generation || write_in_progress || write_queue.empty()) {
        return;
      }
      write_in_progress = true;
      packet = write_queue.front();
    }
    const auto self = shared_from_this();
    asio::async_write(
      socket,
      asio::buffer(*packet),
      [self, session_generation, packet](const boost::system::error_code &error, std::size_t) {
        self->handle_write(session_generation, packet, error);
      });
  }

  void
  handle_write(std::uint64_t session_generation,
               const packet_ptr &packet,
               const boost::system::error_code &error) {
    if (!is_current_session(session_generation)) {
      return;
    }
    bool drained = false;
    {
      std::lock_guard lock(mutex);
      write_in_progress = false;
      if (!write_queue.empty() && write_queue.front() == packet) {
        queued_bytes -= packet->size();
        write_queue.pop_front();
      }
      drained = write_queue.empty() && close_when_drained;
    }
    if (error) {
      close_session(error == asio::error::eof ? close_reason::peer_disconnected : close_reason::internal_error);
      return;
    }
    if (drained) {
      close_session(close_reason::transport_rejected);
      return;
    }
    start_write(session_generation);
  }

  void
  schedule_close(close_reason reason) {
    const auto self = shared_from_this();
    asio::post(io, [self, reason]() { self->close_session(reason); });
  }

  bool
  is_current_session(std::uint64_t session_generation) const {
    std::lock_guard lock(mutex);
    return session_active && generation == session_generation;
  }

  void
  close_session(close_reason reason) {
    std::function<void(close_reason)> closed_callback;
    bool notify = false;
    {
      std::lock_guard lock(mutex);
      if (!session_active) {
        return;
      }
      session_active = false;
      ++generation;
      current_phase = phase::waiting_devlist;
      outstanding.clear();
      cancelled_submits.clear();
      write_queue.clear();
      queued_bytes = 0;
      write_in_progress = false;
      close_when_drained = false;
      closed_callback = callback_set.on_closed;
      notify = true;
    }

    boost::system::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);

    if (notify && closed_callback) {
      try {
        closed_callback(reason);
      } catch (...) {
        // A user callback must not terminate the I/O thread.
      }
    }
    if (!stop_requested.load()) {
      start_accept();
    }
  }

  mutable std::mutex mutex;
  asio::io_context io;
  tcp::acceptor acceptor;
  tcp::socket socket;
  std::thread thread;
  mutable std::mutex thread_mutex;
  std::condition_variable thread_condition;
  std::thread::id worker_id;
  bool run_finished { false };

  device_info device;
  callbacks callback_set;
  endpoint endpoint_info;

  std::atomic_bool running_flag { false };
  std::atomic_bool stop_requested { false };
  bool session_active { false };
  bool imported_callback_sent { false };
  bool close_when_drained { false };
  bool write_in_progress { false };
  phase current_phase { phase::waiting_devlist };
  std::uint64_t generation { 0 };
  std::size_t queued_bytes { 0 };
  std::deque<packet_ptr> write_queue;
  std::unordered_map<std::uint64_t, request_meta> outstanding;
  std::unordered_map<std::uint32_t, request_meta> cancelled_submits;

  std::array<std::uint8_t, 8> control_header {};
  std::array<std::uint8_t, kImportBodySize> import_body {};
  std::array<std::uint8_t, kBasicHeaderSize> data_header {};
  std::vector<std::uint8_t> pending_pdu;
};

loopback_usbip_bridge::loopback_usbip_bridge() = default;

loopback_usbip_bridge::~loopback_usbip_bridge() {
  stop();
}

std::optional<endpoint>
loopback_usbip_bridge::start(device_info device,
                             callbacks callback_set,
                             boost::system::error_code &error) {
  error.clear();
  std::lock_guard lock(mutex_);
  if (state_) {
    /* A self-stop may have detached the I/O thread before its run loop
     * returned.  Retain that state until completion so a concurrent external
     * stop can still wait on it; only then is it safe to start a fresh bridge. */
    if (state_->thread_finished()) {
      state_.reset();
    } else {
      error = busy_error();
      return std::nullopt;
    }
  }
  if (!valid_fixed_string(device.busid, 32, false) ||
      (!device.path.empty() && !valid_fixed_string(device.path, 256, false)) ||
      device.interfaces.size() > 255 || !callback_set.on_request) {
    error = invalid_argument_error();
    return std::nullopt;
  }

  auto state = std::make_shared<impl>();
  state->device = std::move(device);
  try {
    state->device.busid = make_opaque_busid();
  } catch (...) {
    error = boost::system::errc::make_error_code(
      boost::system::errc::resource_unavailable_try_again);
    return std::nullopt;
  }
  state->callback_set = std::move(callback_set);
  state->endpoint_info.busid = state->device.busid;

  boost::system::error_code open_error;
  state->acceptor.open(tcp::v4(), open_error);
  if (open_error) {
    error = open_error;
    return std::nullopt;
  }
  state->acceptor.bind(tcp::endpoint(asio::ip::address_v4::loopback(), 0), open_error);
  if (open_error) {
    state->acceptor.close(open_error);
    error = open_error;
    return std::nullopt;
  }
  state->acceptor.listen(1, open_error);
  if (open_error) {
    state->acceptor.close(open_error);
    error = open_error;
    return std::nullopt;
  }
  const auto local = state->acceptor.local_endpoint(open_error);
  if (open_error || local.port() == 0) {
    state->acceptor.close(open_error);
    error = open_error ? open_error : invalid_argument_error();
    return std::nullopt;
  }
  state->endpoint_info.port = local.port();
  state->running_flag.store(true);
  state_ = state;

  try {
    state->start_accept();
    state->thread = std::thread([state]() { state->run(); });
  } catch (...) {
    state->running_flag.store(false);
    state->stop_requested.store(true);
    boost::system::error_code ignored;
    state->acceptor.close(ignored);
    state_.reset();
    error = boost::system::errc::make_error_code(boost::system::errc::resource_unavailable_try_again);
    return std::nullopt;
  }
  return state->endpoint_info;
}

void
loopback_usbip_bridge::stop() {
  std::shared_ptr<impl> state;
  {
    std::lock_guard lock(mutex_);
    state = state_;
  }
  if (!state) {
    return;
  }
  state->request_stop();
  state->reap_thread();
  /* Keep a self-detached state reachable until io.run() has completed.  This
   * lets a later external stop() observe it and wait for callbacks in flight. */
  if (state->thread_finished()) {
    std::lock_guard lock(mutex_);
    if (state_ == state) {
      state_.reset();
    }
  }
}

bool
loopback_usbip_bridge::send_reply(std::vector<std::uint8_t> wire) {
  std::shared_ptr<impl> state;
  {
    std::lock_guard lock(mutex_);
    state = state_;
  }
  return state && state->send_reply(std::move(wire));
}

bool
loopback_usbip_bridge::running() const noexcept {
  std::lock_guard lock(mutex_);
  return state_ && state_->running_flag.load();
}

std::optional<endpoint>
loopback_usbip_bridge::local_endpoint() const {
  std::lock_guard lock(mutex_);
  if (!state_ || !state_->running_flag.load()) {
    return std::nullopt;
  }
  return state_->endpoint_info;
}

}  // namespace remote_usb
