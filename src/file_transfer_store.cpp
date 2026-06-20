/**
 * @file src/file_transfer_store.cpp
 * @brief See file_transfer_store.h.
 */
#include "file_transfer_store.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include <openssl/rand.h>

namespace file_transfer_store {
  namespace {
    using clock_t = std::chrono::steady_clock;

    std::mutex g_mu;
    std::unordered_map<offer_id, offer_t> g_offers;
    std::deque<offer_id> g_fifo;

    std::string
    to_utf8_string(const std::filesystem::path &path) {
      const auto raw = path.u8string();
      return std::string(raw.begin(), raw.end());
    }

    std::string
    make_id() {
      std::array<std::uint8_t, 32> raw {};
      if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("file_transfer_store: RAND_bytes failed");
      }

      static constexpr char kHex[] = "0123456789abcdef";
      std::string out;
      out.resize(raw.size() * 2);
      for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i * 2] = kHex[raw[i] >> 4];
        out[i * 2 + 1] = kHex[raw[i] & 0x0F];
      }
      return out;
    }

    void
    sweep_locked(clock_t::time_point now) {
      while (!g_fifo.empty()) {
        auto it = g_offers.find(g_fifo.front());
        if (it == g_offers.end()) {
          g_fifo.pop_front();
          continue;
        }
        if (it->second.expires_at > now) {
          break;
        }
        g_offers.erase(it);
        g_fifo.pop_front();
      }
    }

    bool
    is_hex_id(const std::string &id) {
      if (id.size() != 64) {
        return false;
      }
      for (char c : id) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  create_result_t
  create_single_file_offer(const std::filesystem::path &path) {
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(path, ec);
    if (ec) {
      return { false, "not_found", {} };
    }

    if (!std::filesystem::is_regular_file(canonical, ec) || ec) {
      return { false, "unsupported_file_type", {} };
    }

    const auto size = std::filesystem::file_size(canonical, ec);
    if (ec) {
      return { false, "stat_failed", {} };
    }
    if (size > kMaxSingleFileBytes) {
      return { false, "too_large", {} };
    }

    offer_t offer;
    offer.path = canonical;
    offer.display_name = to_utf8_string(canonical.filename());
    if (offer.display_name.empty()) {
      offer.display_name = "download";
    }
    offer.size = static_cast<std::uint64_t>(size);
    offer.expires_at = clock_t::now() + std::chrono::seconds(kOfferTtlSeconds);

    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(clock_t::now());

    offer.id = make_id();
    while (g_offers.find(offer.id) != g_offers.end()) {
      offer.id = make_id();
    }

    const auto id = offer.id;
    g_fifo.push_back(id);
    g_offers.emplace(id, offer);
    return { true, {}, offer };
  }

  get_result_t
  get(const offer_id &id) {
    if (!is_hex_id(id)) {
      return { false, {} };
    }

    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(clock_t::now());

    auto it = g_offers.find(id);
    if (it == g_offers.end()) {
      return { false, {} };
    }
    return { true, it->second };
  }

  void
  sweep_expired() {
    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(clock_t::now());
  }

  stats_t
  stats() {
    std::lock_guard<std::mutex> lk(g_mu);
    return { g_offers.size() };
  }
}  // namespace file_transfer_store
