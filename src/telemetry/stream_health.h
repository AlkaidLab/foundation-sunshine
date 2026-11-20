/**
 * @file src/telemetry/stream_health.h
 * @brief Stream health in 60-second windows: latency distributions, queue and request counters, and the anomaly codes that tell a regression without any text.
 *
 * Pure code (std + nlohmann only). The hot-path calls (record, count) never block: a contended lock drops the sample. The sender thread closes a window once a
 * minute (close_window), which classifies it (anomaly_t) and adds it to the chunk that is uploaded as one stream_health event. The chunk holds fixed-bucket
 * histograms of the per-window statistics, so chunks of any number of streams add up bucket by bucket on the Worker (explicit-bucket histograms, OpenTelemetry
 * metrics data model, fs-otel-histogram-20261115).
 *
 * Anomaly codes are stable: a code is never reused for another meaning. Their thresholds are in thresholds_t.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks.h"

namespace telemetry::health {

  /** Upper bounds of the histogram buckets in milliseconds; a value goes in the first bucket whose bound it does not exceed, larger values in the last one. */
  inline constexpr std::array<double, 18> BUCKET_BOUNDS_MS { 1, 2, 4, 8, 12, 16, 24, 33, 50, 75, 100, 150, 200, 300, 500, 750, 1000, 2000 };
  inline constexpr std::size_t BUCKET_COUNT = BUCKET_BOUNDS_MS.size() + 1;
  std::size_t
  bucket_of(double ms);

  /** The window length. */
  inline constexpr std::int64_t WINDOW_MS = 60'000;
  /** A chunk (one stream_health event) is sent at the latest after this long, and when the stream ends. */
  inline constexpr std::int64_t CHUNK_MAX_MS = 60 * 60'000;

  /** The statistics that are kept as histograms: key on the wire, the metric and the statistic. */
  enum class stat_e : std::uint8_t { p50,
    p95,
    max };
  struct hist_key_t {
    std::string_view key;
    hooks::metric_e metric;
    stat_e stat;
    double bad_ms;  ///< a window whose statistic is above this is a bad window (Tier 0 counts them); the Worker has the same list (BAD_ABOVE_MS)
  };
  const std::vector<hist_key_t> &
  hist_keys();

  /** Anomaly codes (stable). */
  namespace code {
    inline constexpr std::string_view queue_flush = "XP_QUEUE_FLUSH";  ///< the packet queue was full and cleared as a whole: frames were dropped at once
    inline constexpr std::string_view send_backlog = "XP_SEND_BACKLOG";  ///< a frame left late: packet ready to last byte handed to the socket
    inline constexpr std::string_view host_latency = "XP_HOST_LATENCY";  ///< host processing latency, the "host" figure of the client
    inline constexpr std::string_view encode_over = "XP_ENCODE_OVER";  ///< encoding takes longer than a frame interval
    inline constexpr std::string_view capture_stall = "XP_CAPTURE_STALL";  ///< capture to conversion took far longer than a frame
    inline constexpr std::string_view frame_skip = "XP_FRAME_SKIP";  ///< informational: frames captured and not submitted to the encoder; this also counts capture above the stream frame rate and variable-refresh skips, so it is never used to compare versions
    inline constexpr std::string_view idr_storm = "XP_IDR_STORM";  ///< the client asked for key frames or reference invalidation many times in a minute
    inline constexpr std::string_view loss_burst = "XP_LOSS_BURST";  ///< the client reports packet loss (network)
    inline constexpr std::string_view rtt_high = "XP_RTT_HIGH";  ///< round trip time (network)
    inline constexpr std::string_view bitrate_short = "XP_BITRATE_SHORT";  ///< the measured bitrate stays far below the target
  }  // namespace code

  /** Every code, in the order of the documentation. */
  const std::vector<std::string_view> &
  all_codes();
  /** Codes that point at the network or the client rather than at the host; the Worker keeps them out of the host regression rules (as it does XP_FRAME_SKIP, which cannot tell a dropped frame from a skipped one). */
  bool
  is_network_code(std::string_view c);

  struct thresholds_t {
    double backlog_severe_ms = 250;  ///< window max of the send backlog
    double backlog_floor_ms = 20;  ///< window p95 of the backlog must exceed max(this, backlog_intervals frame intervals)
    double backlog_intervals = 2;
    double host_floor_ms = 60;  ///< window p95 of the host latency must exceed max(this, host_intervals frame intervals)
    double host_intervals = 4;
    double host_severe_ms = 500;  ///< window max of the host latency
    double encode_floor_ms = 4;  ///< window p95 of the encode time must exceed max(this, one frame interval)
    double capture_warn_ms = 250;
    double capture_severe_ms = 1000;
    double skip_share = 0.05;
    std::uint64_t skip_min_frames = 6;
    std::uint64_t idr_per_window = 6;
    double loss_pct = 5;
    double rtt_ms = 150;
    double bitrate_share = 0.5;  ///< measured below this share of the target
    std::uint64_t min_frames = 10;  ///< a window with fewer sent frames is idle: it is not classified and not added to the histograms
    std::int64_t cooldown_ms = 300'000;  ///< the same code on the same stream at most once per cooldown (warning); severe ones use severe_cooldown_ms
    std::int64_t severe_cooldown_ms = 120'000;
  };

  struct anomaly_t {
    std::string code;
    bool severe = false;
    double value = 0;  ///< the measured value (ms, count or percent, by code)
    double threshold = 0;
    std::uint64_t count = 0;  ///< frames dropped (queue flush), requests (IDR storm), frames skipped (frame skip); 0 otherwise
    std::uint64_t suppressed = 0;  ///< the same code fired and was held back by the cooldown since the last time it was reported
  };

  struct stat_t {
    double p50 = 0, p95 = 0, max = 0;
    std::uint64_t n = 0;
  };

  /** What the sender thread knows from outside the encoder: the client's reports and the bitrate. */
  struct probe_t {
    std::optional<double> rtt_ms;
    std::optional<double> loss_pct;
    int measured_kbps = 0;
    int target_kbps = 0;
    int fec_pct = 0;
  };

  struct window_t {
    std::int64_t end_ms = 0;
    double seconds = 0;
    bool idle = false;
    std::array<stat_t, static_cast<std::size_t>(hooks::metric_e::count_)> metrics {};
    std::array<std::uint64_t, static_cast<std::size_t>(hooks::counter_e::count_)> counters {};
    probe_t probe;
    std::vector<anomaly_t> anomalies;
  };

  struct dims_t {
    std::string codec = "other";
    int width = 0;
    int height = 0;
    int fps = 0;
    bool hdr = false;
    std::string transport = "enet";
    std::string client_kind = "unknown";
    std::string client_os = "other";
    std::string encoder;  ///< token, empty when unknown
    std::string capture_backend;
    std::string gpu_driver;
  };

  /** A bitrate target in kbps -> the Worker's band (BITRATE_BUCKETS in the Worker schema). */
  std::string
  bitrate_band(int kbps);

  class stream_health_t {
  public:
    explicit stream_health_t(std::int64_t start_ms, dims_t dims = {}, thresholds_t thresholds = {});

    // hot path: any thread, never blocks
    void
    record(hooks::metric_e metric, double value) noexcept;
    void
    count(hooks::counter_e counter, std::uint64_t n = 1) noexcept;

    /** Once per tick (sender thread). Closes the window when 60 s have passed since the last one. -> the closed window. */
    std::optional<window_t>
    tick(std::int64_t now_ms, const probe_t &probe);
    /** The stream ends: closes the partial window (when it saw enough frames). */
    std::optional<window_t>
    finish(std::int64_t now_ms, const probe_t &probe);

    /** A frame left the send thread (steady clock, ns): feeds the interarrival-jitter estimate. */
    void
    frame_done(std::int64_t steady_ns) noexcept;

    /** The chunk is due once it reaches CHUNK_MAX_MS and has active windows or backpressure drops. */
    bool
    chunk_due(std::int64_t now_ms) const;
    bool
    chunk_empty() const;
    /** The props of one stream_health event for the windows so far; resets the chunk. Tier 0 (always): the counters and `bad`; Tier 1 (`detail`): also the per-window histograms `h`. */
    nlohmann::json
    take_chunk(std::int64_t now_ms, bool detail = true);

    const dims_t &
    dims() const { return dims_; }
    /** The encoder family of the stream. The stream may begin before the host knows which encoder it uses (a probe is running), so it can be set later: the sender thread and the thread that ends
     * the stream both read it. Empty while it is not known. */
    std::string
    encoder() const;
    void
    set_encoder(std::string encoder);
    const thresholds_t &
    thresholds() const { return thresholds_; }

  private:
    window_t
    close(std::int64_t now_ms, const probe_t &probe, double seconds);
    void
    classify(window_t &w);
    void
    add_to_chunk(const window_t &w);
    double
    interval_ms() const;

    thresholds_t thresholds_;
    dims_t dims_;
    mutable std::mutex encoder_mutex_;
    std::string late_encoder_;
    std::int64_t window_start_ms_;
    std::int64_t chunk_start_ms_;

    static constexpr std::size_t METRICS = static_cast<std::size_t>(hooks::metric_e::count_);
    static constexpr std::size_t COUNTERS = static_cast<std::size_t>(hooks::counter_e::count_);
    static constexpr std::size_t BINS = 4001;  // 1 ms resolution up to 4000 ms (0.1 ms for the jitter up to 400 ms); the exact maximum is tracked next to it
    struct dist_t {
      std::array<std::uint32_t, BINS> bins {};
      std::vector<float> over;  // the samples beyond the last bin (at most 512 kept), in the unit of the metric: their distribution stays exact
      double max = 0;
      std::uint64_t n = 0;
    };
    // the window side (tick, finish, chunk, take_chunk) is called from the sender thread and from the thread that ends a stream: one lock for all of it, taken before mutex_ (the sample side) and never the other way round
    mutable std::mutex window_mutex_;
    std::mutex mutex_;
    std::array<dist_t, METRICS> dist_ {};
    std::array<std::atomic<std::uint64_t>, COUNTERS> counters_ {};

    struct code_state_t {
      std::int64_t last_ms = -1;  ///< the last report of this code, of any severity
      std::int64_t last_severe_ms = -1;  ///< the last severe report: a warning shortly before does not hold a severe one back
      std::uint64_t suppressed = 0;
    };
    std::vector<std::pair<std::string, code_state_t>> codes_;

    // the chunk
    std::vector<std::array<std::uint32_t, BUCKET_COUNT>> hist_;
    std::vector<std::uint64_t> bad_;
    // interarrival jitter (RFC 3550 section 6.4.1), guarded by mutex_ like the samples
    std::int64_t last_done_ns_ = -1;
    std::int64_t last_delta_ns_ = -1;
    double jitter_ms_ = 0;
    std::uint64_t chunk_windows_ = 0, chunk_idle_ = 0, chunk_anomalies_ = 0;
    double chunk_seconds_ = 0;
    std::array<std::uint64_t, COUNTERS> chunk_counters_ {};
    double loss_sum_ = 0, loss_max_ = 0, fec_sum_ = 0, kbps_sum_ = 0, target_sum_ = 0;  // target_sum_ is the sum over the windows that had a target
    std::uint64_t loss_n_ = 0, fec_n_ = 0, kbps_n_ = 0, target_n_ = 0;
    std::vector<double> rtt_;
  };

}  // namespace telemetry::health
