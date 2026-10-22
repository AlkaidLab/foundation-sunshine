/**
 * @file tests/unit/test_dsu_server.cpp
 * @brief Test the cemuhook DSU server over a real loopback UDP socket.
 *
 * The wire contract belongs to cemuhook (Cemu, Ryujinx, Yuzu and friends), not
 * to this repository, so the test drives the server the way a client does: a
 * 16-byte `DSUS` header, then a message type. Everything is checked on the
 * bytes that come back, including the header CRC, because a malformed frame is
 * silently ignored by those clients instead of reported.
 */
#include <src/dsu_server.h>

#include "../tests_common.h"

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
  using namespace std::chrono_literals;

  constexpr std::uint32_t kMagic = 0x53555344;  // "DSUS"
  constexpr std::uint16_t kProtocolVersion = 1001;
  constexpr std::uint32_t kMessageTypeInfo = 0x100001;
  constexpr std::uint32_t kMessageTypeData = 0x100002;

  std::uint16_t
  free_udp_port() {
    boost::asio::io_context io;
    boost::asio::ip::udp::socket socket { io };
    socket.open(boost::asio::ip::udp::v4());
    socket.bind({ boost::asio::ip::udp::v4(), 0 });
    return socket.local_endpoint().port();
  }

  std::uint32_t
  crc32(const std::uint8_t *data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFF;
    while (size--) {
      crc ^= *data++;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
      }
    }
    return ~crc;
  }

  /**
   * @brief CRC over a packet with its own crc field zeroed, as the server does
   *        before sending.
   */
  std::uint32_t
  packet_crc(const std::vector<std::uint8_t> &packet) {
    std::vector<std::uint8_t> copy = packet;
    std::memset(copy.data() + 8, 0, 4);
    return crc32(copy.data(), copy.size());
  }

  /**
   * @brief A client request: 16-byte header + message type + slot.
   * @details The server reads the slot at offset 20, so the request carries it
   *          there; real clients send 20 or 21 bytes and the remaining bytes of
   *          the receive buffer are whatever the last packet left behind.
   *
   *          @p echo is written to offset 8 and comes back as the response's
   *          `client_id`. That offset is the header's crc field, not its
   *          client_id field, which is not what cemuhook documents — but it is
   *          what this server has always done and what Windows clients see, so
   *          the test pins the behaviour instead of inventing a new one.
   *          Clients leave the field zero, which is why nobody ever noticed.
   */
  std::vector<std::uint8_t>
  make_request(std::uint32_t message_type, std::uint8_t slot, std::uint32_t echo) {
    std::vector<std::uint8_t> packet(24, 0);
    const auto write_u16 = [&packet](std::size_t offset, std::uint16_t value) {
      std::memcpy(packet.data() + offset, &value, sizeof(value));
    };
    const auto write_u32 = [&packet](std::size_t offset, std::uint32_t value) {
      std::memcpy(packet.data() + offset, &value, sizeof(value));
    };

    write_u32(0, kMagic);
    write_u16(4, kProtocolVersion);
    write_u16(6, 4);
    write_u32(8, echo);
    write_u32(12, 0);
    write_u32(16, message_type);
    packet[20] = slot;
    return packet;
  }

  /**
   * @brief A loopback client bound to an ephemeral port, talking to @p port.
   */
  class client_t {
  public:
    explicit client_t(std::uint16_t port):
        socket_ { io_ },
        server_ { boost::asio::ip::make_address("127.0.0.1"), port } {
      socket_.open(boost::asio::ip::udp::v4());
      socket_.bind({ boost::asio::ip::udp::v4(), 0 });
      socket_.non_blocking(true);
    }

    void
    send(const std::vector<std::uint8_t> &packet) {
      socket_.send_to(boost::asio::buffer(packet), server_);
    }

    /**
     * @brief Wait for one datagram, at most @p timeout.
     * @details The server polls its socket every 5 ms, so the reply is never
     *          synchronous with the request.
     */
    std::vector<std::uint8_t>
    receive(std::chrono::milliseconds timeout = 2000ms) {
      std::vector<std::uint8_t> buffer(512);
      const auto deadline = std::chrono::steady_clock::now() + timeout;

      while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        boost::asio::ip::udp::endpoint from;
        const auto size = socket_.receive_from(boost::asio::buffer(buffer), from, 0, ec);
        if (!ec && size > 0) {
          buffer.resize(size);
          return buffer;
        }
        std::this_thread::sleep_for(5ms);
      }
      return {};
    }

  private:
    boost::asio::io_context io_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint server_;
  };

  /**
   * @brief A started server plus the port it listens on.
   */
  struct server_t {
    explicit server_t(std::uint16_t port):
        instance { port } {
      // start() returns 0 on success; anything else means the socket did not
      // bind and the test cannot mean anything.
      EXPECT_EQ(instance.start(), 0);
    }

    ~server_t() {
      instance.stop();
    }

    platf::dsu_server_t instance;
  };
}  // namespace

