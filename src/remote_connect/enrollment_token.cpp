#include "enrollment_token.h"

#include <algorithm>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace remote_connect {
  namespace {
    constexpr auto max_token_lifetime = std::chrono::seconds(120);

    bool
    digest_token(const enrollment_token_store_t::bytes_t &token, enrollment_token_store_t::bytes_t &digest) {
      unsigned int size = 0;
      return EVP_Digest(token.data(), token.size(), digest.data(), &size, EVP_sha256(), nullptr) == 1 &&
             size == digest.size();
    }
  }  // namespace

  enrollment_token_store_t::enrollment_token_store_t():
      enrollment_token_store_t([] { return clock_t::now(); }) {
  }

  enrollment_token_store_t::enrollment_token_store_t(clock_source_t clock_source):
      clock_source_(std::move(clock_source)) {
  }

  enrollment_token_store_t::~enrollment_token_store_t() {
    OPENSSL_cleanse(token_digest_.data(), token_digest_.size());
  }

  void
  enrollment_token_store_t::clear_locked() {
    OPENSSL_cleanse(token_digest_.data(), token_digest_.size());
    expires_at_ = {};
    active_ = false;
  }

  std::optional<enrollment_token_store_t::bytes_t>
  enrollment_token_store_t::issue(std::chrono::seconds ttl) {
    std::lock_guard lock(mutex_);
    clear_locked();
    if (ttl <= std::chrono::seconds::zero() || ttl > max_token_lifetime || !clock_source_) return std::nullopt;

    const auto now = clock_source_();
    if (now > clock_t::time_point::max() - ttl) return std::nullopt;

    bytes_t token {};
    if (RAND_priv_bytes(token.data(), static_cast<int>(token.size())) != 1 || !digest_token(token, token_digest_)) {
      OPENSSL_cleanse(token.data(), token.size());
      clear_locked();
      return std::nullopt;
    }
    expires_at_ = now + ttl;
    active_ = true;
    std::optional<bytes_t> result(token);
    OPENSSL_cleanse(token.data(), token.size());
    return result;
  }

  std::optional<enrollment_token_store_t::claim_t>
  enrollment_token_store_t::claim(const bytes_t &token, const bytes_t &device_public_key) {
    std::lock_guard lock(mutex_);
    if (!active_) return std::nullopt;
    if (clock_source_() >= expires_at_) {
      clear_locked();
      return std::nullopt;
    }
    if (std::all_of(device_public_key.begin(), device_public_key.end(), [](auto byte) { return byte == 0; })) {
      return std::nullopt;
    }

    bytes_t digest {};
    if (!digest_token(token, digest)) {
      clear_locked();
      return std::nullopt;
    }
    const bool matches = CRYPTO_memcmp(token_digest_.data(), digest.data(), digest.size()) == 0;
    OPENSSL_cleanse(digest.data(), digest.size());
    if (!matches) return std::nullopt;
    clear_locked();
    return claim_t {device_public_key};
  }

  void
  enrollment_token_store_t::cancel() {
    std::lock_guard lock(mutex_);
    clear_locked();
  }

}  // namespace remote_connect
