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

TEST(NetworkExternalResponse, AMalformedAnswerIsABadAnswerNeverAnException) {
  for (const char *body : {
         R"({"v":null,"tcp":[]})",
         R"({"v":"1","tcp":[{"role":"http","port":60000,"state":"open"}]})",
         R"({"v":1,"tcp":[{"role":5,"port":60000,"state":"open"}]})",
         R"({"v":1,"tcp":[{"role":"http","port":"60000","state":"open"}]})",
         R"({"v":1,"tcp":[{"role":"http","port":70000,"state":"open"}]})",
         R"({"v":1,"tcp":[{"role":"http","port":60000,"state":"open","identity":7}]})",
         R"({"v":1,"tcp":[{"role":"http","port":60000,"state":"open","ms":"slow"}]})",
         R"([])",
         "not json",
       }) {
    const auto result = parse_response(200, 0, body);
    EXPECT_EQ(result.availability, availability_e::unavailable) << body;
    EXPECT_EQ(result.reason, "bad_answer") << body;
    EXPECT_TRUE(result.tcp.empty()) << body;
  }
  // a refusal whose fields have the wrong type is not a refusal of the target
  const auto odd = parse_response(403, 0, R"({"error":7,"reason":false})");
  EXPECT_EQ(odd.availability, availability_e::unavailable);
}

