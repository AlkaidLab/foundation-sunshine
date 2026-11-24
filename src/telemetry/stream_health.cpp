/**
 * @file src/telemetry/stream_health.cpp
 * @brief Stream health windows: see stream_health.h.
 */
#include "stream_health.h"

#include <algorithm>
#include <cmath>

namespace telemetry::health {

  using hooks::counter_e;
  using hooks::metric_e;

  namespace {
    constexpr std::size_t
    idx(metric_e m) { return static_cast<std::size_t>(m); }
    constexpr std::size_t
    idx(counter_e c) { return static_cast<std::size_t>(c); }

    double
    round2(double v) {
      return std::round(v * 100.0) / 100.0;
    }
  }  // namespace

  std::size_t
  bucket_of(double ms) {
    for (std::size_t i = 0; i < BUCKET_BOUNDS_MS.size(); ++i)
      if (ms <= BUCKET_BOUNDS_MS[i]) return i;
    return BUCKET_BOUNDS_MS.size();
  }

  const std::vector<hist_key_t> &
  hist_keys() {
    static const std::vector<hist_key_t> keys {
      { "host_p50", metric_e::host_ms, stat_e::p50, 30 },
      { "host_p95", metric_e::host_ms, stat_e::p95, 60 },
      { "host_max", metric_e::host_ms, stat_e::max, 250 },
      { "capture_p95", metric_e::capture_ms, stat_e::p95, 33 },
      { "convert_p95", metric_e::convert_ms, stat_e::p95, 33 },
      { "encode_p95", metric_e::encode_ms, stat_e::p95, 16.7 },
      { "encode_max", metric_e::encode_ms, stat_e::max, 100 },
      { "send_p95", metric_e::send_ms, stat_e::p95, 33 },
      { "send_max", metric_e::send_ms, stat_e::max, 100 },
      { "backlog_p95", metric_e::backlog_ms, stat_e::p95, 30 },
      { "backlog_max", metric_e::backlog_ms, stat_e::max, 250 },
      { "jitter_p50", metric_e::jitter_ms, stat_e::p50, 1 },
      { "jitter_p95", metric_e::jitter_ms, stat_e::p95, 1.5 },
    };
    return keys;
  }

  const std::vector<std::string_view> &
  all_codes() {
    static const std::vector<std::string_view> codes {
      code::queue_flush,
      code::send_backlog,
      code::host_latency,
      code::encode_over,
      code::capture_stall,
      code::frame_skip,
      code::idr_storm,
      code::loss_burst,
      code::rtt_high,
      code::bitrate_short,
    };
    return codes;
  }

  bool
  is_network_code(std::string_view c) {
    return c == code::loss_burst || c == code::rtt_high || c == code::bitrate_short;
  }

  std::string
  bitrate_band(int kbps) {
    if (kbps < 8000) return "0-8000";
    if (kbps < 15000) return "8000-15000";
    if (kbps < 30000) return "15000-30000";
    if (kbps < 60000) return "30000-60000";
    if (kbps < 100000) return "60000-100000";
    if (kbps < 200000) return "100000-200000";
    return "200000+";
  }

  stream_health_t::stream_health_t(std::int64_t start_ms, dims_t dims, thresholds_t thresholds):
      thresholds_(thresholds), dims_(std::move(dims)), window_start_ms_(start_ms), chunk_start_ms_(start_ms) {
    hist_.assign(hist_keys().size(), std::array<std::uint32_t, BUCKET_COUNT> {});
    bad_.assign(hist_keys().size(), 0);
  }

  double
  stream_health_t::interval_ms() const {
    return dims_.fps > 0 ? 1000.0 / dims_.fps : 1000.0 / 60.0;
  }

  void
  stream_health_t::record(metric_e metric, double value) noexcept {
    if (!(value >= 0.0) || !std::isfinite(value) || idx(metric) >= METRICS) return;
    std::unique_lock lock { mutex_, std::try_to_lock };
    if (!lock.owns_lock()) return;  // the sender thread is closing the window: this one sample is not worth a wait
    auto &d = dist_[idx(metric)];
    // bin i holds the values in (i - 1, i]: the upper edge is inclusive, as the histogram buckets are
    const auto bin = static_cast<std::size_t>(std::min<double>(std::ceil(value), static_cast<double>(BINS - 1)));
    ++d.bins[bin];
    if (bin == BINS - 1 && d.over.size() < 512) d.over.push_back(static_cast<float>(value));
    ++d.n;
    d.max = std::max(d.max, value);
  }

