#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "src/config.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/remote_connect/credential_transaction.h"
#include "src/remote_connect/device_record_store.h"
#include "src/remote_connect/easytier.h"
#include "src/remote_connect/service.h"

boost::log::sources::severity_logger<int> warning;
boost::log::sources::severity_logger<int> error;

// Test controls for the persistence seam; defined below next to config::nvhttp.
namespace config {
  extern std::atomic<bool> update_allowed;
  extern std::mutex last_update_mutex;
  extern std::map<std::string, std::string> last_update;
}  // namespace config

namespace {
  using namespace remote_connect;
  using credential_transaction::action_t;
  using credential_transaction::device_t;
  using credential_transaction::event_t;
  using credential_transaction::public_key_t;
  using credential_transaction::record_t;
  using credential_transaction::transaction_id_t;

  std::mutex runtime_mutex;
  std::condition_variable runtime_condition;
  bool block_runtime_start;
  bool runtime_start_entered;
  bool release_runtime_start;
  bool runtime_available;
  std::atomic<int> runtime_start_failures = 0;
  std::atomic<int> runtime_start_calls = 0;

  // Every key set the fake core was started with, in order.
  std::mutex start_keys_mutex;
  std::vector<std::vector<std::string>> start_keys_history;

  // The folder platf::appdata() answers with. One fresh folder per test: the
  // service adopts the device record once per location.
  std::filesystem::path appdata_directory;
  std::filesystem::path previous_directory;

  std::string
  key_hex(std::uint8_t fill) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (int index = 0; index < 32; ++index) {
      out.push_back(digits[fill >> 4]);
      out.push_back(digits[fill & 0x0f]);
    }
    return out;
  }

  device_t
  device(std::string id, std::uint8_t key_fill, std::uint8_t certificate_fill) {
    device_t out;
    out.id = std::move(id);
    out.easytier_public_key.fill(key_fill);
    out.gamestream_certificate_sha256.fill(certificate_fill);
    return out;
  }

  event_t
  transition(action_t action, const record_t &record, transaction_id_t id, std::optional<device_t> candidate = std::nullopt) {
    event_t event;
    event.action = action;
    event.expected_revision = record.revision;
    event.transaction_id = id;
    event.candidate = std::move(candidate);
    return event;
  }

  record_t
  reduced(const record_t &before, const event_t &event) {
    auto after = credential_transaction::reduce(before, event);
    if (!after) throw std::runtime_error("Test transition rejected");
    return *after;
  }

  std::filesystem::path
  device_record_path() {
    return appdata_directory / "remote-devices.json";
  }

  // Builds a stable record holding the given devices, by way of the reducer
  // so it is exactly what the service would have written itself.
  record_t
  stable_record(const std::vector<device_t> &devices) {
    record_t record;
    transaction_id_t::value_type next = 0;
    for (const auto &entry : devices) {
      const transaction_id_t id { ++next };
      record = reduced(record, transition(action_t::prepare_add, record, id, entry));
      record = reduced(record, transition(action_t::add_verified, record, id));
      record = reduced(record, transition(action_t::commit_add, record, id));
      record = reduced(record, transition(action_t::finish_closed, record, id));
    }
    return record;
  }

  std::vector<std::vector<std::string>>
  start_keys() {
    std::lock_guard lock(start_keys_mutex);
    return start_keys_history;
  }

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
    runtime_start_failures.store(0);
    runtime_start_calls.store(0);
    {
      std::lock_guard lock(start_keys_mutex);
      start_keys_history.clear();
    }
    config::update_allowed.store(true);
    {
      std::lock_guard lock(config::last_update_mutex);
      config::last_update.clear();
    }

    std::error_code ignored;
    std::filesystem::remove_all(previous_directory, ignored);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    static std::atomic<unsigned long long> sequence = 0;
    appdata_directory = std::filesystem::temp_directory_path() / ("sunshine-remote-connect-" + std::to_string(stamp) + "-" + std::to_string(++sequence));
    previous_directory = appdata_directory;
    std::filesystem::create_directories(appdata_directory);

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

namespace platf {
  std::filesystem::path
  appdata() {
    return appdata_directory;
  }
}  // namespace platf

