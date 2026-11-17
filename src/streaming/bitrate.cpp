#include "bitrate.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <limits>

namespace streaming {
  namespace {
    constexpr std::int64_t update_interval_ms = 1000;
    int
    clamp_bitrate(std::int64_t value) {
      return static_cast<int>(std::clamp<std::int64_t>(value, 1, std::numeric_limits<int>::max()));
    }
  }  // namespace

  int
  parse_bitrate_ceiling_kbps(std::string_view text) {
    int value = 0;
    const auto *first = text.data(), *last = text.data() + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc {} || end != last || value < 0) return 0;
    return value;
  }

  bitrate_range_t
  abr_range_under_ceiling(int client_min, int client_max, int preset_min, int preset_max, int ceiling) {
    ceiling = std::max(0, ceiling);
    int max_kbps = client_max > 0 ? (ceiling > 0 ? std::min(client_max, ceiling) : client_max) : (ceiling > 0 ? ceiling : std::max(0, preset_max));
    int min_kbps = client_min > 0 ? client_min : std::max(0, preset_min);
    if (max_kbps > 0 && min_kbps > max_kbps) min_kbps = max_kbps;
    return { min_kbps, max_kbps };
  }

  int
  encoder_bitrate(int budget, unsigned percentage) {
    return clamp_bitrate(static_cast<std::int64_t>(budget) * 100 / (100 + std::min(percentage, 255U)));
  }

  int
  total_bitrate(int encoder, unsigned percentage) {
    return clamp_bitrate((static_cast<std::int64_t>(encoder) * (100 + std::min(percentage, 255U)) + 99) / 100);
  }

  bitrate_budget_t::bitrate_budget_t(int total, int maximum, int audio, int overhead, unsigned fec, bool automatic):
      maximum_(std::max(0, maximum)), audio_(std::max(0, audio)), overhead_(std::max(0, overhead)),
      requested_total_(maximum > 0 ? std::clamp(total, 1, maximum) : std::max(1, total)), wanted_total_(std::max(1, total)),
      requested_fec_(std::min(fec, 255U)), window_fec_(requested_fec_), automatic_(automatic), applied_(allocate()) {}

  bitrate_allocation_t
  bitrate_budget_t::allocate() const {
    auto video_budget = requested_total_ - std::min(audio_, requested_total_ / 5);
    video_budget -= std::min(overhead_, video_budget / 10);
    return { requested_total_, encoder_bitrate(video_budget, requested_fec_), requested_fec_ };
  }

  int
  bitrate_budget_t::request(int total) {
    std::lock_guard lock(mutex_);
    wanted_total_ = std::max(1, total);
    requested_total_ = maximum_ > 0 ? std::clamp(wanted_total_, 1, maximum_) : wanted_total_;
    return requested_total_;
  }

  int
  bitrate_budget_t::set_maximum(int maximum) {
    std::lock_guard lock(mutex_);
    maximum_ = std::max(0, maximum);
    requested_total_ = maximum_ > 0 ? std::clamp(wanted_total_, 1, maximum_) : wanted_total_;
    return requested_total_;
  }

  int
  bitrate_budget_t::maximum() const {
    std::lock_guard lock(mutex_);
    return maximum_;
  }

  int
  bitrate_budget_t::total() const {
    std::lock_guard lock(mutex_);
    return requested_total_;
  }

  bitrate_allocation_t
  bitrate_budget_t::applied() const {
    std::lock_guard lock(mutex_);
    return applied_;
  }

  void
  bitrate_budget_t::observe_fec(unsigned percentage, std::int64_t now) {
    std::lock_guard lock(mutex_);
    if (!automatic_ || now < 0) return;
    percentage = std::min(percentage, 255U);
    if (window_ms_ < 0) {
      window_ms_ = now;
      window_fec_ = percentage;
      return;
    }
    if (now < window_ms_) return;
    // Use the highest requested ratio in a completed window, so a small or
    // unusually large frame does not repeatedly reconfigure the encoder.
    if (now - window_ms_ >= update_interval_ms) {
      requested_fec_ = window_fec_;
      window_ms_ = now;
      window_fec_ = percentage;
    }
    else {
      window_fec_ = std::max(window_fec_, percentage);
    }
  }

  std::optional<bitrate_allocation_t>
  bitrate_budget_t::pending(std::int64_t now) {
    std::lock_guard lock(mutex_);
    auto next = allocate();
    if (next == applied_ || now < 0) return {};
    // Ignore one-point oscillations, but always allow enabling/disabling protection.
    if (next.total_kbps == applied_.total_kbps && next.fec_percentage != 0 && applied_.fec_percentage != 0 &&
        std::abs(static_cast<int>(next.fec_percentage) - static_cast<int>(applied_.fec_percentage)) < 2) return {};
    if (attempted_ == next && (now < attempt_ms_ || now - attempt_ms_ < update_interval_ms)) return {};
    attempted_ = next;
    attempt_ms_ = now;
    return next;
  }

  void
  bitrate_budget_t::commit(const bitrate_allocation_t &allocation) {
    std::lock_guard lock(mutex_);
    applied_ = allocation;
    // Do not overwrite requests arriving while the encoder was being updated.
  }
}  // namespace streaming