TEST(DsuServer, AnswersAnInfoRequestWithAFullGyroSlot) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(kMessageTypeInfo, 0, 0x11223344u));
  const auto reply = client.receive();

  ASSERT_EQ(reply.size(), 32u);

  platf::dsu_info_response response {};
  std::memcpy(&response, reply.data(), sizeof(response));

  EXPECT_EQ(response.header.magic, kMagic);
  EXPECT_EQ(response.header.version, kProtocolVersion);
  // The length field counts everything after the header.
  EXPECT_EQ(response.header.length, sizeof(platf::dsu_info_response) - sizeof(platf::dsu_header));
  // The value at offset 8 is echoed back, so a client that stamps it can match
  // the answer to its request (see make_request for why that offset is odd).
  EXPECT_EQ(response.header.client_id, 0x11223344u);
  EXPECT_EQ(response.shared.message_type, kMessageTypeInfo);
  EXPECT_EQ(response.shared.slot, 0);

  // A slot that reports itself connected with a full-gyro device is what makes
  // Cemu/Ryujinx offer a motion-capable controller at all.
  EXPECT_EQ(response.shared.slot_state, 2);
  EXPECT_EQ(response.shared.device_model, 2);
  EXPECT_EQ(response.shared.connection_type, 2);
  EXPECT_EQ(response.shared.mac_address[0], 1);
}

TEST(DsuServer, SignsEveryResponseWithACorrectCrc) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(kMessageTypeInfo, 0, 7));
  const auto reply = client.receive();

  ASSERT_EQ(reply.size(), 32u);
  std::uint32_t sent_crc = 0;
  std::memcpy(&sent_crc, reply.data() + 8, sizeof(sent_crc));
  EXPECT_EQ(sent_crc, packet_crc(reply));
}

TEST(DsuServer, SendsNoDataBeforeAClientSubscribes) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  // No DATA request yet, so nobody is subscribed: motion must not produce a
  // packet (the server only knows where to send after a subscription).
  server.instance.send_motion_data(0, 0.0f, 9.8f, 0.0f, 0.0f, 0.0f, 0.0f);
  EXPECT_TRUE(client.receive(300ms).empty());
}

TEST(DsuServer, SendsMotionToASubscribedClient) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(kMessageTypeData, 0, 42));
  // Subscribe first; the server never answers a DATA request on its own.
  ASSERT_TRUE(client.receive(200ms).empty());

  server.instance.send_motion_data(0, 0.0f, 9.8f, 0.0f, 0.0f, 0.0f, 0.0f);
  const auto reply = client.receive();

  ASSERT_EQ(reply.size(), sizeof(platf::dsu_data_packet));

  platf::dsu_data_packet packet {};
  std::memcpy(&packet, reply.data(), sizeof(packet));

  EXPECT_EQ(packet.header.magic, kMagic);
  EXPECT_EQ(packet.header.version, kProtocolVersion);
  EXPECT_EQ(packet.header.length, sizeof(platf::dsu_data_packet) - sizeof(platf::dsu_header));
  EXPECT_EQ(packet.header.client_id, 42u);
  EXPECT_EQ(packet.shared.message_type, kMessageTypeData);
  EXPECT_EQ(packet.connected, 1);
  ASSERT_GT(packet.packet_id, 0u);
}

