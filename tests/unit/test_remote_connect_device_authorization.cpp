#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "src/remote_connect/credential_transaction.h"
#include "src/remote_connect/device_authorization.h"
#include "src/remote_connect/enrollment_session.h"

namespace {
  using namespace std::chrono_literals;
  using namespace remote_connect;
  using credential_transaction::action_t;
  using credential_transaction::device_t;
  using credential_transaction::event_t;
  using credential_transaction::phase_t;
  using credential_transaction::public_key_t;
  using credential_transaction::record_t;
  using credential_transaction::transaction_id_t;
  using bytes32_t = enrollment_bytes32_t;
  using bytes_t = std::vector<std::uint8_t>;

  template<std::size_t N>
  std::array<std::uint8_t, N>
  from_hex(const char *text) {
    const std::string s(text);
    if (s.size() != 2 * N) throw std::runtime_error("Bad test key");
    std::array<std::uint8_t, N> out {};
    for (std::size_t i = 0; i < N; ++i) out[i] = static_cast<std::uint8_t>(std::stoul(s.substr(2 * i, 2), nullptr, 16));
    return out;
  }

  // The two public RFC 7748 section 6.1 key pairs, so each test device has its
  // own identity.
  const auto bob_private = from_hex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  const auto bob_public = from_hex<32>("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  const auto alice_private = from_hex<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const auto alice_public = from_hex<32>("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");

  bytes32_t
  certificate(std::uint8_t fill) {
    bytes32_t out {};
    out.fill(fill);
    return out;
  }

  enrollment_invitation_context_t
  context() {
    enrollment_invitation_context_t c {};
    c.host_certificate_sha256.fill(0x21);
    c.bootstrap_id.fill(0x32);
    c.host_ipv4 = {100, 64, 10, 1};
    c.client_ipv4 = {100, 64, 10, 2};
    c.enrollment_port = 47989;
    c.discovery_peer = "udp://discovery.example:11010";
    return c;
  }

  template<std::size_t N>
  void
  append(bytes_t &out, const std::array<std::uint8_t, N> &input) {
    out.insert(out.end(), input.begin(), input.end());
  }

  template<std::size_t N>
  void
  label(bytes_t &out, const char (&input)[N]) {
    out.insert(out.end(), input, input + N);
  }

  bytes32_t
  sha(const bytes_t &input) {
    bytes32_t out {};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), out.data(), &size, EVP_sha256(), nullptr) != 1 || size != out.size()) throw std::runtime_error("Test SHA failed");
    return out;
  }

  bytes32_t
  mac(const bytes32_t &key, const bytes_t &input) {
    bytes32_t out {};
    unsigned int size = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), input.data(), input.size(), out.data(), &size) || size != out.size()) throw std::runtime_error("Test HMAC failed");
    return out;
  }

  // Test-only client: real X25519, HKDF and HMAC against a fresh host
  // challenge, as the proof suite cross-checks against the shared vector.
  bytes32_t
  make_proof(const bytes32_t &client_private, const bytes32_t &client_public, const bytes32_t &client_certificate,
             const enrollment_invitation_context_t &c, const bytes32_t &token, const enrollment_session_t::challenge_t &challenge) {
    using key_t = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using ctx_t = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    key_t client(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, client_private.data(), client_private.size()), EVP_PKEY_free);
    key_t host(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, challenge.host_ephemeral_public.data(), challenge.host_ephemeral_public.size()), EVP_PKEY_free);
    if (!client || !host) throw std::runtime_error("Test key failed");
    ctx_t derive(EVP_PKEY_CTX_new(client.get(), nullptr), EVP_PKEY_CTX_free);
    bytes32_t shared {};
    std::size_t shared_size = shared.size();
    if (!derive || EVP_PKEY_derive_init(derive.get()) != 1 || EVP_PKEY_derive_set_peer(derive.get(), host.get()) != 1 ||
        EVP_PKEY_derive(derive.get(), shared.data(), &shared_size) != 1 || shared_size != shared.size()) throw std::runtime_error("Test derive failed");

    bytes_t t;
    label(t, "VPLUS-ENROLL-POP-V1");
    t.push_back(1);
    append(t, sha(bytes_t(token.begin(), token.end())));
    append(t, c.host_certificate_sha256);
    append(t, c.bootstrap_id);
    append(t, challenge.challenge);
    append(t, challenge.host_ephemeral_public);
    append(t, client_public);
    append(t, client_certificate);
    append(t, c.host_ipv4);
    append(t, c.client_ipv4);
    t.push_back(static_cast<std::uint8_t>(c.enrollment_port >> 8));
    t.push_back(static_cast<std::uint8_t>(c.enrollment_port));
    t.push_back(static_cast<std::uint8_t>(c.discovery_peer.size() >> 8));
    t.push_back(static_cast<std::uint8_t>(c.discovery_peer.size()));
    t.insert(t.end(), c.discovery_peer.begin(), c.discovery_peer.end());
    bytes_t salt_input;
    label(salt_input, "VPLUS-ENROLL-HKDF-SALT-V1");
    salt_input.insert(salt_input.end(), t.begin(), t.end());
    const auto prk = mac(sha(salt_input), bytes_t(shared.begin(), shared.end()));
    bytes_t key_info;
    label(key_info, "VPLUS-ENROLL-POP-KEY-V1");
    key_info.push_back(1);
    const auto key = mac(prk, key_info);
    bytes_t proof_input;
    label(proof_input, "VPLUS-ENROLL-POP-PROOF-V1");
    proof_input.insert(proof_input.end(), t.begin(), t.end());
    return mac(key, proof_input);
  }

  std::string
  hex(const public_key_t &key) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : key) {
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0f]);
    }
    return out;
  }

  const std::string bob_key = hex(bob_public);
  const std::string alice_key = hex(alice_public);

  event_t
  transition(action_t action, const record_t &record, transaction_id_t id, std::optional<device_t> candidate = std::nullopt, std::string target = {}) {
    event_t event;
    event.action = action;
    event.expected_revision = record.revision;
    event.transaction_id = id;
    event.candidate = std::move(candidate);
    event.target_id = std::move(target);
    return event;
  }

  record_t
  reduced(const record_t &before, const event_t &event) {
    auto after = credential_transaction::reduce(before, event);
    if (!after) throw std::runtime_error("Test transition rejected");
    return *after;
  }

  class FakeClock {
  public:
    enrollment_session_t::clock_t::time_point
    now() {
      return enrollment_session_t::clock_t::time_point(std::chrono::seconds(seconds_.load()));
    }

    void
    advance_past(std::int64_t seconds) {
      seconds_.store(seconds);
    }

  private:
    std::atomic<std::int64_t> seconds_ {100};
  };

  class DeviceAuthorizationTest : public ::testing::Test {
  protected:
    // One entry per side effect, in the order they happened, so a test can
    // assert that a record was stored before the runtime was touched.
    struct effect_t {
      bool is_persist;
      phase_t stored_phase;
      std::vector<std::string> keys;
    };

    void
    SetUp() override {
      session_ = std::make_unique<enrollment_session_t>([clock = &clock_()] { return clock->now(); });
      effects_.persist = [this](const record_t &record) {
        log_.push_back({ true, record.phase, {} });
        if (!persist_ok_) return false;
        stored_ = record;
        return true;
      };
      effects_.apply = [this](const std::vector<std::string> &keys) {
        log_.push_back({ false, phase_t::stable, keys });
        if (!apply_ok_) return false;
        if (fail_non_empty_apply_ && !keys.empty()) return false;
        if (fail_next_apply_) {
          fail_next_apply_ = false;
          return false;
        }
        runtime_keys_ = keys;
        return true;
      };
      effects_.next_transaction_id = [this]() -> std::optional<transaction_id_t> {
        if (!can_start_) return std::nullopt;
        transaction_id_t id {};
        id[0] = static_cast<std::uint8_t>(++transactions_ % 250 + 1);
        id[1] = static_cast<std::uint8_t>(transactions_ / 250 + 1);
        return id;
      };
      authorizer_ = std::make_unique<device_authorizer_t>(*session_, effects_);
    }

    static FakeClock &
    clock_() {
      static FakeClock clock;
      return clock;
    }

    // Runs one enrollment up to the owner-confirmed state and returns its
    // generation, or nullopt when `confirm` is false, in which case it stops
    // after a valid proof.
    std::optional<enrollment_session_t::invitation_t>
    enroll(const bytes32_t &client_private, const bytes32_t &client_public, const bytes32_t &client_certificate, bool confirm) {
      const auto c = context();
      auto invitation = session_->issue(c, 120s);
      if (!invitation) return std::nullopt;
      const auto challenge = session_->challenge(invitation->generation, invitation->token, 1);
      if (!challenge) return std::nullopt;
      const auto proof = make_proof(client_private, client_public, client_certificate, c, invitation->token, *challenge);
      const auto code = session_->prove(invitation->generation, invitation->token, 1, client_public, client_certificate, proof);
      if (!code) return std::nullopt;
      if (confirm && !session_->confirm_candidate(invitation->generation, *code)) return std::nullopt;
      return invitation;
    }

    std::optional<enrollment_session_t::invitation_t>
    confirmed(const bytes32_t &client_private = bob_private, const bytes32_t &client_public = bob_public, const bytes32_t &client_certificate = certificate(0x43)) {
      return enroll(client_private, client_public, client_certificate, true);
    }

    device_t
    device(std::string id, const bytes32_t &public_key, const bytes32_t &cert) {
      device_t out;
      out.id = std::move(id);
      out.easytier_public_key = public_key;
      out.gamestream_certificate_sha256 = cert;
      return out;
    }

    std::unique_ptr<enrollment_session_t> session_;
    device_authorization_effects_t effects_;
    std::unique_ptr<device_authorizer_t> authorizer_;
    std::vector<effect_t> log_;
    record_t stored_;
    std::vector<std::string> runtime_keys_;
    bool persist_ok_ = true;
    bool apply_ok_ = true;
    bool fail_non_empty_apply_ = false;
    bool fail_next_apply_ = false;
    bool can_start_ = true;

  private:
    std::uint64_t transactions_ = 0;
  };
}  // namespace

