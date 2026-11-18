/**
 * @file src/telemetry/self_baseline.cpp
 * @brief See self_baseline.h.
 */
#include "self_baseline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

namespace telemetry {

  namespace {
    struct tracked_t {
      const char *key;
      hooks::metric_e metric;
      health::stat_e stat;
      double min_delta;  ///< the current value must also be above the baseline by this much (ms)
    };
    const tracked_t TRACKED[] = {
      { "host_p50", hooks::metric_e::host_ms, health::stat_e::p50, 10 },
      { "host_p95", hooks::metric_e::host_ms, health::stat_e::p95, 20 },
      { "send_p95", hooks::metric_e::send_ms, health::stat_e::p95, 10 },
      { "backlog_p95", hooks::metric_e::backlog_ms, health::stat_e::p95, 15 },
      { "encode_p95", hooks::metric_e::encode_ms, health::stat_e::p95, 6 },
      { "jitter_p95", hooks::metric_e::jitter_ms, health::stat_e::p95, 1.0 },
    };
    constexpr double ALPHA = 0.02;  // the baseline moves slowly: a bad stretch does not become the normal
  }  // namespace

  self_baseline_t::self_baseline_t(std::filesystem::path file, std::string version):
      file_(std::move(file)), version_(std::move(version)) {
    for (const auto &t : TRACKED) metrics_.push_back({ t.key });
    if (file_.empty()) return;
    try {
      std::ifstream in(file_);
      if (!in) return;
      const auto doc = nlohmann::json::parse(in, nullptr, false);
      if (doc.is_discarded() || !doc.is_object() || doc.value("v", 0) != 1 || !doc.contains("m") || !doc["m"].is_object()) return;
      for (auto &m : metrics_) {
        const auto found = doc["m"].find(m.key);
        if (found == doc["m"].end() || !found->is_object()) continue;
        m.baseline = std::clamp(found->value("base", 0.0), 0.0, 100000.0);
        m.n = static_cast<std::uint64_t>(std::clamp(found->value("n", 0.0), 0.0, 1e12));
        m.baseline_version = found->value("bv", std::string {});
        m.reported_version = found->value("rv", std::string {});
      }
    }
    catch (...) {
    }
  }

  self_baseline_t::metric_state_t *
  self_baseline_t::find(std::string_view key) {
    for (auto &m : metrics_)
      if (m.key == key) return &m;
    return nullptr;
  }

  std::vector<baseline_event_t>
  self_baseline_t::observe(const health::window_t &window) {
    std::vector<baseline_event_t> out;
    if (window.idle) return out;
    const auto day = std::chrono::duration_cast<std::chrono::hours>(std::chrono::system_clock::now().time_since_epoch()).count() / 24;
    if (day != day_) {
      day_ = day;
      events_today_ = 0;
    }
    for (const auto &t : TRACKED) {
      auto *m = find(t.key);
      const auto &s = window.metrics[static_cast<std::size_t>(t.metric)];
      if (!m || s.n == 0) continue;
      const double v = t.stat == health::stat_e::p50 ? s.p50 : t.stat == health::stat_e::p95 ? s.p95 :
                                                                                               s.max;
      if (m->n < MIN_BASELINE_WINDOWS) {
        // still learning: the first windows of an install are the baseline
        m->baseline = m->n == 0 ? v : m->baseline + (v - m->baseline) / static_cast<double>(m->n + 1);
        ++m->n;
        m->baseline_version = version_;
        continue;
      }
      const bool worse = v >= RATIO * m->baseline && v - m->baseline >= t.min_delta;
      if (worse) {
        ++m->bad_run;
        m->run_sum += v;
        if (m->bad_run >= CONSECUTIVE_WINDOWS && m->reported_version != version_ && events_today_ < MAX_EVENTS_PER_DAY) {
          baseline_event_t e;
          e.metric = m->key;
          e.baseline = m->baseline;
          e.current = m->run_sum / m->bad_run;
          e.delta = e.current - e.baseline;
          e.windows = m->bad_run;
          e.baseline_windows = m->n;
          e.version_changed = m->baseline_version != version_;
          out.push_back(e);
          m->reported_version = version_;
          ++events_today_;
        }
        continue;  // a bad window never moves the baseline
      }
      m->bad_run = 0;
      m->run_sum = 0;
      m->baseline += ALPHA * (v - m->baseline);
      ++m->n;
      // a baseline that is now mostly from this version belongs to it
      if (m->baseline_version != version_ && m->n % 50 == 0) m->baseline_version = version_;
    }
    if (++windows_since_save_ >= 10 || !out.empty()) save();
    return out;
  }

  void
  self_baseline_t::save() const {
    if (file_.empty()) return;
    try {
      nlohmann::json m = nlohmann::json::object();
      for (const auto &s : metrics_) m[s.key] = { { "base", std::round(s.baseline * 1000.0) / 1000.0 }, { "n", s.n }, { "bv", s.baseline_version }, { "rv", s.reported_version } };
      std::error_code ec;
      std::filesystem::create_directories(file_.parent_path(), ec);
      const auto temp = file_.string() + ".tmp";
      {
        std::ofstream out(temp, std::ios::trunc);
        out << nlohmann::json { { "v", 1 }, { "m", m } }.dump();
      }
      std::filesystem::rename(temp, file_, ec);
    }
    catch (...) {
    }
  }

}  // namespace telemetry
