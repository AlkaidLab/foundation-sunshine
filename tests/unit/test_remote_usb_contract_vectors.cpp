/**
 * @file tests/unit/test_remote_usb_contract_vectors.cpp
 * @brief Cross-language RUSB v1 contract vector checks.
 */

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/remote_usb/remote_usb_broker_server.h"

#ifndef RUSB_VECTOR_FILE
#error "RUSB_VECTOR_FILE must point at the pinned Rust core contract"
#endif

namespace {

nlohmann::json
load_vectors() {
  std::ifstream input { RUSB_VECTOR_FILE };
  EXPECT_TRUE(input.good()) << "unable to read " << RUSB_VECTOR_FILE;
  return nlohmann::json::parse(
    std::string { std::istreambuf_iterator<char> { input },
                  std::istreambuf_iterator<char> {} });
}

std::vector<std::uint8_t>
decode_hex(const std::string &value) {
  EXPECT_EQ(value.size() % 2, 0u);
  std::vector<std::uint8_t> bytes;
  bytes.reserve(value.size() / 2);
  const auto nibble = [](char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    return static_cast<std::uint8_t>(c - 'A' + 10);
  };
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const auto valid = [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
             (c >= 'A' && c <= 'F');
    };
    EXPECT_TRUE(valid(value[i]) && valid(value[i + 1]));
    if (!valid(value[i]) || !valid(value[i + 1])) return {};
    bytes.push_back(static_cast<std::uint8_t>((nibble(value[i]) << 4) |
                                               nibble(value[i + 1])));
  }
  return bytes;
}

std::vector<std::uint8_t>
vector_bytes(const nlohmann::json &vectors, const char *name) {
  return decode_hex(vectors.at(name).get<std::string>());
}

std::uint32_t
read_u32_be(const std::uint8_t *bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

std::uint32_t
read_u32_le(const std::uint8_t *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t
read_u64_le(const std::uint8_t *bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  }
  return value;
}

}  // namespace

TEST(RemoteUsbContractVectors, RustCoreVectorsDecodeWithSunshineCodec) {
  const auto document = load_vectors();
  ASSERT_EQ(document.at("schema"), 1);
  ASSERT_EQ(document.at("protocol"), "RUSB");
  ASSERT_EQ(document.at("version"), 1);
  const auto &vectors = document.at("vectors");

  const auto hello_bytes = vector_bytes(vectors, "hello");
  ASSERT_EQ(hello_bytes.size(), remote_usb::broker_hello_size);
  std::array<std::uint8_t, remote_usb::broker_hello_size> hello_wire {};
  std::copy(hello_bytes.begin(), hello_bytes.end(), hello_wire.begin());
  remote_usb::broker_hello hello;
  ASSERT_TRUE(remote_usb::decode_broker_hello(hello_wire, hello));
  EXPECT_EQ(hello.stream_generation, 2u);
  EXPECT_EQ(hello.session_token, 3u);
  EXPECT_EQ(hello.attachment_token, 7u);
  EXPECT_EQ(hello.lease_token, 9u);
  EXPECT_EQ(hello.max_urb, 49u);
  EXPECT_EQ(hello.max_inflight, 4u);

  const auto capability = vector_bytes(vectors, "capability_payload");
  remote_usb::device_info device;
  std::uint64_t lease_token = 0;
  std::uint64_t attachment_token = 0;
  ASSERT_TRUE(remote_usb::decode_broker_capability_payload(
    capability, device, lease_token, attachment_token));
  EXPECT_EQ(lease_token, 9u);
  EXPECT_EQ(attachment_token, 7u);
  EXPECT_EQ(device.busid, "moonlight-1");
  EXPECT_EQ(device.vendor_id, 0x1234);
  EXPECT_EQ(device.product_id, 0x5678);

  for (const auto &[name, type, sequence, payload_name] : {
         std::tuple { "capability_frame", 1u, 1u, "capability_payload" },
         std::tuple { "open_frame", 2u, 2u, "open_payload" } }) {
    const auto frame = vector_bytes(vectors, name);
    ASSERT_GE(frame.size(), remote_usb::broker_frame_header_size);
    std::array<std::uint8_t, remote_usb::broker_frame_header_size> header_wire {};
    std::copy_n(frame.begin(), header_wire.size(), header_wire.begin());
    remote_usb::broker_frame_header header;
    ASSERT_TRUE(remote_usb::decode_broker_frame_header(
      header_wire, header, 128u * 1024u));
    EXPECT_EQ(header.type, type);
    EXPECT_EQ(header.sequence, sequence);
    EXPECT_EQ(header.session_token, 3u);
    EXPECT_EQ(header.payload_length, frame.size() - header_wire.size());
    EXPECT_EQ(std::vector<std::uint8_t>(frame.begin() + header_wire.size(), frame.end()),
              vector_bytes(vectors, payload_name));
  }

  const auto submit = vector_bytes(vectors, "usbip_submit");
  const auto fragment = vector_bytes(vectors, "usbip_fragment");
  ASSERT_EQ(submit.size(), 48u);
  ASSERT_EQ(fragment.size(), 32u + submit.size());
  EXPECT_EQ(read_u32_be(submit.data()), 1u);
  EXPECT_EQ(read_u32_be(submit.data() + 4), 17u);
  EXPECT_EQ(read_u64_le(fragment.data()), 9u);
  EXPECT_EQ(read_u64_le(fragment.data() + 8), 11u);
  EXPECT_EQ(read_u32_le(fragment.data() + 16), 48u);
  EXPECT_EQ(read_u32_le(fragment.data() + 20), 0u);
  EXPECT_EQ(read_u32_le(fragment.data() + 24), 48u);
  EXPECT_EQ(std::vector<std::uint8_t>(fragment.begin() + 32, fragment.end()), submit);

  const auto unlink = vector_bytes(vectors, "usbip_unlink");
  ASSERT_EQ(unlink.size(), 48u);
  EXPECT_EQ(read_u32_be(unlink.data()), 2u);
  EXPECT_EQ(read_u32_be(unlink.data() + 4), 18u);
  EXPECT_EQ(read_u32_be(unlink.data() + 20), 17u);

  const auto import = vector_bytes(vectors, "usbip_import");
  ASSERT_EQ(import.size(), 40u);
  EXPECT_EQ(read_u32_be(import.data()), 0x01118003u);
  const std::string expected_bus_id { "moonlight-1\0", 12 };
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(import.data() + 8), 12),
            expected_bus_id);
}
