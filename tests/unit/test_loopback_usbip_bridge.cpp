/**
 * @file tests/unit/test_loopback_usbip_bridge.cpp
 * @brief Protocol and lifecycle tests for the loopback USB/IP bridge.
 */

#include <src/remote_usb/loopback_usbip_bridge.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
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
  put_u16_be(std::uint8_t *out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 8);
    out[1] = static_cast<std::uint8_t>(value);
  }

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

  std::uint16_t
  get_u16_be(const std::uint8_t *in) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) | in[1]);
  }

  std::uint32_t
  get_u32_be(const std::uint8_t *in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
  }

  std::int32_t
  get_i32_be(const std::uint8_t *in) {
    return static_cast<std::int32_t>(get_u32_be(in));
  }

  std::vector<std::uint8_t>
  make_control(std::uint16_t code, std::uint32_t status = 0) {
    std::vector<std::uint8_t> wire(8, 0);
    put_u16_be(wire.data(), 0x0111);
    put_u16_be(wire.data() + 2, code);
    put_u32_be(wire.data() + 4, status);
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
    std::int32_t number_of_packets = -1,
    std::vector<std::uint8_t> data = {}) {
    const auto payload_size = direction == 0 ? data.size() : 0;
    std::vector<std::uint8_t> wire(48 + payload_size, 0);
    put_u32_be(wire.data(), 1);
    put_u32_be(wire.data() + 4, seqnum);
    put_u32_be(wire.data() + 8, devid);
    put_u32_be(wire.data() + 12, direction);
    put_u32_be(wire.data() + 16, endpoint);
    put_i32_be(wire.data() + 24, transfer_length);
    put_i32_be(wire.data() + 32, number_of_packets);
    if (endpoint == 0) {
      wire[40] = direction == 1 ? 0x80 : 0;
      put_u16_le(wire.data() + 46, static_cast<std::uint16_t>(transfer_length));
    }
    if (payload_size != 0) {
      std::copy(data.begin(), data.end(), wire.begin() + 48);
    }
    return wire;
  }

  std::vector<std::uint8_t>
  make_submit_reply_wire(std::uint32_t seqnum,
    std::uint32_t devid,
    std::uint32_t direction,
    std::uint32_t endpoint,
    std::int32_t status,
    std::int32_t actual_length,
    const std::vector<std::uint8_t> &payload,
    std::int32_t number_of_packets = -1,
    std::int32_t error_count = 0) {
    std::vector<std::uint8_t> wire(48 + payload.size(), 0);
    put_u32_be(wire.data(), 3);
    put_u32_be(wire.data() + 4, seqnum);
    put_u32_be(wire.data() + 8, devid);
    put_u32_be(wire.data() + 12, direction);
    put_u32_be(wire.data() + 16, endpoint);
    put_i32_be(wire.data() + 20, status);
    put_i32_be(wire.data() + 24, actual_length);
    put_i32_be(wire.data() + 32, number_of_packets);
    put_i32_be(wire.data() + 36, error_count);
    if (!payload.empty()) {
      std::copy(payload.begin(), payload.end(), wire.begin() + 48);
    }
    return wire;
  }

  std::vector<std::uint8_t>
  make_submit_reply(std::uint32_t seqnum,
    std::uint32_t devid,
    std::uint32_t direction,
    std::uint32_t endpoint,
    std::int32_t status,
    std::vector<std::uint8_t> data = {},
    std::int32_t number_of_packets = -1,
    std::int32_t error_count = 0) {
    const auto payload = direction == 1 ? data : std::vector<std::uint8_t> {};
    return make_submit_reply_wire(seqnum, devid, direction, endpoint, status,
      static_cast<std::int32_t>(data.size()), payload, number_of_packets, error_count);
  }

  std::vector<std::uint8_t>
  make_canonical_submit_reply(std::uint32_t seqnum,
    std::uint32_t request_direction,
    std::int32_t status,
    std::vector<std::uint8_t> data = {},
    std::int32_t number_of_packets = -1,
    std::int32_t error_count = 0) {
    const auto payload = request_direction == 1 ? data : std::vector<std::uint8_t> {};
    return make_submit_reply_wire(seqnum, 0, 0, 0, status,
      static_cast<std::int32_t>(data.size()), payload, number_of_packets, error_count);
  }

  std::vector<std::uint8_t>
  make_unlink(std::uint32_t seqnum,
    std::uint32_t devid,
    std::uint32_t unlink_seqnum,
    std::uint32_t direction = 0,
    std::uint32_t endpoint = 0) {
    std::vector<std::uint8_t> wire(48, 0);
    put_u32_be(wire.data(), 2);
    put_u32_be(wire.data() + 4, seqnum);
    put_u32_be(wire.data() + 8, devid);
    put_u32_be(wire.data() + 12, direction);
    put_u32_be(wire.data() + 16, endpoint);
    put_u32_be(wire.data() + 20, unlink_seqnum);
    return wire;
  }

  std::vector<std::uint8_t>
  make_unlink_reply(std::uint32_t seqnum,
    std::uint32_t devid,
    std::int32_t status,
    std::uint32_t direction = 0,
    std::uint32_t endpoint = 0) {
    std::vector<std::uint8_t> wire(48, 0);
    put_u32_be(wire.data(), 4);
    put_u32_be(wire.data() + 4, seqnum);
    put_u32_be(wire.data() + 8, devid);
    put_u32_be(wire.data() + 12, direction);
    put_u32_be(wire.data() + 16, endpoint);
    put_i32_be(wire.data() + 20, status);
    return wire;
  }

  std::vector<std::uint8_t>
  make_canonical_unlink_reply(std::uint32_t seqnum, std::int32_t status) {
    return make_unlink_reply(seqnum, 0, status);
  }

  bool
  write_all(tcp::socket &socket, const std::vector<std::uint8_t> &wire) {
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error) {
      return false;
    }
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (offset < wire.size() && std::chrono::steady_clock::now() < deadline) {
      const auto written = socket.write_some(asio::buffer(wire.data() + offset, wire.size() - offset), error);
      if (!error) {
        offset += written;
      }
      else if (error == asio::error::would_block || error == asio::error::try_again) {
        std::this_thread::sleep_for(1ms);
        error.clear();
      }
      else {
        return false;
      }
    }
    return offset == wire.size();
  }

  bool
  read_exact(tcp::socket &socket, std::vector<std::uint8_t> &wire) {
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error) {
      return false;
    }
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (offset < wire.size() && std::chrono::steady_clock::now() < deadline) {
      const auto received = socket.read_some(asio::buffer(wire.data() + offset, wire.size() - offset), error);
      if (!error) {
        offset += received;
      }
      else if (error == asio::error::would_block || error == asio::error::try_again) {
        std::this_thread::sleep_for(1ms);
        error.clear();
      }
      else {
        return false;
      }
    }
    return offset == wire.size();
  }

  bool
  no_data_for(tcp::socket &socket, std::chrono::milliseconds timeout = 100ms) {
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error) {
      return false;
    }
    std::array<std::uint8_t, 1> byte {};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      socket.read_some(asio::buffer(byte), error);
      if (!error) {
        return false;
      }
      if (error == asio::error::would_block || error == asio::error::try_again) {
        error.clear();
        std::this_thread::sleep_for(1ms);
        continue;
      }
      return false;
    }
    return true;
  }

  bool
  wait_for_close(tcp::socket &socket) {
    boost::system::error_code error;
    socket.non_blocking(true, error);
    if (error) {
      return false;
    }
    std::array<std::uint8_t, 1> byte {};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      socket.read_some(asio::buffer(byte), error);
      if (error == asio::error::eof || error == asio::error::connection_reset ||
          error == asio::error::operation_aborted) {
        return true;
      }
      if (error == asio::error::would_block || error == asio::error::try_again) {
        error.clear();
        std::this_thread::sleep_for(1ms);
        continue;
      }
      if (!error) {
        return false;
      }
      return false;
    }
    return false;
  }

  struct harness {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::vector<std::uint8_t>> requests;
    std::vector<remote_usb::close_reason> closed;
    bool imported = false;
    bool accept_requests = true;

    remote_usb::callbacks
    callbacks() {
      remote_usb::callbacks result;
      result.on_request = [this](std::vector<std::uint8_t> wire) {
        std::lock_guard lock(mutex);
        requests.push_back(std::move(wire));
        condition.notify_all();
        return accept_requests;
      };
      result.on_imported = [this]() {
        std::lock_guard lock(mutex);
        imported = true;
        condition.notify_all();
      };
      result.on_closed = [this](remote_usb::close_reason reason) {
        std::lock_guard lock(mutex);
        closed.push_back(reason);
        condition.notify_all();
      };
      return result;
    }

    bool
    wait_imported() {
      std::unique_lock lock(mutex);
      return condition.wait_for(lock, 2s, [this]() { return imported; });
    }

    bool
    wait_request_count(std::size_t count) {
      std::unique_lock lock(mutex);
      return condition.wait_for(lock, 2s, [this, count]() { return requests.size() >= count; });
    }

    bool
    wait_closed(remote_usb::close_reason reason) {
      std::unique_lock lock(mutex);
      return condition.wait_for(lock, 2s, [this, reason]() {
        return std::find(closed.begin(), closed.end(), reason) != closed.end();
      });
    }
  };

  remote_usb::device_info
  test_device() {
    remote_usb::device_info device;
    device.busid = "1-2";
    device.path = "/sys/devices/test-usb";
    device.busnum = 1;
    device.devnum = 2;
    device.speed = 3;
    device.vendor_id = 0x1234;
    device.product_id = 0x5678;
    device.device_bcd = 0x0102;
    device.interfaces = { { 3, 0, 0 }, { 8, 6, 80 } };
    return device;
  }

  bool
  perform_devlist(tcp::socket &socket,
                  std::size_t interface_count = 2,
                  std::string *busid = nullptr) {
    if (!write_all(socket, make_control(0x8005))) {
      return false;
    }
    std::vector<std::uint8_t> devlist(8 + 4 + 312 + interface_count * 4);
    if (!read_exact(socket, devlist) || get_u16_be(devlist.data()) != 0x0111 ||
        get_u16_be(devlist.data() + 2) != 0x0005 || get_u32_be(devlist.data() + 4) != 0 ||
        get_u32_be(devlist.data() + 8) != 1) {
      return false;
    }
    /* The bridge must not expose the platform path in USB/IP metadata. */
    const std::string path(reinterpret_cast<const char *>(devlist.data() + 12));
    if (path.empty() || path.size() > 31) {
      return false;
    }
    if (busid != nullptr) {
      *busid = path;
    }
    return true;
  }

  bool
  perform_import(tcp::socket &socket, std::size_t interface_count = 2) {
    std::string busid;
    if (!perform_devlist(socket, interface_count, &busid)) {
      return false;
    }
    if (!write_all(socket, make_import(busid))) {
      return false;
    }
    std::vector<std::uint8_t> imported(8 + 312);
    if (!read_exact(socket, imported) || get_u16_be(imported.data()) != 0x0111 ||
        get_u16_be(imported.data() + 2) != 0x0003 || get_u32_be(imported.data() + 4) != 0) {
      return false;
    }
    const std::string path(reinterpret_cast<const char *>(imported.data() + 8));
    return path == busid;
  }

  bool
  perform_direct_import(tcp::socket &socket, std::string_view busid) {
    if (!write_all(socket, make_import(busid))) {
      return false;
    }
    std::vector<std::uint8_t> imported(8 + 312);
    if (!read_exact(socket, imported) || get_u16_be(imported.data()) != 0x0111 ||
        get_u16_be(imported.data() + 2) != 0x0003 || get_u32_be(imported.data() + 4) != 0) {
      return false;
    }
    const std::string path(reinterpret_cast<const char *>(imported.data() + 8));
    return path == busid;
  }

}  // namespace

