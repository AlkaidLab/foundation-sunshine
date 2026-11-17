/** @file src/streaming/bitrate.h
 * @brief Session bitrate allocation shared by control, encoding and RS protection.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace streaming {
  // FEC is repair/source, not repair/(source + repair). These are nominal
  // budgets; packet rounding, minimum parity and encoder bursts are not a wire cap.
  int
  encoder_bitrate(int video_budget_kbps, unsigned fec_percentage);
  int
  total_bitrate(int encoder_bitrate_kbps, unsigned fec_percentage);

  // What a saved `max_bitrate` text means as the host's ceiling in Kbps: a whole number, 0 (no ceiling) for anything else and for a negative one.
  int
  parse_bitrate_ceiling_kbps(std::string_view text);

  struct bitrate_range_t {
    int min_kbps;
    int max_kbps;
    bool
    operator==(const bitrate_range_t &) const = default;
  };

  // The range an ABR session works in under the host ceiling (0 = none). `client_*` is what the client asked for (0 = nothing), `preset_*` what the mode gives when it asked for nothing. A ceiling holds
  // the client's maximum; without a client maximum it is the maximum itself (the session inherits it); with no ceiling the client's or the preset maximum applies. The minimum never exceeds the maximum.
  // Recomputed from the same inputs whenever the ceiling changes, so a raised or removed ceiling gives back the client's own range.
  bitrate_range_t
  abr_range_under_ceiling(int client_min_kbps, int client_max_kbps, int preset_min_kbps, int preset_max_kbps, int ceiling_kbps);

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
    // What a client or the ABR asks for is remembered as asked; what the session gets is that, held to the host cap (0 = no cap).
    int
    request(int total_kbps);
    // The host cap changed while the session runs (the tray's 码率上限): the session's total follows at once, down to the new cap when it is above it and back up to what was last asked when the cap is raised or
    // removed. Returns the new total. The encoder thread picks the change up through pending() like any other request.
    int
    set_maximum(int maximum_kbps);
    int
    maximum() const;
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
    int maximum_, audio_, overhead_, requested_total_, wanted_total_;
    unsigned requested_fec_, window_fec_;
    bool automatic_;
    std::int64_t window_ms_ = -1;
    std::int64_t attempt_ms_ = -1;
    bitrate_allocation_t applied_;
    std::optional<bitrate_allocation_t> attempted_;
  };
}  // namespace streaming
