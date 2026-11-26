#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
  #include <windows.h>
  #include <aclapi.h>
  #include <sddl.h>
#endif

#include "src/remote_connect/credential_transaction.h"
#include "src/remote_connect/device_record_store.h"

namespace {
  using namespace remote_connect;
  using credential_transaction::action_t;
  using credential_transaction::device_t;
  using credential_transaction::phase_t;
  using credential_transaction::public_key_t;
  using credential_transaction::record_t;
  using credential_transaction::transaction_id_t;

  public_key_t
  key(std::uint8_t fill) {
    public_key_t out {};
    out.fill(fill);
    return out;
  }

  device_t
  device(std::string id, std::uint8_t key_fill, std::uint8_t certificate_fill) {
    device_t out;
    out.id = std::move(id);
    out.easytier_public_key = key(key_fill);
    out.gamestream_certificate_sha256 = key(certificate_fill);
    return out;
  }

  credential_transaction::event_t
  transition(action_t action, const record_t &record, transaction_id_t id, std::optional<device_t> candidate = std::nullopt, std::string target = {}) {
    credential_transaction::event_t event;
    event.action = action;
    event.expected_revision = record.revision;
    event.transaction_id = id;
    event.candidate = std::move(candidate);
    event.target_id = std::move(target);
    return event;
  }

  record_t
  reduced(const record_t &before, const credential_transaction::event_t &event) {
    auto after = credential_transaction::reduce(before, event);
    if (!after) throw std::runtime_error("Test transition rejected");
    return *after;
  }

  bool
  same_record(const record_t &left, const record_t &right) {
    return left.revision == right.revision && left.phase == right.phase && left.active == right.active &&
           left.tombstones == right.tombstones && left.candidate == right.candidate &&
           left.revoke_target == right.revoke_target && left.transaction_id == right.transaction_id &&
           left.last_transaction_id == right.last_transaction_id;
  }

  std::string
  read_bytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return { (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() };
  }

  void
  write_bytes(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
  }

  record_t
  one_device_record() {
    const transaction_id_t id { 7 };
    auto record = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, device("phone", 0x11, 0x43)));
    record = reduced(record, transition(action_t::add_verified, record, id));
    record = reduced(record, transition(action_t::commit_add, record, id));
    return reduced(record, transition(action_t::finish_closed, record, id));
  }

  class DeviceRecordStoreTest : public ::testing::Test {
  protected:
    void
    SetUp() override {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      directory_ = std::filesystem::temp_directory_path() / ("sunshine-device-record-" + std::to_string(stamp) + "-" + std::to_string(++sequence_));
      std::filesystem::create_directories(directory_);
      path_ = directory_ / "remote-devices.json";
    }

    void
    TearDown() override {
      std::error_code ignored;
      std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path temporary_path() const {
      auto temporary = path_;
      temporary += L".tmp";
      return temporary;
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
    inline static std::atomic<unsigned long long> sequence_ = 0;
  };
}  // namespace

TEST_F(DeviceRecordStoreTest, AMissingFileIsFreshNotAnError) {
  const auto loaded = device_record_store::load(path_);

  EXPECT_EQ(loaded.status, device_record_store::load_status_e::fresh);
  EXPECT_TRUE(same_record(loaded.record, record_t {}));
}

TEST_F(DeviceRecordStoreTest, AnEmptyRecordRoundTrips) {
  ASSERT_TRUE(device_record_store::store(path_, record_t {}));

  const auto loaded = device_record_store::load(path_);

  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  EXPECT_TRUE(same_record(loaded.record, record_t {}));
}

// Every phase a crash can leave behind has to survive the round trip exactly,
// because recovery depends on telling them apart.
TEST_F(DeviceRecordStoreTest, EveryPhaseOfTheAddTransactionRoundTrips) {
  const transaction_id_t id { 7 };
  const auto candidate = device("phone", 0x11, 0x43);
  auto record = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, candidate));
  const phase_t phases[] { phase_t::add_prepared, phase_t::add_verified_closed, phase_t::add_committed_closed };
  // steps[index] takes phases[index] to phases[index + 1].
  const action_t steps[] { action_t::add_verified, action_t::commit_add };

  for (std::size_t index = 0; index < 3; ++index) {
    SCOPED_TRACE(static_cast<int>(phases[index]));
    ASSERT_EQ(record.phase, phases[index]);
    ASSERT_TRUE(device_record_store::store(path_, record));
    const auto loaded = device_record_store::load(path_);
    ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
    EXPECT_TRUE(same_record(loaded.record, record));
    if (index < 2) record = reduced(record, transition(steps[index], record, id));
  }
}