TEST(NetworkExternalResponse, AStateThatTheProtocolDoesNotKnowIsNoEvidenceAndNotAClosedPort) {
  const auto result = parse_response(200, 0, R"({"v":1,"tcp":[{"role":"http","port":47989,"state":"error"}]})");
  EXPECT_EQ(result.availability, availability_e::unavailable);
  EXPECT_EQ(result.reason, "bad_answer");
  EXPECT_EQ(tcp_summary(result), "unknown");
  for (const char *state : { "open", "closed", "timeout", "blocked" }) {
    const auto known = parse_response(200, 0, std::string(R"({"v":1,"tcp":[{"role":"http","port":47989,"state":")") + state + R"("}]})");
    EXPECT_EQ(known.availability, availability_e::answered) << state;
  }
}

TEST(NetworkExternalProbe, ARequestThatThrowsIsNoAnswer) {
  const auto result = probe("https://service.example", ask(), [](const std::string &, const std::string &, std::string &, long &) -> int { throw std::runtime_error("down"); });
  EXPECT_EQ(result.availability, availability_e::unavailable);
  EXPECT_EQ(result.reason, "no_answer");
}

TEST(NetworkExternalResponse, TheAddressTheServiceSawIsReadFromAnAnswerAndFromARefusal) {
  const auto answered = parse_response(200, 0, R"({"v":1,"caller":"203.0.113.9","tcp":[{"role":"http","port":60000,"state":"open","identity":"match"}]})");
  EXPECT_EQ(answered.availability, availability_e::answered);
  EXPECT_EQ(answered.caller, "203.0.113.9");
  const auto refused = parse_response(403, 0, R"({"error":"target_not_caller","reason":"ip_mismatch","caller":"198.51.100.7"})");
  EXPECT_EQ(refused.availability, availability_e::refused);
  EXPECT_EQ(refused.caller, "198.51.100.7");
  // an older service says nothing: no address, no harm
  EXPECT_TRUE(parse_response(200, 0, ANSWER).caller.empty());
  // whatever it says is cut to a short text
  EXPECT_LE(parse_response(403, 0, std::string(R"({"error":"target_not_caller","caller":")") + std::string(500, 'x') + R"("})").caller.size(), 64U);
}

TEST(NetworkExternalEndpoint, TheConfiguredServiceOrTheBuiltInOne) {
  EXPECT_EQ(endpoint_of(""), "https://reach.alkaidlab.com");
  EXPECT_EQ(endpoint_of("   "), "https://reach.alkaidlab.com");
  EXPECT_EQ(endpoint_of("https://reach.example.net/"), "https://reach.example.net");
  EXPECT_EQ(endpoint_of("http://127.0.0.1:8787"), "http://127.0.0.1:8787");
}

TEST(NetworkExternalCache, AnAnswerIsKeptTenMinutesAndAClosedPortOneMinute) {
  cache_t cache;
  auto request = ask();
  const auto key = cache_key(request);
  EXPECT_EQ(key, "home.example.net|https:59995|http:60000|rtsp:60021");
  EXPECT_FALSE(cache.find(key, 1000).has_value());
  const auto good = parse_response(200, 0, ANSWER);
  cache.put(key, good, 1000);
  EXPECT_TRUE(cache.find(key, 1599).has_value());
  EXPECT_FALSE(cache.find(key, 1600).has_value()) << "ten minutes";
  const auto closed = parse_response(200, 0, R"({"v":1,"tcp":[{"role":"http","port":60000,"state":"closed"},{"role":"https","port":59995,"state":"timeout"}]})");
  EXPECT_EQ(ttl_seconds(closed), 60);
  cache.put(key, closed, 2000);
  EXPECT_TRUE(cache.find(key, 2059).has_value());
  EXPECT_FALSE(cache.find(key, 2060).has_value()) << "a port that was closed is asked again after a minute";
  // a service out of budget or silent is not asked again for ten minutes; one that says when to come back is not asked before
  EXPECT_EQ(ttl_seconds(parse_response(503, 0, "")), 600);
  EXPECT_EQ(ttl_seconds(parse_response(0, 0, "")), 600);
  EXPECT_EQ(ttl_seconds(parse_response(429, 120, "")), 120);
  EXPECT_EQ(ttl_seconds(parse_response(429, 5, "")), 60);
  EXPECT_EQ(ttl_seconds(parse_response(429, 99999, "")), 600);
  EXPECT_EQ(ttl_seconds(parse_response(403, 0, R"({"error":"target_not_caller","reason":"ip_mismatch"})")), 60) << "a refusal may be fixed in the next minute";
}

TEST(NetworkExternalCache, KeepsAFewAddressesAndForgetsTheOldest) {
  cache_t cache;
  const auto answer = parse_response(200, 0, ANSWER);
  for (int i = 0; i < 40; ++i) cache.put("host" + std::to_string(i), answer, 100);
  int kept = 0;
  for (int i = 0; i < 40; ++i) kept += cache.find("host" + std::to_string(i), 101).has_value() ? 1 : 0;
  EXPECT_LE(kept, 16);
  EXPECT_TRUE(cache.find("host39", 101).has_value()) << "the newest stays";
  cache.clear();
  EXPECT_FALSE(cache.find("host39", 101).has_value());
}

TEST(NetworkExternalResponse, AServiceThatHasUsedUpItsDayIsToldApartFromOneThatIsOff) {
  const auto budget = parse_response(503, 0, R"({"error":"unavailable","reason":"budget"})");
  EXPECT_EQ(budget.availability, availability_e::unavailable);
  EXPECT_EQ(budget.reason, "budget");
  EXPECT_EQ(parse_response(503, 0, R"({"error":"unavailable","reason":"disabled"})").reason, "disabled");
  EXPECT_EQ(parse_response(503, 0, "").reason, "disabled");
  EXPECT_EQ(parse_response(404, 0, R"({"reason":"budget"})").reason, "disabled") << "only a 503 says it";
}

TEST(NetworkExternalCache, AnyConclusionButReachedIsKeptOneMinute) {
  // an HTTP port found closed beside an HTTPS port that is open: the check says "closed", so the answer is not kept ten minutes
  const auto mixed = parse_response(200, 0, R"({"v":1,"tcp":[{"role":"http","port":60000,"state":"closed"},{"role":"https","port":59995,"state":"open"}]})");
  EXPECT_EQ(tcp_summary(mixed), "closed");
  EXPECT_EQ(ttl_seconds(mixed), 60);
  // another device answering is not kept long either, and a host that was reached is
  const auto other = parse_response(200, 0, R"({"v":1,"tcp":[{"role":"http","port":60000,"state":"open","identity":"mismatch"}]})");
  EXPECT_EQ(ttl_seconds(other), 60);
  EXPECT_EQ(ttl_seconds(parse_response(200, 0, ANSWER)), 600);
}

TEST(NetworkExternalCache, TheOldestAnswerIsTheOneThatIsForgotten) {
  cache_t cache;
  const auto answer = parse_response(200, 0, ANSWER);
  // stored in the reverse order of the keys: the first key in the order of the keys is the newest
  for (int i = 39; i >= 0; --i) cache.put("host" + std::string(i < 10 ? "0" : "") + std::to_string(i), answer, 100);
  EXPECT_FALSE(cache.find("host39", 101).has_value()) << "stored first, forgotten first";
  EXPECT_TRUE(cache.find("host00", 101).has_value()) << "stored last, kept even though it sorts first";
}
