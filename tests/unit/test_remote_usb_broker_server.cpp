/**
 * @file tests/unit/test_remote_usb_broker_server.cpp
 * @brief Lifecycle regression tests for the TLS RUSB broker worker.
 */

#include <src/remote_usb/remote_usb_broker_server.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

void
expect_true(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}

void
write_self_signed_certificate(const std::filesystem::path &certificate,
                              const std::filesystem::path &private_key) {
  using evp_key_context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
  using evp_key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  using x509 = std::unique_ptr<X509, decltype(&X509_free)>;
  using bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

  evp_key_context context { EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free };
  expect_true(static_cast<bool>(context), "failed to create key context");
  expect_true(EVP_PKEY_keygen_init(context.get()) > 0, "failed to initialize keygen");
  expect_true(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) > 0,
              "failed to set key size");

  EVP_PKEY *raw_key = nullptr;
  expect_true(EVP_PKEY_keygen(context.get(), &raw_key) > 0, "failed to generate key");
  evp_key key { raw_key, EVP_PKEY_free };

  x509 certificate_object { X509_new(), X509_free };
  expect_true(static_cast<bool>(certificate_object), "failed to create certificate");
  expect_true(X509_set_version(certificate_object.get(), 2) == 1,
              "failed to set certificate version");
  expect_true(ASN1_INTEGER_set(X509_get_serialNumber(certificate_object.get()), 1) == 1,
              "failed to set certificate serial");
  expect_true(X509_gmtime_adj(X509_get_notBefore(certificate_object.get()), 0) != nullptr,
              "failed to set certificate start");
  expect_true(X509_gmtime_adj(X509_get_notAfter(certificate_object.get()), 3600) != nullptr,
              "failed to set certificate expiry");
  expect_true(X509_set_pubkey(certificate_object.get(), key.get()) == 1,
              "failed to set certificate key");

  auto *name = X509_get_subject_name(certificate_object.get());
  expect_true(name != nullptr, "failed to get certificate name");
  expect_true(X509_NAME_add_entry_by_txt(
                name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0) == 1,
              "failed to set certificate name");
  expect_true(X509_set_issuer_name(certificate_object.get(), name) == 1,
              "failed to set certificate issuer");
  expect_true(X509_sign(certificate_object.get(), key.get(), EVP_sha256()) > 0,
              "failed to sign certificate");

  const auto certificate_path = certificate.string();
  bio certificate_bio { BIO_new_file(certificate_path.c_str(), "w"), BIO_free };
  expect_true(static_cast<bool>(certificate_bio), "failed to open certificate file");
  expect_true(PEM_write_bio_X509(certificate_bio.get(), certificate_object.get()) == 1,
              "failed to write certificate");

  const auto key_path = private_key.string();
  bio key_bio { BIO_new_file(key_path.c_str(), "w"), BIO_free };
  expect_true(static_cast<bool>(key_bio), "failed to open key file");
  expect_true(PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0,
                                       nullptr, nullptr) == 1,
              "failed to write key");
}

struct tls_material final {
  std::filesystem::path root;
  std::filesystem::path certificate;
  std::filesystem::path private_key;

  tls_material():
      root(std::filesystem::temp_directory_path() /
           ("sunshine_rusb_lifecycle_" +
            std::to_string(static_cast<unsigned long long>(
              std::chrono::steady_clock::now().time_since_epoch().count())))),
      certificate(root / "certificate.pem"),
      private_key(root / "private-key.pem") {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
      throw std::runtime_error("failed to create TLS test directory");
    }
    write_self_signed_certificate(certificate, private_key);
  }

  ~tls_material() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

remote_usb::broker_server_config
make_config(remote_usb::capability_store &capabilities,
            const tls_material &tls) {
  remote_usb::broker_server_config config;
  config.bind_address = "127.0.0.1";
  config.port = 0;
  config.certificate_file = tls.certificate.string();
  config.private_key_file = tls.private_key.string();
  config.capabilities = &capabilities;
  config.client_certificate_uuid = [](SSL *) { return std::string { "test-client" }; };
  return config;
}

void
put_u16_le(std::uint8_t *bytes, std::uint16_t value) {
  bytes[0] = static_cast<std::uint8_t>(value);
  bytes[1] = static_cast<std::uint8_t>(value >> 8);
}

