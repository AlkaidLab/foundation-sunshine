#include "fec.h"

#include <algorithm>
#include <boost/math/distributions/binomial.hpp>
#include <charconv>

namespace video_fec {
  namespace {
    constexpr unsigned max_wire_percentage = 255;
    constexpr std::size_t max_rs_shards = 255;
    constexpr std::uint32_t max_feedback_data_packets = 1000000;
    constexpr std::uint32_t sequence_half_range = 0x80000000U;
    constexpr std::int64_t feedback_timeout_ms = 3000;
    constexpr double frame_failure_target = 0.001;
    // The summary counts data shards only, so parity that absorbed the loss reads as a clean
    // second: a release can look safe while the link is still overloaded. Loss reported this
    // soon after a release means the protection it removed was still needed.
    constexpr std::int64_t premature_release_ms = 4000;
    // Longest extra clean time demanded before a release after repeated premature ones.
    constexpr std::int64_t release_backoff_maximum_ms = 160000;

    unsigned
    select_percentage(double loss, std::size_t data, unsigned minimum_parity, unsigned maximum) {
      if (loss == 0) return 0;
      unsigned best = 0;
      for (unsigned ratio = 1; ratio <= maximum; ++ratio) {
        const auto frame = plan(data, ratio, minimum_parity);
        if (!frame || frame->skipped) continue;
        best = ratio;
        bool sufficient = true;
        for (std::size_t i = 0; i < frame->count; ++i) {
          const auto &block = frame->blocks[i];
          const boost::math::binomial_distribution<double> distribution(block.data + block.parity, loss);
          // Independent-loss model and union bound, not a measured guarantee.
          const auto failure = boost::math::cdf(boost::math::complement(distribution, block.parity));
          if (failure > frame_failure_target / frame->count) {
            sufficient = false;
            break;
          }
        }
        if (sufficient) return ratio;
      }
      // A cap is not a guarantee. Keep representable protection rather than
      // selecting a ratio that disables FEC for this frame.
      return best;
    }
  }  // namespace

