/**
 * @file tests/unit/test_pairing_pending.cpp
 * @brief Pending-PIN lifecycle, long-poll and boundary tests.
 */
#include "src/http/client.h"
#include "src/http/server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include "src/config.h"
#include "src/confighttp/pairing.h"
#include "src/crypto.h"
#include "src/nvhttp.h"
#include "src/nvhttp/pairing.h"
#include "src/utility.h"

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  struct PairRequest {
    std::jthread thread;
    std::future<std::shared_ptr<web::ClientResponse>> response;
  };

  class PairingPending: public testing::Test {
  protected:
    fs::path directory;
    std::unique_ptr<confighttp::https_server_t> api_server;
    std::unique_ptr<web::Server<web::HTTP>> pair_server;
    std::thread api_listener;
    std::thread pair_listener;
    unsigned short api_port = 0;
    unsigned short pair_port = 0;
    std::string previous_username;
    std::string previous_password;
    std::string previous_salt;
    bool previous_pin_stdin = false;
    std::vector<std::unique_ptr<PairRequest>> pair_requests;

    template <class Server>
    unsigned short
    start_server(Server &server, std::thread &listener) {
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.thread_pool_size = 4;
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto signaled = std::make_shared<std::atomic_bool>(false);
      auto port = ready->get_future();
      listener = std::thread([&server, ready, signaled] {
        try {
          server.start([ready, signaled](unsigned short bound_port) {
            signaled->store(true);
            ready->set_value(bound_port);
          });
        }
        catch (...) {
          if (!signaled->exchange(true)) {
            ready->set_exception(std::current_exception());
          }
        }
      });
      if (port.wait_for(10s) != std::future_status::ready) {
        throw std::runtime_error("test listener did not start");
      }
      return port.get();
    }

    void
    SetUp() override {
      previous_username = config::sunshine.username;
      previous_password = config::sunshine.password;
      previous_salt = config::sunshine.salt;
      previous_pin_stdin = config::sunshine.flags[config::flag::PIN_STDIN];
      config::sunshine.username = "pairing-pending-test";
      config::sunshine.salt = "pairing-pending-test-salt";
      config::sunshine.password = util::hex(crypto::hash("pairing-test-password" + config::sunshine.salt)).to_string();
      config::sunshine.flags[config::flag::PIN_STDIN] = false;
      nvhttp::clear_preset_pin();
      nvhttp::reset_pairing_sessions_for_test();
      nvhttp::set_pending_pair_timeout_for_test(5s);

      directory = fs::temp_directory_path() /
                  ("sunshine-pairing-pending-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      fs::create_directories(directory);
      const auto credentials = crypto::gen_creds("localhost", 2048);
      std::ofstream(directory / "cert.pem") << credentials.x509;
      std::ofstream(directory / "key.pem") << credentials.pkey;

      api_server = std::make_unique<confighttp::https_server_t>(
        (directory / "cert.pem").string(), (directory / "key.pem").string());
      confighttp::pairing::register_routes(*api_server);
      api_port = start_server(*api_server, api_listener);

      pair_server = std::make_unique<web::Server<web::HTTP>>();
      pair_server->resource["^/pair$"]["GET"] = nvhttp::pairing::pair_http;
      pair_port = start_server(*pair_server, pair_listener);
    }

    void
    TearDown() override {
      for (const auto &request : nvhttp::get_pending_pair_requests().requests) {
        nvhttp::reject_pending_pair_request(request.id);
      }
      pair_requests.clear();

      if (pair_server) {
        pair_server->stop();
      }
      if (api_server) {
        api_server->stop();
      }
      if (pair_listener.joinable()) {
        pair_listener.join();
      }
      if (api_listener.joinable()) {
        api_listener.join();
      }
      pair_server.reset();
      api_server.reset();

      nvhttp::reset_pairing_sessions_for_test();
      config::sunshine.flags[config::flag::PIN_STDIN] = previous_pin_stdin;
      config::sunshine.username = previous_username;
      config::sunshine.password = previous_password;
      config::sunshine.salt = previous_salt;
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }

    std::unique_ptr<web::Client<web::HTTPS>>
    api(int timeout_seconds = 5) {
      auto client = std::make_unique<web::Client<web::HTTPS>>(
        "127.0.0.1:" + std::to_string(api_port), false);
      client->config.timeout = timeout_seconds;
      client->config.timeout_connect = 5;
      return client;
    }

    static web::CaseInsensitiveMultimap
    credentials() {
      return { { "Authorization", "Basic cGFpcmluZy1wZW5kaW5nLXRlc3Q6cGFpcmluZy10ZXN0LXBhc3N3b3Jk" } };
    }

    PairRequest &
    startPair(const std::string &uniqueid, const std::string &name, const std::string &name_argument = "clientname") {
      auto client = std::make_unique<web::Client<web::HTTP>>(
        "127.0.0.1:" + std::to_string(pair_port), false);
      client->config.timeout = 10;

      auto promise = std::make_shared<std::promise<std::shared_ptr<web::ClientResponse>>>();
      auto future = promise->get_future();
      auto holder = std::make_unique<PairRequest>();
      holder->response = std::move(future);
      holder->thread = std::jthread(
        [client = std::move(client), promise, uniqueid, name, name_argument]() mutable {
          try {
            const auto target = "/pair?uniqueid=" + web::Percent::encode(uniqueid) +
                                "&clientcert=3000" +
                                "&salt=00112233445566778899aabbccddeeff" +
                                "&phrase=getservercert&" + name_argument + "=" + web::Percent::encode(name);
            promise->set_value(client->request("GET", target));
          }
          catch (...) {
            promise->set_exception(std::current_exception());
          }
        });
      pair_requests.push_back(std::move(holder));
      return *pair_requests.back();
    }

    bool
    waitForPendingCount(std::size_t expected, std::chrono::milliseconds timeout = 5s) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (nvhttp::get_pending_pair_requests().requests.size() == expected) {
          return true;
        }
        std::this_thread::sleep_for(10ms);
      }
      return nvhttp::get_pending_pair_requests().requests.size() == expected;
    }

    std::pair<std::string, nlohmann::json>
    getPending(const std::string &query = "", int timeout_seconds = 5) {
      auto client = api(timeout_seconds);
      const auto response = client->request("GET", "/api/pairing/pending" + query, "", credentials());
      if (response->status_code != "200 OK") {
        return { response->status_code, nlohmann::json::object() };
      }
      return { response->status_code, nlohmann::json::parse(response->content.string()) };
    }

    std::shared_ptr<web::ClientResponse>
    deletePending(const std::string &id) {
      auto client = api();
      return client->request("DELETE", "/api/pairing/pending/" + id, "", credentials());
    }

    void
    expectPairStatus(PairRequest &pair, int expected_status) {
      ASSERT_EQ(pair.response.wait_for(5s), std::future_status::ready);
      const auto response = pair.response.get();
      ASSERT_TRUE(response);
      pt::ptree tree;
      std::istringstream stream(response->content.string());
      ASSERT_NO_THROW(pt::read_xml(stream, tree));
      EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), expected_status);
    }
  };
}  // namespace

