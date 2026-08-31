/**
 * @file tests/unit/test_remote_usb_broker_adapter.cpp
 * @brief End-to-end glue tests for the typed broker adapter.
 */

#include <src/remote_usb/remote_usb_broker_adapter.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>

namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

void
put_u16_le(std::uint8_t *out, std::uint16_t value) {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8);
}

void
put_u32_be(std::uint8_t *out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value >> 24);
  out[1] = static_cast<std::uint8_t>(value >> 16);
  out[2] = static_cast<std::uint8_t>(value >> 8);
  out[3] = static_cast<std::uint8_t>(value);
}

void
put_i32_be(std::uint8_t *out, std::int32_t value) {
  put_u32_be(out, static_cast<std::uint32_t>(value));
}

std::uint32_t
get_u32_be(const std::uint8_t *in) {
  return (static_cast<std::uint32_t>(in[0]) << 24) |
         (static_cast<std::uint32_t>(in[1]) << 16) |
         (static_cast<std::uint32_t>(in[2]) << 8) |
         static_cast<std::uint32_t>(in[3]);
}

std::vector<std::uint8_t>
make_control(std::uint16_t code) {
  std::vector<std::uint8_t> wire(8, 0);
  wire[0] = 0x01;
  wire[1] = 0x11;
  wire[2] = static_cast<std::uint8_t>(code >> 8);
  wire[3] = static_cast<std::uint8_t>(code);
  return wire;
}

std::vector<std::uint8_t>
make_import(std::string_view busid) {
  auto wire = make_control(0x8003);
  wire.resize(40, 0);
  std::copy(busid.begin(), busid.end(), wire.begin() + 8);
  return wire;
}

std::vector<std::uint8_t>
make_submit(std::uint32_t seqnum,
            std::uint32_t devid,
            std::uint32_t direction,
            std::uint32_t endpoint,
            std::int32_t transfer_length,
            std::vector<std::uint8_t> data = {}) {
  const auto payload_size = direction == 0 ? data.size() : std::size_t { 0 };
  std::vector<std::uint8_t> wire(48 + payload_size, 0);
  put_u32_be(wire.data(), 1);
  put_u32_be(wire.data() + 4, seqnum);
  put_u32_be(wire.data() + 8, devid);
  put_u32_be(wire.data() + 12, direction);
  put_u32_be(wire.data() + 16, endpoint);
  put_i32_be(wire.data() + 24, transfer_length);
  if (endpoint == 0) {
    wire[40] = direction == 1 ? 0x80 : 0;
    put_u16_le(wire.data() + 46, static_cast<std::uint16_t>(transfer_length));
  }
  if (!data.empty()) {
    std::copy(data.begin(), data.end(), wire.begin() + 48);
  }
  return wire;
}

std::vector<std::uint8_t>
make_reply(std::uint32_t seqnum,
           std::uint32_t devid,
           std::uint32_t direction,
           std::uint32_t endpoint) {
  std::vector<std::uint8_t> wire(48, 0);
  put_u32_be(wire.data(), 3);
  put_u32_be(wire.data() + 4, seqnum);
  put_u32_be(wire.data() + 8, devid);
  put_u32_be(wire.data() + 12, direction);
  put_u32_be(wire.data() + 16, endpoint);
  return wire;
}

std::vector<std::uint8_t>
make_unlink(std::uint32_t seqnum,
            std::uint32_t target_seqnum,
            std::uint32_t devid = 0x00010002) {
  std::vector<std::uint8_t> wire(48, 0);
  put_u32_be(wire.data(), 2);
  put_u32_be(wire.data() + 4, seqnum);
  put_u32_be(wire.data() + 8, devid);
  put_u32_be(wire.data() + 20, target_seqnum);
  return wire;
}

std::vector<std::uint8_t>
make_unlink_reply(std::uint32_t seqnum) {
  std::vector<std::uint8_t> wire(48, 0);
  put_u32_be(wire.data(), 4);
  put_u32_be(wire.data() + 4, seqnum);
  return wire;
}

