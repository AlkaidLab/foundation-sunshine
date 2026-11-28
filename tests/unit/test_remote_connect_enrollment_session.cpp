#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "src/remote_connect/enrollment_session.h"

namespace {
  using namespace std::chrono_literals;
  using namespace remote_connect;
  using session_t = enrollment_session_t;
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

  const auto client_private = from_hex<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  const auto client_public = from_hex<32>("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

  struct fake_clock_t {
    std::atomic<std::int64_t> seconds {0};
    std::atomic<int> calls {0};
    std::atomic<int> advance_at {0};
    session_t::clock_t::time_point now() {
      const auto call = ++calls;
      if (advance_at.load() != 0 && call >= advance_at.load()) seconds.store(30);
      return session_t::clock_t::time_point(std::chrono::seconds(seconds.load()));
    }
  };

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

  enrollment_bytes32_t
  client_cert() {
    enrollment_bytes32_t result {};
    result.fill(0x43);
    return result;
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

  enrollment_bytes32_t
  sha(const bytes_t &input) {
    enrollment_bytes32_t out {};
    unsigned int size = 0;
    if (EVP_Digest(input.data(), input.size(), out.data(), &size, EVP_sha256(), nullptr) != 1 || size != out.size()) throw std::runtime_error("Test SHA failed");
    return out;
  }

  enrollment_bytes32_t
  mac(const enrollment_bytes32_t &key, const bytes_t &input) {
    enrollment_bytes32_t out {};
    unsigned int size = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), input.data(), input.size(), out.data(), &size) || size != out.size()) throw std::runtime_error("Test HMAC failed");
    return out;
  }

  // Test-only client implementation. Uses a public RFC test private key, and
  // performs real X25519/HKDF/HMAC against each fresh host challenge. The
  // separate proof suite cross-checks the shared fixed Node/Python vector.
  enrollment_bytes32_t
  make_proof(const enrollment_invitation_context_t &c, const enrollment_bytes32_t &token,
             const session_t::challenge_t &challenge) {
    using key_t = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using ctx_t = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    key_t client(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, client_private.data(), client_private.size()), EVP_PKEY_free);
    key_t host(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, challenge.host_ephemeral_public.data(), challenge.host_ephemeral_public.size()), EVP_PKEY_free);
    if (!client || !host) throw std::runtime_error("Test key failed");
    ctx_t derive(EVP_PKEY_CTX_new(client.get(), nullptr), EVP_PKEY_CTX_free);
    enrollment_bytes32_t shared {};
    std::size_t shared_size = shared.size();
    if (!derive || EVP_PKEY_derive_init(derive.get()) != 1 || EVP_PKEY_derive_set_peer(derive.get(), host.get()) != 1 ||
        EVP_PKEY_derive(derive.get(), shared.data(), &shared_size) != 1 || shared_size != shared.size()) throw std::runtime_error("Test derive failed");

    bytes_t t;
    label(t, "VPLUS-ENROLL-POP-V1"); t.push_back(1);
    const bytes_t token_bytes(token.begin(), token.end());
    append(t, sha(token_bytes)); append(t, c.host_certificate_sha256); append(t, c.bootstrap_id);
    append(t, challenge.challenge); append(t, challenge.host_ephemeral_public); append(t, client_public);
    append(t, client_cert()); append(t, c.host_ipv4); append(t, c.client_ipv4);
    t.push_back(static_cast<std::uint8_t>(c.enrollment_port >> 8)); t.push_back(static_cast<std::uint8_t>(c.enrollment_port));
    t.push_back(static_cast<std::uint8_t>(c.discovery_peer.size() >> 8)); t.push_back(static_cast<std::uint8_t>(c.discovery_peer.size()));
    t.insert(t.end(), c.discovery_peer.begin(), c.discovery_peer.end());
    bytes_t salt_input; label(salt_input, "VPLUS-ENROLL-HKDF-SALT-V1"); salt_input.insert(salt_input.end(), t.begin(), t.end());
    const auto salt = sha(salt_input);
    const auto prk = mac(salt, bytes_t(shared.begin(), shared.end()));
    bytes_t key_info; label(key_info, "VPLUS-ENROLL-POP-KEY-V1"); key_info.push_back(1);
    const auto key = mac(prk, key_info);
    bytes_t proof_input; label(proof_input, "VPLUS-ENROLL-POP-PROOF-V1"); proof_input.insert(proof_input.end(), t.begin(), t.end());
    return mac(key, proof_input);
  }

  TEST(RemoteEnrollmentSession, SingleConnectionProofAndConfirmationIsNotAuthorization) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    const auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    const auto proof = make_proof(c, invitation->token, *ch);
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 2, client_public, client_cert(), proof));
    const auto code = session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof);
    ASSERT_TRUE(code);
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::awaiting_confirmation);
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof));
    EXPECT_TRUE(session.confirm_candidate(invitation->generation, *code));
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::candidate_confirmed_not_authorized);
    EXPECT_FALSE(session.confirm_candidate(invitation->generation, *code));
    EXPECT_TRUE(session.cancel(invitation->generation));
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::inactive);
  }

  TEST(RemoteEnrollmentSession, WrongProofBurnsChallengeAndReplayIsRejected) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    auto ch = session.challenge(invitation->generation, invitation->token, 7);
    ASSERT_TRUE(ch);
    const auto old_proof = make_proof(c, invitation->token, *ch);
    auto wrong = old_proof; wrong[0] ^= 1;
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 7, client_public, client_cert(), wrong));
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 7, client_public, client_cert(), old_proof));
    ch = session.challenge(invitation->generation, invitation->token, 7);
    ASSERT_TRUE(ch);
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 7, client_public, client_cert(), old_proof));
  }

  TEST(RemoteEnrollmentSession, ThreeFailedAttemptsInvalidateInvitation) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    for (int attempt = 0; attempt < 3; ++attempt) {
      const auto ch = session.challenge(invitation->generation, invitation->token, 1);
      ASSERT_TRUE(ch);
      auto wrong = make_proof(c, invitation->token, *ch);
      wrong[0] ^= 1;
      EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), wrong));
    }
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::inactive);
    EXPECT_FALSE(session.challenge(invitation->generation, invitation->token, 1));
  }

  TEST(RemoteEnrollmentSession, ModifiedCertificateOrTokenBurnsBoundChallenge) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    auto proof = make_proof(c, invitation->token, *ch);
    auto wrong_cert = client_cert(); wrong_cert[0] ^= 1;
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, wrong_cert, proof));
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof));
    ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    proof = make_proof(c, invitation->token, *ch);
    auto wrong_token = invitation->token; wrong_token[0] ^= 1;
    EXPECT_FALSE(session.prove(invitation->generation, wrong_token, 1, client_public, client_cert(), proof));
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof));
  }

  TEST(RemoteEnrollmentSession, InvalidReplacementAndConnectionFailClosed) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto old = session.issue(c, 120s);
    ASSERT_TRUE(old);
    EXPECT_FALSE(session.challenge(old->generation, old->token, 0));
    auto invalid = c; invalid.host_ipv4 = invalid.client_ipv4;
    EXPECT_FALSE(session.issue(invalid, 120s));
    EXPECT_FALSE(session.challenge(old->generation, old->token, 1));
  }

  TEST(RemoteEnrollmentSession, ExpiresChallengeAndInvitationAtExactDeadlines) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    const auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    const auto proof = make_proof(c, invitation->token, *ch);
    clock.seconds = 30;
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof));
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::awaiting_connection);
    EXPECT_TRUE(session.challenge(invitation->generation, invitation->token, 2));
    clock.seconds = 120;
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::inactive);
    EXPECT_FALSE(session.confirm_candidate(invitation->generation, {}));
  }

  TEST(RemoteEnrollmentSession, RechecksChallengeDeadlineAfterProofVerification) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    const auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    const auto proof = make_proof(c, invitation->token, *ch);
    // prove() checks invitation, challenge and token, then checks time again
    // after cryptographic verification. Advance exactly at the latter check.
    clock.advance_at = clock.calls.load() + 4;
    EXPECT_FALSE(session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof));
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::awaiting_connection);
  }

  TEST(RemoteEnrollmentSession, StaleGenerationAndClosedConnectionCannotActOnReplacement) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto old = session.issue(c, 120s);
    ASSERT_TRUE(old);
    ASSERT_TRUE(session.challenge(old->generation, old->token, 1));
    const auto current = session.issue(c, 120s);
    ASSERT_TRUE(current);
    EXPECT_FALSE(session.cancel(old->generation));
    session.connection_closed(1);
    EXPECT_EQ(session.state(current->generation), session_t::state_t::awaiting_connection);
    EXPECT_FALSE(session.challenge(current->generation, old->token, 2));
    EXPECT_TRUE(session.challenge(current->generation, current->token, 2));
  }

  TEST(RemoteEnrollmentSession, WrongConfirmationAndConnectionCloseInvalidatePendingCandidate) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    auto proof = make_proof(c, invitation->token, *ch);
    auto code = session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof);
    ASSERT_TRUE(code);
    auto wrong = *code; wrong[0] ^= 1;
    EXPECT_FALSE(session.confirm_candidate(invitation->generation, wrong));
    EXPECT_FALSE(session.confirm_candidate(invitation->generation, *code));
    invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    ch = session.challenge(invitation->generation, invitation->token, 2);
    ASSERT_TRUE(ch);
    proof = make_proof(c, invitation->token, *ch);
    code = session.prove(invitation->generation, invitation->token, 2, client_public, client_cert(), proof);
    ASSERT_TRUE(code);
    session.connection_closed(2);
    EXPECT_FALSE(session.confirm_candidate(invitation->generation, *code));
  }

  TEST(RemoteEnrollmentSession, ConcurrentProofsHaveAtMostOnePendingCandidate) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    const auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    const auto proof = make_proof(c, invitation->token, *ch);
    std::atomic<bool> start {false};
    std::atomic<int> successes {0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) threads.emplace_back([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      if (session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof)) ++successes;
    });
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    EXPECT_EQ(successes.load(), 1);
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::awaiting_confirmation);
  }

  TEST(RemoteEnrollmentSession, CancelAndProofRaceCannotResurrectInvitation) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    const auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    const auto proof = make_proof(c, invitation->token, *ch);
    std::atomic<bool> start {false};
    std::thread prover([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof);
    });
    std::thread canceller([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) session.cancel(invitation->generation);
    });
    start.store(true, std::memory_order_release);
    prover.join(); canceller.join();
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::inactive);
    EXPECT_FALSE(session.challenge(invitation->generation, invitation->token, 1));
  }

  TEST(RemoteEnrollmentSession, CancelAndConfirmationRaceNeverAuthorizesDevice) {
    fake_clock_t clock;
    session_t session([&] { return clock.now(); });
    const auto c = context();
    const auto invitation = session.issue(c, 120s);
    ASSERT_TRUE(invitation);
    const auto ch = session.challenge(invitation->generation, invitation->token, 1);
    ASSERT_TRUE(ch);
    const auto proof = make_proof(c, invitation->token, *ch);
    const auto code = session.prove(invitation->generation, invitation->token, 1, client_public, client_cert(), proof);
    ASSERT_TRUE(code);
    std::atomic<bool> start {false};
    std::thread confirmer([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) session.confirm_candidate(invitation->generation, *code);
    });
    std::thread canceller([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      (void) session.cancel(invitation->generation);
    });
    start.store(true, std::memory_order_release);
    confirmer.join(); canceller.join();
    EXPECT_EQ(session.state(invitation->generation), session_t::state_t::inactive);
  }
}  // namespace