  void
  stream_health_t::count(counter_e counter, std::uint64_t n) noexcept {
    if (idx(counter) >= COUNTERS) return;
    counters_[idx(counter)].fetch_add(n, std::memory_order_relaxed);
  }

  void
  stream_health_t::frame_done(std::int64_t steady_ns) noexcept {
    std::unique_lock lock { mutex_, std::try_to_lock };
    if (!lock.owns_lock()) return;
    const auto interval_ns = static_cast<std::int64_t>(interval_ms() * 1e6);
    if (last_done_ns_ >= 0) {
      const auto delta = steady_ns - last_done_ns_;
      // a long gap (a static picture, a pause) is not jitter: the estimator starts over after it
      if (delta <= 0 || delta > 3 * interval_ns) {
        last_delta_ns_ = -1;
      }
      else {
        if (last_delta_ns_ >= 0) {
          const auto d = std::abs(static_cast<double>(delta - last_delta_ns_)) / 1e6;
          jitter_ms_ += (d - jitter_ms_) / 16.0;
          auto &dist = dist_[idx(metric_e::jitter_ms)];
          // jitter is far below 1 ms in a healthy stream: it is kept in tenths of a millisecond so the 1 ms bins of the distribution can tell 0.3 from 2
          const auto jitter_bin = std::min<std::size_t>(static_cast<std::size_t>(std::ceil(jitter_ms_ * 10.0)), BINS - 1);
          ++dist.bins[jitter_bin];
          if (jitter_bin == BINS - 1 && dist.over.size() < 512) dist.over.push_back(static_cast<float>(jitter_ms_));
          ++dist.n;
          dist.max = std::max(dist.max, jitter_ms_);
        }
        last_delta_ns_ = delta;
      }
    }
    last_done_ns_ = steady_ns;
  }

  std::string
  stream_health_t::encoder() const {
    std::lock_guard lock { encoder_mutex_ };
    return late_encoder_.empty() ? dims_.encoder : late_encoder_;
  }

  void
  stream_health_t::set_encoder(std::string encoder) {
    std::lock_guard lock { encoder_mutex_ };
    late_encoder_ = std::move(encoder);
  }

  std::optional<window_t>
  stream_health_t::tick(std::int64_t now_ms, const probe_t &probe) {
    std::lock_guard window { window_mutex_ };
    if (now_ms - window_start_ms_ < WINDOW_MS) return std::nullopt;
    const auto seconds = static_cast<double>(now_ms - window_start_ms_) / 1000.0;
    auto w = close(now_ms, probe, seconds);
    return w;
  }

  std::optional<window_t>
  stream_health_t::finish(std::int64_t now_ms, const probe_t &probe) {
    std::lock_guard window { window_mutex_ };
    const auto span = now_ms - window_start_ms_;
    if (span < 5000) {
      // too short to say anything; the counters of these seconds are dropped with the window
      std::lock_guard lock { mutex_ };
      for (auto &d : dist_) d = dist_t {};
      for (auto &c : counters_) c.store(0, std::memory_order_relaxed);
      window_start_ms_ = now_ms;
      return std::nullopt;
    }
    return close(now_ms, probe, static_cast<double>(span) / 1000.0);
  }