TEST(RemoteEnrollmentSession, ConfirmedCandidateIsHandedOverExactlyOnceAndCannotBeCancelledAfterwards) {
  FakeClock clock;
  enrollment_session_t session([&clock] { return clock.now(); });
  const auto c = context();
  const auto invitation = session.issue(c, 120s);
  ASSERT_TRUE(invitation);
  const auto challenge = session.challenge(invitation->generation, invitation->token, 1);
  ASSERT_TRUE(challenge);
  const auto proof = make_proof(bob_private, bob_public, certificate(0x43), c, invitation->token, *challenge);
  const auto code = session.prove(invitation->generation, invitation->token, 1, bob_public, certificate(0x43), proof);
  ASSERT_TRUE(code);

  // Before the owner compares the code there is nothing to hand over.
  EXPECT_FALSE(session.take_confirmed_candidate(invitation->generation).has_value());
  EXPECT_EQ(session.state(invitation->generation), enrollment_session_t::state_t::awaiting_confirmation);

  ASSERT_TRUE(session.confirm_candidate(invitation->generation, *code));
  EXPECT_FALSE(session.take_confirmed_candidate(invitation->generation + 1).has_value());

  const auto candidate = session.take_confirmed_candidate(invitation->generation);
  ASSERT_TRUE(candidate);
  EXPECT_EQ(candidate->client_public, bob_public);
  EXPECT_EQ(candidate->client_certificate_sha256, certificate(0x43));

  // Taken once: the invitation is over and cannot be taken or cancelled again.
  EXPECT_FALSE(session.take_confirmed_candidate(invitation->generation).has_value());
  EXPECT_FALSE(session.cancel(invitation->generation));
  EXPECT_EQ(session.state(invitation->generation), enrollment_session_t::state_t::inactive);
}

