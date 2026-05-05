/**
 * @file src/security/verify_log_throttle.h
 * @brief Throttles repeated SSL verify-failure logs from the same peer IP.
 */
#pragma once

#include <cstdint>

#include <boost/asio/ip/address.hpp>

#include "security/ip_counter.h"

namespace security {

  /**
   * @brief Counts verify-failure events per peer IP and tells the caller
   * whether the current event should be logged. Logs the first occurrence
   * per IP and then every Nth occurrence afterwards. This is purely an
   * observability tool — security policy belongs elsewhere.
   */
  class verify_log_throttle_t: public ip_counter_t<std::uint64_t> {
  public:
    static constexpr std::uint64_t kRepeatEvery = 50;

    /**
     * @brief Record an event for the given peer.
     * The address is normalized (IPv4-mapped IPv6 collapsed to IPv4).
     * @return The post-increment count when the caller should emit a log
     *         line, or 0 to indicate the event was suppressed.
     */
    std::uint64_t
    record(const boost::asio::ip::address &address);
  };

  /// Process-wide singleton used by the nvhttp verify-failure path.
  verify_log_throttle_t &
  verify_log_throttle();

}  // namespace security
