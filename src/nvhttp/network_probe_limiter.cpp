#include "network_probe_limiter.h"

#include <algorithm>
#include <limits>

namespace nvhttp::network_probe {
  namespace {
    constexpr auto SESSION_LIFETIME = std::chrono::seconds(3);
    constexpr auto COOLDOWN = std::chrono::milliseconds(COOLDOWN_MS);
    constexpr auto QUOTA_WINDOW = std::chrono::minutes(1);
    constexpr std::size_t CLIENT_QUOTA_BYTES = 16 * 1024 * 1024;
    constexpr std::size_t GLOBAL_QUOTA_BYTES = 64 * 1024 * 1024;
    constexpr std::size_t GLOBAL_CONCURRENCY = 4;
  }

  bool valid_nonce(std::string_view nonce) {
    if (nonce.empty() || nonce.size() > 64) {
      return false;
    }
    return std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) {
      const bool ascii_alphanumeric = (c >= 'a' && c <= 'z') ||
                                      (c >= 'A' && c <= 'Z') ||
                                      (c >= '0' && c <= '9');
      return ascii_alphanumeric || c == '-' || c == '_' || c == '.';
    });
  }

  bool parse_payload_bytes(std::string_view value, std::size_t &result) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
      return false;
    }
    std::size_t parsed = 0;
    for (const auto c : value) {
      const auto digit = static_cast<std::size_t>(c - '0');
      if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
        return false;
      }
      parsed = parsed * 10 + digit;
    }
    if (parsed < MIN_BYTES || parsed > MAX_BYTES) {
      return false;
    }
    result = parsed;
    return true;
  }

  void limiter_t::prune(std::deque<charge_t> &charges, std::size_t &total, clock_t::time_point now) {
    while (!charges.empty() && now - charges.front().at >= QUOTA_WINDOW) {
      total -= charges.front().bytes;
      charges.pop_front();
    }
  }

  std::uint32_t limiter_t::quota_retry_ms(const std::deque<charge_t> &charges, clock_t::time_point now) {
    if (charges.empty()) {
      return COOLDOWN_MS;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(QUOTA_WINDOW - (now - charges.front().at));
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, remaining.count()));
  }

  admission_t limiter_t::admit(const std::string &client, const std::string &nonce, std::size_t bytes, clock_t::time_point now) {
    std::lock_guard lock(mutex_);
    auto &state = clients_[client];
    prune(state.charges, state.charged_bytes, now);
    prune(global_charges_, global_charged_bytes_, now);

    if (state.in_flight) return { rejection_e::client_busy, COOLDOWN_MS };
    if (global_in_flight_ >= GLOBAL_CONCURRENCY) return { rejection_e::global_busy, COOLDOWN_MS };

    if (!state.nonce.empty() && now - state.session_started >= SESSION_LIFETIME) {
      state.retired_nonces.emplace(state.nonce);
      state.cooldown_until = state.session_started + SESSION_LIFETIME + COOLDOWN;
      state.nonce.clear();
      state.session_bytes = 0;
    }
    if (state.retired_nonces.contains(nonce)) return { rejection_e::session_expired, COOLDOWN_MS };

    const bool continuing = state.nonce == nonce;
    if (!continuing && state.nonce.empty() && now < state.cooldown_until) {
      const auto retry = std::chrono::duration_cast<std::chrono::milliseconds>(state.cooldown_until - now).count();
      return { rejection_e::cooldown, static_cast<std::uint32_t>(std::max<std::int64_t>(1, retry)) };
    }
    if (!continuing && !state.nonce.empty()) {
      const auto cooldown_end = state.session_started + SESSION_LIFETIME + COOLDOWN;
      if (now < cooldown_end) {
        const auto retry = std::chrono::duration_cast<std::chrono::milliseconds>(cooldown_end - now).count();
        return { rejection_e::cooldown, static_cast<std::uint32_t>(std::max<std::int64_t>(1, retry)) };
      }
    }
    if (continuing && state.session_bytes + bytes > SESSION_MAX_BYTES) return { rejection_e::session_too_large, COOLDOWN_MS };
    if (state.charged_bytes + bytes > CLIENT_QUOTA_BYTES) return { rejection_e::client_quota, quota_retry_ms(state.charges, now) };
    if (global_charged_bytes_ + bytes > GLOBAL_QUOTA_BYTES) return { rejection_e::global_quota, quota_retry_ms(global_charges_, now) };

    if (!continuing) {
      if (!state.nonce.empty()) state.retired_nonces.emplace(std::move(state.nonce));
      state.nonce = nonce;
      state.session_started = now;
      state.cooldown_until = {};
      state.session_bytes = 0;
    }
    state.last_activity = now;
    state.session_bytes += bytes;
    state.in_flight = true;
    state.admission_id = next_admission_id_++;
    ++global_in_flight_;
    state.charges.push_back({ now, bytes });
    state.charged_bytes += bytes;
    global_charges_.push_back({ now, bytes });
    global_charged_bytes_ += bytes;
    return { rejection_e::none, 0, state.admission_id };
  }

  void limiter_t::complete(const std::string &client, std::uint64_t admission_id, clock_t::time_point now) {
    std::lock_guard lock(mutex_);
    const auto it = clients_.find(client);
    if (it == clients_.end() || !it->second.in_flight || it->second.admission_id != admission_id) return;
    it->second.in_flight = false;
    it->second.last_activity = now;
    if (global_in_flight_ > 0) --global_in_flight_;
  }
}  // namespace nvhttp::network_probe
