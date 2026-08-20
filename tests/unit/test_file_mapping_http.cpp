/**
 * @file tests/unit/test_file_mapping_http.cpp
 * @brief Test src/file_mapping_http.*.
 */
#include <src/file_mapping/file_mapping_http.h>
#include <src/file_mapping/file_mapping_rpc.h>

#include <algorithm>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

TEST(FileMappingHttp, CapabilityResponseAdvertisesProtocol) {
  file_mapping_http::capability_state_t state;
  state.enabled = true;
  state.listening = true;
  state.session_endpoint = "/api/v1/file-mapping/session";
  state.session_url = "wss://127.0.0.1:47999/api/v1/file-mapping/session";
  state.session_token = "abc123";
  state.diagnostics["token"] = "issued";
  state.port = 47999;

  auto response = file_mapping_http::make_capability_response(state);
  ASSERT_EQ(response.status, SimpleWeb::StatusCode::success_ok);

  auto body = nlohmann::json::parse(response.body);
  EXPECT_TRUE(body["ok"].get<bool>());
  EXPECT_TRUE(body["enabled"].get<bool>());
  EXPECT_TRUE(body["listening"].get<bool>());
  EXPECT_EQ(body["version"].get<int>(), file_mapping::rpc::kProtocolVersion);
  EXPECT_EQ(body["transport"].get<std::string>(), "wss");
  EXPECT_EQ(body["control"].get<std::string>(), "json");
  EXPECT_EQ(body["data"].get<std::string>(), "binary");
  EXPECT_EQ(body["session_url"].get<std::string>(), state.session_url);
  EXPECT_EQ(body["session_token"].get<std::string>(), state.session_token);
  EXPECT_EQ(body["port"].get<int>(), state.port);
  EXPECT_EQ(body["diagnostics"]["token"].get<std::string>(), "issued");
  EXPECT_TRUE(std::find(body["features"].begin(), body["features"].end(), "cancel_job") != body["features"].end());
  EXPECT_TRUE(std::find(body["features"].begin(), body["features"].end(), "binary_frames") == body["features"].end());
  EXPECT_EQ(body["limits"]["binary_header_size"].get<std::size_t>(), file_mapping::rpc::kBinaryHeaderSize);
}

TEST(FileMappingHttp, CapabilityDoesNotBuildSessionUrlFromRequestHost) {
  file_mapping_http::capability_state_t state;
  state.enabled = true;
  state.listening = true;
  state.port = 47999;
  state.session_token = "abc123";

  auto response = file_mapping_http::make_capability_response(state, "evil.example:47990");
  auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body["session_url"].get<std::string>(), "");
  EXPECT_EQ(body["session_endpoint"].get<std::string>(), "/api/v1/file-mapping/session");
  EXPECT_EQ(body["port"].get<int>(), 47999);
  EXPECT_EQ(body["session_token"].get<std::string>(), "abc123");
}

TEST(FileMappingHttp, CapabilityStripsTokenFromExplicitSessionUrl) {
  file_mapping_http::capability_state_t state;
  state.enabled = true;
  state.listening = true;
  state.session_url = "wss://127.0.0.1:47999/api/v1/file-mapping/session?token=abc123";
  state.session_token = "abc123";

  auto response = file_mapping_http::make_capability_response(state);
  auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(
    body["session_url"].get<std::string>(),
    "wss://127.0.0.1:47999/api/v1/file-mapping/session");
}

TEST(FileMappingHttp, SessionPlaceholderRequiresUpgrade) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  auto response = file_mapping_http::make_session_placeholder_response(headers);

  EXPECT_EQ(response.status, SimpleWeb::StatusCode::client_error_upgrade_required);
  auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body["error"].get<std::string>(), "upgrade_required");
  EXPECT_NE(response.headers.find("Upgrade"), response.headers.end());
}

TEST(FileMappingHttp, SessionPlaceholderReservesWebsocketEndpoint) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Upgrade", "websocket");
  auto response = file_mapping_http::make_session_placeholder_response(headers);

  EXPECT_EQ(response.status, SimpleWeb::StatusCode::server_error_not_implemented);
  auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body["error"].get<std::string>(), "session_not_implemented");
}