TEST(RemoteEnrollmentSession, ExpiredConfirmationIsNotHandedOver) {
  FakeClock clock;
  enrollment_session_t session([&clock] { return clock.now(); });
  const auto c = context();
  const auto invitation = session.issue(c, 120s);
  ASSERT_TRUE(invitation);
  const auto challenge = session.challenge(invitation->generation, invitation->token, 1);
  ASSERT_TRUE(challenge);
  const auto proof = make_proof(bob_private, bob_public, certificate(0x43), c, invitation->token, *challenge);
  const auto code = session.prove(invitation->generation, invitation->token, 1, bob_public, certificate(0x43), proof);
  ASSERT_TRUE(code);
  ASSERT_TRUE(session.confirm_candidate(invitation->generation, *code));

  clock.advance_past(100 + 120);

  EXPECT_FALSE(session.take_confirmed_candidate(invitation->generation).has_value());
}

TEST_F(DeviceAuthorizationTest, AdoptingAnEmptyRecordClosesAccessAndAuthorizesNobody) {
  EXPECT_TRUE(authorizer_->adopt(record_t {}).success);
  EXPECT_TRUE(authorizer_->devices().empty());
  ASSERT_EQ(log_.size(), 1u);
  EXPECT_FALSE(log_[0].is_persist);
  EXPECT_TRUE(log_[0].keys.empty());
}