bool
write_all(tcp::socket &socket, const std::vector<std::uint8_t> &wire) {
  boost::system::error_code error;
  asio::write(socket, asio::buffer(wire), error);
  return !error;
}

bool
read_exact(tcp::socket &socket, std::vector<std::uint8_t> &wire) {
  boost::system::error_code error;
  asio::read(socket, asio::buffer(wire), error);
  return !error;
}

bool
perform_import(tcp::socket &socket) {
  if (!write_all(socket, make_control(0x8005))) {
    return false;
  }
  std::vector<std::uint8_t> devlist(8 + 4 + 312 + 8);
  if (!read_exact(socket, devlist) || get_u32_be(devlist.data() + 4) != 0 ||
      get_u32_be(devlist.data() + 8) != 1) {
    return false;
  }
  const std::string busid(reinterpret_cast<const char *>(devlist.data() + 12));
  if (busid.empty() || busid.size() > 31 || !write_all(socket, make_import(busid))) {
    return false;
  }
  std::vector<std::uint8_t> imported(8 + 312);
  if (!read_exact(socket, imported) || get_u32_be(imported.data() + 4) != 0) {
    return false;
  }
  return std::string(reinterpret_cast<const char *>(imported.data() + 8)) == busid;
}

remote_usb::session_binding
test_binding() {
  remote_usb::session_binding binding;
  binding.client_uuid.fill(0x11);
  binding.capability_nonce.fill(0x22);
  binding.stream_generation = 3;
  binding.session_token = 0x1001;
  binding.attachment_token = 0x2002;
  binding.lease_token = 0x3003;
  binding.max_urb = 4096;
  binding.max_inflight = 4;
  return binding;
}

remote_usb::device_info
test_device() {
  remote_usb::device_info device;
  device.busid = "1-2";
  device.path = "/sys/devices/test-usb";
  device.busnum = 1;
  device.devnum = 2;
  device.vendor_id = 0x1234;
  device.product_id = 0x5678;
  device.interfaces = { { 3, 0, 0 }, { 8, 6, 80 } };
  return device;
}

struct transport_harness {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<remote_usb::transport_message> sent;
  std::vector<remote_usb::close_reason> closed;
  bool accept = true;

  remote_usb::broker_transport_callbacks callbacks() {
    remote_usb::broker_transport_callbacks result;
    result.send_frame = [this](remote_usb::transport_message message) {
      std::lock_guard lock(mutex);
      sent.push_back(std::move(message));
      condition.notify_all();
      return accept;
    };
    result.on_closed = [this](remote_usb::close_reason reason) {
      std::lock_guard lock(mutex);
      closed.push_back(reason);
      condition.notify_all();
    };
    return result;
  }

  bool wait_sent(std::size_t count) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, 2s, [this, count]() { return sent.size() >= count; });
  }
};

}  // namespace