TEST(LoopbackUsbipBridge, GeneratesFreshOpaqueBusidPerStart) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  const auto first = bridge.start(test_device(), events.callbacks(), error);
  ASSERT_TRUE(first.has_value()) << error.message();
  ASSERT_EQ(first->busid.size(), 29U);
  bridge.stop();

  const auto second = bridge.start(test_device(), events.callbacks(), error);
  ASSERT_TRUE(second.has_value()) << error.message();
  EXPECT_EQ(second->busid.size(), 29U);
  EXPECT_EQ(second->busid.rfind("rusb-", 0), 0U);
  EXPECT_NE(first->busid, second->busid);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, ServesControlAndSubmitRoundTrip) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  const auto endpoint = bridge.start(test_device(), events.callbacks(), error);
  ASSERT_TRUE(endpoint.has_value()) << error.message();
  EXPECT_EQ(endpoint->address, "127.0.0.1");
  EXPECT_NE(endpoint->port, 0);
  EXPECT_NE(endpoint->port, 3240);
  EXPECT_EQ(endpoint->busid.size(), 29U);
  EXPECT_EQ(endpoint->busid.rfind("rusb-", 0), 0U);
  EXPECT_NE(endpoint->busid, "1-2");
  ASSERT_TRUE(bridge.running());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::uint32_t devid = 0x00010002;
  ASSERT_TRUE(write_all(socket, make_submit(7, devid, 1, 1, 4)));
  ASSERT_TRUE(events.wait_request_count(1));
  {
    std::lock_guard lock(events.mutex);
    ASSERT_EQ(events.requests.front().size(), std::size_t { 48 });
    EXPECT_EQ(get_u32_be(events.requests.front().data()), 1U);
    EXPECT_EQ(get_u32_be(events.requests.front().data() + 4), 7U);
    EXPECT_EQ(get_u32_be(events.requests.front().data() + 12), 1U);
  }

  const std::vector<std::uint8_t> data { 1, 2, 3, 4 };
  ASSERT_TRUE(bridge.send_reply(make_submit_reply(7, devid, 1, 1, 0, data)));
  std::vector<std::uint8_t> reply(52);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u32_be(reply.data()), 3U);
  EXPECT_EQ(get_u32_be(reply.data() + 4), 7U);
  EXPECT_EQ(reply[48], 1);
  EXPECT_EQ(reply[51], 4);

  socket.close(error);
  bridge.stop();
  EXPECT_FALSE(bridge.running());
}