void
put_u32_le(std::uint8_t *bytes, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
}

void
put_u64_le(std::uint8_t *bytes, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
}

std::array<std::uint8_t, 84>
make_hello(const remote_usb::capability &capability,
           std::uint64_t stream_generation,
           std::uint64_t session_token,
           std::uint64_t attachment_token,
           std::uint64_t lease_token) {
  std::array<std::uint8_t, 84> wire {};
  put_u32_le(wire.data(), 0x42535552u);
  put_u16_le(wire.data() + 4, 1);
  put_u16_le(wire.data() + 6, 84);
  const std::array<std::uint8_t, 16> client_uuid {
    't', 'e', 's', 't', '-', 'c', 'l', 'i', 'e', 'n', 't', '-', 'u', 'u', 'i', 'd' };
  std::copy(client_uuid.begin(), client_uuid.end(), wire.begin() + 8);
  put_u64_le(wire.data() + 24, stream_generation);
  put_u64_le(wire.data() + 32, session_token);
  put_u64_le(wire.data() + 40, attachment_token);
  put_u64_le(wire.data() + 48, lease_token);
  std::copy(capability.nonce.begin(), capability.nonce.end(), wire.begin() + 56);
  put_u32_le(wire.data() + 72, 4096);
  put_u32_le(wire.data() + 76, 4);
  return wire;
}

std::uint16_t
read_u16_be(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(bytes[0] << 8) |
         static_cast<std::uint16_t>(bytes[1]);
}