TEST_F(DeviceAuthorizationTest, AnInvalidStoredRecordAuthorizesNothingAndTouchesNoRuntime) {
  record_t broken;
  broken.active.emplace("phone", device("phone", bob_public, certificate(0x43)));
  // A key that is zero is not a valid device identity.
  broken.active["phone"].easytier_public_key = {};

  const auto result = authorizer_->adopt(broken);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("invalid"), std::string::npos) << result.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_TRUE(log_.empty());
}

TEST_F(DeviceAuthorizationTest, AuthorizingStoresTheDeviceBeforeItTouchesTheRuntime) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  log_.clear();
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);

  const auto result = authorizer_->authorize(invitation->generation, "phone");

  ASSERT_TRUE(result.success) << result.error;
  const auto devices = authorizer_->devices();
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices[0], device("phone", bob_public, certificate(0x43)));
  EXPECT_EQ(runtime_keys_, std::vector<std::string> { bob_key });

  // Every revision is stored as it is reached, the committed device among
  // them, and only then is the runtime aligned and the transaction closed.
  ASSERT_EQ(log_.size(), 5u);
  const phase_t expected[] { phase_t::add_prepared, phase_t::add_verified_closed, phase_t::add_committed_closed };
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_TRUE(log_[index].is_persist) << index;
    EXPECT_EQ(log_[index].stored_phase, expected[index]) << index;
  }
  EXPECT_FALSE(log_[3].is_persist);
  EXPECT_EQ(log_[3].keys, std::vector<std::string> { bob_key });
  EXPECT_TRUE(log_[4].is_persist);
  EXPECT_EQ(log_[4].stored_phase, phase_t::stable);
  EXPECT_EQ(stored_.phase, phase_t::stable);
  EXPECT_EQ(stored_.active.size(), 1u);
}

TEST_F(DeviceAuthorizationTest, NothingIsAuthorizedWithoutAnOwnerConfirmedEnrollment) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  log_.clear();

  // A generation that was never issued.
  auto result = authorizer_->authorize(99, "phone");
  EXPECT_FALSE(result.success);

  // A valid proof that the owner has not confirmed yet.
  const auto invitation = enroll(bob_private, bob_public, certificate(0x43), false);
  ASSERT_TRUE(invitation);
  result = authorizer_->authorize(invitation->generation, "phone");
  EXPECT_FALSE(result.success);

  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_TRUE(log_.empty()) << "an unconfirmed candidate must not reach the record or the runtime";
}

