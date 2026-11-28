#include <atomic>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "src/remote_connect/credential_transaction.h"

namespace {
  namespace tx = remote_connect::credential_transaction;

  tx::device_t
  device(std::string id, std::uint8_t byte) {
    tx::device_t value;
    value.id = std::move(id);
    value.easytier_public_key.fill(byte);
    value.gamestream_certificate_sha256.fill(static_cast<std::uint8_t>(byte + 50));
    return value;
  }

  tx::transaction_id_t
  id(std::uint8_t byte) {
    tx::transaction_id_t value {};
    value[0] = byte;
    return value;
  }

  struct fake_store_t {
    std::mutex mutex;
    tx::record_t durable;
    bool fail_next = false;

    tx::record_t snapshot() {
      std::lock_guard lock(mutex);
      return durable;
    }

    bool write(const tx::event_t &event) {
      std::lock_guard lock(mutex);
      const auto next = tx::reduce(durable, event);
      if (!next || fail_next) {
        fail_next = false;
        return false;
      }
      durable = *next;
      return true;
    }
  };

  // Deliberately no real processes, network, credentials, ACLs or files.
  struct fake_core_t {
    bool gate_open = false;
    bool running = false;
    bool fail_start = false;
    std::map<std::string, tx::device_t> loaded;

    void close_gate() { gate_open = false; }
    void stop() { EXPECT_FALSE(gate_open); running = false; loaded.clear(); }
    bool start(const std::map<std::string, tx::device_t> &devices) {
      EXPECT_FALSE(gate_open);
      EXPECT_FALSE(running);
      if (fail_start) { fail_start = false; return false; }
      loaded = devices;
      running = true;
      return true;
    }
    void open_gate() { EXPECT_TRUE(running); gate_open = true; }
    void crash() { gate_open = false; running = false; loaded.clear(); }
  };

  tx::event_t
  event(tx::action_t action, const tx::record_t &record, tx::transaction_id_t transaction) {
    return {action, record.revision, transaction, std::nullopt, {}};
  }

  TEST(CredentialTransaction, FailedAddRollsBackOldSnapshotBehindClosedGate) {
    fake_store_t store;
    store.durable.active.emplace("old", device("old", 1));
    fake_core_t core;
    ASSERT_TRUE(core.start(store.snapshot().active));
    core.open_gate();
    core.close_gate();
    auto prepare = event(tx::action_t::prepare_add, store.snapshot(), id(1));
    prepare.candidate = device("new", 2);
    ASSERT_TRUE(store.write(prepare));
    EXPECT_FALSE(store.snapshot().active.contains("new"));
    core.stop();
    auto intended = store.snapshot().active;
    intended.emplace("new", *store.snapshot().candidate);
    core.fail_start = true;
    EXPECT_FALSE(core.start(intended));
    ASSERT_TRUE(store.write(event(tx::action_t::abort_add, store.snapshot(), id(1))));
    EXPECT_EQ(store.snapshot().phase, tx::phase_t::add_rollback_closed);
    EXPECT_FALSE(core.gate_open);
    ASSERT_TRUE(core.start(store.snapshot().active));
    ASSERT_TRUE(store.write(event(tx::action_t::finish_closed, store.snapshot(), id(1))));
    core.open_gate();
    EXPECT_TRUE(core.loaded.contains("old"));
    EXPECT_FALSE(core.loaded.contains("new"));
    EXPECT_EQ(store.snapshot().phase, tx::phase_t::stable);
  }

  TEST(CredentialTransaction, FailedRollbackStaysClosedAndCannotFinishWithoutTrustedEvent) {
    fake_store_t store;
    store.durable.active.emplace("old", device("old", 1));
    fake_core_t core;
    auto prepare = event(tx::action_t::prepare_add, store.snapshot(), id(1));
    prepare.candidate = device("new", 2);
    ASSERT_TRUE(store.write(prepare));
    ASSERT_TRUE(store.write(event(tx::action_t::abort_add, store.snapshot(), id(1))));
    core.fail_start = true;
    EXPECT_FALSE(core.start(store.snapshot().active));
    // No finish event is sent by the adapter when core activation fails.
    EXPECT_EQ(store.snapshot().phase, tx::phase_t::add_rollback_closed);
    EXPECT_FALSE(core.gate_open);
    EXPECT_FALSE(core.running);
  }

