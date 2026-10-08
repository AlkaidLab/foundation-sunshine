#include "fec.h"

#include <algorithm>
#include <boost/math/distributions/binomial.hpp>
#include <cmath>

namespace video_fec {
  std::optional<block_t>
  plan_block(std::size_t data, unsigned percentage, unsigned minimum_parity) {
    if (!data || data > 1023 || percentage > 255 || minimum_parity > 255) return {};
    if (!percentage) return block_t { static_cast<std::uint16_t>(data), 0, 0 };
    auto ratio = percentage;
    auto parity = (data * ratio + 99) / 100;
    if (parity < minimum_parity) {
      ratio = static_cast<unsigned>(100 * (minimum_parity - 1) / data + 1);
      parity = (data * ratio + 99) / 100;
    }
    if (ratio > 255 || data + parity > 255) return {};
    return block_t { static_cast<std::uint16_t>(data), static_cast<std::uint16_t>(parity), static_cast<std::uint8_t>(ratio) };
  }
  std::optional<frame_t>
  plan(std::size_t data, unsigned percentage, unsigned minimum_parity) {
    if (!data || data > 4092 || percentage > 255 || minimum_parity > 255) return {};
    if (percentage) {
      for (std::size_t count = 1; count <= 4; ++count) {
        if (data < count || data > count * 255) continue;
        frame_t frame;
        frame.count = count;
        bool valid = true;
        for (std::size_t i = 0; i < count; ++i) {
          const auto d = data / count + (i < data % count);
          const auto block = plan_block(d, percentage, minimum_parity);
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
    frame.count = std::min<std::size_t>(4, (data + 254) / 255);
    for (std::size_t i = 0; i < frame.count; ++i)
      frame.blocks[i].data = static_cast<std::uint16_t>(data / frame.count + (i < data % frame.count));
    return frame;
  }

  void
  controller_t::initialize(unsigned host_percentage, bool host_automatic, unsigned maximum) {
    std::lock_guard lock(mutex_);
    host_percentage_ = std::min(host_percentage, 255U);
    fixed_percentage_ = host_percentage_;
    maximum_ = std::min(maximum, 100U);
    host_automatic_ = host_automatic;
    mode_ = mode_e::host;
    has_report_ = false;
    last_report_ms_ = -1;
  }

  bool
  controller_t::set_mode(mode_e mode, unsigned percentage) {
    if (mode != mode_e::host && mode != mode_e::automatic && mode != mode_e::fixed) return false;
    if (percentage > 100) return false;
    std::lock_guard lock(mutex_);
    mode_ = mode;
    fixed_percentage_ = percentage;
    return true;
  }

  bool
  controller_t::report(std::uint32_t sequence, std::uint32_t data, std::uint32_t missing, std::int64_t now) {
    if (!data || data > 1000000 || missing > data || now < 0) return false;
    std::lock_guard lock(mutex_);
    const auto delta = sequence - last_sequence_;
    if (has_report_ && (now < last_report_ms_ || delta == 0 || delta >= 0x80000000U)) return false;
    const double sample = static_cast<double>(missing) / data;
    // Periodic aggregate samples include clean blocks. There is no burst detector.
    loss_ = !has_report_ || now - last_report_ms_ > 3000 ? sample : 0.75 * loss_ + 0.25 * sample;
    last_sequence_ = sequence;
    last_report_ms_ = now;
    has_report_ = true;
    return true;
  }

  unsigned
  controller_t::percentage(std::size_t data, unsigned minimum_parity, std::int64_t now) const {
    unsigned fallback, maximum;
    double loss;
    {
      std::lock_guard lock(mutex_);
      if (mode_ == mode_e::fixed) return fixed_percentage_;
      fallback = host_percentage_;
      if ((mode_ == mode_e::host && !host_automatic_) || !has_report_ || now < last_report_ms_ || now - last_report_ms_ > 3000)
        return fallback;
      maximum = maximum_;
      loss = loss_;
    }
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
        // Union bound: modeled probability of any unrecoverable block <= 0.1%.
        const auto failure = boost::math::cdf(boost::math::complement(distribution, block.parity));
        if (failure > 0.001 / frame->count) {
          sufficient = false;
          break;
        }
      }
      if (sufficient) return ratio;
    }
    // A cap is not a guarantee. Keep the highest representable protection,
    // rather than selecting a ratio that disables FEC for this frame.
    return best;
  }
}  // namespace video_fec
