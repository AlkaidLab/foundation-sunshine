/** @file src/streaming/bitrate.h
 * @brief Session bitrate allocation shared by control, encoding and RS protection.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace streaming {
  // FEC is repair/source, not repair/(source + repair). These are nominal
  // budgets; packet rounding, minimum parity and encoder bursts are not a wire cap.
  int
  encoder_bitrate(int video_budget_kbps, unsigned fec_percentage);
  int
  total_bitrate(int encoder_bitrate_kbps, unsigned fec_percentage);

  struct bitrate_allocation_t {
    int total_kbps;
    int encoder_kbps;
    unsigned fec_percentage;
    bool
    operator==(const bitrate_allocation_t &) const = default;
  };

  class bitrate_budget_t {
  public:
    bitrate_budget_t(int total_kbps, int maximum_kbps, int audio_kbps,
      int overhead_kbps, unsigned fec_percentage, bool automatic);
    int
    request(int total_kbps);
    int
    total() const;
    bitrate_allocation_t
    applied() const;
    void
    observe_fec(unsigned percentage, std::int64_t now_ms);
    std::optional<bitrate_allocation_t>
    pending(std::int64_t now_ms);
    // Called only by the encoding thread after successful reconfiguration.
    void
    commit(const bitrate_allocation_t &allocation);

  private:
    bitrate_allocation_t
    allocate() const;
    mutable std::mutex mutex_;
    int maximum_, audio_, overhead_, requested_total_;
    unsigned requested_fec_, window_fec_;
    bool automatic_;
    std::int64_t window_ms_ = -1;
    std::int64_t attempt_ms_ = -1;
    bitrate_allocation_t applied_;
    std::optional<bitrate_allocation_t> attempted_;
  };
}  // namespace streaming
