/**
 * @file tests/unit/test_remote_usb_capability.cpp
 * @brief Tests for the Remote USB capability bootstrap.
 */

#include <array>
#include <chrono>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "src/remote_usb/remote_usb_capability.h"
#include "src/remote_usb/remote_usb_http.h"

namespace {

remote_usb::capability_endpoint
endpoint() {
  return { "192.0.2.10", 49152 };
}

}  // namespace

TEST(RemoteUsbCapability, IssuesAndConsumesOnce) {
  remote_usb::capability_store store;
  const auto now = remote_usb::capability_store::clock_t::now();
  const auto issued = store.issue("0123456789abcdef", 7, endpoint(), now);
  ASSERT_TRUE(issued.has_value());
  EXPECT_EQ(issued->client_uuid, "0123456789abcdef");
  EXPECT_EQ(issued->stream_generation, 7u);
  EXPECT_EQ(issued->max_urb, remote_usb::capability_default_max_urb);
  EXPECT_EQ(issued->max_inflight, remote_usb::capability_default_max_inflight);
  EXPECT_EQ(issued->expires_ms, remote_usb::capability_default_expires_ms);

  const auto consumed = store.consume(issued->client_uuid, issued->stream_generation, issued->nonce, now);
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(consumed->endpoint.port, 49152);
  EXPECT_FALSE(store.consume(issued->client_uuid, issued->stream_generation, issued->nonce, now).has_value());
}

TEST(RemoteUsbCapability, MismatchRevokesNonce) {
  remote_usb::capability_store store;
  const auto issued = store.issue("0123456789abcdef", 7, endpoint());
  ASSERT_TRUE(issued.has_value());

  EXPECT_FALSE(store.consume("fedcba9876543210", 7, issued->nonce).has_value());
  EXPECT_FALSE(store.consume(issued->client_uuid, issued->stream_generation, issued->nonce).has_value());
}

TEST(RemoteUsbCapability, KeepsCertificateAndWireIdentitiesSeparate) {
  remote_usb::capability_store store;
  const auto issued = store.issue(
    "server-paired-uuid", 11, endpoint(), "0123456789abcdef");
  ASSERT_TRUE(issued.has_value());
  EXPECT_EQ(issued->client_uuid, "server-paired-uuid");
  EXPECT_EQ(issued->wire_client_uuid, "0123456789abcdef");
  EXPECT_TRUE(remote_usb::capability_store::matches_wire_identity(
    *issued, "0123456789abcdef"));
  EXPECT_FALSE(remote_usb::capability_store::matches_wire_identity(
    *issued, "fedcba9876543210"));

  std::array<std::uint8_t, remote_usb::capability_wire_identity_size> wire_bytes {};
  for (std::size_t index = 0; index < wire_bytes.size(); ++index) {
    wire_bytes[index] = static_cast<std::uint8_t>(index + 1);
  }
  auto binary = store.issue("server-paired-uuid", 12, endpoint(),
                            std::string {
                              reinterpret_cast<const char *>(wire_bytes.data()), wire_bytes.size() });
  ASSERT_TRUE(binary.has_value());
  EXPECT_TRUE(remote_usb::capability_store::matches_wire_identity(*binary, wire_bytes));
  wire_bytes[0] ^= 0x80u;
  EXPECT_FALSE(remote_usb::capability_store::matches_wire_identity(*binary, wire_bytes));

  /* Consuming the certificate-bound capability does not silently rewrite the
   * independent wire identity. */
  const auto consumed = store.consume(
    "server-paired-uuid", 11, issued->nonce);
  ASSERT_TRUE(consumed.has_value());
  EXPECT_EQ(consumed->wire_client_uuid, "0123456789abcdef");
}

TEST(RemoteUsbCapability, MatchesUppercaseHashedWireIdentity) {
  remote_usb::capability_store store;
  const auto issued = store.issue(
    "server-paired-uuid", 13, endpoint(),
    "416A688F089C1E7B6243939008D401DE");
  ASSERT_TRUE(issued.has_value());

  std::array<std::uint8_t, remote_usb::capability_wire_identity_size> wire_bytes {};
  const std::string digest = "416A688F089C1E7B6243939008D401DE";
  for (std::size_t index = 0; index < wire_bytes.size(); ++index) {
    const auto high = static_cast<unsigned char>(digest[index * 2]);
    const auto low = static_cast<unsigned char>(digest[index * 2 + 1]);
    const auto decode = [](unsigned char value) {
      if (value >= '0' && value <= '9') return static_cast<unsigned char>(value - '0');
      return static_cast<unsigned char>(value - 'A' + 10);
    };
    wire_bytes[index] = static_cast<std::uint8_t>((decode(high) << 4) | decode(low));
  }

  EXPECT_TRUE(remote_usb::capability_store::matches_wire_identity(*issued, wire_bytes));
  EXPECT_FALSE(remote_usb::capability_store::matches_wire_identity(
    *issued, "416a688f089c1e7b6243939008d401de"));
  wire_bytes.back() ^= 0x01u;
  EXPECT_FALSE(remote_usb::capability_store::matches_wire_identity(*issued, wire_bytes));
}