TEST_F(DeviceAuthorizationTest, AnInvalidDeviceIdDoesNotConsumeTheCandidate) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);

  const auto rejected = authorizer_->authorize(invitation->generation, "my phone!");
  EXPECT_FALSE(rejected.success);
  EXPECT_NE(rejected.error.find("device id"), std::string::npos) << rejected.error;

  // The same confirmed enrollment still goes through with a valid id.
  EXPECT_TRUE(authorizer_->authorize(invitation->generation, "my-phone").success);
  EXPECT_EQ(authorizer_->devices().size(), 1u);
}

TEST_F(DeviceAuthorizationTest, TheSecondDeviceKeepsItsOwnEntryAndTheRuntimeHoldsBothKeys) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto first = confirmed(bob_private, bob_public, certificate(0x43));
  ASSERT_TRUE(first);
  ASSERT_TRUE(authorizer_->authorize(first->generation, "phone").success);
  const auto second = confirmed(alice_private, alice_public, certificate(0x44));
  ASSERT_TRUE(second);

  ASSERT_TRUE(authorizer_->authorize(second->generation, "tablet").success);

  const auto devices = authorizer_->devices();
  ASSERT_EQ(devices.size(), 2u);
  EXPECT_EQ(devices[0].id, "phone");
  EXPECT_EQ(devices[1].id, "tablet");
  // The set is in device id order: "phone" holds Bob's key, "tablet" Alice's.
  EXPECT_EQ(runtime_keys_, (std::vector<std::string> { bob_key, alice_key }));
}

TEST_F(DeviceAuthorizationTest, AKeyOrCertificateThatIsAlreadyEnrolledIsRejectedAgain) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto first = confirmed();
  ASSERT_TRUE(first);
  ASSERT_TRUE(authorizer_->authorize(first->generation, "phone").success);

  // The same EasyTier key, under a different device id.
  const auto same_key = confirmed(bob_private, bob_public, certificate(0x99));
  ASSERT_TRUE(same_key);
  auto result = authorizer_->authorize(same_key->generation, "phone-2");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("already enrolled"), std::string::npos) << result.error;

  // A different key that presents an already enrolled certificate.
  const auto same_certificate = confirmed(alice_private, alice_public, certificate(0x43));
  ASSERT_TRUE(same_certificate);
  result = authorizer_->authorize(same_certificate->generation, "tablet");
  EXPECT_FALSE(result.success);

  EXPECT_EQ(authorizer_->devices().size(), 1u);
  EXPECT_EQ(stored_.active.size(), 1u);
}

TEST_F(DeviceAuthorizationTest, RevokingRemovesTheDeviceAndClosesItOutForGood) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);
  ASSERT_TRUE(authorizer_->authorize(invitation->generation, "phone").success);
  log_.clear();

  const auto result = authorizer_->revoke("phone");

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_TRUE(runtime_keys_.empty());
  ASSERT_EQ(stored_.tombstones.size(), 1u);
  EXPECT_EQ(stored_.tombstones.at("phone").easytier_public_key, bob_public);
  EXPECT_EQ(stored_.phase, phase_t::stable);

  // The record stops authorizing the device before the runtime is told.
  ASSERT_GE(log_.size(), 2u);
  EXPECT_TRUE(log_[0].is_persist);
  EXPECT_EQ(log_[0].stored_phase, phase_t::revoke_pending);
  EXPECT_FALSE(log_[1].is_persist);
  EXPECT_TRUE(log_[1].keys.empty());

  // The revoked key cannot come back under a new device id.
  const auto again = confirmed();
  ASSERT_TRUE(again);
  const auto rejected = authorizer_->authorize(again->generation, "phone-again");
  EXPECT_FALSE(rejected.success);
  EXPECT_TRUE(authorizer_->devices().empty());
}

TEST_F(DeviceAuthorizationTest, RevokingAnUnknownDeviceChangesNothing) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  log_.clear();

  const auto result = authorizer_->revoke("nobody");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("not enrolled"), std::string::npos) << result.error;
  EXPECT_TRUE(log_.empty());
  EXPECT_EQ(stored_.phase, phase_t::stable);
}