TEST_F(PairingPending, ListsSanitisesAndPinsOneRequest) {
  std::string device_name = "  Living";
  device_name.push_back('\x01');
  device_name += "Room  ";
  auto &pair = startPair("client-one", device_name);
  ASSERT_TRUE(waitForPendingCount(1));

  const auto [status, body] = getPending();
  EXPECT_EQ(status, "200 OK");
  EXPECT_EQ(body["pending"], true);
  ASSERT_EQ(body["requests"].size(), 1);
  const auto &request = body["requests"][0];
  EXPECT_NE(request["id"].get<std::string>(), "client-one");
  EXPECT_EQ(request["id"].get<std::string>().size(), 36);
  EXPECT_EQ(request["device_name"], "Living Room");
  EXPECT_GT(request["started_at"].get<std::int64_t>(), 0);
  EXPECT_GT(request["expires_at"].get<std::int64_t>(), request["started_at"].get<std::int64_t>());

  EXPECT_TRUE(nvhttp::pin_for_pending_request(
    request["id"].get<std::string>(), "1234", "Living Room"));
  expectPairStatus(pair, 200);

  const auto [empty_status, empty_body] = getPending();
  EXPECT_EQ(empty_status, "200 OK");
  EXPECT_EQ(empty_body["pending"], false);
  EXPECT_TRUE(empty_body["requests"].empty());
}

