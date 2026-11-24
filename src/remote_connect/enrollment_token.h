#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace remote_connect {

  // An in-memory, single-use token primitive. This is not an enrollment
  // protocol: callers must first authenticate the host and verify proof of
  // possession for the supplied device public key. No network API uses this
  // class yet.
  class enrollment_token_store_t {
  public:
    using bytes_t = std::array<std::uint8_t, 32>;
    using clock_t = std::chrono::steady_clock;
    using clock_source_t = std::function<clock_t::time_point()>;

    struct claim_t {
      bytes_t device_public_key;
    };

    enrollment_token_store_t();
    explicit enrollment_token_store_t(clock_source_t clock_source);
    ~enrollment_token_store_t();

    enrollment_token_store_t(const enrollment_token_store_t &) = delete;
    enrollment_token_store_t &operator=(const enrollment_token_store_t &) = delete;

    // Replaces any outstanding token. Invalid TTL or random/hash failure
    // invalidates the previous token and returns no token. Tokens cannot live
    // longer than 120 seconds. A process restart also invalidates them.
    std::optional<bytes_t>
    issue(std::chrono::seconds ttl);

    // Non-consuming, constant-time comparison for a future challenge. This
    // is not authorization and must be coordinated with claim/cancel by the
    // owning state machine under its own lock.
    bool
    matches(const bytes_t &token);

    // Atomic single-use claim, bound to the provided public-key bytes.
    // The caller is responsible for authenticating these bytes before calling.
    // A failed token/key check does not consume an unexpired valid token.
    // A returned claim is not revoked by subsequent cancel() or issue(); a
    // future enrollment transaction must coordinate cancellation separately.
    std::optional<claim_t>
    claim(const bytes_t &token, const bytes_t &device_public_key);

    // Prevents any future claim of the outstanding token.
    void
    cancel();

  private:
    void
    clear_locked();

    std::mutex mutex_;
    clock_source_t clock_source_;
    bytes_t token_digest_ {};
    clock_t::time_point expires_at_ {};
    bool active_ = false;
  };

}  // namespace remote_connect
