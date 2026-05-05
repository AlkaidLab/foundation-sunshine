/**
 * @file src/security/verify_log_throttle.cpp
 */
#include "security/verify_log_throttle.h"

#include "network.h"

namespace security {

  std::uint64_t
  verify_log_throttle_t::record(const boost::asio::ip::address &address) {
    const auto key = net::addr_to_normalized_string(address);
    return with_entry(key, [](std::uint64_t &n) -> std::uint64_t {
      ++n;
      if (n == 1 || n % kRepeatEvery == 0) {
        return n;
      }
      return 0;
    });
  }

  verify_log_throttle_t &
  verify_log_throttle() {
    static verify_log_throttle_t instance;
    return instance;
  }

}  // namespace security