  std::optional<int>
  parse_preference(std::string_view text) noexcept {
    if (text.empty()) return {};
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
        value < host_preference || value > static_cast<int>(max_client_percentage)) {
      return {};
    }
    return value;
  }
  std::optional<block_t>
  plan_block(std::size_t data, unsigned percentage, unsigned minimum_parity) {
    if (!data || data > max_data_shards_per_block ||
        percentage > max_wire_percentage || minimum_parity > max_rs_shards) {
      return {};
    }
    if (!percentage) return block_t { static_cast<std::uint16_t>(data), 0, 0 };
    auto ratio = percentage;
    auto parity = (data * ratio + 99) / 100;
    if (parity < minimum_parity) {
      ratio = static_cast<unsigned>(100 * (minimum_parity - 1) / data + 1);
      parity = (data * ratio + 99) / 100;
    }
    if (ratio > max_wire_percentage || data + parity > max_rs_shards) return {};
    return block_t { static_cast<std::uint16_t>(data), static_cast<std::uint16_t>(parity), static_cast<std::uint8_t>(ratio) };
  }
  std::optional<frame_t>
  plan(std::size_t data, unsigned percentage, unsigned minimum_parity) {
    if (!data || data > max_data_shards_per_frame ||
        percentage > max_wire_percentage || minimum_parity > max_rs_shards) {
      return {};
    }
    if (percentage) {
      for (std::size_t count = 1; count <= max_blocks; ++count) {
        if (data < count || data > count * max_rs_shards) continue;
        frame_t frame;
        frame.count = count;
        bool valid = true;
        for (std::size_t i = 0; i < count; ++i) {
          const auto block_data = data / count + (i < data % count);
          const auto block = plan_block(block_data, percentage, minimum_parity);
          if (!block) {
            valid = false;
            break;
          }
          frame.blocks[i] = *block;
        }
        if (valid) return frame;
      }
    }
    frame_t frame;
    frame.skipped = percentage != 0;
    frame.count = std::min(max_blocks, (data + max_rs_shards - 1) / max_rs_shards);
    for (std::size_t i = 0; i < frame.count; ++i) {
      frame.blocks[i].data = static_cast<std::uint16_t>(data / frame.count + (i < data % frame.count));
    }
    return frame;
  }

  void
  loss_window_t::record(std::uint32_t data, std::uint32_t missing, std::int64_t now) {
    const auto second = now / 1000;
    auto &bucket = buckets_[static_cast<std::size_t>(second) % buckets_.size()];
    if (bucket.second != second) bucket = { second, 0, 0 };
    bucket.data += data;
    bucket.missing += missing;
  }

  std::optional<double>
  loss_window_t::estimate(std::int64_t now) const {
    if (now < 0) return {};
    const auto second = now / 1000;
    std::optional<double> result;
    for (const auto &bucket : buckets_) {
      if (bucket.data && bucket.second >= 0 && bucket.second <= second &&
          second - bucket.second < static_cast<std::int64_t>(buckets_.size())) {
        result = std::max(result.value_or(0), static_cast<double>(bucket.missing) / bucket.data);
      }
    }
    return result;
  }

  void
  controller_t::initialize(unsigned host_percentage, bool host_automatic, unsigned maximum) {
    std::lock_guard lock(mutex_);
    host_percentage_ = std::min(host_percentage, max_wire_percentage);
    fixed_percentage_ = host_percentage_;
    maximum_ = std::min(maximum, max_client_percentage);
    host_automatic_ = host_automatic;
    mode_ = mode_e::host;
    last_report_ms_ = -1;
    resume_since_ms_ = -1;
    loss_window_ = {};
    clean_since_ms_ = -1;
    released_ms_ = -1;
    decay_at_ms_ = -1;
    release_delay_ms_ = 0;
    epoch_loss_ = 0;
    released_loss_ = 0;
  }

  bool
  controller_t::set_mode(mode_e mode, unsigned percentage) {
    if (mode != mode_e::host && mode != mode_e::automatic && mode != mode_e::fixed) {
      return false;
    }
    if (percentage > max_client_percentage) return false;
    std::lock_guard lock(mutex_);
    mode_ = mode;
    fixed_percentage_ = percentage;
    return true;
  }

  bool
  controller_t::report(std::uint32_t sequence, std::uint32_t data, std::uint32_t missing, std::int64_t now) {
    if (!data || data > max_feedback_data_packets || missing > data || now < 0) return false;
    std::lock_guard lock(mutex_);
    const auto delta = sequence - last_sequence_;
    if (last_report_ms_ >= 0 && (now < last_report_ms_ || delta == 0 || delta >= sequence_half_range)) {
      return false;
    }
    if (last_report_ms_ >= 0 && (delta != 1 || now - last_report_ms_ > feedback_timeout_ms)) {
      resume_since_ms_ = now;
      loss_window_ = {};  // Do not combine observations across an unknown gap.
      // The clean and decay clocks of the discarded history must not count the unknown
      // interval: resume as protected by the held epoch and demand the whole wait again
      // before the next release.
      clean_since_ms_ = -1;
      released_ms_ = -1;
    }
    loss_window_.record(data, missing, now);
    last_sequence_ = sequence;
    last_report_ms_ = now;
    return true;
  }

  unsigned
  controller_t::percentage(std::size_t data, unsigned minimum_parity, std::int64_t now) {
    unsigned fallback, maximum;
    double loss;
    {
      std::lock_guard lock(mutex_);
      if (mode_ == mode_e::fixed) return fixed_percentage_;
      fallback = host_percentage_;
      const bool automatic = mode_ == mode_e::automatic || host_automatic_;
      const bool fresh_report = last_report_ms_ >= 0 && now >= last_report_ms_ &&
                                now - last_report_ms_ <= feedback_timeout_ms;
      if (!automatic || !fresh_report) {
        return fallback;
      }
      maximum = maximum_;
      const auto estimate = loss_window_.estimate(now);
      // A missing report or feedback timeout is unknown, not clean. Before releasing protection,
      // require a new observation period after resumption. Startup is distinct.
      if (!estimate || (*estimate == 0 && resume_since_ms_ >= 0 &&
                         now - resume_since_ms_ < loss_window_t::duration_ms)) {
        return fallback;
      }
      if (*estimate > 0) {
        if (released_ms_ >= 0) {
          // Loss returned after a release. Within a few seconds it means the release removed
          // protection that was still needed: demand more clean time before the next release,
          // and re-arm no weaker than what was released, because the summary counts data
          // shards only and parity that absorbed the loss read as clean seconds.
          if (now - released_ms_ <= premature_release_ms) {
            release_delay_ms_ = release_delay_ms_ > 0 ? std::min(release_delay_ms_ * 2, release_backoff_maximum_ms) :
                                                        loss_window_t::duration_ms;
            epoch_loss_ = std::max(*estimate, released_loss_);
          }
          else {
            epoch_loss_ = *estimate;
          }
          released_ms_ = -1;
        }
        else {
          // Protected: follow the loss estimate up, never down. A falling estimate only says
          // the parity in place is absorbing the loss, not that the link has recovered.
          epoch_loss_ = std::max(epoch_loss_, *estimate);
        }
        clean_since_ms_ = -1;
        loss = epoch_loss_;
      }
      else if (released_ms_ >= 0) {
        // Released and still clean. A release that stays safe halves the extra wait for the next one.
        if (release_delay_ms_ > 0 && now >= decay_at_ms_) {
          release_delay_ms_ /= 2;
          decay_at_ms_ = now + release_delay_ms_ + loss_window_t::duration_ms;
        }
        return 0;
      }
      else {
        if (epoch_loss_ == 0) {
          // Started clean and stayed clean: staying at zero protects nothing and is not a
          // release, so a first loss must not pay the premature-release penalty.
          return 0;
        }
        // The loss history has expired. Keep the protection that silenced the loss until the
        // stream has stayed clean for the extra time earlier premature releases earned.
        if (clean_since_ms_ < 0) clean_since_ms_ = now;
        if (now - clean_since_ms_ < release_delay_ms_) {
          loss = epoch_loss_;
        }
        else {
          released_ms_ = now;
          released_loss_ = epoch_loss_;
          decay_at_ms_ = now + release_delay_ms_ + loss_window_t::duration_ms;
          return 0;
        }
      }
    }
    return select_percentage(loss, data, minimum_parity, maximum);
  }
}  // namespace video_fec