TEST_F(PairingPending, ExpiresWithTheSessionTimeout) {
  nvhttp::set_pending_pair_timeout_for_test(200ms);
  auto &pair = startPair("client-expire", "Expiring client");
  ASSERT_TRUE(waitForPendingCount(1));
  std::this_thread::sleep_for(450ms);

  const auto [status, body] = getPending();
  EXPECT_EQ(status, "200 OK");
  EXPECT_EQ(body["pending"], false);
  EXPECT_TRUE(body["requests"].empty());
  expectPairStatus(pair, 408);
}

TEST_F(PairingPending, MultipleRequestsAndRejection) {
  auto &first = startPair("client-first", "First client");
  ASSERT_TRUE(waitForPendingCount(1));
  const auto first_id = nvhttp::get_pending_pair_requests().requests[0].id;

  auto &second = startPair("client-second", "Second client");
  ASSERT_TRUE(waitForPendingCount(2));
  const auto snapshot = nvhttp::get_pending_pair_requests();
  ASSERT_EQ(snapshot.requests.size(), 2);
  EXPECT_EQ(snapshot.requests[0].id, first_id);
  EXPECT_EQ(snapshot.requests[1].device_name, "Second client");

  const auto delete_response = deletePending(snapshot.requests[1].id);
  EXPECT_EQ(delete_response->status_code, "200 OK");
  expectPairStatus(second, 403);

  const auto [status, body] = getPending();
  EXPECT_EQ(status, "200 OK");
  ASSERT_EQ(body["requests"].size(), 1);
  EXPECT_EQ(body["requests"][0]["id"], first_id);

  EXPECT_TRUE(nvhttp::pin_for_pending_request(first_id, "4321", "First client"));
  expectPairStatus(first, 200);
}

TEST_F(PairingPending, PinWithoutIdTargetsOldest) {
  auto &first = startPair("legacy-first", "Legacy first");
  ASSERT_TRUE(waitForPendingCount(1));
  const auto first_id = nvhttp::get_pending_pair_requests().requests[0].id;
  auto &second = startPair("legacy-second", "Legacy second");
  ASSERT_TRUE(waitForPendingCount(2));

  EXPECT_TRUE(nvhttp::pin_for_pending_request({}, "2468", "Legacy first"));
  expectPairStatus(first, 200);

  const auto snapshot = nvhttp::get_pending_pair_requests();
  ASSERT_EQ(snapshot.requests.size(), 1);
  EXPECT_NE(snapshot.requests[0].id, first_id);
  nvhttp::reject_pending_pair_request(snapshot.requests[0].id);
  expectPairStatus(second, 403);
}

TEST_F(PairingPending, LongPollReturnsOnChangeAndHonoursTimeout) {
  auto &first = startPair("poll-first", "Poll first");
  ASSERT_TRUE(waitForPendingCount(1));
  const auto generation = nvhttp::get_pending_pair_requests().generation;

  auto changed = std::async(std::launch::async, [this, generation] {
    auto client = api(10);
    const auto started = std::chrono::steady_clock::now();
    const auto response = client->request(
      "GET", "/api/pairing/pending?wait=5&since=" + std::to_string(generation), "", credentials());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return std::make_tuple(
      response->status_code,
      nlohmann::json::parse(response->content.string()),
      elapsed);
  });

  std::this_thread::sleep_for(150ms);
  auto &second = startPair("poll-second", "Poll second");
  ASSERT_TRUE(waitForPendingCount(2));

  ASSERT_EQ(changed.wait_for(3s), std::future_status::ready);
  const auto [status, body, elapsed] = changed.get();
  EXPECT_EQ(status, "200 OK");
  EXPECT_EQ(body["pending"], true);
  EXPECT_EQ(body["requests"].size(), 2);
  EXPECT_LT(elapsed, 2s);

  const auto timeout_generation = body["generation"].get<std::uint64_t>();
  const auto started = std::chrono::steady_clock::now();
  const auto [timeout_status, timeout_body] = getPending(
    "?wait=1&since=" + std::to_string(timeout_generation), 5);
  const auto timeout_elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_EQ(timeout_status, "200 OK");
  EXPECT_EQ(timeout_body["requests"].size(), 2);
  EXPECT_GE(timeout_elapsed, 700ms);
  EXPECT_LT(timeout_elapsed, 2500ms);

  for (const auto &request : nvhttp::get_pending_pair_requests().requests) {
    nvhttp::reject_pending_pair_request(request.id);
  }
  expectPairStatus(first, 403);
  expectPairStatus(second, 403);
}

