/**
 * @file src/telemetry/service_lifecycle.cpp
 */
#include "service_lifecycle.h"

#include <algorithm>
#include <cmath>

#include "install_journal.h"

namespace telemetry::service_lifecycle {

  using nlohmann::json;

  namespace {
    std::optional<long long>
    integer_of(const json &j, const char *key, long long lo, long long hi) {
      const auto it = j.find(key);
      if (it == j.end() || !it->is_number_integer()) return std::nullopt;
      const auto v = it->get<long long>();
      if (v < lo || v > hi) return std::nullopt;
      return v;
    }

    bool
    word_ok(const std::string &w, std::initializer_list<const char *> allowed) {
      return std::any_of(allowed.begin(), allowed.end(), [&](const char *a) { return w == a; });
    }

    template <class T>
    void
    keep_last(std::vector<T> &v, std::size_t n) {
      if (v.size() > n) v.erase(v.begin(), v.end() - static_cast<std::ptrdiff_t>(n));
    }

    json
    ints(const std::vector<std::int64_t> &v) {
      json a = json::array();
      for (const auto x : v) a.push_back(x);
      return a;
    }
    json
    nums(const std::vector<double> &v) {
      json a = json::array();
      for (const auto x : v) a.push_back(std::round(x));
      return a;
    }
    template <class T>
    void
    read_array(const json &j, const char *key, std::vector<T> &out, std::size_t max, double lo, double hi) {
      const auto it = j.find(key);
      if (it == j.end() || !it->is_array()) return;
      for (const auto &x : *it) {
        if (!x.is_number() || out.size() >= max) continue;
        const double v = x.get<double>();
        if (std::isfinite(v) && v >= lo && v <= hi) out.push_back(static_cast<T>(v));
      }
    }
  }  // namespace

  std::vector<record_t>
  parse(std::string_view text) {
    std::vector<record_t> out;
    if (text.size() > MAX_BYTES) text = text.substr(text.size() - MAX_BYTES);
    std::size_t pos = 0, lines = 0;
    constexpr long long TIME_MAX = 4'102'444'800'000LL;  // year 2100 in ms
    constexpr long long SPAN_MAX = 31LL * 24 * 3600 * 1000;
    while (pos < text.size() && lines < MAX_LINES) {
      auto end = text.find('\n', pos);
      if (end == std::string_view::npos) end = text.size();
      auto line = text.substr(pos, end - pos);
      pos = end + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
      if (line.empty()) continue;
      ++lines;
      const json j = json::parse(line.begin(), line.end(), nullptr, false);
      if (!j.is_object() || j.value("v", 0) != 1 || !j.contains("kind") || !j["kind"].is_string()) continue;
      record_t r;
      r.kind = j["kind"].get<std::string>();
      const auto t = integer_of(j, "t", 1, TIME_MAX);
      if (!t) continue;
      r.t = *t;
      if (r.kind == "svc_start") {
      }
      else if (r.kind == "core_start") {
        const auto how = j.value("how", std::string {});
        if (!word_ok(how, { "launched", "adopted" })) continue;
        r.adopted = how == "adopted";
        r.pid = integer_of(j, "pid", 0, 4294967295LL).value_or(0);
        r.age_ms = integer_of(j, "age_ms", 0, SPAN_MAX * 12).value_or(0);
        r.cause = j.value("cause", std::string {});
        if (!word_ok(r.cause, { "boot", "core_exit", "session_change", "start_failed" })) r.cause = "boot";
      }
      else if (r.kind == "extra_ended") {
        r.n = static_cast<int>(integer_of(j, "n", 0, 64).value_or(0));
      }
      else if (r.kind == "core_exit") {
        r.exit_code = integer_of(j, "exit", -2147483648LL, 4294967295LL).value_or(0);
        r.life_ms = integer_of(j, "life_ms", -1, SPAN_MAX * 12).value_or(-1);
        r.since_adopt_ms = integer_of(j, "since_adopt_ms", -1, SPAN_MAX * 12).value_or(-1);
        r.requested = j.value("requested", false);
        r.restart = j.value("restart", false);
        {
          const auto word = j.value("reason", std::string {});
          static const char *const known[] = { "settings", "guide", "troubleshooting", "doctor", "home", "shell", "tray", "widget", "import", "api" };
          r.reason = std::any_of(std::begin(known), std::end(known), [&](const char *k) { return word == k; }) ? word : std::string { "api" };
        }
      }
      else {
        continue;
      }
      out.push_back(std::move(r));
    }
    return out;
  }