namespace file_handler {
  std::string
  path_to_utf8(const std::filesystem::path &path) {
    return path.generic_string();
  }
}  // namespace file_handler

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
  runtime_t::start(const enrollment_t &, const std::vector<std::string> &authorized_client_keys, const std::string &, std::string &error) {
    if (!runtime_available || runtime_start_failures.load() > 0) {
      // A scheduled failure is consumed once, so a later restart can succeed.
      if (runtime_start_failures.load() > 0) runtime_start_failures.fetch_sub(1);
      error = installation_required_error;
      return false;
    }
    {
      std::lock_guard lock(start_keys_mutex);
      start_keys_history.push_back(authorized_client_keys);
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

// The device record is the single source of truth for who is authorized, and
// the core is told the keys of the devices on it each time it starts.
TEST(RemoteConnectService, WithoutADeviceRecordTheCoreIsStartedAuthorizingNobody) {
  configure_remote_connect();

  const auto pairing = remote_connect::prepare_pairing();

  ASSERT_TRUE(pairing.success) << pairing.error;
  ASSERT_EQ(start_keys().size(), 1u);
  EXPECT_TRUE(start_keys().back().empty());
  EXPECT_TRUE(remote_connect::devices().empty());
  // Adopting an empty record creates nothing on disk until there is a change.
  EXPECT_FALSE(std::filesystem::exists(device_record_path()));
}

TEST(RemoteConnectService, TheCoreIsStartedWithTheKeysOfTheDevicesOnRecord) {
  configure_remote_connect();
  const auto record = stable_record({ device("phone", 0x11, 0x43), device("tablet", 0x22, 0x44) });
  ASSERT_TRUE(device_record_store::store(device_record_path(), record));

  const auto pairing = remote_connect::prepare_pairing();

  ASSERT_TRUE(pairing.success) << pairing.error;
  ASSERT_EQ(start_keys().size(), 1u);
  // Key sets follow the record's id order.
  EXPECT_EQ(start_keys().back(), (std::vector<std::string> { key_hex(0x11), key_hex(0x22) }));
  EXPECT_EQ(remote_connect::devices(), (std::vector<device_t> { device("phone", 0x11, 0x43), device("tablet", 0x22, 0x44) }));
}

TEST(RemoteConnectService, DisablingAndReEnablingStartsTheCoreWithTheSameKeys) {
  configure_remote_connect();
  ASSERT_TRUE(device_record_store::store(device_record_path(), stable_record({ device("phone", 0x11, 0x43) })));
  ASSERT_TRUE(remote_connect::prepare_pairing().success);

  ASSERT_TRUE(remote_connect::set_enabled(false).success);
  ASSERT_TRUE(remote_connect::set_enabled(true).success);

  ASSERT_EQ(start_keys().size(), 2u);
  EXPECT_EQ(start_keys().front(), start_keys().back());
}

TEST(RemoteConnectService, RevokingADeviceRestartsTheCoreWithoutItsKeyAndRecordsItForGood) {
  configure_remote_connect();
  ASSERT_TRUE(device_record_store::store(device_record_path(), stable_record({ device("phone", 0x11, 0x43), device("tablet", 0x22, 0x44) })));
  ASSERT_TRUE(remote_connect::prepare_pairing().success);

  const auto result = remote_connect::revoke_device("phone");

  ASSERT_TRUE(result.success) << result.status.error;
  EXPECT_TRUE(result.status.running);
  EXPECT_EQ(remote_connect::devices(), (std::vector<device_t> { device("tablet", 0x22, 0x44) }));
  // One restart, with the revoked device's key gone.
  ASSERT_EQ(start_keys().size(), 2u);
  EXPECT_EQ(start_keys().back(), (std::vector<std::string> { key_hex(0x22) }));

  // The record on disk says the same, and keeps the device revoked.
  const auto loaded = device_record_store::load(device_record_path());
  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  EXPECT_EQ(loaded.record.phase, credential_transaction::phase_t::stable);
  EXPECT_EQ(loaded.record.active.size(), 1u);
  EXPECT_EQ(loaded.record.tombstones.size(), 1u);
  EXPECT_EQ(loaded.record.tombstones.at("phone").easytier_public_key, device("phone", 0x11, 0x43).easytier_public_key);

  // Revoking it again has nothing left to do.
  const auto again = remote_connect::revoke_device("phone");
  EXPECT_FALSE(again.success);
  EXPECT_EQ(start_keys().size(), 2u);
}

TEST(RemoteConnectService, RevokingWhileDisabledRestartsNothingAndTheNextStartSkipsTheKey) {
  configure_remote_connect();
  config::nvhttp.remote_connect_enabled = false;
  ASSERT_TRUE(device_record_store::store(device_record_path(), stable_record({ device("phone", 0x11, 0x43), device("tablet", 0x22, 0x44) })));

  const auto revoked = remote_connect::revoke_device("phone");

  ASSERT_TRUE(revoked.success) << revoked.status.error;
  EXPECT_TRUE(start_keys().empty()) << "the core is not running, so nothing is restarted";

  // The next start, after enabling, goes without the revoked key.
  ASSERT_TRUE(remote_connect::set_enabled(true).success);
  ASSERT_EQ(start_keys().size(), 1u);
  EXPECT_EQ(start_keys().back(), (std::vector<std::string> { key_hex(0x22) }));
}

TEST(RemoteConnectService, RevokingAnUnknownDeviceChangesNothing) {
  configure_remote_connect();
  ASSERT_TRUE(device_record_store::store(device_record_path(), stable_record({ device("phone", 0x11, 0x43) })));
  ASSERT_TRUE(remote_connect::prepare_pairing().success);

  const auto result = remote_connect::revoke_device("nobody");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status.error.find("not enrolled"), std::string::npos) << result.status.error;
  EXPECT_EQ(start_keys().size(), 1u) << "nothing changed, so the core is not restarted";
  EXPECT_EQ(remote_connect::devices().size(), 1u);
}

// A crash between committing a device and closing its transaction must not
// lose the device, and must be sorted out before the core first starts.
TEST(RemoteConnectService, ACommittedAddACrashLeftOpenIsRecoveredBeforeTheCoreStarts) {
  configure_remote_connect();
  const transaction_id_t id { 7 };
  auto interrupted = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, device("phone", 0x11, 0x43)));
  interrupted = reduced(interrupted, transition(action_t::add_verified, interrupted, id));
  interrupted = reduced(interrupted, transition(action_t::commit_add, interrupted, id));
  ASSERT_EQ(interrupted.phase, credential_transaction::phase_t::add_committed_closed);
  ASSERT_TRUE(device_record_store::store(device_record_path(), interrupted));

  const auto pairing = remote_connect::prepare_pairing();

  ASSERT_TRUE(pairing.success) << pairing.error;
  ASSERT_EQ(start_keys().size(), 1u);
  EXPECT_EQ(start_keys().back(), (std::vector<std::string> { key_hex(0x11) }));
  EXPECT_EQ(remote_connect::devices(), (std::vector<device_t> { device("phone", 0x11, 0x43) }));
  const auto recovered = device_record_store::load(device_record_path());
  ASSERT_EQ(recovered.status, device_record_store::load_status_e::loaded);
  EXPECT_EQ(recovered.record.phase, credential_transaction::phase_t::stable);
}