TEST_F(DeviceRecordStoreTest, EveryPhaseOfTheRevokeTransactionRoundTrips) {
  const transaction_id_t add_id { 7 };
  auto record = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, add_id, device("phone", 0x11, 0x43)));
  record = reduced(record, transition(action_t::add_verified, record, add_id));
  record = reduced(record, transition(action_t::commit_add, record, add_id));
  record = reduced(record, transition(action_t::finish_closed, record, add_id));

  const transaction_id_t revoke_id { 8 };
  record = reduced(record, transition(action_t::prepare_revoke, record, revoke_id, std::nullopt, "phone"));
  const phase_t phases[] { phase_t::revoke_pending, phase_t::revoke_verified_closed, phase_t::revoke_committed_closed };
  // steps[index] takes phases[index] to phases[index + 1].
  const action_t steps[] { action_t::revoke_verified, action_t::commit_revoke };
  for (std::size_t index = 0; index < 3; ++index) {
    SCOPED_TRACE(static_cast<int>(phases[index]));
    ASSERT_EQ(record.phase, phases[index]);
    ASSERT_TRUE(device_record_store::store(path_, record));
    const auto loaded = device_record_store::load(path_);
    ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
    EXPECT_TRUE(same_record(loaded.record, record));
    if (index < 2) record = reduced(record, transition(steps[index], record, revoke_id));
  }
}

TEST_F(DeviceRecordStoreTest, ARollbackAndARecoveryRoundTrip) {
  const transaction_id_t id { 7 };
  auto record = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, device("phone", 0x11, 0x43)));
  record = reduced(record, transition(action_t::abort_add, record, id));
  ASSERT_EQ(record.phase, phase_t::add_rollback_closed);
  ASSERT_TRUE(device_record_store::store(path_, record));
  EXPECT_TRUE(same_record(device_record_store::load(path_).record, record));

  record = reduced(record, transition(action_t::finish_closed, record, id));
  // Recovery from a stable record is its own transaction, with a new id.
  const transaction_id_t recovery_id { 8 };
  record = reduced(record, transition(action_t::recover_closed, record, recovery_id));
  ASSERT_EQ(record.phase, phase_t::recovery_closed);
  ASSERT_TRUE(device_record_store::store(path_, record));
  EXPECT_TRUE(same_record(device_record_store::load(path_).record, record));
}

TEST_F(DeviceRecordStoreTest, SeveralDevicesKeepTheirIdsKeysAndCertificates) {
  const transaction_id_t id { 7 };
  auto record = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, device("phone", 0x11, 0x43)));
  record = reduced(record, transition(action_t::add_verified, record, id));
  record = reduced(record, transition(action_t::commit_add, record, id));
  record = reduced(record, transition(action_t::finish_closed, record, id));

  const transaction_id_t second { 8 };
  record = reduced(record, transition(action_t::prepare_add, record, second, device("tablet", 0x22, 0x44)));
  record = reduced(record, transition(action_t::add_verified, record, second));
  record = reduced(record, transition(action_t::commit_add, record, second));
  record = reduced(record, transition(action_t::finish_closed, record, second));

  ASSERT_TRUE(device_record_store::store(path_, record));
  const auto loaded = device_record_store::load(path_);
  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  ASSERT_EQ(loaded.record.active.size(), 2u);
  EXPECT_EQ(loaded.record.active.at("phone"), device("phone", 0x11, 0x43));
  EXPECT_EQ(loaded.record.active.at("tablet"), device("tablet", 0x22, 0x44));
  EXPECT_EQ(loaded.record.revision, record.revision);
  EXPECT_EQ(loaded.record.last_transaction_id, second);
}

TEST_F(DeviceRecordStoreTest, TextThatIsNotJsonIsInvalid) {
  write_bytes(path_, "{ this is not json");

  const auto loaded = device_record_store::load(path_);

  EXPECT_EQ(loaded.status, device_record_store::load_status_e::invalid);
  EXPECT_TRUE(same_record(loaded.record, record_t {}));
}

TEST_F(DeviceRecordStoreTest, ARecordFromAnotherVersionIsInvalidRatherThanGuessedAt) {
  const auto stored = read_bytes(path_);
  (void) stored;
  write_bytes(path_, R"({"version":2,"revision":0,"phase":"stable","devices":[],"revoked":[],"candidate":null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"00000000000000000000000000000000"})");

  EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::invalid);
}

