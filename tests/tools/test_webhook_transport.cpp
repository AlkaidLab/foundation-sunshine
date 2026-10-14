/**
 * @file tests/tools/test_webhook_transport.cpp
 * @brief Real Webhook dispatcher regression, isolated from host state.
 */
#include "src/config.h"
#include "src/crypto.h"
#include "src/http/server.h"
#include "src/logging.h"
#include "src/webhook/webhook.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

boost::log::sources::severity_logger<int> verbose, debug, info, warning, error, fatal;
namespace config {
  sunshine_t sunshine {};
}
namespace platf {
  std::string
  get_host_name() { return "webhook-regression-host"; }
}  // namespace platf
namespace {
  using namespace std::chrono_literals;
  class Receiver {
    web::Server<web::HTTP> plain;
    web::Server<web::HTTPS> tls;
    std::thread plain_thread, tls_thread;
    std::mutex mutex;
    std::vector<std::shared_ptr<web::Server<web::HTTP>::Response>> retained;
    std::vector<std::string> retry_ids;
    template <class Server>
    unsigned short
    start(Server &server, std::thread &thread) {
      server.config.address = "127.0.0.1";
      server.config.thread_pool_size = 2;
      auto ready = std::make_shared<std::promise<unsigned short>>();
      auto future = ready->get_future();
      thread = std::thread([&server, ready] {
        bool announced = false;
        try {
          server.start([&](auto port) { announced = true; ready->set_value(port); });
        }
        catch (...) {
          if (!announced) ready->set_exception(std::current_exception());
        }
      });
      if (future.wait_for(5s) != std::future_status::ready) throw std::runtime_error("Webhook receiver did not start");
      return future.get();
    }

