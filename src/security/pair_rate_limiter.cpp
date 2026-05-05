/**
 * @file src/security/pair_rate_limiter.cpp
 */
#include "security/pair_rate_limiter.h"

#include "config.h"
#include "network.h"

namespace security {

  bool
  pair_rate_limiter_t::check_and_record(const boost::asio::ip::address &address) {
    const int max_attempts = config::nvhttp.pair_max_attempts;
    // 0 disables rate limiting.
    if (max_attempts <= 0) {
      return true;
    }

    const auto key = net::addr_to_normalized_string(address);
    const auto now = std::chrono::steady_clock::now();
    return with_entry(key, [&](pair_window_t &entry) {
      // Reset window if expired.
      if (now - entry.window_start > std::chrono::seconds(kWindowSeconds)) {
        entry = { 0, now };
      }
      if (entry.count >= max_attempts) {
        return false;
      }
      ++entry.count;
      return true;
    });
  }

  void
  pair_rate_limiter_t::cleanup() {
    const auto now = std::chrono::steady_clock::now();
    erase_if_locked([&](const pair_window_t &entry) {
      return now - entry.window_start > std::chrono::seconds(kWindowSeconds * 2);
    });
  }

  pair_rate_limiter_t &
  pair_rate_limit() {
    static pair_rate_limiter_t instance;
    return instance;
  }

}  // namespace security