TEST(DsuServer, MapsAccelerometerAxesTheWayTheClientsExpect) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(kMessageTypeData, 0, 1));
  ASSERT_TRUE(client.receive(200ms).empty());

  // Gravity on the client's Y axis. Ryujinx reads X = -X, Y = Z, Z = -Y, so a
  // Y-only input has to surface on the packet's Z axis with the sign flipped.
  server.instance.send_motion_data(0, 0.0f, 9.8f, 0.0f, 0.0f, 0.0f, 0.0f);
  const auto reply = client.receive();

  ASSERT_EQ(reply.size(), sizeof(platf::dsu_data_packet));
  platf::dsu_data_packet packet {};
  std::memcpy(&packet, reply.data(), sizeof(packet));

  EXPECT_FLOAT_EQ(packet.motion.accelerometer_x, 0.0f);
  EXPECT_FLOAT_EQ(packet.motion.accelerometer_y, 0.0f);
  EXPECT_FLOAT_EQ(packet.motion.accelerometer_z, 9.8f);
  ASSERT_GT(packet.motion.motion_timestamp, 0u);
}

TEST(DsuServer, MapsGyroscopeAxesTheWayTheClientsExpect) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(kMessageTypeData, 0, 1));
  ASSERT_TRUE(client.receive(200ms).empty());

  // Yaw is negated on the wire, and it sits before roll in the struct.
  server.instance.send_motion_data(0, 0.0f, 0.0f, 0.0f, 10.0f, 20.0f, 30.0f);
  const auto reply = client.receive();

  ASSERT_EQ(reply.size(), sizeof(platf::dsu_data_packet));
  platf::dsu_data_packet packet {};
  std::memcpy(&packet, reply.data(), sizeof(packet));

  EXPECT_FLOAT_EQ(packet.motion.gyroscope_pitch, 10.0f);
  EXPECT_FLOAT_EQ(packet.motion.gyroscope_yaw, -20.0f);
  EXPECT_FLOAT_EQ(packet.motion.gyroscope_roll, 30.0f);
}

TEST(DsuServer, KeepsOneAxisSetWhenTheOtherArrivesSeparately) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(kMessageTypeData, 0, 1));
  ASSERT_TRUE(client.receive(200ms).empty());

  // Clients send accel and gyro as separate events with the other half zeroed;
  // the server has to accumulate them instead of zeroing what it already had.
  server.instance.send_motion_data(0, 0.0f, 0.0f, 0.0f, 5.0f, 0.0f, 0.0f);
  ASSERT_FALSE(client.receive().empty());
  server.instance.send_motion_data(0, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
  const auto reply = client.receive();

  ASSERT_EQ(reply.size(), sizeof(platf::dsu_data_packet));
  platf::dsu_data_packet packet {};
  std::memcpy(&packet, reply.data(), sizeof(packet));

  EXPECT_FLOAT_EQ(packet.motion.gyroscope_pitch, 5.0f);
  // accel_x = 1 -> packet x = -1.
  EXPECT_FLOAT_EQ(packet.motion.accelerometer_x, -1.0f);
}

TEST(DsuServer, IgnoresRequestsWithAnUnknownMessageType) {
  const auto port = free_udp_port();
  server_t server { port };
  client_t client { port };

  client.send(make_request(0xdeadbeefu, 0, 1));
  EXPECT_TRUE(client.receive(300ms).empty());

  // The server must still be alive and answering known requests.
  client.send(make_request(kMessageTypeInfo, 0, 1));
  EXPECT_EQ(client.receive().size(), 32u);
}

TEST(DsuServer, ReportsItselfStoppedAfterStop) {
  const auto port = free_udp_port();
  {
    server_t server { port };
    EXPECT_TRUE(server.instance.is_running());
  }
  // The destructor stops the server; a second start on the same port must work,
  // which only holds if the socket was released.
  server_t restarted { port };
  EXPECT_TRUE(restarted.instance.is_running());
}
