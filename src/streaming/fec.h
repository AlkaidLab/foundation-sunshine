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

  class controller_t {
  public:
    void
    initialize(unsigned host_percentage, bool host_automatic, unsigned automatic_maximum);
    bool
    set_mode(mode_e mode, unsigned percentage);
    bool
    report(std::uint32_t sequence, std::uint32_t data, std::uint32_t missing, std::int64_t now_ms);
    unsigned
    percentage(std::size_t data, unsigned minimum_parity, std::int64_t now_ms) const;

  private:
    mutable std::mutex mutex_;
    mode_e mode_ = mode_e::host;
    unsigned host_percentage_ = 20;
    unsigned fixed_percentage_ = 20;
    unsigned maximum_ = 50;
    bool host_automatic_ = false;
    bool has_report_ = false;
    std::uint32_t last_sequence_ = 0;
    std::int64_t last_report_ms_ = -1;
    double loss_ = 0;
  };
}  // namespace video_fec