  summary_t
  summarize(const std::vector<record_t> &records) {
    summary_t s;
    for (const auto &r : records) {
      if (r.kind == "svc_start") ++s.svc_starts;
      else if (r.kind == "extra_ended") s.extras_ended += std::max(1, r.n);
      else if (r.kind == "core_start") {
        if (r.adopted) ++s.adopted;
        else ++s.launched;
        if (r.cause == "core_exit") ++s.restarts_core_exit;
        else if (r.cause == "session_change") ++s.restarts_session;
      }
      else if (r.kind == "core_exit" && !r.requested && r.restart && r.exit_code == 0) {
        ++s.restarts_requested;
        s.restart_reason = r.reason.empty() ? std::string { "api" } : r.reason;
      }
      else if (r.kind == "core_exit" && !r.requested) {
        ++s.exits;
        const bool soon = r.since_adopt_ms >= 0 && r.since_adopt_ms <= SOON_AFTER_ADOPT_MS;
        const bool shortlife = r.life_ms >= 0 && r.life_ms < SHORT_LIFE_MS;
        if (soon) ++s.soon_after_adopt;
        if (shortlife) ++s.short_life;
        // an exit with code 0 after a long life is how a person restarts the host from its own interface: counted, not a problem
        if (r.exit_code != 0 || soon || shortlife) {
          ++s.unexpected;
          const long long ms = soon ? r.since_adopt_ms : (r.life_ms >= 0 ? r.life_ms : r.since_adopt_ms);
          if (s.worst_ms < 0 || (ms >= 0 && ms < s.worst_ms)) {
            s.worst_ms = ms;
            s.worst_exit_code = r.exit_code;
          }
        }
      }
    }
    return s;
  }

  json
  state_to_json(const state_t &s) {
    return { { "v", 1 }, { "restart", ints(s.restart_anomalies_ms) }, { "slow", ints(s.slow_ms) }, { "svc_core", nums(s.svc_core_ms) }, { "page", nums(s.page_ms) }, { "core", nums(s.core_ms) } };
  }

  state_t
  state_from_json(const json &j) {
    state_t s;
    if (!j.is_object() || j.value("v", 0) != 1) return s;
    read_array(j, "restart", s.restart_anomalies_ms, 16, 1, 4.2e12);
    read_array(j, "slow", s.slow_ms, 16, 1, 4.2e12);
    read_array(j, "svc_core", s.svc_core_ms, BASELINE_KEEP, 0, 3.6e7);
    read_array(j, "page", s.page_ms, BASELINE_KEEP, 0, 3.6e7);
    read_array(j, "core", s.core_ms, BASELINE_KEEP, 0, 3.6e7);
    return s;
  }

  std::optional<double>
  usual_ms(const std::vector<double> &values) {
    if (values.size() < BASELINE_MIN_SAMPLES) return std::nullopt;
    auto v = values;
    std::sort(v.begin(), v.end());
    const auto n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
  }

  namespace {
    std::size_t
    recent(const std::vector<std::int64_t> &times, std::int64_t now_ms, std::int64_t window) {
      return static_cast<std::size_t>(std::count_if(times.begin(), times.end(), [&](std::int64_t t) { return now_ms - t <= window && now_ms >= t; }));
    }
  }  // namespace

