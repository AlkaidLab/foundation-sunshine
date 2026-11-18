#include <gtest/gtest.h>

#include "src/network_external.h"

namespace {
  using namespace network_external;

  request_t
  ask() {
    request_t request;
    request.install_id = "3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11";
    request.uniqueid = "0123456789ABCDEF";
    request.target = "home.example.net";
    request.ports = { { "https", 59995 }, { "http", 60000 }, { "rtsp", 60021 } };
    return request;
  }

  const char *ANSWER = R"({"v":1,"target":{"kind":"name","matches_caller":true},"tcp":[
    {"role":"http","port":60000,"state":"open","identity":"match","ms":41},
    {"role":"https","port":59995,"state":"open","identity":"unverified","ms":38},
    {"role":"rtsp","port":60021,"state":"closed","identity":"unverified","ms":3}],"udp":{"state":"unverified"}})";
}  // namespace

TEST(NetworkExternalRequest, CarriesTheAddressTheUniqueidTheInstallAndThePortsOfTheHostTable) {
  const auto json = nlohmann::json::parse(build_request(ask()));
  EXPECT_EQ(json["v"], 1);
  EXPECT_EQ(json["target"], "home.example.net");
  EXPECT_EQ(json["uniqueid"], "0123456789ABCDEF");
  EXPECT_EQ(json["install_id"], "3f2b8a52-1c3e-4d0e-9d57-0a7c1f6b9e11");
  ASSERT_EQ(json["ports"].size(), 3U);
  EXPECT_EQ(json["ports"][1]["role"], "http");
  EXPECT_EQ(json["ports"][1]["port"], 60000);
  EXPECT_EQ(json.size(), 5U);  // nothing else leaves the computer
}

TEST(NetworkExternalResponse, AnAnswerIsAPerPortResultAndTheHttpPortsUniqueidDecides) {
  const auto result = parse_response(200, 0, ANSWER);
  ASSERT_EQ(result.availability, availability_e::answered);
  ASSERT_EQ(result.tcp.size(), 3U);
  EXPECT_EQ(result.tcp[0].identity, "match");
  EXPECT_EQ(tcp_summary(result), "reachable");
}

TEST(NetworkExternalResponse, AnotherHostOnTheAddressACloseHttpPortAndATimeoutAreToldApart) {
  const auto with = [](const char *state, const char *identity) {
    return parse_response(200, 0, std::string(R"({"v":1,"tcp":[{"role":"http","port":60000,"state":")") + state + R"(","identity":")" + identity + R"("}]})");
  };
  EXPECT_EQ(tcp_summary(with("open", "mismatch")), "another_host");
  EXPECT_EQ(tcp_summary(with("open", "unverified")), "unknown");
  EXPECT_EQ(tcp_summary(with("open", "not_http")), "unknown");
  EXPECT_EQ(tcp_summary(with("closed", "unverified")), "closed");
  EXPECT_EQ(tcp_summary(with("timeout", "unverified")), "timeout");
  EXPECT_EQ(tcp_summary(with("blocked", "unverified")), "closed");
}

TEST(NetworkExternalResponse, ARefusedTargetNamesItsReasonAndEveryOtherFailureIsJustNotAvailable) {
  const auto refused = parse_response(403, 0, R"({"error":"target_not_caller","reason":"dns_mismatch"})");
  EXPECT_EQ(refused.availability, availability_e::refused);
  EXPECT_EQ(refused.reason, "dns_mismatch");
  for (const auto &[status, reason] : std::vector<std::pair<int, std::string>> { { 0, "no_answer" }, { 429, "rate_limited" }, { 503, "disabled" }, { 404, "disabled" }, { 400, "bad_request" }, { 500, "bad_answer" } }) {
    const auto result = parse_response(status, 0, "{}");
    EXPECT_EQ(result.availability, availability_e::unavailable) << status;
    EXPECT_EQ(result.reason, reason) << status;
    EXPECT_EQ(tcp_summary(result), "unknown");
  }
  EXPECT_EQ(parse_response(200, 0, "not json").reason, "bad_answer");
  EXPECT_EQ(parse_response(200, 0, R"({"v":2,"tcp":[]})").reason, "bad_answer");
  EXPECT_EQ(parse_response(200, 0, R"({"v":1,"tcp":[]})").reason, "bad_answer");
  EXPECT_EQ(parse_response(429, 90, "{}").retry_after_s, 90);
}

TEST(NetworkExternalProbe, PostsToTheServiceAndReturnsItsAnswer) {
  std::string seen_url;
  std::string seen_body;
  const auto result = probe("https://api.example.test", ask(), [&](const std::string &url, const std::string &body, std::string &answer, long &) {
    seen_url = url;
    seen_body = body;
    answer = ANSWER;
    return 200;
  });
  EXPECT_EQ(seen_url, "https://api.example.test/v1/reach-probe");
  EXPECT_EQ(nlohmann::json::parse(seen_body)["target"], "home.example.net");
  EXPECT_EQ(result.availability, availability_e::answered);
  const auto down = probe("https://api.example.test", ask(), [](const std::string &, const std::string &, std::string &, long &) { return 0; });
  EXPECT_EQ(down.availability, availability_e::unavailable);
  EXPECT_EQ(down.reason, "no_answer");
}

TEST(NetworkExternalSetting, OnlyEnabledTurnsItOn) {
  EXPECT_TRUE(enabled("enabled"));
  for (const char *value : { "", "disabled", "true", "on", "Enabled" }) EXPECT_FALSE(enabled(value)) << value;
}
