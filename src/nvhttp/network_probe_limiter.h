#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nvhttp::network_probe {
  constexpr std::size_t MIN_BYTES = 64 * 1024;
  constexpr std::size_t MAX_BYTES = 4 * 1024 * 1024;
  constexpr std::size_t CHUNK_BYTES = 64 * 1024;
  constexpr std::size_t SESSION_MAX_BYTES = 6 * 1024 * 1024;
  constexpr std::uint32_t COOLDOWN_MS = 5000;
  constexpr auto MAX_TRANSFER_DURATION = std::chrono::seconds(3);

  using clock_t = std::chrono::steady_clock;

  bool valid_nonce(std::string_view nonce);
  bool parse_payload_bytes(std::string_view value, std::size_t &result);

  enum class rejection_e {
    none,
    client_busy,
    global_busy,
    cooldown,
    session_expired,
    session_too_large,
    client_quota,
    global_quota,
  };

  struct admission_t {
    rejection_e rejection = rejection_e::none;
    std::uint32_t retry_after_ms = 0;
    std::uint64_t id = 0;

    explicit operator bool() const noexcept {
      return rejection == rejection_e::none;
    }
  };

  class limiter_t {
  public:
    admission_t admit(const std::string &client, const std::string &nonce, std::size_t bytes, clock_t::time_point now = clock_t::now());
    void complete(const std::string &client, std::uint64_t admission_id, clock_t::time_point now = clock_t::now());

  private:
    struct charge_t {
      clock_t::time_point at;
      std::size_t bytes;
    };

    struct client_t {
      struct retired_nonce_t {
        std::string value;
        clock_t::time_point retired_at;
      };

      std::string nonce;
      clock_t::time_point session_started {};
      clock_t::time_point last_activity {};
      clock_t::time_point cooldown_until {};
      std::size_t session_bytes = 0;
      std::uint64_t admission_id = 0;
      bool in_flight = false;
      std::deque<charge_t> charges;
      std::size_t charged_bytes = 0;
      std::deque<retired_nonce_t> retired_nonces;
    };

    static void prune(std::deque<charge_t> &charges, std::size_t &total, clock_t::time_point now);
    static std::uint32_t quota_retry_ms(const std::deque<charge_t> &charges, clock_t::time_point now);

    std::mutex mutex_;
    std::unordered_map<std::string, client_t> clients_;
    std::deque<charge_t> global_charges_;
    std::size_t global_charged_bytes_ = 0;
    std::size_t global_in_flight_ = 0;
    std::uint64_t next_admission_id_ = 1;
  };
}  // namespace nvhttp::network_probe
