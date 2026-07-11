/**
 * @file src/launch_session_manager.cpp
 * @brief Bounded lifecycle manager for pending GameStream launch tickets.
 */

#include "launch_session_manager.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "rtsp.h"

namespace rtsp_stream {
  namespace {
    struct launch_ticket_t {
      std::shared_ptr<launch_session_t> session;
      launch_ticket_state_e state { launch_ticket_state_e::pending };
      launch_session_manager_t::clock_t::time_point expires_at;
    };

    bool
    same_authenticated_client(const launch_ticket_t &ticket, const launch_session_t &session) {
      return ticket.session &&
             !session.client_cert_uuid.empty() &&
             ticket.session->client_cert_uuid == session.client_cert_uuid;
    }

    bool
    same_legacy_client(const launch_ticket_t &ticket, const launch_session_t &session) {
      return ticket.session &&
             ticket.session->client_cert_uuid.empty() &&
             session.client_cert_uuid.empty() &&
             !session.unique_id.empty() &&
             session.unique_id != "unknown" &&
             ticket.session->unique_id == session.unique_id &&
             ticket.session->rtsp_peer_address == session.rtsp_peer_address;
    }
  }  // namespace

  struct launch_session_manager_t::impl_t {
    explicit impl_t(std::size_t global_limit, std::size_t per_client_limit):
        global_limit { std::max<std::size_t>(1, global_limit) },
        per_client_limit { std::max<std::size_t>(1, per_client_limit) } {
    }

    void
    prune_locked(clock_t::time_point now) {
      tickets.erase(std::remove_if(tickets.begin(), tickets.end(), [now](const auto &ticket) {
                      return !ticket.session ||
                             (ticket.state == launch_ticket_state_e::pending && now >= ticket.expires_at);
                    }),
                    tickets.end());
    }

    std::mutex mutex;
    std::vector<launch_ticket_t> tickets;
    std::size_t global_limit;
    std::size_t per_client_limit;
  };

  launch_session_manager_t::launch_session_manager_t(std::size_t global_limit, std::size_t per_client_limit):
      _impl { std::make_unique<impl_t>(global_limit, per_client_limit) } {
  }

  launch_session_manager_t::~launch_session_manager_t() = default;

  launch_ticket_register_e
  launch_session_manager_t::register_session(std::shared_ptr<launch_session_t> session,
                                             std::chrono::milliseconds timeout,
                                             clock_t::time_point now) {
    if (!session) {
      return launch_ticket_register_e::global_limit;
    }

    std::lock_guard lock { _impl->mutex };
    _impl->prune_locked(now);

    if (!session->client_cert_uuid.empty() || (!session->unique_id.empty() && session->unique_id != "unknown")) {
      auto same_client = std::find_if(_impl->tickets.begin(), _impl->tickets.end(), [&session](const auto &ticket) {
        return same_authenticated_client(ticket, *session) || same_legacy_client(ticket, *session);
      });
      if (same_client != _impl->tickets.end()) {
        if (same_client->state == launch_ticket_state_e::claimed) {
          return launch_ticket_register_e::client_busy;
        }

        same_client->session = std::move(session);
        same_client->state = launch_ticket_state_e::pending;
        same_client->expires_at = now + timeout;
        return launch_ticket_register_e::replaced;
      }

    }
    if (session->client_cert_uuid.empty() && !session->rtsp_peer_address.empty()) {
      const auto peer_count = std::count_if(_impl->tickets.begin(), _impl->tickets.end(), [&session](const auto &ticket) {
        return ticket.session && ticket.session->client_cert_uuid.empty() &&
               ticket.session->rtsp_peer_address == session->rtsp_peer_address;
      });
      if (static_cast<std::size_t>(peer_count) >= _impl->per_client_limit) {
        return launch_ticket_register_e::client_limit;
      }
    }

    if (_impl->tickets.size() >= _impl->global_limit) {
      return launch_ticket_register_e::global_limit;
    }

    _impl->tickets.push_back({
      .session = std::move(session),
      .state = launch_ticket_state_e::pending,
      .expires_at = now + timeout,
    });
    return launch_ticket_register_e::accepted;
  }

