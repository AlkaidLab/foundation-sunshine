/**
 * @file tests/unit/test_client_sessions.cpp
 * @brief Removing all paired clients ends the sessions of each of them.
 */
#include "src/confighttp/client_sessions.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using confighttp::clients::terminate_sessions_of_all;

TEST(ClientSessions, EndsTheSessionsOfEveryPairedClient) {
  const auto clients = nlohmann::json::parse(R"([
    {"name": "A", "uuid": "u-1", "enabled": true},
    {"name": "B", "uuid": "u-2", "enabled": false},
    {"name": "C", "uuid": "u-3", "enabled": true}
  ])");
  std::vector<std::string> ended;
  const auto count = terminate_sessions_of_all(clients, [&](const std::string &uuid) { ended.push_back(uuid); });
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(ended, (std::vector<std::string> { "u-1", "u-2", "u-3" }));
}

TEST(ClientSessions, StoppedClientsAreIncludedBecauseTheyCanStillHoldASession) {
  const auto clients = nlohmann::json::parse(R"([{"uuid": "u-1", "enabled": false}])");
  std::vector<std::string> ended;
  terminate_sessions_of_all(clients, [&](const std::string &uuid) { ended.push_back(uuid); });
  EXPECT_EQ(ended.size(), 1u);
}

TEST(ClientSessions, SkipsEntriesWithoutAUsableUuid) {
  const auto clients = nlohmann::json::parse(R"([{"name": "A"}, {"uuid": ""}, {"uuid": 7}, "x", {"uuid": "u-9"}])");
  std::vector<std::string> ended;
  const auto count = terminate_sessions_of_all(clients, [&](const std::string &uuid) { ended.push_back(uuid); });
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(ended, (std::vector<std::string> { "u-9" }));
}

TEST(ClientSessions, NothingToEndForAnEmptyOrMalformedList) {
  int calls = 0;
  EXPECT_EQ(terminate_sessions_of_all(nlohmann::json::array(), [&](const std::string &) { ++calls; }), 0u);
  EXPECT_EQ(terminate_sessions_of_all(nlohmann::json::object(), [&](const std::string &) { ++calls; }), 0u);
  EXPECT_EQ(calls, 0);
}