  std::optional<anomaly_t>
  judge_restart(const summary_t &summary, state_t &state, std::int64_t now_ms) {
    if (summary.unexpected <= 0) return std::nullopt;
    anomaly_t a;
    a.code = "XP_SVC_CORE_RESTART";
    a.count = summary.unexpected;
    const bool repeat = summary.unexpected >= 2 || recent(state.restart_anomalies_ms, now_ms, RESTART_REPEAT_WINDOW_MS) > 0;
    const bool soon = summary.soon_after_adopt > 0;
    a.severe = repeat || soon;
    a.cause = soon ? "soon_after_adopt" : repeat ? "repeat" : "unexpected_exit";
    a.value = static_cast<double>(std::max<long long>(summary.worst_ms, 0));
    a.threshold = static_cast<double>(soon ? SOON_AFTER_ADOPT_MS : SHORT_LIFE_MS);
    a.exit_code = install_journal::exit_code_text(summary.worst_exit_code);
    state.restart_anomalies_ms.push_back(now_ms);
    keep_last(state.restart_anomalies_ms, 16);
    return a;
  }

  std::optional<anomaly_t>
  judge_ready(std::string_view which, std::int64_t ms, state_t &state, std::int64_t now_ms) {
    auto &usual = which == "page" ? state.page_ms : which == "core" ? state.core_ms : state.svc_core_ms;
    const auto median = usual_ms(usual);
    usual.push_back(static_cast<double>(ms));
    keep_last(usual, BASELINE_KEEP);
    const bool above_limit = ms > SLOW_READY_MS;
    const bool above_baseline = median && static_cast<double>(ms) >= SLOW_READY_FACTOR * *median && static_cast<double>(ms) - *median >= static_cast<double>(SLOW_READY_MIN_EXTRA_MS);
    if (!above_limit && !above_baseline) return std::nullopt;
    anomaly_t a;
    a.code = "XP_SLOW_READY";
    a.which = which == "page" ? "page" : which == "core" ? "core" : "svc_core";
    a.value = static_cast<double>(ms);
    a.threshold = above_limit ? static_cast<double>(SLOW_READY_MS) : *median * SLOW_READY_FACTOR;
    a.cause = above_limit ? "above_limit" : "above_baseline";
    a.severe = recent(state.slow_ms, now_ms, SLOW_REPEAT_WINDOW_MS) > 0;
    state.slow_ms.push_back(now_ms);
    keep_last(state.slow_ms, 16);
    return a;
  }

  std::optional<std::int64_t>
  svc_to_ready_ms(const std::vector<record_t> &records, long long own_pid, std::int64_t ready_wall_ms) {
    if (own_pid <= 0) return std::nullopt;
    std::int64_t svc_start = 0;
    std::optional<std::int64_t> found;
    for (const auto &r : records) {
      if (r.kind == "svc_start") svc_start = r.t;
      else if (r.kind == "core_start" && !r.adopted && r.pid == own_pid && svc_start > 0 && r.t >= svc_start) found = svc_start;
    }
    if (!found || ready_wall_ms < *found) return std::nullopt;
    const auto ms = ready_wall_ms - *found;
    // a core that took over an hour to be ready is not a start-up we can describe
    if (ms > 3'600'000) return std::nullopt;
    return ms;
  }

  json
  summary_props(const summary_t &s) {
    json p = json::object();
    p["launched"] = s.launched;
    p["adopted"] = s.adopted;
    p["extras_ended"] = s.extras_ended;
    p["exits"] = s.exits;
    p["restarts_requested"] = s.restarts_requested;
    if (s.restarts_requested > 0 && !s.restart_reason.empty()) p["restart_reason"] = s.restart_reason;
    p["unexpected"] = s.unexpected;
    p["soon_after_adopt"] = s.soon_after_adopt;
    p["restarts_core_exit"] = s.restarts_core_exit;
    p["restarts_session"] = s.restarts_session;
    return p;
  }

  json
  timing_props(std::string_view which, std::int64_t ms, bool slow) {
    return { { "which", which == "page" ? "page" : which == "core" ? "core" : "svc_core" }, { "ms", std::clamp<std::int64_t>(ms, 0, 3'600'000) }, { "slow", slow } };
  }

  json
  anomaly_props(const anomaly_t &a) {
    json p = json::object();
    p["code"] = a.code;
    p["severe"] = a.severe;
    p["value"] = a.value;
    p["threshold"] = a.threshold;
    p["count"] = a.count;
    p["cause"] = a.cause;
    if (!a.which.empty()) p["which"] = a.which;
    if (!a.exit_code.empty()) p["exit_code"] = a.exit_code;
    return p;
  }

}  // namespace telemetry::service_lifecycle
