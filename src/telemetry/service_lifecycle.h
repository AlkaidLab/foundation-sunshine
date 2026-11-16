/**
 * @file src/telemetry/service_lifecycle.h
 * @brief What the service and the startup tell about the core: how it started, whether it ended without being asked, and how long it took to be ready.
 *
 * sunshinesvc writes a small file of facts (tools/sunshinesvc_lifecycle.h); the host reads it at its next start, reports the counts (`svc_lifecycle`), the start-up time (`startup_timing`) and, when something
 * is clearly wrong, one anomaly (`host_anomaly`):
 *   XP_SVC_CORE_RESTART  the core ended without the service asking for it and without asking for a restart itself, and was started again, especially soon after the service took over a core that was already running; repeated, it is priority 0
 *   XP_SLOW_READY        the time from the start of the service to the web interface of the core (or from the opening of a page to its ready mark) is above 15 s, or well above the install's own usual time
 * Pure code (std + nlohmann::json), so it is unit tested on its own; the text is read like the installer's journal: field by field, bounded, never trusted.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace telemetry::service_lifecycle {

  /// An exit this soon after the service took over a running core is the sequence of the 30-second bug: the adopted core was on its way out.
  inline constexpr std::int64_t SOON_AFTER_ADOPT_MS = 120'000;
  /// A core that ended by itself within a minute of its start.
  inline constexpr std::int64_t SHORT_LIFE_MS = 60'000;
  /// Time to ready above this is slow whatever the install is used to.
  inline constexpr std::int64_t SLOW_READY_MS = 15'000;
  /// Well above the install's own usual time: three times its median and at least five seconds more, after five readings.
  inline constexpr double SLOW_READY_FACTOR = 3.0;
  inline constexpr std::int64_t SLOW_READY_MIN_EXTRA_MS = 5'000;
  inline constexpr std::size_t BASELINE_MIN_SAMPLES = 5;
  inline constexpr std::size_t BASELINE_KEEP = 20;
  /// "Repeats": a second problem of the same kind within this long.
  inline constexpr std::int64_t RESTART_REPEAT_WINDOW_MS = 24LL * 3600 * 1000;
  inline constexpr std::int64_t SLOW_REPEAT_WINDOW_MS = 7LL * 24 * 3600 * 1000;
  inline constexpr std::size_t MAX_BYTES = 32 * 1024;
  inline constexpr std::size_t MAX_LINES = 300;

  struct record_t {
    std::string kind;  ///< svc_start | core_start | extra_ended | core_exit
    std::int64_t t = 0;
    bool adopted = false;
    long long pid = 0;
    long long age_ms = 0;
    std::string cause;  ///< boot | core_exit | session_change | start_failed
    long long exit_code = 0;
    long long life_ms = -1;
    long long since_adopt_ms = -1;
    int n = 0;
    bool requested = false;  ///< the service asked the core to end
    std::string reason;  ///< restart_reason.h word of a requested restart (empty when not said)
    bool restart = false;  ///< the core asked for its own restart (Panel, a saved setting): exit code 0, started again by the service
  };

  std::vector<record_t> parse(std::string_view text);

  struct summary_t {
    int svc_starts = 0;
    int launched = 0;
    int adopted = 0;
    int extras_ended = 0;
    int exits = 0;  ///< the core ended without the service asking and without asking for a restart itself
    std::string restart_reason;  ///< the word of the latest requested restart
    int restarts_requested = 0;  ///< the core asked for its own restart (Panel, a saved setting): expected, never an anomaly
    int unexpected = 0;  ///< of those: an exit code other than 0, a life under a minute, or soon after an adoption
    int soon_after_adopt = 0;
    int short_life = 0;
    int restarts_core_exit = 0;
    int restarts_session = 0;
    long long worst_ms = -1;  ///< the shortest time to the end among the unexpected exits (since the adoption when there was one, else the life)
    long long worst_exit_code = 0;
  };
  summary_t summarize(const std::vector<record_t> &records);

  struct anomaly_t {
    std::string code;  ///< XP_SVC_CORE_RESTART | XP_SLOW_READY
    bool severe = false;  ///< priority 0: it repeats, or it follows an adoption
    double value = 0;  ///< ms
    double threshold = 0;  ///< ms
    int count = 1;
    std::string cause;  ///< soon_after_adopt | repeat | unexpected_exit | above_limit | above_baseline
    std::string which;  ///< XP_SLOW_READY: svc_core | core | page
    std::string exit_code;  ///< XP_SVC_CORE_RESTART: the exit code as text, empty when not known
  };

  /// What the host remembers between starts: when the last problems were, and the usual start-up times. Small, numbers only.
  struct state_t {
    std::vector<std::int64_t> restart_anomalies_ms;
    std::vector<std::int64_t> slow_ms;
    std::vector<double> svc_core_ms;
    std::vector<double> page_ms;
    std::vector<double> core_ms;  ///< the start of the core to its web interface, every install
  };
  nlohmann::json state_to_json(const state_t &state);
  state_t state_from_json(const nlohmann::json &json);

  /// nullopt when the exits were deliberate. Records the problem in the state.
  std::optional<anomaly_t> judge_restart(const summary_t &summary, state_t &state, std::int64_t now_ms);

  /// `which` is "svc_core", "core" or "page". The time is added to the install's usual times afterwards. nullopt when it is not slow.
  std::optional<anomaly_t> judge_ready(std::string_view which, std::int64_t ms, state_t &state, std::int64_t now_ms);

  /// The median of the usual times, nullopt before BASELINE_MIN_SAMPLES readings.
  std::optional<double> usual_ms(const std::vector<double> &values);

  /// From the start of the service to `ready_wall_ms` for the core with `own_pid` that the service launched (not one it adopted, not one a person started). nullopt when the file does not say.
  std::optional<std::int64_t> svc_to_ready_ms(const std::vector<record_t> &records, long long own_pid, std::int64_t ready_wall_ms);

  /// The props of the three events.
  nlohmann::json summary_props(const summary_t &summary);
  nlohmann::json timing_props(std::string_view which, std::int64_t ms, bool slow);
  nlohmann::json anomaly_props(const anomaly_t &anomaly);

}  // namespace telemetry::service_lifecycle