TEST(RemoteUsbBrokerAdapter, OpensAndRoutesCompletePduWithoutReplyInspection) {
  transport_harness transport;
  remote_usb::loopback_usbip_bridge bridge;
  remote_usb::broker_adapter adapter;
  boost::system::error_code error;

  ASSERT_TRUE(adapter.start(test_binding(), bridge, transport.callbacks(), error))
    << error.message();
  ASSERT_EQ(adapter.state(), remote_usb::adapter_state::awaiting_capability);

  const auto binding = test_binding();
  remote_usb::capability_event capability;
  capability.session_token = binding.session_token;
  capability.lease_token = binding.lease_token;
  capability.attachment_token = binding.attachment_token;
  capability.device = test_device();
  ASSERT_TRUE(adapter.accept_capability(capability));
  ASSERT_EQ(adapter.state(), remote_usb::adapter_state::awaiting_open);
  {
    std::lock_guard lock(transport.mutex);
    EXPECT_TRUE(transport.sent.empty());
  }

  ASSERT_TRUE(adapter.request_open());
  ASSERT_TRUE(transport.wait_sent(1));
  {
    std::lock_guard lock(transport.mutex);
    ASSERT_EQ(transport.sent.front().kind, remote_usb::transport_message_kind::open);
    EXPECT_EQ(transport.sent.front().session_token, binding.session_token);
    EXPECT_EQ(transport.sent.front().lease_token, binding.lease_token);
    EXPECT_EQ(transport.sent.front().attachment_token, binding.attachment_token);
  }

  ASSERT_TRUE(adapter.accept_open_ok(binding.session_token));
  ASSERT_EQ(adapter.state(), remote_usb::adapter_state::open);
  const auto endpoint = adapter.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  for (int attempt = 0; attempt < 200 && !adapter.imported(); ++attempt) {
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(adapter.imported());

  constexpr std::uint32_t devid = 0x00010002;
  const auto request = make_submit(7, devid, 0, 1, 4, { 1, 2, 3, 4 });
  ASSERT_TRUE(write_all(socket, request));
  ASSERT_TRUE(transport.wait_sent(2));

  std::uint64_t pdu_id = 0;
  std::vector<std::uint8_t> forwarded;
  {
    std::lock_guard lock(transport.mutex);
    ASSERT_EQ(transport.sent[1].kind, remote_usb::transport_message_kind::usbip_data);
    pdu_id = transport.sent[1].pdu_id;
    forwarded = transport.sent[1].pdu;
  }
  EXPECT_NE(pdu_id, 0U);
  EXPECT_EQ(forwarded, request);
  EXPECT_EQ(adapter.pending_count(), 1U);

  /* Linux USB/IP servers use canonical zero devid/direction/endpoint fields;
   * routing must still happen by the adapter pdu id and request sequence. */
  ASSERT_TRUE(adapter.accept_usbip_pdu(pdu_id, make_reply(7, 0, 0, 0)));
  std::vector<std::uint8_t> reply(48);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u32_be(reply.data()), 3U);
  EXPECT_EQ(get_u32_be(reply.data() + 4), 7U);
  EXPECT_EQ(adapter.pending_count(), 0U);

  ASSERT_TRUE(adapter.accept_close(binding.session_token, binding.lease_token));
  EXPECT_EQ(adapter.state(), remote_usb::adapter_state::closed);
  EXPECT_FALSE(adapter.running());
  socket.close(error);
}

TEST(RemoteUsbBrokerAdapter, RejectsWrongLeaseAndLatePdu) {
  transport_harness transport;
  remote_usb::loopback_usbip_bridge bridge;
  remote_usb::broker_adapter adapter;
  boost::system::error_code error;
  const auto binding = test_binding();
  ASSERT_TRUE(adapter.start(binding, bridge, transport.callbacks(), error));

  remote_usb::capability_event capability;
  capability.session_token = binding.session_token;
  capability.lease_token = binding.lease_token;
  capability.attachment_token = binding.attachment_token;
  capability.device = test_device();
  ASSERT_TRUE(adapter.accept_capability(capability));
  ASSERT_FALSE(adapter.accept_open(remote_usb::open_event {
    binding.session_token, binding.lease_token + 1, binding.attachment_token }));
  EXPECT_EQ(adapter.last_status(), remote_usb::adapter_status::token_mismatch);
  EXPECT_EQ(adapter.state(), remote_usb::adapter_state::closed);
  EXPECT_FALSE(adapter.accept_usbip_pdu(1, std::vector<std::uint8_t>(48, 0)));
}