TEST_F(DeviceAuthorizationTest, ARefusedRuntimeOnAuthorizeClosesAccessAndKeepsTheDeviceRecorded) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);
  fail_non_empty_apply_ = true;

  const auto result = authorizer_->authorize(invitation->generation, "phone");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("remote access is closed"), std::string::npos) << result.error;
  // The runtime was asked for the device and, refused, asked to close.
  ASSERT_GE(log_.size(), 2u);
  EXPECT_EQ(log_[log_.size() - 2].keys, std::vector<std::string> { bob_key });
  EXPECT_TRUE(log_[log_.size() - 1].keys.empty());
  EXPECT_TRUE(runtime_keys_.empty());
  // The record is committed but its transaction is left open for recovery.
  const auto devices = authorizer_->devices();
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(stored_.phase, phase_t::add_committed_closed);

  // Until then no new operation can start.
  const auto blocked = authorizer_->revoke("phone");
  EXPECT_FALSE(blocked.success);
  EXPECT_NE(blocked.error.find("unfinished"), std::string::npos) << blocked.error;

  // A restart recovers the transaction and aligns the runtime.
  fail_non_empty_apply_ = false;
  const auto recovered = authorizer_->adopt(stored_);
  EXPECT_TRUE(recovered.success) << recovered.error;
  EXPECT_EQ(authorizer_->devices().size(), 1u);
  EXPECT_EQ(runtime_keys_, std::vector<std::string> { bob_key });
  EXPECT_EQ(stored_.phase, phase_t::stable);
}

TEST_F(DeviceAuthorizationTest, ARefusedRuntimeOnRevokeClosesAccessAndKeepsTheDeviceRevoked) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);
  ASSERT_TRUE(authorizer_->authorize(invitation->generation, "phone").success);
  // Revoking the only device aligns the runtime with an empty set, so a
  // runtime that refuses only non-empty sets would never fail here. Refuse
  // the next call once instead; the closing attempt after it still works.
  fail_next_apply_ = true;

  const auto result = authorizer_->revoke("phone");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("remote access is closed"), std::string::npos) << result.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_EQ(stored_.phase, phase_t::revoke_pending);
  EXPECT_EQ(stored_.tombstones.size(), 1u);
  EXPECT_TRUE(runtime_keys_.empty());

  fail_non_empty_apply_ = false;
  const auto recovered = authorizer_->adopt(stored_);
  EXPECT_TRUE(recovered.success) << recovered.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_EQ(stored_.phase, phase_t::stable);
}

TEST_F(DeviceAuthorizationTest, ARuntimeThatCannotEvenCloseIsReportedAsUnknownNotAsClosed) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);
  ASSERT_TRUE(authorizer_->authorize(invitation->generation, "phone").success);
  // Nothing the runtime is asked for succeeds any more.
  apply_ok_ = false;

  const auto result = authorizer_->revoke("phone");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("state is unknown"), std::string::npos) << result.error;
  // It must not claim access is closed when closing was refused too.
  EXPECT_EQ(result.error.find("remote access is closed"), std::string::npos) << result.error;
  // The record still says revoked, which is the safe side to be wrong on.
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_EQ(stored_.tombstones.size(), 1u);
}

TEST_F(DeviceAuthorizationTest, AFailedStoreStopsEverythingUntilTheRecordIsAdoptedAgain) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);
  log_.clear();
  persist_ok_ = false;

  const auto result = authorizer_->authorize(invitation->generation, "phone");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("could not be stored"), std::string::npos) << result.error;
  // The record never reached a revision that authorizes the device, so the
  // runtime was not touched at all.
  EXPECT_TRUE(runtime_keys_.empty());
  EXPECT_FALSE(std::any_of(log_.begin(), log_.end(), [](const auto &entry) { return !entry.is_persist; }));
  EXPECT_TRUE(authorizer_->devices().empty());

  const auto blocked = authorizer_->revoke("phone");
  EXPECT_FALSE(blocked.success);
  EXPECT_NE(blocked.error.find("unavailable"), std::string::npos) << blocked.error;

  // What was last stored before the failure is a sound record.
  persist_ok_ = true;
  const auto recovered = authorizer_->adopt(stored_);
  EXPECT_TRUE(recovered.success) << recovered.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_EQ(stored_.phase, phase_t::stable);
}