TEST(LoopbackUsbipBridge, AcceptsDirectImportWithoutDevlist) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  const auto endpoint = bridge.start(test_device(), events.callbacks(), error);
  ASSERT_TRUE(endpoint.has_value()) << error.message();

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();

  /* usbip-win2 UDE sends OP_REQ_IMPORT as its first control PDU. */
  ASSERT_TRUE(perform_direct_import(socket, endpoint->busid));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::uint32_t seqnum = 13;
  constexpr std::uint32_t devid = 0x00010002;
  ASSERT_TRUE(write_all(socket, make_submit(seqnum, devid, 1, 1, 0)));
  ASSERT_TRUE(events.wait_request_count(1));
  {
    std::lock_guard lock(events.mutex);
    ASSERT_EQ(events.requests.front().size(), 48U);
    EXPECT_EQ(get_u32_be(events.requests.front().data()), 1U);
    EXPECT_EQ(get_u32_be(events.requests.front().data() + 4), seqnum);
    EXPECT_EQ(get_u32_be(events.requests.front().data() + 8), devid);
  }

  ASSERT_TRUE(bridge.send_reply(make_canonical_submit_reply(seqnum, 1, 0)));
  std::vector<std::uint8_t> reply(48);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u32_be(reply.data()), 3U);
  EXPECT_EQ(get_u32_be(reply.data() + 4), seqnum);
  EXPECT_EQ(get_i32_be(reply.data() + 20), 0);

  socket.close(error);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, RejectsIsochronousAndReportsProtocolClose) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));

  ASSERT_TRUE(write_all(socket, make_submit(9, 0x00010002, 1, 1, 8, 1)));
  ASSERT_TRUE(wait_for_close(socket));
  EXPECT_TRUE(events.wait_closed(remote_usb::close_reason::protocol_error));
  bridge.stop();
}