  std::shared_ptr<launch_session_t>
  launch_session_manager_t::claim_plaintext(std::string_view remote_address, clock_t::time_point now) {
    std::lock_guard lock { _impl->mutex };
    _impl->prune_locked(now);

    launch_ticket_t *match = nullptr;
    if (!remote_address.empty()) {
      for (auto &ticket : _impl->tickets) {
        if (ticket.state != launch_ticket_state_e::pending ||
            !ticket.session ||
            ticket.session->rtsp_cipher ||
            ticket.session->rtsp_peer_address != remote_address) {
          continue;
        }
        if (match) {
          return nullptr;
        }
        match = &ticket;
      }
    }

    if (!match) {
      auto only_pending = static_cast<launch_ticket_t *>(nullptr);
      for (auto &ticket : _impl->tickets) {
        if (ticket.state != launch_ticket_state_e::pending || !ticket.session || ticket.session->rtsp_cipher) {
          continue;
        }
        if (only_pending) {
          return nullptr;
        }
        only_pending = &ticket;
      }
      match = only_pending;
    }

    if (!match) {
      return nullptr;
    }

    match->state = launch_ticket_state_e::claimed;
    return match->session;
  }

  encrypted_launch_claim_t
  launch_session_manager_t::claim_encrypted(std::string_view remote_address,
                                            std::string_view tagged_cipher,
                                            crypto::aes_t iv,
                                            clock_t::time_point now) {
    std::lock_guard lock { _impl->mutex };
    _impl->prune_locked(now);

    launch_ticket_t *match = nullptr;
    std::vector<std::uint8_t> matched_plaintext;

    // Route-matching candidates are attempted first for efficiency, but the
    // GCM authentication tag remains the sole identity decision.
    for (int route_pass = 0; route_pass < 2; ++route_pass) {
      for (auto &ticket : _impl->tickets) {
        if (ticket.state != launch_ticket_state_e::pending ||
            !ticket.session ||
            !ticket.session->rtsp_cipher) {
          continue;
        }

        const bool route_matches = !remote_address.empty() && ticket.session->rtsp_peer_address == remote_address;
        if ((route_pass == 0) != route_matches) {
          continue;
        }

        std::vector<std::uint8_t> plaintext;
        auto candidate_iv = iv;
        if (ticket.session->rtsp_cipher->decrypt(tagged_cipher, plaintext, &candidate_iv) != 0) {
          continue;
        }

        if (match) {
          // Reused keys would make the claim ambiguous. Fail closed.
          return {};
        }
        match = &ticket;
        matched_plaintext = std::move(plaintext);
      }
    }

    if (!match) {
      return {};
    }

    match->state = launch_ticket_state_e::claimed;
    return {
      .session = match->session,
      .plaintext = std::move(matched_plaintext),
    };
  }

  bool
  launch_session_manager_t::activate(std::uint32_t launch_session_id) {
    return erase(launch_session_id);
  }

  bool
  launch_session_manager_t::release(std::uint32_t launch_session_id,
                                    std::chrono::milliseconds timeout,
                                    clock_t::time_point now) {
    std::lock_guard lock { _impl->mutex };
    auto ticket = std::find_if(_impl->tickets.begin(), _impl->tickets.end(), [launch_session_id](const auto &entry) {
      return entry.session && entry.session->id == launch_session_id;
    });
    if (ticket == _impl->tickets.end() || ticket->state != launch_ticket_state_e::claimed) {
      return false;
    }

    ticket->state = launch_ticket_state_e::pending;
    ticket->expires_at = now + timeout;
    return true;
  }

  bool
  launch_session_manager_t::erase(std::uint32_t launch_session_id) {
    std::lock_guard lock { _impl->mutex };
    const auto old_size = _impl->tickets.size();
    std::erase_if(_impl->tickets, [launch_session_id](const auto &ticket) {
      return ticket.session && ticket.session->id == launch_session_id;
    });
    return _impl->tickets.size() != old_size;
  }

  std::size_t
  launch_session_manager_t::prune(clock_t::time_point now) {
    std::lock_guard lock { _impl->mutex };
    const auto old_size = _impl->tickets.size();
    _impl->prune_locked(now);
    return old_size - _impl->tickets.size();
  }

  void
  launch_session_manager_t::clear() {
    std::lock_guard lock { _impl->mutex };
    _impl->tickets.clear();
  }

  std::size_t
  launch_session_manager_t::size(clock_t::time_point now) {
    std::lock_guard lock { _impl->mutex };
    _impl->prune_locked(now);
    return _impl->tickets.size();
  }

}  // namespace rtsp_stream
