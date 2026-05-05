/**
 * @file src/security/pair_rate_limiter.h
 * @brief Sliding-window rate limiter for PIN pairing attempts, keyed by IP.
 */
#pragma once

#include <chrono>

#include <boost/asio/ip/address.hpp>

#include "security/ip_counter.h"

namespace security {

  struct pair_window_t {
    int count = 0;
    std::chrono::steady_clock::time_point window_start {};
  };

  /**
   * @brief Limits the number of /pair attempts per client IP within a fixed
   * window. The maximum is read from `config::nvhttp.pair_max_attempts` on
   * each call; 0 disables the limiter so configuration changes take effect
   * without a server restart.
   */
  class pair_rate_limiter_t: public ip_counter_t<pair_window_t> {
  public:
    static constexpr int kWindowSeconds = 60;

    /**
     * @brief Record an attempt and report whether it is within the budget.
     * The address is normalized (IPv4-mapped IPv6 collapsed to IPv4) so the
     * same peer is consistently keyed regardless of the socket family.
     * @return true if allowed, false if rate-limited.
     */
    bool
    check_and_record(const boost::asio::ip::address &address);

    /**
     * @brief Drop entries whose window expired more than 2× the window ago.
     * Safe to call periodically from request handlers.
     */
    void
    cleanup();
  };

  /// Process-wide singleton used by the nvhttp pair handler.
  pair_rate_limiter_t &
  pair_rate_limit();

}  // namespace security