  public:
    std::atomic_int retry_calls { 0 }, bad_calls { 0 }, dropped_calls { 0 }, idle_calls { 0 }, tls_calls { 0 };
    unsigned short plain_port = 0, tls_port = 0;
    Receiver(const std::filesystem::path &directory): tls((directory / "cert.pem").string(), (directory / "key.pem").string()) {
      plain.resource["^/accepted$"]["POST"] = [&](auto response, auto request) {
        const auto payload = nlohmann::json::parse(request->content.string());
        EXPECT_EQ(payload.at("event_type"), "webhook_test");
        EXPECT_FALSE(request->header.find("X-Webhook-Delivery")->second.empty());
        response->write(web::StatusCode::accepted, { { "Content-Length", "1000000000" } });
        response->send();
        std::lock_guard lock(mutex);
        retained.push_back(response);
      };
      plain.resource["^/retry$"]["POST"] = [&](auto response, auto request) {
        const auto count = ++retry_calls;
        {
          std::lock_guard lock(mutex);
          retry_ids.push_back(request->header.find("X-Webhook-Delivery")->second);
        }
        response->write(count == 1 ? web::StatusCode::too_many_requests : count == 2 ? web::StatusCode::service_unavailable :
                                                                                       web::StatusCode::no_content,
          { { "Retry-After", "0" } });
      };
      plain.resource["^/bad$"]["POST"] = [&](auto response, auto) { ++bad_calls; response->write(web::StatusCode::bad_request); };
      plain.resource["^/drop$"]["POST"] = [&](auto response, auto) { ++dropped_calls; response->abort(); };
      plain.resource["^/huge-header$"]["POST"] = [](auto response, auto) { response->write("body", { { "X-Wide", std::string(17 * 1024, 'x') } }); };
      plain.resource["^/idle$"]["POST"] = [&](auto response, auto) { std::lock_guard lock(mutex); retained.push_back(response); ++idle_calls; };
      tls.resource["^/tls$"]["POST"] = [&](auto response, auto) { ++tls_calls; response->write(web::StatusCode::no_content); };
      try {
        plain_port = start(plain, plain_thread);
        tls_port = start(tls, tls_thread);
      }
      catch (...) {
        plain.stop();
        tls.stop();
        if (plain_thread.joinable()) plain_thread.join();
        if (tls_thread.joinable()) tls_thread.join();
        throw;
      }
    }
    ~Receiver() {
      plain.stop();
      tls.stop();
      if (plain_thread.joinable()) plain_thread.join();
      if (tls_thread.joinable()) tls_thread.join();
    }
    std::string
    url(std::string path) const { return "http://127.0.0.1:" + std::to_string(plain_port) + path; }
    void
    assert_retry_ids() {
      std::lock_guard lock(mutex);
      ASSERT_EQ(retry_ids.size(), 3);
      EXPECT_EQ(retry_ids[0], retry_ids[1]);
      EXPECT_EQ(retry_ids[1], retry_ids[2]);
    }
  };
  webhook::delivery_result_t
  deliver(const std::string &url, int retries, bool skip_tls = false, std::chrono::milliseconds timeout = 2000ms) {
    auto completion = std::make_shared<std::promise<webhook::delivery_result_t>>();
    auto future = completion->get_future();
    if (!webhook::send_test_async({ url, skip_tls, timeout }, retries, [completion](auto result) { completion->set_value(result); })) throw std::runtime_error("Webhook test was not admitted");
    if (future.wait_for(12s) != std::future_status::ready) throw std::runtime_error("Webhook delivery did not complete");
    return future.get();
  }
}  // namespace
TEST(WebhookTransport, ReceiverStartupFailureJoinsThreads) {
  const auto missing = std::filesystem::temp_directory_path() / ("sunshine-webhook-missing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_FALSE(std::filesystem::exists(missing));
  EXPECT_THROW({ Receiver receiver(missing); }, boost::system::system_error);
}

// Shutdown closes process-wide admission, so all lifecycle scenarios share
// this isolated executable and end with the terminal shutdown assertions.
TEST(WebhookTransport, DeliveryPoliciesTlsAndCancellationUseTheRealDispatcher) {
  config::sunshine.locale = "en";
  const auto directory = std::filesystem::temp_directory_path() / ("sunshine-webhook-transport-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto credentials = crypto::gen_creds("localhost", 2048);
  std::ofstream(directory / "cert.pem") << credentials.x509;
  std::ofstream(directory / "key.pem") << credentials.pkey;
  {
    Receiver receiver(directory);
    auto runtime = webhook::init();
    ASSERT_TRUE(runtime);
    const auto before = std::chrono::steady_clock::now();
    auto result = deliver(receiver.url("/accepted"), 2);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.attempts, 1);
    EXPECT_EQ(result.http_status, 202);
    EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);
    result = deliver(receiver.url("/retry"), 2);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.attempts, 3);
    EXPECT_EQ(receiver.retry_calls, 3);
    receiver.assert_retry_ids();
    result = deliver(receiver.url("/bad"), 3);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 400);
    EXPECT_EQ(result.attempts, 1);
    EXPECT_EQ(receiver.bad_calls, 1);
    result = deliver(receiver.url("/drop"), 2);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.attempts, 3);
    EXPECT_EQ(receiver.dropped_calls, 3);
    result = deliver(receiver.url("/huge-header"), 0);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.http_status, 0);
    EXPECT_EQ(result.error, webhook::delivery_error_t::TRANSPORT);
    const auto tls_url = "https://localhost:" + std::to_string(receiver.tls_port) + "/tls";
    result = deliver(tls_url, 0, false, 5000ms);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(receiver.tls_calls, 0);
    result = deliver(tls_url, 0, true, 5000ms);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(receiver.tls_calls, 1);
    // Keep a short-budget TLS case independent of localhost's sequential
    // IPv6/IPv4 connection fallback on the Windows test host.
    result = deliver("https://127.0.0.1:" + std::to_string(receiver.tls_port) + "/tls", 0, true, 2000ms);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(receiver.tls_calls, 2);
    auto completed = std::make_shared<std::atomic_int>(0);
    for (int i = 0; i < 4; ++i) {
      ASSERT_TRUE(webhook::send_test_async({ receiver.url("/idle"), false, 15000ms }, 0, [completed](auto result) { EXPECT_EQ(result.error, webhook::delivery_error_t::CANCELLED); ++*completed; }));
    }
    const auto ready_deadline = std::chrono::steady_clock::now() + 3s;
    while (receiver.idle_calls < 2 && std::chrono::steady_clock::now() < ready_deadline) std::this_thread::sleep_for(10ms);
    EXPECT_EQ(receiver.idle_calls, 2);
    const auto stop_started = std::chrono::steady_clock::now();
    runtime.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_started, 3s);
    EXPECT_EQ(completed->load(), 4);
    EXPECT_FALSE(webhook::runtime_active());
    EXPECT_FALSE(webhook::ensure_running());
  }
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);
}