TEST(RemoteUsbCapability, BindsCompleteLeaseTupleAndConsumesOnMismatch) {
  remote_usb::capability_store store;
  const auto issued = store.issue(
    "server-paired-uuid", 17, endpoint(), "0123456789abcdef", 101, 202, 303);
  ASSERT_TRUE(issued.has_value());
  EXPECT_EQ(issued->session_token, 101u);
  EXPECT_EQ(issued->attachment_token, 202u);
  EXPECT_EQ(issued->lease_token, 303u);
  EXPECT_TRUE(remote_usb::capability_store::matches_tokens(*issued, 101, 202, 303));
  EXPECT_FALSE(remote_usb::capability_store::matches_tokens(*issued, 101, 202, 304));

  /* A failed tuple check still burns the nonce and cannot be retried. */
  EXPECT_FALSE(store.consume(
    issued->client_uuid, issued->stream_generation, 101, 202, 304, issued->nonce));
  EXPECT_FALSE(store.consume(
    issued->client_uuid, issued->stream_generation, 101, 202, 303, issued->nonce));

  const auto unbound = store.issue("server-paired-uuid", 18, endpoint());
  ASSERT_TRUE(unbound.has_value());
  EXPECT_FALSE(remote_usb::capability_store::matches_tokens(*unbound, 1, 2, 3));
  EXPECT_FALSE(store.consume(
    unbound->client_uuid, unbound->stream_generation, 1, 2, 3, unbound->nonce));
}

TEST(RemoteUsbCapability, RejectsInvalidInputsBeforeAllocating) {
  remote_usb::capability_store store;
  EXPECT_FALSE(store.issue("", 1, endpoint()).has_value());
  EXPECT_FALSE(store.issue("client", 0, endpoint()).has_value());
  EXPECT_FALSE(store.issue("client", 1, { "", 1 }).has_value());
  EXPECT_FALSE(store.issue("client", 1, { "127.0.0.1", 0 }).has_value());
  EXPECT_FALSE(store.issue("client", 1, endpoint(), std::string(65, 'x')).has_value());
  EXPECT_EQ(store.size(), 0u);
}

TEST(RemoteUsbCapability, ClearRevokesOutstandingCapabilities) {
  remote_usb::capability_store store;
  ASSERT_TRUE(store.issue("a", 1, endpoint()).has_value());
  ASSERT_TRUE(store.issue("b", 1, endpoint()).has_value());
  EXPECT_EQ(store.size(), 2u);
  store.clear();
  EXPECT_EQ(store.size(), 0u);
}

TEST(RemoteUsbCapability, ExpiresWithoutSleeping) {
  using store_t = remote_usb::capability_store;
  store_t::policy policy;
  policy.ttl = std::chrono::milliseconds { 10 };
  store_t store { policy };
  const auto now = store_t::clock_t::now();
  const auto issued = store.issue("0123456789abcdef", 1, endpoint(), now);
  ASSERT_TRUE(issued.has_value());
  EXPECT_FALSE(store.consume(issued->client_uuid, 1, issued->nonce, now + std::chrono::milliseconds { 10 }).has_value());
  EXPECT_EQ(store.size(), 0u);
}

TEST(RemoteUsbCapability, Base64UrlRoundTripRejectsMalformed) {
  std::array<std::uint8_t, remote_usb::capability_nonce_size> input {};
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::uint8_t>(i * 17u + 3u);
  }
  const auto encoded = remote_usb::capability_store::encode_nonce(input);
  EXPECT_EQ(encoded.size(), 22u);
  const auto decoded = remote_usb::capability_store::decode_nonce(encoded);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, input);
  EXPECT_FALSE(remote_usb::capability_store::decode_nonce(encoded + "=").has_value());
  EXPECT_FALSE(remote_usb::capability_store::decode_nonce(std::string(22, 'A')).has_value());
}

TEST(RemoteUsbCapability, ParsesStrictGeneration) {
  std::string error;
  ASSERT_TRUE(remote_usb_http::parse_capability_request("42", error).has_value());
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(remote_usb_http::parse_capability_request("+42", error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(remote_usb_http::parse_capability_request("0", error).has_value());
  EXPECT_FALSE(remote_usb_http::parse_capability_request("18446744073709551616", error).has_value());
}

TEST(RemoteUsbCapability, ParsesStrictLeaseBinding) {
  std::string error;
  const auto parsed = remote_usb_http::parse_capability_request(
    "42", "101", "202", "303", error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->stream_generation, 42u);
  EXPECT_EQ(parsed->session_token, 101u);
  EXPECT_EQ(parsed->attachment_token, 202u);
  EXPECT_EQ(parsed->lease_token, 303u);

  EXPECT_FALSE(remote_usb_http::parse_capability_request(
    "42", "0", "202", "303", error));
  EXPECT_FALSE(remote_usb_http::parse_capability_request(
    "42", "+101", "202", "303", error));
  EXPECT_FALSE(remote_usb_http::parse_capability_request(
    "42", "101", "202", "18446744073709551616", error));
  EXPECT_FALSE(error.empty());
}

TEST(RemoteUsbCapability, ResponseUsesStableWireShape) {
  remote_usb::capability_store store;
  const auto issued = store.issue("0123456789abcdef", 3, endpoint());
  ASSERT_TRUE(issued.has_value());
  const auto response = remote_usb_http::make_capability_response(*issued);
  EXPECT_EQ(response.status, SimpleWeb::StatusCode::success_ok);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("version"), 1);
  EXPECT_EQ(body.at("endpoint").at("port"), 49152);
  EXPECT_TRUE(body.at("endpoint").at("host").is_string());
  EXPECT_TRUE(body.at("nonce").is_string());
  EXPECT_EQ(body.at("maxUrb"), remote_usb::capability_default_max_urb);
  EXPECT_EQ(body.at("maxInflight"), remote_usb::capability_default_max_inflight);
  EXPECT_EQ(body.at("expiresMs"), remote_usb::capability_default_expires_ms);
}