TEST_F(DeviceRecordStoreTest, AWronglyTypedFieldIsInvalid) {
  const std::string valid = R"({"version":1,"revision":0,"phase":"stable","devices":[],"revoked":[],"candidate":null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"00000000000000000000000000000000"})";
  const std::string with_device = R"({"version":1,"revision":4,"phase":"stable","devices":[{"id":"phone","easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000011","gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000043"}],"revoked":[],"candidate":null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"07000000000000000000000000000000"})";

  struct replacement_t {
    std::string from;
    std::string to;
  };
  const replacement_t replacements[] {
    { R"("revision":0)", R"("revision":-1)" },
    { R"("revision":0)", R"("revision":1.5)" },
    { R"("revision":0)", R"("revision":"0")" },
    { R"("phase":"stable")", R"("phase":"enrolling")" },
    { R"("phase":"stable")", R"("phase":0)" },
    { R"("devices":[])", R"("devices":{})" },
    { R"("devices":[])", R"("devices":"none")" },
    { R"("candidate":null)", R"("candidate":"phone")" },
    { R"("revoke_target":null)", R"("revoke_target":7)" },
    { R"("transaction_id":"00000000000000000000000000000000")", R"("transaction_id":"00")" },
    { R"("transaction_id":"00000000000000000000000000000000")", R"("transaction_id":null)" },
    { R"("last_transaction_id":"00000000000000000000000000000000")", R"("last_transaction_id":"zz000000000000000000000000000000")" },
    { R"("last_transaction_id":"00000000000000000000000000000000")", },
    { R"("devices":[],"revoked":[])", R"("revoked":[])" },
    { R"("version":1)", R"("version":"1")" },
    { R"("version":1)", },
  };
  for (const auto &replacement : replacements) {
    std::string text = valid;
    const auto at = text.find(replacement.from);
    ASSERT_NE(at, std::string::npos) << replacement.from;
    text.replace(at, replacement.from.size(), replacement.to);
    write_bytes(path_, text);
    EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::invalid) << text;
  }

  // The same shape with one well-formed device is accepted, so the rejections
  // above are about the changed field and not about this text.
  write_bytes(path_, with_device);
  const auto loaded = device_record_store::load(path_);
  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  EXPECT_EQ(loaded.record.active.size(), 1u);

  const replacement_t device_replacements[] {
    { R"("easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000011")", R"("easytier_public_key":"11")" },
    { R"("easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000011")", R"("easytier_public_key":"zz0000000000000000000000000000000000000000000000000000000000000011")" },
    { R"("gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000043")", R"("gamestream_certificate_sha256":null)" },
    { R"("gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000043")", },
    { R"("id":"phone")", R"("id":"")" },
    { R"("id":"phone")", R"("id":"my phone")" },
    { R"("id":"phone")", R"("id":5)" },
  };
  for (const auto &replacement : device_replacements) {
    std::string text = with_device;
    const auto at = text.find(replacement.from);
    ASSERT_NE(at, std::string::npos) << replacement.from;
    text.replace(at, replacement.from.size(), replacement.to);
    write_bytes(path_, text);
    EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::invalid) << text;
  }
}

// The reducer has invariants a file must satisfy too: one id, key or
// certificate cannot be both active and revoked, and a stable record cannot
// still carry a candidate.
TEST_F(DeviceRecordStoreTest, ARecordTheReducerWouldRejectIsInvalid) {
  const std::string device_json = R"({"id":"phone","easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000011","gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000043"})";
  const std::string other_json = R"({"id":"tablet","easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000022","gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000044"})";
  const std::string head = R"({"version":1,"revision":4,"phase":"stable","devices":[)";
  const std::string middle = R"(],"revoked":[)";
  const std::string tail = R"(],"candidate":)";

  const std::string texts[] {
    // The same id in both lists.
    head + device_json + middle + device_json + tail + R"(null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"07000000000000000000000000000000"})",
    // The same key under another id.
    head + device_json + middle + R"({"id":"tablet","easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000011","gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000044"})" + tail + R"(null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"07000000000000000000000000000000"})",
    // A candidate on a stable record.
    head + middle + tail + device_json + R"(,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"07000000000000000000000000000000"})",
    // A zero key.
    R"({"version":1,"revision":4,"phase":"stable","devices":[{"id":"phone","easytier_public_key":"0000000000000000000000000000000000000000000000000000000000000000","gamestream_certificate_sha256":"0000000000000000000000000000000000000000000000000000000000000043"}],"revoked":[],"candidate":null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"07000000000000000000000000000000"})",
  };
  for (const auto &text : texts) {
    write_bytes(path_, text);
    EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::invalid) << text;
  }

  // Both devices, each with its own identity, are fine.
  write_bytes(path_, head + device_json + "," + other_json + middle + tail + R"(null,"revoke_target":null,"transaction_id":"00000000000000000000000000000000","last_transaction_id":"07000000000000000000000000000000"})");
  const auto loaded = device_record_store::load(path_);
  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  EXPECT_EQ(loaded.record.active.size(), 2u);
}

TEST_F(DeviceRecordStoreTest, AnEmptyFileIsInvalid) {
  write_bytes(path_, "");

  EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::invalid);
}