// An add that had not committed when the crash happened is dropped.
TEST(RemoteConnectService, AnUncommittedAddACrashLeftBehindIsDroppedBeforeTheCoreStarts) {
  configure_remote_connect();
  const transaction_id_t id { 7 };
  const auto interrupted = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, device("phone", 0x11, 0x43)));
  ASSERT_TRUE(device_record_store::store(device_record_path(), interrupted));

  const auto pairing = remote_connect::prepare_pairing();

  ASSERT_TRUE(pairing.success) << pairing.error;
  EXPECT_TRUE(start_keys().back().empty());
  EXPECT_TRUE(remote_connect::devices().empty());
}

// A damaged record must not be read as "no devices": remote access stays off
// so that the file is not overwritten, since it may still hold the devices.
TEST(RemoteConnectService, ADamagedDeviceRecordStopsRemoteAccessAndIsLeftAsItIs) {
  configure_remote_connect();
  const std::string damaged = "{ not a device record";
  {
    std::ofstream out(device_record_path(), std::ios::binary | std::ios::trunc);
    out << damaged;
  }

  const auto enabled = remote_connect::set_enabled(true);

  EXPECT_FALSE(enabled.success);
  EXPECT_NE(enabled.status.error.find("not valid"), std::string::npos) << enabled.status.error;
  EXPECT_FALSE(enabled.status.running);
  EXPECT_TRUE(start_keys().empty()) << "no core may be started on a record that cannot be trusted";
  EXPECT_TRUE(remote_connect::devices().empty());
  EXPECT_FALSE(remote_connect::revoke_device("phone").success);
  EXPECT_FALSE(remote_connect::reset_enrollment().success) << "a reset can start the core, so it is held to the same rule";

  std::ifstream in(device_record_path(), std::ios::binary);
  const std::string after { (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() };
  EXPECT_EQ(after, damaged);
}

TEST(RemoteConnectService, ADeviceRecordThatCannotBeReadStopsRemoteAccess) {
  configure_remote_connect();
  // A folder where the record should be is something that exists and cannot
  // be read as one.
  std::filesystem::create_directories(device_record_path());

  const auto enabled = remote_connect::set_enabled(true);
  const auto pairing = remote_connect::prepare_pairing();

  EXPECT_FALSE(enabled.success);
  EXPECT_NE(enabled.status.error.find("could not be read"), std::string::npos) << enabled.status.error;
  EXPECT_FALSE(pairing.success);
  EXPECT_TRUE(start_keys().empty());
  EXPECT_TRUE(remote_connect::devices().empty());
}

TEST(RemoteConnectService, TheServiceStopsOnADamagedRecordUntilItIsRepaired) {
  configure_remote_connect();
  {
    std::ofstream out(device_record_path(), std::ios::binary | std::ios::trunc);
    out << "garbage";
  }
  ASSERT_FALSE(remote_connect::set_enabled(true).success);
  ASSERT_TRUE(remote_connect::devices().empty());

  // Repairing the record is enough; the service reads it again.
  ASSERT_TRUE(device_record_store::store(device_record_path(), stable_record({ device("phone", 0x11, 0x43) })));

  EXPECT_EQ(remote_connect::devices(), (std::vector<device_t> { device("phone", 0x11, 0x43) }));
  ASSERT_TRUE(remote_connect::set_enabled(true).success);
  EXPECT_EQ(start_keys().back(), (std::vector<std::string> { key_hex(0x11) }));
}

// When the core cannot be restarted with a device removed, the device is
// still revoked in the record and access is closed rather than left as it was.
TEST(RemoteConnectService, ARevocationTheRuntimeRefusesIsStillRecordedAndClosesAccess) {
  configure_remote_connect();
  ASSERT_TRUE(device_record_store::store(device_record_path(), stable_record({ device("phone", 0x11, 0x43) })));
  ASSERT_TRUE(remote_connect::prepare_pairing().success);
  runtime_available = false;

  const auto result = remote_connect::revoke_device("phone");

  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.status.running);
  EXPECT_TRUE(remote_connect::devices().empty());
  const auto loaded = device_record_store::load(device_record_path());
  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  EXPECT_EQ(loaded.record.tombstones.size(), 1u);
  EXPECT_TRUE(loaded.record.active.empty());
  // The restart with the device removed was refused, so no core is left
  // running at all: there is nothing through which a device could connect.
  EXPECT_EQ(start_keys().size(), 1u) << "the refused restart never got as far as starting the core";

  // Enabling again does not touch the record, so the open revocation is left
  // for the next start of Sunshine to sort out. A core started meanwhile is
  // told to authorize nobody.
  runtime_available = true;
  ASSERT_TRUE(remote_connect::set_enabled(true).success);
  ASSERT_EQ(start_keys().size(), 2u);
  EXPECT_TRUE(start_keys().back().empty());
  EXPECT_TRUE(remote_connect::devices().empty());
  EXPECT_EQ(device_record_store::load(device_record_path()).record.phase, credential_transaction::phase_t::revoke_pending);
}