TEST_F(PairingPending, RateLimitsPendingPerSourceIp) {
  std::vector<PairRequest *> pairs;
  for (int index = 0; index < 4; ++index) {
    pairs.push_back(&startPair("rate-" + std::to_string(index), "Rate " + std::to_string(index)));
  }
  ASSERT_TRUE(waitForPendingCount(4));

  auto &rejected = startPair("rate-rejected", "Rate rejected");
  expectPairStatus(rejected, 429);
  EXPECT_EQ(nvhttp::get_pending_pair_requests().requests.size(), 4);

  for (const auto &request : nvhttp::get_pending_pair_requests().requests) {
    nvhttp::reject_pending_pair_request(request.id);
  }
  for (auto *pair : pairs) {
    expectPairStatus(*pair, 403);
  }
}

TEST_F(PairingPending, RequiresAuthenticationAndRejectsCrossSiteReads) {
  config::sunshine.username.clear();
  {
    auto client = api();
    const auto response = client->request("GET", "/api/pairing/pending");
    EXPECT_EQ(response->status_code, "401 Unauthorized");
  }

  config::sunshine.username = "pairing-pending-test";
  auto client = api();
  const auto response = client->request(
    "GET", "/api/pairing/pending", "", { { "Origin", "https://evil.example" } });
  EXPECT_EQ(response->status_code, "403 Forbidden");
  EXPECT_EQ(nlohmann::json::parse(response->content.string())["error"], "Cross-site request denied");
}

TEST_F(PairingPending, ValidatesPinRequestAndOptionalId) {
  confighttp::pairing::pin_request_t request {
    .pin = "1234",
    .name = "Living Room",
    .id = "01234567-89ab-cdef-0123-456789abcdef",
  };
  EXPECT_TRUE(confighttp::pairing::validate_pin_request(request).empty());

  request.id = "bad id";
  EXPECT_EQ(
    confighttp::pairing::validate_pin_request(request),
    "id must be 1-64 characters of [A-Za-z0-9_-]");

  request.id = std::string(65, 'a');
  EXPECT_EQ(
    confighttp::pairing::validate_pin_request(request),
    "id must be 1-64 characters of [A-Za-z0-9_-]");

  request.id.clear();
  request.pin = "12";
  EXPECT_EQ(confighttp::pairing::validate_pin_request(request), "PIN must be 4-8 digits");

  request.pin = "1234";
  request.name.clear();
  EXPECT_EQ(
    confighttp::pairing::validate_pin_request(request),
    "Client name must be 1-256 characters");
}

// The standard Moonlight exchange (macOS Enhanced among the clients) names the device with devicename; the Foundation extension uses clientname.
TEST_F(PairingPending, DevicenameNamesThePendingRequest) {
  startPair("client-devicename", "macos-test-host.local", "devicename");
  ASSERT_TRUE(waitForPendingCount(1));
  const auto [status, body] = getPending();
  ASSERT_EQ(body["requests"].size(), 1);
  EXPECT_EQ(body["requests"][0]["device_name"], "macos-test-host.local");
}

TEST_F(PairingPending, TheSharedMoonlightDevicenameNamesNothing) {
  startPair("client-roth", "roth", "devicename");
  ASSERT_TRUE(waitForPendingCount(1));
  const auto [status, body] = getPending();
  ASSERT_EQ(body["requests"].size(), 1);
  EXPECT_EQ(body["requests"][0]["device_name"], "Named Zako");
}

TEST_F(PairingPending, ClientnameWinsOverDevicename) {
  auto client = std::make_unique<web::Client<web::HTTP>>("127.0.0.1:" + std::to_string(pair_port), false);
  client->config.timeout = 10;
  auto request = std::async(std::launch::async, [client = std::move(client)]() {
    return client->request("GET", "/pair?uniqueid=both&clientcert=3000&salt=00112233445566778899aabbccddeeff&phrase=getservercert&devicename=roth&clientname=Living%20room");
  });
  ASSERT_TRUE(waitForPendingCount(1));
  const auto [status, body] = getPending();
  ASSERT_EQ(body["requests"].size(), 1);
  EXPECT_EQ(body["requests"][0]["device_name"], "Living room");
  nvhttp::reject_pending_pair_request(body["requests"][0]["id"].get<std::string>());
  request.wait_for(5s);
}