TEST(RemoteUsbBrokerAdapter, RetiresUnlinkTargetAndAcceptsLateSubmit) {
  transport_harness transport;
  remote_usb::loopback_usbip_bridge bridge;
  remote_usb::broker_adapter adapter;
  boost::system::error_code error;
  const auto binding = test_binding();
  ASSERT_TRUE(adapter.start(binding, bridge, transport.callbacks(), error));

  remote_usb::capability_event capability;
  capability.session_token = binding.session_token;
  capability.lease_token = binding.lease_token;
  capability.attachment_token = binding.attachment_token;
  capability.device = test_device();
  ASSERT_TRUE(adapter.accept_capability(capability));
  ASSERT_TRUE(adapter.request_open());
  ASSERT_TRUE(adapter.accept_open_ok(binding.session_token));
  const auto endpoint = adapter.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));

  constexpr std::uint32_t devid = 0x00010002;
  ASSERT_TRUE(write_all(socket, make_submit(7, devid, 1, 1, 0)));
  ASSERT_TRUE(transport.wait_sent(2));
  std::uint64_t submit_id = 0;
  {
    std::lock_guard lock(transport.mutex);
    submit_id = transport.sent[1].pdu_id;
  }
  ASSERT_NE(submit_id, 0U);

  ASSERT_TRUE(write_all(socket, make_unlink(8, 7, devid)));
  ASSERT_TRUE(transport.wait_sent(3));
  std::uint64_t unlink_id = 0;
  {
    std::lock_guard lock(transport.mutex);
    unlink_id = transport.sent[2].pdu_id;
  }
  ASSERT_NE(unlink_id, 0U);

  /* Canonical RET_UNLINK retires the target while allowing its late reply. */
  ASSERT_TRUE(adapter.accept_usbip_pdu(unlink_id, make_unlink_reply(8)));
  EXPECT_EQ(adapter.pending_count(), 1U);
  ASSERT_TRUE(adapter.accept_usbip_pdu(submit_id, make_reply(7, 0, 0, 0)));
  EXPECT_EQ(adapter.pending_count(), 0U);

  adapter.stop();
  socket.close(error);
}

TEST(RemoteUsbBrokerAdapter, StopEmitsCloseOnceAndCanBeRepeated) {
  transport_harness transport;
  remote_usb::loopback_usbip_bridge bridge;
  remote_usb::broker_adapter adapter;
  boost::system::error_code error;
  const auto binding = test_binding();
  ASSERT_TRUE(adapter.start(binding, bridge, transport.callbacks(), error));
  remote_usb::capability_event capability;
  capability.session_token = binding.session_token;
  capability.lease_token = binding.lease_token;
  capability.attachment_token = binding.attachment_token;
  capability.device = test_device();
  ASSERT_TRUE(adapter.accept_capability(capability));
  ASSERT_TRUE(adapter.request_open());
  ASSERT_TRUE(adapter.accept_open_ok(binding.session_token));

  adapter.stop();
  adapter.stop();
  std::lock_guard lock(transport.mutex);
  ASSERT_EQ(transport.sent.size(), 2U);
  EXPECT_EQ(transport.sent.back().kind, remote_usb::transport_message_kind::close);
  EXPECT_EQ(transport.sent.back().lease_token, binding.lease_token);
  EXPECT_EQ(transport.closed.size(), 1U);
}

TEST(RemoteUsbBrokerAdapter, LocalDisconnectStopsBridgeAndAllowsReuse) {
  transport_harness transport;
  remote_usb::loopback_usbip_bridge bridge;
  remote_usb::broker_adapter adapter;
  boost::system::error_code error;
  const auto binding = test_binding();

  ASSERT_TRUE(adapter.start(binding, bridge, transport.callbacks(), error));
  remote_usb::capability_event capability;
  capability.session_token = binding.session_token;
  capability.lease_token = binding.lease_token;
  capability.attachment_token = binding.attachment_token;
  capability.device = test_device();
  ASSERT_TRUE(adapter.accept_capability(capability));
  ASSERT_TRUE(adapter.request_open());
  ASSERT_TRUE(adapter.accept_open_ok(binding.session_token));

  const auto endpoint = adapter.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());
  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  socket.close(error);

  bool closed = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (adapter.state() == remote_usb::adapter_state::closed && !bridge.running()) {
      closed = true;
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(closed);

  // The terminal binding can be reused after the bridge's self-thread stop.
  ASSERT_TRUE(adapter.start(binding, bridge, transport.callbacks(), error));
  ASSERT_TRUE(adapter.accept_capability(capability));
  ASSERT_TRUE(adapter.request_open());
  ASSERT_TRUE(adapter.accept_open_ok(binding.session_token));
  EXPECT_TRUE(adapter.running());
  adapter.stop();
}