TEST_F(DeviceRecordStoreTest, AnOversizedFileIsInvalidWithoutBeingParsed) {
  ASSERT_TRUE(device_record_store::store(path_, record_t {}));
  // Still well-formed JSON, just far larger than any record of this host.
  write_bytes(path_, read_bytes(path_) + std::string(1024 * 1024 + 16, ' '));

  EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::invalid);
}

TEST_F(DeviceRecordStoreTest, ADirectoryWhereTheRecordShouldBeIsUnreadable) {
  std::filesystem::create_directories(path_);

  EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::unreadable);
}

TEST_F(DeviceRecordStoreTest, StoringReplacesThePreviousRecordAndLeavesNoTemporaryFile) {
  const auto first = one_device_record();
  ASSERT_TRUE(device_record_store::store(path_, first));
  ASSERT_TRUE(device_record_store::store(path_, record_t {}));

  const auto loaded = device_record_store::load(path_);
  ASSERT_EQ(loaded.status, device_record_store::load_status_e::loaded);
  EXPECT_TRUE(same_record(loaded.record, record_t {}));
  EXPECT_FALSE(std::filesystem::exists(temporary_path()));
}

TEST_F(DeviceRecordStoreTest, StoringAnInvalidRecordWritesNothing) {
  ASSERT_TRUE(device_record_store::store(path_, one_device_record()));
  const auto before = read_bytes(path_);

  record_t invalid = one_device_record();
  invalid.active.at("phone").easytier_public_key = {};
  EXPECT_FALSE(device_record_store::store(path_, invalid));
  record_t unknown_phase = one_device_record();
  unknown_phase.phase = static_cast<phase_t>(99);
  EXPECT_FALSE(device_record_store::store(path_, unknown_phase));
  record_t inconsistent = one_device_record();
  // A stable record does not carry an open transaction.
  inconsistent.transaction_id = { 9 };
  EXPECT_FALSE(device_record_store::store(path_, inconsistent));

  EXPECT_EQ(read_bytes(path_), before);
  EXPECT_FALSE(std::filesystem::exists(temporary_path()));
}

TEST_F(DeviceRecordStoreTest, StoringIntoAMissingDirectoryFailsAndCreatesNothing) {
  const auto nowhere = directory_ / "missing" / "remote-devices.json";

  EXPECT_FALSE(device_record_store::store(nowhere, record_t {}));
  EXPECT_FALSE(std::filesystem::exists(directory_ / "missing"));
}

TEST_F(DeviceRecordStoreTest, ARecordWhoseFileIsDeletedReadsAsFreshAgain) {
  ASSERT_TRUE(device_record_store::store(path_, one_device_record()));
  std::filesystem::remove(path_);

  EXPECT_EQ(device_record_store::load(path_).status, device_record_store::load_status_e::fresh);
}

#ifdef _WIN32
// Whoever can write the record can authorize a device, so it is readable and
// writable only by SYSTEM, administrators and its owner, and it does not
// inherit wider access from the folder it lies in.
TEST_F(DeviceRecordStoreTest, TheRecordIsOpenOnlyToSystemAdministratorsAndItsOwner) {
  ASSERT_TRUE(device_record_store::store(path_, record_t {}));

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PSID owner = nullptr;
  PSID group = nullptr;
  PACL dacl = nullptr;
  const DWORD result = GetNamedSecurityInfoW(
    path_.c_str(),
    SE_FILE_OBJECT,
    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
    &owner,
    &group,
    &dacl,
    nullptr,
    &descriptor
  );
  ASSERT_EQ(result, ERROR_SUCCESS);
  const auto release = [](void *pointer) { if (pointer) LocalFree(pointer); };

  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  ASSERT_TRUE(GetSecurityDescriptorControl(descriptor, &control, &revision));
  EXPECT_TRUE(control & SE_DACL_PROTECTED) << "the record must not inherit access from its folder";

  ASSERT_NE(dacl, nullptr);
  ASSERT_EQ(dacl->AceCount, 3);
  std::set<std::wstring> trustees;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void *ace = nullptr;
    ASSERT_TRUE(GetAce(dacl, index, &ace));
    const auto *header = static_cast<const ACE_HEADER *>(ace);
    EXPECT_EQ(header->AceType, ACCESS_ALLOWED_ACE_TYPE);
    const auto *allowed = static_cast<const ACCESS_ALLOWED_ACE *>(ace);
    EXPECT_EQ(allowed->Mask, FILE_ALL_ACCESS);
    LPWSTR text = nullptr;
    ASSERT_TRUE(ConvertSidToStringSidW(reinterpret_cast<PSID>(const_cast<ULONG *>(&allowed->SidStart)), &text));
    trustees.insert(text);
    release(text);
  }
  // SYSTEM, Administrators, and Owner Rights.
  EXPECT_EQ(trustees, (std::set<std::wstring> { L"S-1-5-18", L"S-1-5-32-544", L"S-1-3-4" }));

  release(descriptor);
}
#endif
