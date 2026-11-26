#include "enrollment_session.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "enrollment_proof_internal.h"

namespace remote_connect {
  namespace {
    constexpr auto max_lifetime = std::chrono::seconds(120);
    constexpr auto max_challenge_lifetime = std::chrono::seconds(30);
    constexpr unsigned int max_attempts = 3;

    struct sensitive_private_t {
      enrollment_bytes32_t value {};
      ~sensitive_private_t() { OPENSSL_cleanse(value.data(), value.size()); }
    };

    template<std::size_t N>
    bool
    is_zero(const std::array<std::uint8_t, N> &value) {
      return std::all_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte == 0; });
    }

    bool
    valid_context(const enrollment_invitation_context_t &context) {
      const std::string_view peer(context.discovery_peer);
      return !is_zero(context.host_certificate_sha256) && !is_zero(context.bootstrap_id) &&
             !is_zero(context.host_ipv4) && !is_zero(context.client_ipv4) && context.host_ipv4 != context.client_ipv4 &&
             context.enrollment_port != 0 && peer.size() > 6 && peer.size() <= 512 && peer.starts_with("udp://") &&
             std::all_of(peer.begin(), peer.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
    }
  }  // namespace

  enrollment_session_t::enrollment_session_t():
      enrollment_session_t([] { return clock_t::now(); }) {
  }

  enrollment_session_t::enrollment_session_t(clock_source_t clock_source):
      clock_source_(std::move(clock_source)), token_store_(clock_source_) {
  }

  enrollment_session_t::~enrollment_session_t() {
    OPENSSL_cleanse(host_private_.data(), host_private_.size());
  }

  void
  enrollment_session_t::clear_challenge_locked() {
    OPENSSL_cleanse(host_private_.data(), host_private_.size());
    host_public_ = {};
    challenge_ = {};
    challenge_expires_at_ = {};
  }

  void
  enrollment_session_t::clear_locked() {
    token_store_.cancel();
    clear_challenge_locked();
    context_ = {};
    confirmation_ = {};
    candidate_public_ = {};
    candidate_certificate_sha256_ = {};
    generation_ = 0;
    expires_at_ = {};
    connection_ = 0;
    attempts_ = 0;
    state_ = state_t::inactive;
  }

  bool
  enrollment_session_t::current_locked(std::uint64_t generation) {
    if (state_ == state_t::inactive || !clock_source_) return false;
    if (clock_source_() >= expires_at_) {
      clear_locked();
      return false;
    }
    return generation != 0 && generation == generation_;
  }

  void
  enrollment_session_t::reject_attempt_locked() {
    clear_challenge_locked();
    connection_ = 0;
    if (attempts_ >= max_attempts) clear_locked();
    else state_ = state_t::awaiting_connection;
  }

  std::optional<enrollment_session_t::invitation_t>
  enrollment_session_t::issue(const enrollment_invitation_context_t &context, std::chrono::seconds ttl) {
    std::lock_guard lock(mutex_);
    clear_locked();
    if (!clock_source_ || ttl <= std::chrono::seconds::zero() || ttl > max_lifetime || !valid_context(context) ||
        last_generation_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    const auto now = clock_source_();
    if (now > clock_t::time_point::max() - ttl) return std::nullopt;
    auto token = token_store_.issue(ttl);
    if (!token) return std::nullopt;
    generation_ = ++last_generation_;
    context_ = context;
    expires_at_ = now + ttl;
    state_ = state_t::awaiting_connection;
    std::optional<invitation_t> invitation(invitation_t {generation_, *token});
    OPENSSL_cleanse(token->data(), token->size());
    return invitation;
  }

  std::optional<enrollment_session_t::challenge_t>
  enrollment_session_t::challenge(std::uint64_t generation, const enrollment_bytes32_t &token, connection_id_t connection) {
    std::lock_guard lock(mutex_);
    if (!current_locked(generation) || connection == 0) return std::nullopt;
    if (state_ == state_t::awaiting_proof && clock_source_() >= challenge_expires_at_) reject_attempt_locked();
    if (state_ != state_t::awaiting_connection || !token_store_.matches(token)) return std::nullopt;
    if (RAND_priv_bytes(host_private_.data(), static_cast<int>(host_private_.size())) != 1 ||
        detail::matches_clamped_zero_x25519_private(host_private_) ||
        RAND_bytes(challenge_.data(), static_cast<int>(challenge_.size())) != 1 || is_zero(challenge_)) {
      clear_locked();
      return std::nullopt;
    }
    using pkey_t = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    pkey_t host(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, host_private_.data(), host_private_.size()), EVP_PKEY_free);
    std::size_t size = host_public_.size();
    if (!host || EVP_PKEY_get_raw_public_key(host.get(), host_public_.data(), &size) != 1 || size != host_public_.size()) {
      clear_locked();
      return std::nullopt;
    }
    const auto now = clock_source_();
    if (now >= expires_at_) {
      clear_locked();
      return std::nullopt;
    }
    const auto remaining = expires_at_ - now;
    challenge_expires_at_ = now + std::min(remaining, std::chrono::duration_cast<clock_t::duration>(max_challenge_lifetime));
    connection_ = connection;
    ++attempts_;
    state_ = state_t::awaiting_proof;
    return challenge_t {challenge_, host_public_};
  }

  std::optional<enrollment_confirmation_t>
  enrollment_session_t::prove(std::uint64_t generation, const enrollment_bytes32_t &token, connection_id_t connection,
                              const enrollment_bytes32_t &client_public, const enrollment_bytes32_t &client_certificate_sha256,
                              const enrollment_bytes32_t &proof) {
    std::lock_guard lock(mutex_);
    if (!current_locked(generation) || state_ != state_t::awaiting_proof || connection == 0 || connection != connection_) return std::nullopt;
    if (clock_source_() >= challenge_expires_at_ || !token_store_.matches(token) || is_zero(client_certificate_sha256)) {
      reject_attempt_locked();
      return std::nullopt;
    }
    try {
      enrollment_proof_context_t proof_context {
        1, context_.host_certificate_sha256, context_.bootstrap_id, challenge_, host_public_,
        client_public, client_certificate_sha256, context_.host_ipv4, context_.client_ipv4,
        context_.enrollment_port, context_.discovery_peer
      };
      const auto challenge_deadline = challenge_expires_at_;
      sensitive_private_t private_copy {host_private_};
      // Burn the session's challenge before verification. The only remaining
      // private copy is scoped and cleansed even if verification throws.
      clear_challenge_locked();
      const auto confirmation = verify_enrollment_proof_v1(proof_context, token, private_copy.value, proof);
      const auto after_verify = clock_source_();
      if (after_verify >= expires_at_) {
        clear_locked();
        return std::nullopt;
      }
      if (!confirmation || after_verify >= challenge_deadline || !token_store_.claim(token, client_public)) {
        reject_attempt_locked();
        return std::nullopt;
      }
      confirmation_ = *confirmation;
      candidate_public_ = client_public;
      candidate_certificate_sha256_ = client_certificate_sha256;
      state_ = state_t::awaiting_confirmation;
      return confirmation;
    }
    catch (...) {
      // Invalid requests, allocator failures and crypto exceptions must never
      // leave a challenge reusable or a candidate authorized.
      reject_attempt_locked();
      return std::nullopt;
    }
  }

  bool
  enrollment_session_t::confirm_candidate(std::uint64_t generation, const enrollment_confirmation_t &compared_code) {
    std::lock_guard lock(mutex_);
    if (!current_locked(generation) || state_ != state_t::awaiting_confirmation) return false;
    if (CRYPTO_memcmp(confirmation_.data(), compared_code.data(), confirmation_.size()) != 0) {
      clear_locked();
      return false;
    }
    state_ = state_t::candidate_confirmed_not_authorized;
    return true;
  }

  std::optional<enrollment_session_t::confirmed_candidate_t>
  enrollment_session_t::take_confirmed_candidate(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (!current_locked(generation) || state_ != state_t::candidate_confirmed_not_authorized) return std::nullopt;
    confirmed_candidate_t candidate {candidate_public_, candidate_certificate_sha256_};
    // Ends the invitation as well: the candidate is now the caller's to commit
    // or discard, and must not be taken a second time or cancelled.
    clear_locked();
    return candidate;
  }

  bool
  enrollment_session_t::cancel(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (!current_locked(generation)) return false;
    clear_locked();
    return true;
  }

  void
  enrollment_session_t::connection_closed(connection_id_t connection) {
    std::lock_guard lock(mutex_);
    if (connection == 0 || connection != connection_ || state_ == state_t::inactive) return;
    if (state_ == state_t::awaiting_proof) reject_attempt_locked();
    else clear_locked();
  }

  enrollment_session_t::state_t
  enrollment_session_t::state(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (!current_locked(generation)) return state_t::inactive;
    if (state_ == state_t::awaiting_proof && clock_source_() >= challenge_expires_at_) reject_attempt_locked();
    return state_;
  }
}  // namespace remote_connect