TEST(LoopbackUsbipBridge, RejectsDuplicateSequence) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(write_all(socket, make_submit(11, 0x00010002, 1, 1, 0)));
  ASSERT_TRUE(events.wait_request_count(1));
  ASSERT_TRUE(write_all(socket, make_submit(11, 0x00010002, 1, 1, 0)));
  ASSERT_TRUE(wait_for_close(socket));
  EXPECT_TRUE(events.wait_closed(remote_usb::close_reason::protocol_error));
  bridge.stop();
}

TEST(LoopbackUsbipBridge, StopRejectsLateReply) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  bridge.stop();
  EXPECT_FALSE(bridge.send_reply(make_submit_reply(1, 0x00010002, 1, 1, 0)));
  EXPECT_TRUE(events.wait_closed(remote_usb::close_reason::stopped));
}

TEST(LoopbackUsbipBridge, RejectsWrongBusidAndReleasesSession) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_devlist(socket));

  ASSERT_TRUE(write_all(socket, make_import("wrong-busid")));
  std::vector<std::uint8_t> reply(8);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u16_be(reply.data()), 0x0111);
  EXPECT_EQ(get_u16_be(reply.data() + 2), 0x0003);
  EXPECT_EQ(get_u32_be(reply.data() + 4), 4U);
  EXPECT_TRUE(wait_for_close(socket));
  EXPECT_TRUE(events.wait_closed(remote_usb::close_reason::transport_rejected));
  {
    std::lock_guard lock(events.mutex);
    EXPECT_FALSE(events.imported);
    EXPECT_TRUE(events.requests.empty());
  }

  // A rejected import must not consume the listener lease.  A subsequent
  // client can enumerate and import the same device normally.
  asio::io_context retry_io;
  tcp::socket retry(retry_io);
  retry.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(retry));
  EXPECT_TRUE(events.wait_imported());

  retry.close(error);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, ForwardsOutSubmitPayloadAndValidatesReply) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::uint32_t seqnum = 23;
  constexpr std::uint32_t devid = 0x00010002;
  constexpr std::uint32_t endpoint_number = 2;
  const std::vector<std::uint8_t> payload { 0x00, 0x7f, 0x80, 0xff, 0x12, 0x34 };
  ASSERT_TRUE(write_all(socket,
    make_submit(seqnum, devid, 0, endpoint_number,
      static_cast<std::int32_t>(payload.size()), -1, payload)));
  ASSERT_TRUE(events.wait_request_count(1));
  {
    std::lock_guard lock(events.mutex);
    const auto &request = events.requests.front();
    ASSERT_EQ(request.size(), 48U + payload.size());
    EXPECT_EQ(get_u32_be(request.data()), 1U);
    EXPECT_EQ(get_u32_be(request.data() + 4), seqnum);
    EXPECT_EQ(get_u32_be(request.data() + 8), devid);
    EXPECT_EQ(get_u32_be(request.data() + 12), 0U);
    EXPECT_EQ(get_u32_be(request.data() + 16), endpoint_number);
    EXPECT_EQ(get_i32_be(request.data() + 24), static_cast<std::int32_t>(payload.size()));
    EXPECT_EQ(get_i32_be(request.data() + 32), -1);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), request.begin() + 48));
  }

  ASSERT_TRUE(bridge.send_reply(make_canonical_submit_reply(seqnum, 0, 0, payload)));
  std::vector<std::uint8_t> reply(48);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u32_be(reply.data()), 3U);
  EXPECT_EQ(get_u32_be(reply.data() + 4), seqnum);
  EXPECT_EQ(get_u32_be(reply.data() + 8), 0U);
  EXPECT_EQ(get_u32_be(reply.data() + 12), 0U);
  EXPECT_EQ(get_u32_be(reply.data() + 16), 0U);
  EXPECT_EQ(get_i32_be(reply.data() + 20), 0);
  EXPECT_EQ(get_i32_be(reply.data() + 24), static_cast<std::int32_t>(payload.size()));
  EXPECT_EQ(get_i32_be(reply.data() + 28), 0);
  EXPECT_EQ(get_i32_be(reply.data() + 32), -1);
  EXPECT_EQ(get_i32_be(reply.data() + 36), 0);
  EXPECT_TRUE(std::all_of(reply.begin() + 40, reply.end(), [](std::uint8_t value) { return value == 0; }));

  socket.close(error);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, ForwardsUnlinkAndReturnsConnectionReset) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::uint32_t submit_seqnum = 31;
  constexpr std::uint32_t unlink_seqnum = 32;
  constexpr std::uint32_t devid = 0x00010002;
  ASSERT_TRUE(write_all(socket, make_submit(submit_seqnum, devid, 1, 1, 4)));
  ASSERT_TRUE(events.wait_request_count(1));
  ASSERT_TRUE(write_all(socket, make_unlink(unlink_seqnum, devid, submit_seqnum)));
  ASSERT_TRUE(events.wait_request_count(2));
  {
    std::lock_guard lock(events.mutex);
    const auto &request = events.requests[1];
    ASSERT_EQ(request.size(), 48U);
    EXPECT_EQ(get_u32_be(request.data()), 2U);
    EXPECT_EQ(get_u32_be(request.data() + 4), unlink_seqnum);
    EXPECT_EQ(get_u32_be(request.data() + 8), devid);
    EXPECT_EQ(get_u32_be(request.data() + 12), 0U);
    EXPECT_EQ(get_u32_be(request.data() + 16), 0U);
    EXPECT_EQ(get_u32_be(request.data() + 20), submit_seqnum);
    EXPECT_TRUE(std::all_of(request.begin() + 24, request.end(), [](std::uint8_t value) { return value == 0; }));
  }

  constexpr std::int32_t connection_reset = -104;  // -ECONNRESET in USB/IP.
  ASSERT_TRUE(bridge.send_reply(make_canonical_unlink_reply(unlink_seqnum, connection_reset)));
  std::vector<std::uint8_t> reply(48);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u32_be(reply.data()), 4U);
  EXPECT_EQ(get_u32_be(reply.data() + 4), unlink_seqnum);
  EXPECT_EQ(get_u32_be(reply.data() + 8), 0U);
  EXPECT_EQ(get_u32_be(reply.data() + 12), 0U);
  EXPECT_EQ(get_u32_be(reply.data() + 16), 0U);
  EXPECT_EQ(get_i32_be(reply.data() + 20), connection_reset);
  EXPECT_TRUE(std::all_of(reply.begin() + 24, reply.end(), [](std::uint8_t value) { return value == 0; }));

  // A successful unlink replaces the target completion.  A racing submit
  // reply is accepted for compatibility but must be consumed without putting
  // another RET_* packet on the wire.
  const std::vector<std::uint8_t> late_data { 0x10, 0x20, 0x30, 0x40 };
  ASSERT_TRUE(bridge.send_reply(make_canonical_submit_reply(submit_seqnum, 1, 0, late_data)));
  EXPECT_TRUE(no_data_for(socket));

  // The session remains usable after the cancellation race is drained.
  ASSERT_TRUE(write_all(socket, make_submit(33, devid, 1, 1, 0)));
  ASSERT_TRUE(events.wait_request_count(3));
  ASSERT_TRUE(bridge.send_reply(make_canonical_submit_reply(33, 1, 0)));
  std::vector<std::uint8_t> follow_up_reply(48);
  ASSERT_TRUE(read_exact(socket, follow_up_reply));
  EXPECT_EQ(get_u32_be(follow_up_reply.data()), 3U);
  EXPECT_EQ(get_u32_be(follow_up_reply.data() + 4), 33U);

  socket.close(error);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, ForwardsUnlinkForCompletedSubmit) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::uint32_t submit_seqnum = 41;
  constexpr std::uint32_t unlink_seqnum = 42;
  constexpr std::uint32_t devid = 0x00010002;
  // Zero is accepted as the legacy non-isochronous marker as well as -1.
  ASSERT_TRUE(write_all(socket, make_submit(submit_seqnum, devid, 1, 1, 0, 0)));
  ASSERT_TRUE(events.wait_request_count(1));
  ASSERT_TRUE(bridge.send_reply(make_canonical_submit_reply(submit_seqnum, 1, 0)));
  std::vector<std::uint8_t> submit_reply(48);
  ASSERT_TRUE(read_exact(socket, submit_reply));
  EXPECT_EQ(get_u32_be(submit_reply.data()), 3U);
  EXPECT_EQ(get_u32_be(submit_reply.data() + 4), submit_seqnum);
  EXPECT_EQ(get_u32_be(submit_reply.data() + 8), 0U);
  EXPECT_EQ(get_u32_be(submit_reply.data() + 12), 0U);
  EXPECT_EQ(get_u32_be(submit_reply.data() + 16), 0U);

  ASSERT_TRUE(write_all(socket, make_unlink(unlink_seqnum, devid, submit_seqnum)));
  ASSERT_TRUE(events.wait_request_count(2));
  ASSERT_TRUE(bridge.send_reply(make_canonical_unlink_reply(unlink_seqnum, 0)));
  std::vector<std::uint8_t> unlink_reply(48);
  ASSERT_TRUE(read_exact(socket, unlink_reply));
  EXPECT_EQ(get_u32_be(unlink_reply.data()), 4U);
  EXPECT_EQ(get_u32_be(unlink_reply.data() + 4), unlink_seqnum);
  EXPECT_EQ(get_u32_be(unlink_reply.data() + 8), 0U);
  EXPECT_EQ(get_u32_be(unlink_reply.data() + 12), 0U);
  EXPECT_EQ(get_u32_be(unlink_reply.data() + 16), 0U);
  EXPECT_EQ(get_i32_be(unlink_reply.data() + 20), 0);

  socket.close(error);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, ForwardsUnlinkForUnknownTarget) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::uint32_t unlink_seqnum = 51;
  constexpr std::uint32_t unknown_target = 999;
  constexpr std::uint32_t devid = 0x00010002;
  ASSERT_TRUE(write_all(socket, make_unlink(unlink_seqnum, devid, unknown_target)));
  ASSERT_TRUE(events.wait_request_count(1));
  {
    std::lock_guard lock(events.mutex);
    const auto &request = events.requests.front();
    ASSERT_EQ(request.size(), 48U);
    EXPECT_EQ(get_u32_be(request.data()), 2U);
    EXPECT_EQ(get_u32_be(request.data() + 4), unlink_seqnum);
    EXPECT_EQ(get_u32_be(request.data() + 20), unknown_target);
  }

  // usbipd returns status 0 when the target has already completed (or was
  // never pending); the bridge must leave that policy to the USB backend.
  ASSERT_TRUE(bridge.send_reply(make_canonical_unlink_reply(unlink_seqnum, 0)));
  std::vector<std::uint8_t> reply(48);
  ASSERT_TRUE(read_exact(socket, reply));
  EXPECT_EQ(get_u32_be(reply.data()), 4U);
  EXPECT_EQ(get_u32_be(reply.data() + 4), unlink_seqnum);
  EXPECT_EQ(get_u32_be(reply.data() + 8), 0U);
  EXPECT_EQ(get_i32_be(reply.data() + 20), 0);
  EXPECT_TRUE(std::all_of(reply.begin() + 24, reply.end(), [](std::uint8_t value) { return value == 0; }));
  EXPECT_TRUE(bridge.running());

  socket.close(error);
  bridge.stop();
}

