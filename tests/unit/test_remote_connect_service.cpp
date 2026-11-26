#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <atomic>

#include <gtest/gtest.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/remote_connect/easytier.h"
#include "src/remote_connect/service.h"

boost::log::sources::severity_logger<int> warning;

namespace config {
  nvhttp_t nvhttp;

  // Test controls for the persistence seam: a failing write, and a record of
  // the most recent requested change.
  std::atomic<bool> update_allowed = true;
  std::mutex last_update_mutex;
  std::map<std::string, std::string> last_update;

  bool
  update_config(const std::map<std::string, std::string> &updates) {
    {
      std::lock_guard lock(last_update_mutex);
      last_update = updates;
    }
    return update_allowed.load();
  }
}  // namespace config

namespace {
  std::mutex runtime_mutex;
  std::condition_variable runtime_condition;
  bool block_runtime_start;
  bool runtime_start_entered;
  bool release_runtime_start;
  bool runtime_available;
  std::atomic<int> runtime_start_failures = 0;
  std::atomic<int> runtime_start_calls = 0;

  void
  reset_runtime_barrier() {
    std::lock_guard lock(runtime_mutex);
    block_runtime_start = true;
    runtime_start_entered = false;
    release_runtime_start = false;
  }

  // Brings the fake runtime up under the configured enrollment without
  // persisting anything, mirroring what prepare_pairing does for a host whose
  // remote connection is already enabled.
  void
  start_fake_runtime() {
    const auto pairing = remote_connect::prepare_pairing();
    EXPECT_TRUE(pairing.success) << pairing.error;
    EXPECT_TRUE(pairing.enabled);
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
    runtime_start_failures.store(0);
    runtime_start_calls.store(0);
    config::update_allowed.store(true);
    {
      std::lock_guard lock(config::last_update_mutex);
      config::last_update.clear();
    }
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
    if (!runtime_available || runtime_start_failures.load() > 0) {
      // A scheduled failure is consumed once, so a later restart can succeed.
      if (runtime_start_failures.load() > 0) runtime_start_failures.fetch_sub(1);
      error = installation_required_error;
      return false;
    }
    std::unique_lock lock(runtime_mutex);
    runtime_start_calls.fetch_add(1);
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

namespace {
  const char client_key_a[] = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  const char client_key_a_upper[] = "0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20";
  const char client_key_b[] = "ff02030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";

  std::map<std::string, std::string>
  last_config_update() {
    std::lock_guard lock(config::last_update_mutex);
    return config::last_update;
  }

  std::string
  persisted_key_update() {
    return last_config_update().at("remote_connect_strict_client_keys");
  }
}  // namespace

TEST(RemoteConnectService, EnrollStrictClientKeyPersistsNormalizedKeyAndRestartsRuntime) {
  configure_remote_connect();
  start_fake_runtime();
  const auto starts_before = runtime_start_calls.load();

  const auto result = remote_connect::enroll_strict_client_key(client_key_a_upper);

  ASSERT_TRUE(result.success) << result.status.error;
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_a);
  EXPECT_EQ(persisted_key_update(), client_key_a);
  EXPECT_TRUE(result.status.error.empty()) << result.status.error;
  // A running core must be restarted so it loads the new policy.
  EXPECT_EQ(runtime_start_calls.load(), starts_before + 1);
  EXPECT_TRUE(result.status.running);
}

TEST(RemoteConnectService, EnrollStrictClientKeyIsIdempotent) {
  configure_remote_connect();

  ASSERT_TRUE(remote_connect::enroll_strict_client_key(client_key_a).success);
  const auto again = remote_connect::enroll_strict_client_key(client_key_a_upper);

  EXPECT_TRUE(again.success);
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_a);
}

TEST(RemoteConnectService, RevokeStrictClientKeyRemovesOnlyTheNamedKey) {
  configure_remote_connect();
  ASSERT_TRUE(remote_connect::enroll_strict_client_key(client_key_a).success);
  ASSERT_TRUE(remote_connect::enroll_strict_client_key(client_key_b).success);
  ASSERT_EQ(config::nvhttp.remote_connect_strict_client_keys, std::string(client_key_a) + ";" + client_key_b);

  const auto result = remote_connect::revoke_strict_client_key(client_key_a_upper);

  ASSERT_TRUE(result.success) << result.status.error;
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_b);
  EXPECT_EQ(persisted_key_update(), client_key_b);

  // Revoking a key that is already gone is a successful no-op.
  const auto repeat = remote_connect::revoke_strict_client_key(client_key_a);
  EXPECT_TRUE(repeat.success);
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_b);
}