  TEST(CredentialTransaction, CrashBeforeCommitRestoresOldAfterCommitRestoresNew) {
    for (const bool committed : {false, true}) {
      fake_store_t store;
      store.durable.active.emplace("old", device("old", 1));
      fake_core_t core;
      auto prepare = event(tx::action_t::prepare_add, store.snapshot(), id(3));
      prepare.candidate = device("new", 2);
      ASSERT_TRUE(store.write(prepare));
      auto desired = store.snapshot().active;
      desired.emplace("new", *store.snapshot().candidate);
      ASSERT_TRUE(core.start(desired));
      ASSERT_TRUE(store.write(event(tx::action_t::add_verified, store.snapshot(), id(3))));
      if (committed) {
        ASSERT_TRUE(store.write(event(tx::action_t::commit_add, store.snapshot(), id(3))));
      }
      core.crash();
      ASSERT_TRUE(store.write(event(tx::action_t::recover_closed, store.snapshot(), id(3))));
      EXPECT_EQ(store.snapshot().phase, tx::phase_t::recovery_closed);
      EXPECT_FALSE(core.gate_open);
      EXPECT_EQ(store.snapshot().active.contains("new"), committed);
      ASSERT_TRUE(core.start(store.snapshot().active));
      ASSERT_TRUE(store.write(event(tx::action_t::finish_closed, store.snapshot(), id(3))));
      core.open_gate();
      EXPECT_EQ(core.loaded.contains("new"), committed);
    }
  }

  TEST(CredentialTransaction, RevocationFailureAndCrashNeverRestoreTombstonedKey) {
    fake_store_t store;
    const auto revoked = device("phone", 4);
    store.durable.active.emplace(revoked.id, revoked);
    store.durable.active.emplace("other", device("other", 5));
    fake_core_t core;
    ASSERT_TRUE(core.start(store.snapshot().active));
    core.open_gate();
    core.close_gate();
    auto revoke = event(tx::action_t::prepare_revoke, store.snapshot(), id(4));
    revoke.target_id = revoked.id;
    ASSERT_TRUE(store.write(revoke)); // durable tombstone precedes activation
    EXPECT_TRUE(store.snapshot().tombstones.contains("phone"));
    EXPECT_FALSE(store.snapshot().active.contains("phone"));
    core.stop();
    core.fail_start = true;
    EXPECT_FALSE(core.start(store.snapshot().active));
    EXPECT_FALSE(store.write(event(tx::action_t::abort_add, store.snapshot(), id(4))));
    core.crash();
    ASSERT_TRUE(store.write(event(tx::action_t::recover_closed, store.snapshot(), id(4))));
    EXPECT_FALSE(store.snapshot().active.contains("phone"));
    ASSERT_TRUE(core.start(store.snapshot().active));
    ASSERT_TRUE(store.write(event(tx::action_t::finish_closed, store.snapshot(), id(4))));
    core.open_gate();
    EXPECT_FALSE(core.loaded.contains("phone"));
    EXPECT_TRUE(core.loaded.contains("other"));
    auto attempt = event(tx::action_t::prepare_add, store.snapshot(), id(5));
    attempt.candidate = revoked;
    EXPECT_FALSE(store.write(attempt));
    attempt.candidate = device("different-id", 4);
    EXPECT_FALSE(store.write(attempt));
  }