TEST_F(DeviceAuthorizationTest, NoTransactionIdMeansNothingChanges) {
  ASSERT_TRUE(authorizer_->adopt(record_t {}).success);
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);
  log_.clear();
  can_start_ = false;

  const auto result = authorizer_->authorize(invitation->generation, "phone");

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("transaction"), std::string::npos) << result.error;
  EXPECT_TRUE(log_.empty());
  EXPECT_TRUE(authorizer_->devices().empty());
}

TEST_F(DeviceAuthorizationTest, NothingWorksBeforeARecordIsAdopted) {
  const auto invitation = confirmed();
  ASSERT_TRUE(invitation);

  EXPECT_FALSE(authorizer_->authorize(invitation->generation, "phone").success);
  EXPECT_FALSE(authorizer_->revoke("phone").success);
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_TRUE(log_.empty());
}

TEST_F(DeviceAuthorizationTest, AdoptingDropsAnAddThatWasInterruptedBeforeItCommitted) {
  const transaction_id_t id { 7 };
  const auto candidate = device("phone", bob_public, certificate(0x43));
  auto interrupted = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, candidate));

  const auto result = authorizer_->adopt(interrupted);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_EQ(stored_.phase, phase_t::stable);
  EXPECT_TRUE(stored_.tombstones.empty());
  EXPECT_TRUE(runtime_keys_.empty());
}

TEST_F(DeviceAuthorizationTest, AdoptingKeepsAnAddThatWasCommittedWhenTheCrashHappened) {
  const transaction_id_t id { 7 };
  const auto candidate = device("phone", bob_public, certificate(0x43));
  auto committed = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, id, candidate));
  committed = reduced(committed, transition(action_t::add_verified, committed, id));
  committed = reduced(committed, transition(action_t::commit_add, committed, id));

  const auto result = authorizer_->adopt(committed);

  ASSERT_TRUE(result.success) << result.error;
  const auto devices = authorizer_->devices();
  ASSERT_EQ(devices.size(), 1u);
  EXPECT_EQ(devices[0], candidate);
  EXPECT_EQ(runtime_keys_, std::vector<std::string> { bob_key });
  EXPECT_EQ(stored_.phase, phase_t::stable);
  // The runtime is aligned only once the recovery is stored.
  ASSERT_FALSE(log_.empty());
  EXPECT_TRUE(log_.front().is_persist);
  EXPECT_FALSE(log_.back().is_persist);
}

TEST_F(DeviceAuthorizationTest, AdoptingKeepsADeviceRevokedWhenTheCrashInterruptedItsRevoke) {
  const transaction_id_t add_id { 7 };
  const auto candidate = device("phone", bob_public, certificate(0x43));
  auto active = reduced(record_t {}, transition(action_t::prepare_add, record_t {}, add_id, candidate));
  active = reduced(active, transition(action_t::add_verified, active, add_id));
  active = reduced(active, transition(action_t::commit_add, active, add_id));
  active = reduced(active, transition(action_t::finish_closed, active, add_id));
  const transaction_id_t revoke_id { 8 };
  const auto interrupted = reduced(active, transition(action_t::prepare_revoke, active, revoke_id, std::nullopt, "phone"));

  const auto result = authorizer_->adopt(interrupted);

  ASSERT_TRUE(result.success) << result.error;
  EXPECT_TRUE(authorizer_->devices().empty());
  EXPECT_EQ(stored_.tombstones.size(), 1u);
  EXPECT_EQ(stored_.phase, phase_t::stable);
  EXPECT_TRUE(runtime_keys_.empty());
}
