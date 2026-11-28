/** @file src/streaming/fec.h
 * @brief Per-session RS protection, without changing the video send path.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace video_fec {
  // Client preferences are negotiated once when the stream connects.
  constexpr int host_preference = -2;
  constexpr int automatic_preference = -1;
  constexpr unsigned max_client_percentage = 100;
  constexpr std::size_t max_blocks = 4;  // Two-bit FEC block count on the wire.
  constexpr std::size_t max_data_shards_per_block = 1023;  // Ten-bit packet index.
  constexpr std::size_t max_data_shards_per_frame = max_blocks * max_data_shards_per_block;

  std::optional<int>
  parse_preference(std::string_view text) noexcept;
  enum class mode_e {
    host = 0,
    automatic = 1,
    fixed = 2,
  };
  struct block_t {
    std::uint16_t data = 0;
    std::uint16_t parity = 0;
    std::uint8_t percentage = 0;
  };
  struct frame_t {
    std::array<block_t, max_blocks> blocks {};
    std::size_t count = 0;
    bool skipped = false;
  };
  std::optional<block_t>
  plan_block(std::size_t data, unsigned percentage, unsigned minimum_parity);
  std::optional<frame_t>
  plan(std::size_t data, unsigned percentage, unsigned minimum_parity);

  // Finite history of aggregate feedback. No feedback is distinct from zero
  // loss; controller_t owns validation, monotonic time and synchronization.
  class loss_window_t {
  public:
    static constexpr std::int64_t duration_ms = 10000;
    void
    record(std::uint32_t data, std::uint32_t missing, std::int64_t now_ms);
    // Highest packet-weighted one-second loss rate, or no valid observation.
    std::optional<double>
    estimate(std::int64_t now_ms) const;

  private:
    struct bucket_t {
      std::int64_t second = -1;
      std::uint64_t data = 0;
      std::uint64_t missing = 0;
    };
    std::array<bucket_t, duration_ms / 1000> buckets_ {};
  };

  class controller_t {
  public:
    void
    initialize(unsigned host_percentage, bool host_automatic, unsigned automatic_maximum);
    bool
    set_mode(mode_e mode, unsigned percentage);
    bool
    report(std::uint32_t sequence, std::uint32_t data, std::uint32_t missing, std::int64_t now_ms);
    // Ratio for a frame of `data` packets. While loss is reported it follows the loss estimate.
    // Once the loss history expires, the protection that silenced the loss is kept for any extra
    // clean time that earlier premature releases earned, then released.
    unsigned
    percentage(std::size_t data, unsigned minimum_parity, std::int64_t now_ms);

  private:
    std::mutex mutex_;
    mode_e mode_ = mode_e::host;
    unsigned host_percentage_ = 20;
    unsigned fixed_percentage_ = 20;
    unsigned maximum_ = 50;
    bool host_automatic_ = false;
    std::uint32_t last_sequence_ = 0;
    std::int64_t last_report_ms_ = -1;
    std::int64_t resume_since_ms_ = -1;
    loss_window_t loss_window_;
    std::int64_t clean_since_ms_ = -1;  // When the loss history first expired after a loss.
    std::int64_t released_ms_ = -1;  // Negative while protected, the release time afterwards.
    std::int64_t decay_at_ms_ = -1;  // A release clean until then halves the extra wait.
    std::int64_t release_delay_ms_ = 0;  // Extra clean time demanded before the next release.
    double epoch_loss_ = 0;  // Highest estimate of the current protected period.
    double released_loss_ = 0;  // The estimate the released protection was selected for.
  };
}  // namespace video_fec