TEST(LoopbackUsbipBridge, RejectsUnlinkWithNonzeroDirection) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));

  ASSERT_TRUE(write_all(socket, make_unlink(61, 0x00010002, 60, 1, 0)));
  EXPECT_TRUE(wait_for_close(socket));
  EXPECT_TRUE(events.wait_closed(remote_usb::close_reason::protocol_error));
  {
    std::lock_guard lock(events.mutex);
    EXPECT_TRUE(events.requests.empty());
  }
  bridge.stop();
}

TEST(LoopbackUsbipBridge, RejectsUnlinkWithNonzeroEndpoint) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;
  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), events.callbacks(), error).has_value());
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));

  ASSERT_TRUE(write_all(socket, make_unlink(71, 0x00010002, 70, 0, 1)));
  EXPECT_TRUE(wait_for_close(socket));
  EXPECT_TRUE(events.wait_closed(remote_usb::close_reason::protocol_error));
  {
    std::lock_guard lock(events.mutex);
    EXPECT_TRUE(events.requests.empty());
  }
  bridge.stop();
}

TEST(LoopbackUsbipBridge, SerializesConcurrentStops) {
  harness events;
  remote_usb::loopback_usbip_bridge bridge;

  std::mutex close_mutex;
  std::condition_variable close_condition;
  bool close_entered = false;
  bool release_close = false;

  auto bridge_callbacks = events.callbacks();
  bridge_callbacks.on_closed = [&](remote_usb::close_reason) {
    std::unique_lock lock(close_mutex);
    close_entered = true;
    close_condition.notify_all();
    close_condition.wait_for(lock, 2s, [&]() { return release_close; });
  };

  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), std::move(bridge_callbacks), error).has_value())
    << error.message();
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  ASSERT_TRUE(events.wait_imported());

  constexpr std::size_t kStopCallers = 8;
  std::mutex start_mutex;
  std::condition_variable start_condition;
  std::size_t ready = 0;
  bool start = false;
  std::vector<std::thread> stoppers;
  stoppers.reserve(kStopCallers);
  for (std::size_t i = 0; i < kStopCallers; ++i) {
    stoppers.emplace_back([&]() {
      {
        std::unique_lock lock(start_mutex);
        ++ready;
        start_condition.notify_all();
        start_condition.wait(lock, [&]() { return start; });
      }
      bridge.stop();
    });
  }

  {
    std::unique_lock lock(start_mutex);
    EXPECT_TRUE(start_condition.wait_for(lock, 2s, [&]() { return ready == kStopCallers; }));
    start = true;
    start_condition.notify_all();
  }

  {
    std::unique_lock lock(close_mutex);
    EXPECT_TRUE(close_condition.wait_for(lock, 2s, [&]() { return close_entered; }));
  }
  /* Keep the I/O thread alive while all stop callers contend for the thread
   * lifecycle claim.  This makes a duplicate join/detach observable under
   * thread sanitizers as well as in ordinary builds. */
  std::this_thread::sleep_for(50ms);
  {
    std::lock_guard lock(close_mutex);
    release_close = true;
  }
  close_condition.notify_all();

  for (auto &stopper : stoppers) {
    stopper.join();
  }
  socket.close(error);
  EXPECT_FALSE(bridge.running());
  bridge.stop();
}