std::uint16_t
read_u16_le(const std::uint8_t *bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t
read_u32_le(const std::uint8_t *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint32_t
read_u32_be(const std::uint8_t *bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t
read_u64_le(const std::uint8_t *bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
  }
  return value;
}

void
put_u16_be(std::uint8_t *bytes, std::uint16_t value) {
  bytes[0] = static_cast<std::uint8_t>(value >> 8);
  bytes[1] = static_cast<std::uint8_t>(value);
}

void
put_u32_be(std::uint8_t *bytes, std::uint32_t value) {
  bytes[0] = static_cast<std::uint8_t>(value >> 24);
  bytes[1] = static_cast<std::uint8_t>(value >> 16);
  bytes[2] = static_cast<std::uint8_t>(value >> 8);
  bytes[3] = static_cast<std::uint8_t>(value);
}

struct wire_frame {
  std::uint8_t type { 0 };
  std::uint32_t flags { 0 };
  std::uint64_t sequence { 0 };
  std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t>
make_frame(std::uint8_t type,
           std::uint64_t session_token,
           std::uint64_t sequence,
           std::vector<std::uint8_t> payload,
           std::uint32_t flags = 0) {
  std::vector<std::uint8_t> wire(32 + payload.size(), 0);
  put_u32_le(wire.data(), 0x42535552u);
  wire[4] = 1;
  wire[5] = type;
  put_u16_le(wire.data() + 6, 32);
  put_u32_le(wire.data() + 8, flags);
  put_u32_le(wire.data() + 12, static_cast<std::uint32_t>(payload.size()));
  put_u64_le(wire.data() + 16, session_token);
  put_u64_le(wire.data() + 24, sequence);
  std::copy(payload.begin(), payload.end(), wire.begin() + 32);
  return wire;
}

wire_frame
read_frame(ssl::stream<tcp::socket> &stream, std::uint64_t session_token) {
  std::array<std::uint8_t, 32> header {};
  asio::read(stream, asio::buffer(header));
  expect_true(read_u32_le(header.data()) == 0x42535552u, "bad RUSB frame magic");
  expect_true(header[4] == 1 && read_u16_le(header.data() + 6) == 32,
              "bad RUSB frame header");
  expect_true(read_u64_le(header.data() + 16) == session_token,
              "RUSB frame session token mismatch");
  const auto payload_size = read_u32_le(header.data() + 12);
  expect_true(payload_size <= 128u * 1024u, "RUSB frame payload too large");
  wire_frame frame;
  frame.type = header[5];
  frame.flags = read_u32_le(header.data() + 8);
  frame.sequence = read_u64_le(header.data() + 24);
  frame.payload.assign(payload_size, 0);
  if (!frame.payload.empty()) {
    asio::read(stream, asio::buffer(frame.payload));
  }
  return frame;
}

std::vector<std::uint8_t>
make_capability_payload(std::uint64_t lease_token,
                        std::uint64_t attachment_token) {
  constexpr std::array<std::uint8_t, 43> descriptors {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 0, 0, 0, 1,
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,
    7, 5, 0x81, 3, 64, 0, 1
  };
  constexpr std::string_view busid = "moonlight-1";
  constexpr std::array<std::uint8_t, 8> endpoint { 0, 0, 0x81, 3, 64, 0, 1, 0 };
  std::vector<std::uint8_t> payload(34 + busid.size() + descriptors.size() + endpoint.size(), 0);
  put_u64_le(payload.data(), lease_token);
  put_u64_le(payload.data() + 8, attachment_token);
  put_u16_le(payload.data() + 16, 0x1234);
  put_u16_le(payload.data() + 18, 0x5678);
  put_u16_le(payload.data() + 20, 0x0100);
  payload[25] = static_cast<std::uint8_t>(busid.size());
  put_u16_le(payload.data() + 26, 1);
  put_u32_le(payload.data() + 30, static_cast<std::uint32_t>(descriptors.size()));
  auto output = payload.begin() + 34;
  output = std::copy(busid.begin(), busid.end(), output);
  output = std::copy(descriptors.begin(), descriptors.end(), output);
  std::copy(endpoint.begin(), endpoint.end(), output);
  return payload;
}

std::vector<std::uint8_t>
make_usbip_fragment(std::uint64_t lease_token,
                    std::uint64_t pdu_id,
                    const std::vector<std::uint8_t> &pdu) {
  std::vector<std::uint8_t> payload(32 + pdu.size(), 0);
  put_u64_le(payload.data(), lease_token);
  put_u64_le(payload.data() + 8, pdu_id);
  put_u32_le(payload.data() + 16, static_cast<std::uint32_t>(pdu.size()));
  put_u32_le(payload.data() + 24, static_cast<std::uint32_t>(pdu.size()));
  std::copy(pdu.begin(), pdu.end(), payload.begin() + 32);
  return payload;
}

std::pair<std::uint64_t, std::vector<std::uint8_t>>
decode_usbip_frame(const wire_frame &frame) {
  expect_true(frame.type == 5 && frame.flags == 0 && frame.payload.size() >= 80,
              "expected one complete USB/IP data frame");
  const auto total = read_u32_le(frame.payload.data() + 16);
  const auto offset = read_u32_le(frame.payload.data() + 20);
  const auto chunk = read_u32_le(frame.payload.data() + 24);
  expect_true(offset == 0 && total == chunk && frame.payload.size() == 32u + chunk,
              "unexpected USB/IP fragment layout");
  return {
    read_u64_le(frame.payload.data() + 8),
    std::vector<std::uint8_t>(frame.payload.begin() + 32, frame.payload.end())
  };
}

std::vector<std::uint8_t>
make_import_request(std::string_view busid) {
  std::vector<std::uint8_t> wire(40, 0);
  put_u16_be(wire.data(), 0x0111);
  put_u16_be(wire.data() + 2, 0x8003);
  std::copy(busid.begin(), busid.end(), wire.begin() + 8);
  return wire;
}

std::vector<std::uint8_t>
make_submit(std::uint32_t sequence) {
  std::vector<std::uint8_t> wire(48, 0);
  put_u32_be(wire.data(), 1);
  put_u32_be(wire.data() + 4, sequence);
  put_u32_be(wire.data() + 8, 0x00010001);
  put_u32_be(wire.data() + 12, 1);
  put_u32_be(wire.data() + 16, 1);
  put_u32_be(wire.data() + 24, 8);
  put_u32_be(wire.data() + 32, 0xffffffffu);
  return wire;
}

std::vector<std::uint8_t>
make_unlink(std::uint32_t sequence, std::uint32_t target) {
  std::vector<std::uint8_t> wire(48, 0);
  put_u32_be(wire.data(), 2);
  put_u32_be(wire.data() + 4, sequence);
  put_u32_be(wire.data() + 8, 0x00010001);
  put_u32_be(wire.data() + 20, target);
  return wire;
}

std::vector<std::uint8_t>
make_submit_reply(std::uint32_t sequence, std::string_view data = {}) {
  std::vector<std::uint8_t> wire(48 + data.size(), 0);
  put_u32_be(wire.data(), 3);
  put_u32_be(wire.data() + 4, sequence);
  put_u32_be(wire.data() + 24, static_cast<std::uint32_t>(data.size()));
  put_u32_be(wire.data() + 32, 0xffffffffu);
  std::copy(data.begin(), data.end(), wire.begin() + 48);
  return wire;
}

std::vector<std::uint8_t>
make_unlink_reply(std::uint32_t sequence) {
  std::vector<std::uint8_t> wire(48, 0);
  put_u32_be(wire.data(), 4);
  put_u32_be(wire.data() + 4, sequence);
  return wire;
}

struct host_result {
  bool ok { false };
  std::string error;
};

host_result
run_usbip_host(const remote_usb::endpoint &endpoint,
               const std::shared_future<void> &release,
               std::promise<void> &exchange_complete) {
  try {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(asio::ip::make_address(endpoint.address), endpoint.port));

    const auto import = make_import_request(endpoint.busid);
    asio::write(socket, asio::buffer(import));
    std::vector<std::uint8_t> import_reply(320, 0);
    asio::read(socket, asio::buffer(import_reply));
    expect_true(read_u16_be(import_reply.data()) == 0x0111 &&
                  read_u16_be(import_reply.data() + 2) == 0x0003 &&
                  read_u32_be(import_reply.data() + 4) == 0,
                "loopback IMPORT failed");

    const auto first_submit = make_submit(7);
    asio::write(socket, asio::buffer(first_submit));
    std::vector<std::uint8_t> first_reply(52, 0);
    asio::read(socket, asio::buffer(first_reply));
    expect_true(read_u32_be(first_reply.data()) == 3 &&
                  read_u32_be(first_reply.data() + 4) == 7 &&
                  std::string_view(reinterpret_cast<const char *>(first_reply.data() + 48), 4) ==
                    "pong",
                "loopback SUBMIT reply mismatch");

    const auto second_submit = make_submit(8);
    const auto unlink = make_unlink(9, 8);
    asio::write(socket, asio::buffer(second_submit));
    asio::write(socket, asio::buffer(unlink));
    std::vector<std::uint8_t> unlink_reply(48, 0);
    asio::read(socket, asio::buffer(unlink_reply));
    expect_true(read_u32_be(unlink_reply.data()) == 4 &&
                  read_u32_be(unlink_reply.data() + 4) == 9,
                "loopback UNLINK reply mismatch");

    exchange_complete.set_value();
    release.wait();
    boost::system::error_code ignored;
    socket.close(ignored);
    return { true, {} };
  }
  catch (const std::exception &exception) {
    return { false, exception.what() };
  }
}

}  // namespace

TEST(RemoteUsbBrokerServer, ConcurrentStopIsIdempotent) {
  tls_material tls;
  remote_usb::capability_store capabilities;
  remote_usb::broker_server server(make_config(capabilities, tls));

  const auto result = server.start();
  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(server.running());
  ASSERT_NE(server.bound_port(), 0U);

  std::vector<std::thread> stoppers;
  for (int index = 0; index < 8; ++index) {
    stoppers.emplace_back([&server]() { server.stop(); });
  }
  for (auto &stopper : stoppers) {
    stopper.join();
  }

  EXPECT_FALSE(server.running());
  EXPECT_EQ(server.bound_port(), 0U);
  server.stop();
}

TEST(RemoteUsbBrokerServer, WorkerExceptionClosesSessionBeforeFinished) {
  tls_material tls;
  remote_usb::capability_store capabilities;
  constexpr std::uint64_t generation = 7;
  constexpr std::uint64_t session_token = 11;
  constexpr std::uint64_t attachment_token = 12;
  constexpr std::uint64_t lease_token = 13;
  const auto capability = capabilities.issue(
    "test-client", generation, { "127.0.0.1", 1 }, std::string { "test-client-uuid" },
    session_token, attachment_token, lease_token);
  ASSERT_TRUE(capability.has_value());

  auto config = make_config(capabilities, tls);
  std::atomic_bool authorize_called { false };
  config.authorize_client = [&authorize_called](std::string_view,
                                                 const remote_usb::broker_hello &,
                                                 const remote_usb::capability &) -> bool {
    authorize_called.store(true);
    throw std::runtime_error("intentional worker exception");
  };
  remote_usb::broker_server server(std::move(config));
  const auto result = server.start();
  ASSERT_TRUE(result) << result.error;

  asio::io_context client_io;
  ssl::context client_ssl(ssl::context::tls_client);
  client_ssl.set_verify_mode(ssl::verify_none);
  client_ssl.use_certificate_chain_file(tls.certificate.string());
  client_ssl.use_private_key_file(tls.private_key.string(), ssl::context::pem);
  ssl::stream<tcp::socket> client(client_io, client_ssl);
  client.lowest_layer().connect(
    tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.bound_port()));
  client.handshake(ssl::stream_base::client);
  const auto hello = make_hello(
    *capability, generation, session_token, attachment_token, lease_token);
  boost::system::error_code write_error;
  asio::write(client, asio::buffer(hello), write_error);

  for (int attempt = 0; attempt < 200 && server.running(); ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(authorize_called.load());
  EXPECT_FALSE(server.running());
  EXPECT_EQ(server.bound_port(), 0U);

  /* The worker publishes `running == false` only after its emergency sweep.
   * A non-blocking read therefore observes the broker-side close instead of
   * leaving the TLS connection stranded after io.run() unwinds. */
  boost::system::error_code nonblocking_error;
  client.lowest_layer().non_blocking(true, nonblocking_error);
  ASSERT_FALSE(nonblocking_error);
  bool closed = false;
  for (int attempt = 0; attempt < 200 && !closed; ++attempt) {
    std::array<char, 1> byte {};
    boost::system::error_code read_error;
    (void)client.read_some(asio::buffer(byte), read_error);
    if (read_error && read_error != asio::error::would_block &&
        read_error != asio::error::try_again) {
      closed = true;
    }
    if (!closed) {
      std::this_thread::sleep_for(10ms);
    }
  }
  EXPECT_TRUE(closed);

  boost::system::error_code close_error;
  client.lowest_layer().close(close_error);
  server.stop();
}

TEST(RemoteUsbBrokerServer, MoonlightWireContractRoutesImportSubmitAndUnlink) {
  tls_material tls;
  remote_usb::capability_store capabilities;
  constexpr std::uint64_t generation = 0x101;
  constexpr std::uint64_t session_token = 0x202;
  constexpr std::uint64_t attachment_token = 0x303;
  constexpr std::uint64_t lease_token = 0x404;
  const auto capability = capabilities.issue(
    "test-client", generation, { "127.0.0.1", 1 }, std::string { "test-client-uuid" },
    session_token, attachment_token, lease_token);
  ASSERT_TRUE(capability.has_value());

  std::promise<remote_usb::endpoint> endpoint_promise;
  auto endpoint_future = endpoint_promise.get_future();
  std::promise<remote_usb::close_reason> close_promise;
  auto close_future = close_promise.get_future();
  std::atomic_bool endpoint_published { false };
  std::atomic_bool close_published { false };
  auto config = make_config(capabilities, tls);
  config.idle_timeout_ms = 3000;
  config.on_local_endpoint_ready =
    [&](const remote_usb::endpoint &endpoint,
        const remote_usb::session_binding &,
        remote_usb::local_endpoint_ready_completion completion) {
      if (!endpoint_published.exchange(true)) {
        endpoint_promise.set_value(endpoint);
      }
      completion(true, remote_usb::adapter_status::ok);
    };
  config.on_session_closed =
    [&](const remote_usb::session_binding &, remote_usb::close_reason reason) {
      if (!close_published.exchange(true)) {
        close_promise.set_value(reason);
      }
    };

  remote_usb::broker_server server(std::move(config));
  const auto start_result = server.start();
  ASSERT_TRUE(start_result) << start_result.error;

  asio::io_context client_io;
  ssl::context client_ssl(ssl::context::tls_client);
  client_ssl.set_verify_mode(ssl::verify_none);
  client_ssl.use_certificate_chain_file(tls.certificate.string());
  client_ssl.use_private_key_file(tls.private_key.string(), ssl::context::pem);
  ssl::stream<tcp::socket> client(client_io, client_ssl);
  client.lowest_layer().connect(
    tcp::endpoint(asio::ip::make_address("127.0.0.1"), server.bound_port()));
  client.handshake(ssl::stream_base::client);

  const auto hello = make_hello(
    *capability, generation, session_token, attachment_token, lease_token);
  asio::write(client, asio::buffer(hello));
  std::array<std::uint8_t, 84> hello_echo {};
  asio::read(client, asio::buffer(hello_echo));
  ASSERT_EQ(hello_echo, hello);

  const auto capability_frame = make_frame(
    1, session_token, 1, make_capability_payload(lease_token, attachment_token));
  asio::write(client, asio::buffer(capability_frame));
  const auto open = read_frame(client, session_token);
  ASSERT_EQ(open.type, 2);
  ASSERT_EQ(open.sequence, 1U);
  ASSERT_EQ(open.payload.size(), 16U);
  ASSERT_EQ(read_u64_le(open.payload.data()), lease_token);
  ASSERT_EQ(read_u64_le(open.payload.data() + 8), attachment_token);

  const auto open_ok = make_frame(3, session_token, 2, {});
  asio::write(client, asio::buffer(open_ok));
  ASSERT_EQ(endpoint_future.wait_for(2s), std::future_status::ready);
  const auto endpoint = endpoint_future.get();
  ASSERT_EQ(endpoint.address, "127.0.0.1");
  ASSERT_FALSE(endpoint.busid.empty());
  ASSERT_TRUE(endpoint.busid.size() <= 31);

  std::promise<void> release_host_promise;
  const auto release_host = release_host_promise.get_future().share();
  std::promise<host_result> host_result_promise;
  auto host_result_future = host_result_promise.get_future();
  std::promise<void> host_exchange_promise;
  auto host_exchange_future = host_exchange_promise.get_future();
  std::thread host([&]() {
    host_result_promise.set_value(
      run_usbip_host(endpoint, release_host, host_exchange_promise));
  });

  std::string exchange_error;
  try {
    const auto first = decode_usbip_frame(read_frame(client, session_token));
    expect_true(read_u32_be(first.second.data()) == 1 &&
                  read_u32_be(first.second.data() + 4) == 7,
                "first broker PDU was not SUBMIT(7)");
    const auto first_reply = make_frame(
      5, session_token, 3,
      make_usbip_fragment(lease_token, first.first, make_submit_reply(7, "pong")));
    asio::write(client, asio::buffer(first_reply));

    const auto second = decode_usbip_frame(read_frame(client, session_token));
    expect_true(read_u32_be(second.second.data()) == 1 &&
                  read_u32_be(second.second.data() + 4) == 8,
                "second broker PDU was not SUBMIT(8)");
    const auto unlink = decode_usbip_frame(read_frame(client, session_token));
    expect_true(read_u32_be(unlink.second.data()) == 2 &&
                  read_u32_be(unlink.second.data() + 4) == 9 &&
                  read_u32_be(unlink.second.data() + 20) == 8,
                "third broker PDU was not UNLINK(9, 8)");

    const auto unlink_reply = make_frame(
      5, session_token, 4,
      make_usbip_fragment(lease_token, unlink.first, make_unlink_reply(9)));
    asio::write(client, asio::buffer(unlink_reply));
    const auto late_submit_reply = make_frame(
      5, session_token, 5,
      make_usbip_fragment(lease_token, second.first, make_submit_reply(8)));
    asio::write(client, asio::buffer(late_submit_reply));

    expect_true(host_exchange_future.wait_for(2s) == std::future_status::ready,
                "usbip host did not receive broker replies");
  }
  catch (const std::exception &exception) {
    exchange_error = exception.what();
  }

  const std::vector<std::uint8_t> close_payload = [&]() {
    std::vector<std::uint8_t> value(8, 0);
    put_u64_le(value.data(), lease_token);
    return value;
  }();
  if (exchange_error.empty()) {
    const auto close = make_frame(6, session_token, 6, close_payload);
    asio::write(client, asio::buffer(close));
    EXPECT_EQ(close_future.wait_for(2s), std::future_status::ready);
  }
  else {
    server.stop();
  }

  release_host_promise.set_value();
  host.join();
  const auto host_outcome = host_result_future.get();

  boost::system::error_code ignored;
  client.lowest_layer().close(ignored);
  server.stop();
  EXPECT_TRUE(exchange_error.empty()) << exchange_error;
  EXPECT_TRUE(host_outcome.ok) << host_outcome.error;
}