  TEST(CredentialTransaction, EveryDurableRevocationPhaseRecoversWithoutResurrection) {
    for (int stop_after = 0; stop_after < 3; ++stop_after) {
      fake_store_t store;
      store.durable.active.emplace("phone", device("phone", 4));
      store.durable.active.emplace("other", device("other", 5));
      auto revoke = event(tx::action_t::prepare_revoke, store.snapshot(), id(7));
      revoke.target_id = "phone";
      ASSERT_TRUE(store.write(revoke));
      if (stop_after >= 1) {
        ASSERT_TRUE(store.write(event(tx::action_t::revoke_verified, store.snapshot(), id(7))));
      }
      if (stop_after >= 2) {
        ASSERT_TRUE(store.write(event(tx::action_t::commit_revoke, store.snapshot(), id(7))));
      }
      fake_core_t core;
      core.crash();
      ASSERT_TRUE(store.write(event(tx::action_t::recover_closed, store.snapshot(), id(7))));
      EXPECT_TRUE(store.snapshot().tombstones.contains("phone"));
      EXPECT_FALSE(store.snapshot().active.contains("phone"));
      ASSERT_TRUE(core.start(store.snapshot().active));
      ASSERT_TRUE(store.write(event(tx::action_t::finish_closed, store.snapshot(), id(7))));
      core.open_gate();
      EXPECT_FALSE(core.loaded.contains("phone"));
      EXPECT_TRUE(core.loaded.contains("other"));
    }
  }

  TEST(CredentialTransaction, FailedRevocationPersistenceCannotBeAcknowledged) {
    fake_store_t store;
    store.durable.active.emplace("phone", device("phone", 4));
    fake_core_t core;
    ASSERT_TRUE(core.start(store.snapshot().active));
    core.open_gate();
    core.close_gate();
    auto revoke = event(tx::action_t::prepare_revoke, store.snapshot(), id(4));
    revoke.target_id = "phone";
    store.fail_next = true;
    EXPECT_FALSE(store.write(revoke));
    EXPECT_TRUE(store.snapshot().active.contains("phone"));
    EXPECT_FALSE(store.snapshot().tombstones.contains("phone"));
    EXPECT_FALSE(core.gate_open);
    // No acknowledgement or core reopening is performed after the failed write.
  }

  TEST(CredentialTransaction, FailedPersistenceAndReplayDoNotCommit) {
    fake_store_t store;
    store.durable.active.emplace("old", device("old", 1));
    auto prepare = event(tx::action_t::prepare_add, store.snapshot(), id(1));
    prepare.candidate = device("new", 2);
    store.fail_next = true;
    EXPECT_FALSE(store.write(prepare));
    EXPECT_EQ(store.snapshot().revision, std::uint64_t {0});
    ASSERT_TRUE(store.write(prepare));
    EXPECT_FALSE(store.write(prepare));
    EXPECT_FALSE(store.write(event(tx::action_t::add_verified, store.snapshot(), id(2))));
    ASSERT_TRUE(store.write(event(tx::action_t::abort_add, store.snapshot(), id(1))));
    ASSERT_TRUE(store.write(event(tx::action_t::finish_closed, store.snapshot(), id(1))));
    prepare = event(tx::action_t::prepare_add, store.snapshot(), id(1));
    prepare.candidate = device("new", 2);
    EXPECT_FALSE(store.write(prepare));
  }

  TEST(CredentialTransaction, RejectsCandidateReusingActiveCertificate) {
    fake_store_t store;
    const auto first = device("phone-a", 1);
    store.durable.active.emplace(first.id, first);
    auto prepare = event(tx::action_t::prepare_add, store.snapshot(), id(1));
    auto second = device("phone-b", 2);
    second.gamestream_certificate_sha256 = first.gamestream_certificate_sha256;
    prepare.candidate = second;
    EXPECT_FALSE(store.write(prepare));
    EXPECT_EQ(store.snapshot().revision, std::uint64_t {0});
    EXPECT_EQ(store.snapshot().active.size(), std::size_t {1});
  }