  window_t
  stream_health_t::close(std::int64_t now_ms, const probe_t &probe, double seconds) {
    window_t w;
    w.end_ms = now_ms;
    w.seconds = seconds;
    w.probe = probe;
    {
      std::lock_guard lock { mutex_ };
      for (std::size_t m = 0; m < METRICS; ++m) {
        auto &d = dist_[m];
        stat_t s;
        // the jitter distribution is kept in tenths of a millisecond (see frame_done), every other one in milliseconds
        const double scale = m == idx(metric_e::jitter_ms) ? 10.0 : 1.0;
        s.n = d.n;
        s.max = d.max;
        if (d.n > 0) {
          const auto at = [&](double p) {
            const auto rank = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(d.n)));
            std::uint64_t seen = 0;
            for (std::size_t i = 0; i < BINS; ++i) {
              seen += d.bins[i];
              if (seen >= std::max<std::uint64_t>(rank, 1)) {
                if (i == BINS - 1) {
                  // beyond the last bin: the kept samples give the rank exactly, the largest value stands in when more than were kept lie there
                  auto over = d.over;
                  std::sort(over.begin(), over.end());
                  const auto before = seen - d.bins[i];
                  const auto at_rank = std::max<std::uint64_t>(rank, 1) - before - 1;
                  return at_rank < over.size() ? static_cast<double>(over[at_rank]) : d.max;
                }
                return std::min(d.max, static_cast<double>(i) / scale);
              }
            }
            return d.max;
          };
          s.p50 = at(50);
          s.p95 = at(95);
        }
        w.metrics[m] = s;
        d = dist_t {};
      }
      for (std::size_t c = 0; c < COUNTERS; ++c) w.counters[c] = counters_[c].exchange(0, std::memory_order_relaxed);
    }
    window_start_ms_ = now_ms;
    w.idle = w.counters[idx(counter_e::frames_sent)] < thresholds_.min_frames;
    if (!w.idle) classify(w);
    add_to_chunk(w);
    return w;
  }

  void
  stream_health_t::classify(window_t &w) {
    const auto &t = thresholds_;
    const auto interval = interval_ms();
    const auto &host = w.metrics[idx(metric_e::host_ms)];
    const auto &encode = w.metrics[idx(metric_e::encode_ms)];
    const auto &capture = w.metrics[idx(metric_e::capture_ms)];
    const auto &backlog = w.metrics[idx(metric_e::backlog_ms)];
    std::vector<anomaly_t> found;
    const auto add = [&](std::string_view c, bool severe, double value, double threshold, std::uint64_t count = 0) {
      anomaly_t a;
      a.code = std::string { c };
      a.severe = severe;
      a.value = round2(value);
      a.threshold = round2(threshold);
      a.count = count;
      found.push_back(std::move(a));
    };

    if (w.counters[idx(counter_e::queue_flushes)] > 0) add(code::queue_flush, true, static_cast<double>(w.counters[idx(counter_e::queue_flushes)]), 0, w.counters[idx(counter_e::queue_frames_dropped)]);
    if (backlog.n > 0) {
      const auto warn = std::max(t.backlog_floor_ms, t.backlog_intervals * interval);
      if (backlog.max > t.backlog_severe_ms)
        add(code::send_backlog, true, backlog.max, t.backlog_severe_ms);
      else if (backlog.p95 > warn)
        add(code::send_backlog, false, backlog.p95, warn);
    }
    if (host.n > 0) {
      const auto warn = std::max(t.host_floor_ms, t.host_intervals * interval);
      if (host.max > t.host_severe_ms)
        add(code::host_latency, true, host.max, t.host_severe_ms);
      else if (host.p95 > warn)
        add(code::host_latency, false, host.p95, warn);
    }
    if (encode.n > 0) {
      const auto warn = std::max(t.encode_floor_ms, interval);
      if (encode.p95 > warn) add(code::encode_over, false, encode.p95, warn);
    }
    if (capture.n > 0) {
      if (capture.max > t.capture_severe_ms)
        add(code::capture_stall, true, capture.max, t.capture_severe_ms);
      else if (capture.max > t.capture_warn_ms)
        add(code::capture_stall, false, capture.max, t.capture_warn_ms);
    }
    const auto captured = w.counters[idx(counter_e::captured)];
    const auto submitted = w.counters[idx(counter_e::submitted)];
    if (captured > submitted) {
      const auto skipped = captured - submitted;
      if (skipped >= t.skip_min_frames && static_cast<double>(skipped) >= t.skip_share * static_cast<double>(captured)) add(code::frame_skip, false, 100.0 * static_cast<double>(skipped) / static_cast<double>(captured), t.skip_share * 100.0, skipped);
    }
    const auto requests = w.counters[idx(counter_e::idr_requests)] + w.counters[idx(counter_e::rfi_requests)];
    if (requests >= t.idr_per_window) add(code::idr_storm, false, static_cast<double>(requests), static_cast<double>(t.idr_per_window), requests);
    if (w.probe.loss_pct && *w.probe.loss_pct >= t.loss_pct) add(code::loss_burst, false, *w.probe.loss_pct, t.loss_pct);
    if (w.probe.rtt_ms && *w.probe.rtt_ms >= t.rtt_ms) add(code::rtt_high, false, *w.probe.rtt_ms, t.rtt_ms);
    // XP_BITRATE_SHORT is no longer raised: the configured bitrate is a ceiling. A hardware encoder in constant-bitrate mode without filler data stays far below it on static or low-motion content
    // (NVENC, AMF), and rate control may keep a window low on purpose; the measurement is taken at the host's own output, so it says nothing about the client's network either. The measured and the
    // configured bitrate are still reported in the stream summaries (bitrate_kbps_avg, the bitrate band) for whoever looks at a stream. The code stays in the list for older Workers and data.

    // cooldown: the same code on the same stream is reported once per cooldown; what was held back is counted into the next report
    for (auto &a : found) {
      auto it = std::find_if(codes_.begin(), codes_.end(), [&](const auto &p) { return p.first == a.code; });
      if (it == codes_.end()) it = codes_.insert(codes_.end(), { a.code, code_state_t {} });
      auto &state = it->second;
      const auto cooldown = a.severe ? t.severe_cooldown_ms : t.cooldown_ms;
      const auto last = a.severe ? state.last_severe_ms : state.last_ms;
      if (last >= 0 && w.end_ms - last < cooldown) {
        ++state.suppressed;
        continue;
      }
      state.last_ms = w.end_ms;
      if (a.severe) state.last_severe_ms = w.end_ms;
      a.suppressed = state.suppressed;
      state.suppressed = 0;
      w.anomalies.push_back(std::move(a));
    }
  }

  void
  stream_health_t::add_to_chunk(const window_t &w) {
    if (w.idle) {
      ++chunk_idle_;
      const auto blocked = idx(counter_e::send_would_block_packets);
      chunk_counters_[blocked] += w.counters[blocked];
      return;
    }
    ++chunk_windows_;
    chunk_seconds_ += w.seconds;
    chunk_anomalies_ += w.anomalies.size();
    const auto &keys = hist_keys();
    for (std::size_t k = 0; k < keys.size(); ++k) {
      const auto &s = w.metrics[idx(keys[k].metric)];
      if (s.n == 0) continue;
      const auto v = keys[k].stat == stat_e::p50 ? s.p50 : keys[k].stat == stat_e::p95 ? s.p95 :
                                                                                         s.max;
      ++hist_[k][bucket_of(v)];
      if (v > keys[k].bad_ms) ++bad_[k];
    }
    for (std::size_t c = 0; c < COUNTERS; ++c) chunk_counters_[c] += w.counters[c];
    if (w.probe.loss_pct) {
      loss_sum_ += *w.probe.loss_pct;
      loss_max_ = std::max(loss_max_, *w.probe.loss_pct);
      ++loss_n_;
    }
    if (w.probe.rtt_ms) rtt_.push_back(*w.probe.rtt_ms);
    if (w.probe.fec_pct > 0) {
      fec_sum_ += w.probe.fec_pct;
      ++fec_n_;
    }
    if (w.probe.measured_kbps > 0) {
      kbps_sum_ += w.probe.measured_kbps;
      ++kbps_n_;
    }
    // a window without a target (the probe had none yet) must not pull the average down
    if (w.probe.target_kbps > 0) {
      target_sum_ += w.probe.target_kbps;
      ++target_n_;
    }
  }

  bool
  stream_health_t::chunk_empty() const {
    std::lock_guard window { window_mutex_ };
    return chunk_windows_ == 0 && chunk_idle_ == 0;
  }

  bool
  stream_health_t::chunk_due(std::int64_t now_ms) const {
    std::lock_guard window { window_mutex_ };
    const bool has_data = chunk_windows_ > 0 || chunk_counters_[idx(counter_e::send_would_block_packets)] > 0;
    return has_data && now_ms - chunk_start_ms_ >= CHUNK_MAX_MS;
  }

  nlohmann::json
  stream_health_t::take_chunk(std::int64_t now_ms, bool detail) {
    std::lock_guard window { window_mutex_ };
    using nlohmann::json;
    json props = json::object();
    props["windows"] = chunk_windows_;
    props["idle_windows"] = chunk_idle_;
    props["window_s"] = static_cast<std::int64_t>(std::llround(chunk_seconds_));
    const auto &keys = hist_keys();
    // Tier 0, every stream: how many windows were over the bound of each statistic (only the ones that were)
    json bad = json::object();
    for (std::size_t k = 0; k < keys.size(); ++k)
      if (bad_[k] > 0) bad[std::string { keys[k].key }] = bad_[k];
    props["bad"] = std::move(bad);
    // Tier 1, a share of the streams: the histograms
    if (detail) {
      json hist = json::object();
      for (std::size_t k = 0; k < keys.size(); ++k) hist[std::string { keys[k].key }] = hist_[k];
      props["h"] = std::move(hist);
    }
    props["frames"] = chunk_counters_[idx(counter_e::frames_sent)];
    props["captured"] = chunk_counters_[idx(counter_e::captured)];
    props["submitted"] = chunk_counters_[idx(counter_e::submitted)];
    props["queue_flushes"] = chunk_counters_[idx(counter_e::queue_flushes)];
    props["queue_frames_dropped"] = chunk_counters_[idx(counter_e::queue_frames_dropped)];
    props["send_would_block_packets"] = chunk_counters_[idx(counter_e::send_would_block_packets)];
    props["idr_requests"] = chunk_counters_[idx(counter_e::idr_requests)];
    props["rfi_requests"] = chunk_counters_[idx(counter_e::rfi_requests)];
    props["anomalies"] = chunk_anomalies_;
    if (loss_n_ > 0) {
      props["loss_pct_avg"] = round2(loss_sum_ / static_cast<double>(loss_n_));
      props["loss_pct_max"] = round2(loss_max_);
    }
    if (!rtt_.empty()) {
      std::sort(rtt_.begin(), rtt_.end());
      props["rtt_ms_p50"] = round2(rtt_[(rtt_.size() - 1) / 2]);
      props["rtt_ms_p95"] = round2(rtt_[std::min(rtt_.size() - 1, static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(rtt_.size()))) - 1)]);
    }
    if (fec_n_ > 0) props["fec_pct_avg"] = round2(fec_sum_ / static_cast<double>(fec_n_));
    if (kbps_n_ > 0) props["actual_kbps"] = static_cast<std::int64_t>(kbps_sum_ / static_cast<double>(kbps_n_));
    if (target_n_ > 0) props["target_kbps"] = static_cast<std::int64_t>(target_sum_ / static_cast<double>(target_n_));
    // dimensions: the same fixed words as stream_start
    props["codec"] = dims_.codec;
    props["width"] = dims_.width;
    props["height"] = dims_.height;
    props["fps"] = dims_.fps;
    props["hdr"] = dims_.hdr;
    props["transport"] = dims_.transport;
    props["client_kind"] = dims_.client_kind;
    props["client_os"] = dims_.client_os;
    if (target_n_ > 0) props["bitrate_band"] = bitrate_band(static_cast<int>(target_sum_ / static_cast<double>(target_n_)));
    if (const auto enc = encoder(); !enc.empty()) props["encoder"] = enc;
    if (!dims_.capture_backend.empty()) props["capture_backend"] = dims_.capture_backend;
    if (!dims_.gpu_driver.empty()) props["gpu_driver"] = dims_.gpu_driver;

    // reset
    for (auto &h : hist_) h.fill(0);
    std::fill(bad_.begin(), bad_.end(), 0);
    chunk_windows_ = chunk_idle_ = chunk_anomalies_ = 0;
    chunk_seconds_ = 0;
    chunk_counters_.fill(0);
    loss_sum_ = loss_max_ = fec_sum_ = kbps_sum_ = target_sum_ = 0;
    loss_n_ = fec_n_ = kbps_n_ = target_n_ = 0;
    rtt_.clear();
    chunk_start_ms_ = now_ms;
    return props;
  }

}  // namespace telemetry::health
