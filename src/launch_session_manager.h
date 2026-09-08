/**
 * @file src/launch_session_manager.h
 * @brief Bounded lifecycle manager for pending GameStream launch tickets.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "crypto.h"

namespace rtsp_stream {
  struct launch_session_t;

  constexpr std::size_t DEFAULT_PENDING_LAUNCH_LIMIT = 16;
  constexpr std::size_t DEFAULT_PENDING_LAUNCH_LIMIT_PER_CLIENT = 2;

  enum class launch_ticket_state_e {
    pending,
    claimed,
  };

  enum class launch_ticket_register_e {
    accepted,
    replaced,
    global_limit,
    client_limit,
    client_busy,
  };

  struct encrypted_launch_claim_t {
    std::shared_ptr<launch_session_t> session;
    std::vector<std::uint8_t> plaintext;

    explicit operator bool() const noexcept {
      return session != nullptr;
    }
  };

  class launch_session_manager_t {
  public:
    using clock_t = std::chrono::steady_clock;

    explicit launch_session_manager_t(
      std::size_t global_limit = DEFAULT_PENDING_LAUNCH_LIMIT,
      std::size_t per_client_limit = DEFAULT_PENDING_LAUNCH_LIMIT_PER_CLIENT);
    ~launch_session_manager_t();

    launch_session_manager_t(const launch_session_manager_t &) = delete;
    launch_session_manager_t &
    operator=(const launch_session_manager_t &) = delete;

    launch_ticket_register_e
    register_session(std::shared_ptr<launch_session_t> session,
                     std::chrono::milliseconds timeout,
                     clock_t::time_point now = clock_t::now());

    std::shared_ptr<launch_session_t>
    claim_plaintext(std::string_view remote_address,
                    clock_t::time_point now = clock_t::now());

    encrypted_launch_claim_t
    claim_encrypted(std::string_view remote_address,
                    std::string_view tagged_cipher,
                    crypto::aes_t iv,
                    clock_t::time_point now = clock_t::now());

    bool
    activate(std::uint32_t launch_session_id);

    bool
    release(std::uint32_t launch_session_id,
            std::chrono::milliseconds timeout,
            clock_t::time_point now = clock_t::now());

    bool
    erase(std::uint32_t launch_session_id);

    std::size_t
    prune(clock_t::time_point now = clock_t::now());

    void
    clear();

    std::size_t
    size(clock_t::time_point now = clock_t::now());

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace rtsp_stream