  TEST(CredentialTransaction, TombstonedCertificateCannotBeReusedWithFreshEasyTierKey) {
    fake_store_t store;
    const auto first = device("old-phone", 1);
    store.durable.active.emplace(first.id, first);
    auto revoke = event(tx::action_t::prepare_revoke, store.snapshot(), id(1));
    revoke.target_id = first.id;
    ASSERT_TRUE(store.write(revoke));
    ASSERT_TRUE(store.write(event(tx::action_t::revoke_verified, store.snapshot(), id(1))));
    ASSERT_TRUE(store.write(event(tx::action_t::commit_revoke, store.snapshot(), id(1))));
    ASSERT_TRUE(store.write(event(tx::action_t::finish_closed, store.snapshot(), id(1))));
    auto prepare = event(tx::action_t::prepare_add, store.snapshot(), id(2));
    auto second = device("new-phone", 2);
    second.gamestream_certificate_sha256 = first.gamestream_certificate_sha256;
    prepare.candidate = second;
    EXPECT_FALSE(store.write(prepare));
    EXPECT_TRUE(store.snapshot().tombstones.contains(first.id));
    prepare.candidate = device("new-phone", 2);
    EXPECT_TRUE(store.write(prepare));
  }

  TEST(CredentialTransaction, RejectsPersistedDuplicateCertificatesAcrossAllSets) {
    tx::record_t record;
    const auto first = device("first", 1);
    auto second = device("second", 2);
    second.gamestream_certificate_sha256 = first.gamestream_certificate_sha256;
    record.active.emplace(first.id, first);
    record.active.emplace(second.id, second);
    const auto recovery = event(tx::action_t::recover_closed, record, id(1));
    EXPECT_FALSE(tx::reduce(record, recovery));
    record.active.erase(second.id);
    record.tombstones.emplace(second.id, second);
    EXPECT_FALSE(tx::reduce(record, recovery));
    record.active.erase(first.id);
    record.tombstones.emplace(first.id, first);
    EXPECT_FALSE(tx::reduce(record, recovery));
    record.tombstones.clear();
    record.active.emplace(first.id, first);
    record.phase = tx::phase_t::add_prepared;
    record.transaction_id = id(1);
    record.candidate = second;
    EXPECT_FALSE(tx::reduce(record, recovery));
  }

  TEST(CredentialTransaction, ConcurrentCompareAndPersistAllowsOnlyOneMutation) {
    fake_store_t store;
    std::atomic<bool> start {false};
    std::atomic<int> writes {0};
    std::vector<std::thread> threads;
    for (int i = 1; i <= 24; ++i) threads.emplace_back([&, i] {
      tx::event_t prepare {tx::action_t::prepare_add, 0, id(static_cast<std::uint8_t>(i)), device("device-" + std::to_string(i), static_cast<std::uint8_t>(i)), {}};
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      if (store.write(prepare)) ++writes;
    });
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(writes.load(), 1);
    EXPECT_EQ(store.snapshot().revision, std::uint64_t {1});
    EXPECT_EQ(store.snapshot().phase, tx::phase_t::add_prepared);
    EXPECT_TRUE(store.snapshot().active.empty());
  }

  TEST(CredentialTransaction, RejectsMalformedRecordsAndRevisionOverflow) {
    tx::record_t record;
    auto prepare = event(tx::action_t::prepare_add, record, id(1));
    prepare.candidate = device("ok", 1);
    record.phase = static_cast<tx::phase_t>(255);
    EXPECT_FALSE(tx::reduce(record, prepare));
    record = {};
    record.revision = std::numeric_limits<std::uint64_t>::max();
    prepare.expected_revision = record.revision;
    EXPECT_FALSE(tx::reduce(record, prepare));
    record = {};
    record.active.emplace("different-id", device("wrong", 2));
    prepare.expected_revision = 0;
    EXPECT_FALSE(tx::reduce(record, prepare));
  }
}  // namespace