TEST(LoopbackUsbipBridge, AllowsSelfStopFromClosedCallback) {
  remote_usb::loopback_usbip_bridge bridge;
  std::mutex callback_mutex;
  std::condition_variable callback_condition;
  bool imported = false;
  bool close_entered = false;
  bool self_stop_returned = false;
  bool release_callback = false;
  std::size_t close_calls = 0;

  remote_usb::callbacks bridge_callbacks;
  bridge_callbacks.on_request = [](std::vector<std::uint8_t>) { return true; };
  bridge_callbacks.on_imported = [&]() {
    std::lock_guard lock(callback_mutex);
    imported = true;
    callback_condition.notify_all();
  };
  bridge_callbacks.on_closed = [&](remote_usb::close_reason) {
    {
      std::lock_guard lock(callback_mutex);
      ++close_calls;
      close_entered = true;
      callback_condition.notify_all();
    }
    /* This executes on the bridge I/O thread.  It must not join or detach a
     * thread which an external stop() has already claimed. */
    bridge.stop();
    {
      std::unique_lock lock(callback_mutex);
      self_stop_returned = true;
      callback_condition.notify_all();
      /* Hold the I/O thread after self-stop so a later external stop is forced
       * to exercise the completion barrier for a detached thread. */
      callback_condition.wait_for(lock, 2s, [&]() { return release_callback; });
    }
  };

  boost::system::error_code error;
  ASSERT_TRUE(bridge.start(test_device(), std::move(bridge_callbacks), error).has_value())
    << error.message();
  const auto endpoint = bridge.local_endpoint();
  ASSERT_TRUE(endpoint.has_value());

  asio::io_context client_io;
  tcp::socket socket(client_io);
  socket.connect(tcp::endpoint(asio::ip::make_address(endpoint->address), endpoint->port), error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(perform_import(socket));
  {
    std::unique_lock lock(callback_mutex);
    ASSERT_TRUE(callback_condition.wait_for(lock, 2s, [&]() { return imported; }));
  }

  /* Closing the peer drives on_closed without an external stop already
   * owning the thread, so the callback deterministically takes the detach
   * path first. */
  socket.close(error);
  {
    std::unique_lock lock(callback_mutex);
    EXPECT_TRUE(callback_condition.wait_for(lock, 2s, [&]() { return self_stop_returned; }));
    EXPECT_TRUE(close_entered);
    EXPECT_EQ(close_calls, 1U);
  }

  /* A self-detached bridge must remain busy until its I/O callback returns;
   * otherwise the detached run loop could overlap a new bridge generation. */
  remote_usb::callbacks replacement_callbacks;
  replacement_callbacks.on_request = [](std::vector<std::uint8_t>) { return true; };
  boost::system::error_code restart_error;
  EXPECT_FALSE(bridge.start(test_device(), std::move(replacement_callbacks),
                            restart_error).has_value());
  EXPECT_EQ(restart_error,
            boost::system::errc::make_error_code(
              boost::system::errc::device_or_resource_busy));
  {
    std::lock_guard lock(callback_mutex);
    release_callback = true;
  }
  callback_condition.notify_all();
  /* stop() now deterministically waits for the self-detached I/O thread's
   * completion barrier before releasing the retained state. */
  bridge.stop();
  EXPECT_FALSE(bridge.running());
  /* Idempotence also covers the case where the callback won the detach race. */
  bridge.stop();
}