TEST_F(PairingPending, PairingTheSameCertificateAgainKeepsOneEntryAndItsUuid) {
  const auto saved_state = config::nvhttp.file_state;
  config::nvhttp.file_state = (directory / "state.json").string();
  std::ofstream(config::nvhttp.file_state) << R"({"root":{"uniqueid":"dedupe","named_devices":[]}})";
  nvhttp::pairing::load_state();
  auto restore = util::fail_guard([&]() {
    nvhttp::erase_all_clients();
    config::nvhttp.file_state = saved_state;
  });

  nvhttp::add_paired_client_for_test("Named Zako", "CERT-A", {});
  ASSERT_EQ(nvhttp::get_all_clients().size(), 1U);
  const auto uuid = nvhttp::get_all_clients().at(0).at("uuid").get<std::string>();

  client_family::record_t mac;
  mac.family = client_family::family_e::moonlight_macos_enhanced;
  mac.os = "macos";
  mac.last_seen = 1762243200;
  nvhttp::add_paired_client_for_test("Living room", "CERT-A", mac);
  ASSERT_EQ(nvhttp::get_all_clients().size(), 1U);
  EXPECT_EQ(nvhttp::get_all_clients().at(0).at("uuid"), uuid);
  EXPECT_EQ(nvhttp::get_all_clients().at(0).at("name"), "Living room");
  EXPECT_EQ(nvhttp::get_all_clients().at(0).at("family"), "moonlight-macos-enhanced");

  // A pairing that observed nothing about the family does not erase what the entry already knows.
  nvhttp::add_paired_client_for_test("Living room", "CERT-A", {});
  ASSERT_EQ(nvhttp::get_all_clients().size(), 1U);
  EXPECT_EQ(nvhttp::get_all_clients().at(0).at("family"), "moonlight-macos-enhanced");

  // Another certificate is another device.
  nvhttp::add_paired_client_for_test("Phone", "CERT-B", {});
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2U);
}

TEST(PairingStateCompat, AReleasedStateFileLoadsEveryNamedDevice) {
  const auto root = fs::path(__FILE__).parent_path().parent_path() / "fixtures" / "config_upgrade";
  for (const char *era : { "2026.1031", "2026.1112" }) {
    SCOPED_TRACE(era);
    const auto source = root / era / "sunshine_state.json";
    const auto copy = fs::temp_directory_path() / ("sunshine-state-upgrade-" + std::string(era) + ".json");
    std::error_code ignored;
    fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ignored);
    ASSERT_FALSE(ignored) << source;

    const auto saved_state = config::nvhttp.file_state;
    config::nvhttp.file_state = copy.string();
    auto restore = util::fail_guard([&]() {
      nvhttp::erase_all_clients();
      config::nvhttp.file_state = saved_state;
      std::error_code remove_error;
      fs::remove(copy, remove_error);
    });

    nvhttp::pairing::load_state();
    const auto clients = nvhttp::get_all_clients();
    ASSERT_EQ(clients.size(), 2U);
    EXPECT_EQ(clients[0]["name"], "Tablet");
    EXPECT_EQ(clients[0]["uuid"], "8f14e45f-ceea-467a-9e4a-2b1f6f0a1c11");
    EXPECT_TRUE(clients[0]["enabled"]);
    EXPECT_EQ(clients[0]["lastSessionEnded"], 1700000000);
    // The family was added after these releases; an old entry keeps its identity
    // and reads as unknown until a later pairing observes one.
    EXPECT_EQ(clients[0]["family"], "unknown");
    EXPECT_TRUE(clients[0]["version"].is_null());
    EXPECT_TRUE(clients[0]["os"].is_null());
    EXPECT_EQ(clients[1]["name"], "Phone");
    EXPECT_FALSE(clients[1]["enabled"]);
    EXPECT_EQ(clients[1]["lastSessionEnded"], 1700000200);
    EXPECT_EQ(clients[1]["family"], "unknown");
    EXPECT_TRUE(clients[1]["version"].is_null());
  }
}
