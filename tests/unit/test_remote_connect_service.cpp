#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/remote_connect/easytier.h"
#include "src/remote_connect/service.h"

boost::log::sources::severity_logger<int> warning;

namespace {
  std::mutex runtime_mutex;
  std::condition_variable runtime_condition;
  bool block_runtime_start;
  bool runtime_start_entered;
  bool release_runtime_start;
  bool runtime_available;

  void
  reset_runtime_barrier() {
    std::lock_guard lock(runtime_mutex);
    block_runtime_start = true;
    runtime_start_entered = false;
    release_runtime_start = false;
  }

  bool
  wait_for_runtime_start() {
    using namespace std::chrono_literals;

    std::unique_lock lock(runtime_mutex);
    if (runtime_condition.wait_for(lock, 5s, []() { return runtime_start_entered; })) {
      return true;
    }

    // Never leave the async start blocked when the test barrier times out.
    release_runtime_start = true;
    lock.unlock();
    runtime_condition.notify_all();
    return false;
  }

  void
  allow_runtime_start() {
    {
      std::lock_guard lock(runtime_mutex);
      release_runtime_start = true;
    }
    runtime_condition.notify_all();
  }

  void
  configure_remote_connect() {
    remote_connect::stop();
    runtime_available = true;
    config::nvhttp = {};
    config::nvhttp.sunshine_name = "test-host";
    config::nvhttp.remote_connect_enabled = true;
    config::nvhttp.remote_connect_profile = "host-profile";
    config::nvhttp.remote_connect_virtual_ip = "100.64.10.1";
    config::nvhttp.remote_connect_network_name = "remote-network";
    config::nvhttp.remote_connect_network_secret = "remote-secret";
    config::nvhttp.remote_connect_peer = "udp://peer.example:11010";
  }
}  // namespace

namespace config {
  nvhttp_t nvhttp;

  bool
  update_config(const std::map<std::string, std::string> &) {
    return true;
  }
}  // namespace config

namespace remote_connect::easytier {
  struct runtime_t::state_t {
    bool running = false;
  };

  bool
  virtual_subnet_conflicts(const std::string &) {
    return false;
  }

  runtime_t::runtime_t():
      state_(std::make_unique<state_t>()) {
  }

  runtime_t::~runtime_t() = default;

  bool
  runtime_t::available() const {
    return runtime_available;
  }

  bool
  runtime_t::running(std::string &) {
    return state_->running;
  }

  bool
  runtime_t::start(const enrollment_t &, const std::string &, std::string &error) {
    if (!runtime_available) {
      error = installation_required_error;
      return false;
    }
    std::unique_lock lock(runtime_mutex);
    runtime_start_entered = true;
    runtime_condition.notify_all();
    if (block_runtime_start) {
      runtime_condition.wait(lock, []() { return release_runtime_start; });
    }
    state_->running = true;
    return true;
  }

  void
  runtime_t::stop() {
    state_->running = false;
  }
}  // namespace remote_connect::easytier

TEST(RemoteConnectService, MissingExternalRuntimeCannotBeEnabled) {
  configure_remote_connect();
  config::nvhttp.remote_connect_enabled = false;
  runtime_available = false;

  const auto result = remote_connect::set_enabled(true);

  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.status.enabled);
  EXPECT_FALSE(result.status.running);
  EXPECT_FALSE(result.status.available);
  EXPECT_NE(result.status.error.find("Install EasyTier separately"), std::string::npos);
}

TEST(RemoteConnectService, PairingSnapshotSerializesConcurrentDisable) {
  using namespace std::chrono_literals;

  configure_remote_connect();
  reset_runtime_barrier();

  auto pairing_future = std::async(std::launch::async, []() {
    return remote_connect::prepare_pairing();
  });
  ASSERT_TRUE(wait_for_runtime_start()) << "EasyTier runtime start did not reach the test barrier";

  std::promise<void> disable_started;
  auto disable_future = std::async(std::launch::async, [&disable_started]() {
    disable_started.set_value();
    return remote_connect::set_enabled(false);
  });
  disable_started.get_future().wait();

  EXPECT_EQ(disable_future.wait_for(100ms), std::future_status::timeout);
  allow_runtime_start();

  const auto pairing = pairing_future.get();
  const auto disable = disable_future.get();

  ASSERT_TRUE(pairing.success);
  ASSERT_TRUE(pairing.enabled);
  ASSERT_TRUE(pairing.enrollment.has_value());
  EXPECT_EQ(pairing.enrollment->virtual_ip, "100.64.10.1");
  EXPECT_EQ(pairing.enrollment->network_secret, "remote-secret");
  EXPECT_TRUE(disable.success);
  EXPECT_FALSE(disable.status.enabled);
}

TEST(RemoteConnectService, PairingSnapshotSerializesConcurrentCredentialReset) {
  using namespace std::chrono_literals;

  configure_remote_connect();
  reset_runtime_barrier();

  auto pairing_future = std::async(std::launch::async, []() {
    return remote_connect::prepare_pairing();
  });
  ASSERT_TRUE(wait_for_runtime_start()) << "EasyTier runtime start did not reach the test barrier";

  std::promise<void> reset_started;
  auto reset_future = std::async(std::launch::async, [&reset_started]() {
    reset_started.set_value();
    return remote_connect::reset_enrollment();
  });
  reset_started.get_future().wait();

  EXPECT_EQ(reset_future.wait_for(100ms), std::future_status::timeout);
  allow_runtime_start();

  const auto pairing = pairing_future.get();
  const auto reset = reset_future.get();

  ASSERT_TRUE(pairing.success);
  ASSERT_TRUE(pairing.enabled);
  ASSERT_TRUE(pairing.enrollment.has_value());
  EXPECT_EQ(pairing.enrollment->virtual_ip, "100.64.10.1");
  EXPECT_EQ(pairing.enrollment->network_secret, "remote-secret");
  EXPECT_TRUE(reset.success);
  EXPECT_NE(config::nvhttp.remote_connect_network_secret, "remote-secret");
}