TEST(RemoteConnectService, RevokingTheLastKeyClearsTheList) {
  configure_remote_connect();
  ASSERT_TRUE(remote_connect::enroll_strict_client_key(client_key_a).success);

  ASSERT_TRUE(remote_connect::revoke_strict_client_key(client_key_a).success);

  EXPECT_TRUE(config::nvhttp.remote_connect_strict_client_keys.empty());
  EXPECT_TRUE(persisted_key_update().empty());
}

TEST(RemoteConnectService, StrictClientKeyChangeDoesNotStartADisabledRuntime) {
  configure_remote_connect();
  config::nvhttp.remote_connect_enabled = false;

  const auto result = remote_connect::enroll_strict_client_key(client_key_a);

  ASSERT_TRUE(result.success) << result.status.error;
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_a);
  EXPECT_EQ(runtime_start_calls.load(), 0);
}

TEST(RemoteConnectService, InvalidPersistedKeyListRejectsEveryChange) {
  configure_remote_connect();
  config::nvhttp.remote_connect_strict_client_keys = "not-a-key";

  const auto enroll = remote_connect::enroll_strict_client_key(client_key_a);
  const auto revoke = remote_connect::revoke_strict_client_key(client_key_a);

  EXPECT_FALSE(enroll.success);
  EXPECT_NE(enroll.status.error.find("invalid"), std::string::npos) << enroll.status.error;
  EXPECT_FALSE(revoke.success);
  // A broken list is surfaced, never rewritten behind the owner's back.
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, "not-a-key");
  EXPECT_TRUE(last_config_update().empty());
}

TEST(RemoteConnectService, MalformedKeyIsRejectedWithoutPersisting) {
  configure_remote_connect();

  for (const auto &bad : {
         std::string("tooshort"),
         std::string(64, 'z'),
         std::string(64, '0'),
         std::string(client_key_a) + ";" + client_key_b,
       }) {
    const auto result = remote_connect::enroll_strict_client_key(bad);

    EXPECT_FALSE(result.success) << bad;
    EXPECT_NE(result.status.error.find("64 hex"), std::string::npos) << result.status.error;
    EXPECT_TRUE(config::nvhttp.remote_connect_strict_client_keys.empty()) << bad;
    EXPECT_TRUE(last_config_update().empty()) << bad;
  }
}

TEST(RemoteConnectService, PersistFailureLeavesKeysAndRunningRuntimeUntouched) {
  configure_remote_connect();
  start_fake_runtime();
  const auto starts_before = runtime_start_calls.load();
  config::update_allowed.store(false);

  const auto result = remote_connect::enroll_strict_client_key(client_key_a);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status.error.find("Unable to persist"), std::string::npos) << result.status.error;
  EXPECT_TRUE(config::nvhttp.remote_connect_strict_client_keys.empty());
  // Nothing was applied, so the running core was neither stopped nor restarted.
  EXPECT_EQ(runtime_start_calls.load(), starts_before);
  EXPECT_TRUE(result.status.running);
}

TEST(RemoteConnectService, RuntimeFailureAfterEnrollRestoresPreviousKeys) {
  configure_remote_connect();
  ASSERT_TRUE(remote_connect::enroll_strict_client_key(client_key_a).success);
  start_fake_runtime();
  // The restart after the change fails once; the rollback restart succeeds.
  runtime_start_failures.store(1);

  const auto result = remote_connect::enroll_strict_client_key(client_key_b);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status.error.find("were restored"), std::string::npos) << result.status.error;
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_a);
  EXPECT_EQ(persisted_key_update(), client_key_a);
  EXPECT_TRUE(result.status.running);
}

TEST(RemoteConnectService, UnstartableRuntimeAfterEnrollRestoresKeysAndStaysDown) {
  configure_remote_connect();
  ASSERT_TRUE(remote_connect::enroll_strict_client_key(client_key_a).success);
  start_fake_runtime();
  runtime_available = false;

  const auto result = remote_connect::enroll_strict_client_key(client_key_b);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status.error.find("also failed to start"), std::string::npos) << result.status.error;
  // The persisted list is still the previous one, so a later start uses it.
  EXPECT_EQ(config::nvhttp.remote_connect_strict_client_keys, client_key_a);
  EXPECT_EQ(persisted_key_update(), client_key_a);
  EXPECT_FALSE(result.status.running);

  runtime_available = true;
  remote_connect::stop();
}
