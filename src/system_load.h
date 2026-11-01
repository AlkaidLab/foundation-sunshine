/**
 * @file src/system_load.h
 * @brief Host load figures for the Web UI home tiles: CPU, memory and GPU utilisation.
 *
 * The functions in the first half are pure and portable. `sample()` is implemented per
 * platform (src/platform/windows/system_load_win.cpp, src/system_load_stub.cpp).
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace system_load {

  // Cumulative system times in 100 ns ticks as reported by GetSystemTimes.
  // `kernel` includes `idle`, as documented for that call.
  struct cpu_times_t {
    std::uint64_t idle;
    std::uint64_t kernel;
    std::uint64_t user;
  };

  /**
   * @brief Percentage of non-idle time between two samples, 0..100.
   * @return Nothing when the interval is empty or a counter went backwards.
   */
  std::optional<double>
  cpu_percent_from_times(const cpu_times_t &prev, const cpu_times_t &now);

  // Identity parsed from a "GPU Engine" counter instance name such as
  // `pid_1234_luid_0x00000000_0x0000A1B2_phys_0_eng_0_engtype_3D`.
  struct gpu_engine_id_t {
    std::uint32_t luid_high;
    std::uint32_t luid_low;
    std::uint32_t phys;
    std::uint32_t eng;  // engine index of the adapter; 0 when the name has none
    std::string engtype;

    bool
    operator==(const gpu_engine_id_t &) const = default;
  };

  /**
   * @brief Parse a "GPU Engine" instance name.
   * @return Nothing when the luid, phys or engtype part is missing or malformed.
   */
  std::optional<gpu_engine_id_t>
  parse_gpu_engine_instance(std::string_view name);

  struct gpu_engine_sample_t {
    std::string name;
    double value;
  };

  struct gpu_load_t {
    double percent;  // 0..100
    std::uint32_t luid_high;  // busiest adapter
    std::uint32_t luid_low;
  };

  /**
   * @brief Busiest adapter and its load, the way Task Manager reports it.
   *
   * Per adapter (luid and phys) the values are summed per engine type; the adapter's load is
   * the largest of those sums, clamped to 0..100. Names that do not parse and non-finite
   * values are ignored.
   * @return Nothing when no usable sample remains.
   */
  std::optional<gpu_load_t>
  aggregate_gpu_adapters(const std::vector<gpu_engine_sample_t> &samples);

  // Same as aggregate_gpu_adapters, percent only.
  std::optional<double>
  aggregate_gpu_engines(const std::vector<gpu_engine_sample_t> &samples);

  struct snapshot_t {
    bool supported = false;
    std::optional<double> cpu_percent;
    std::optional<std::uint64_t> memory_total;
    std::optional<std::uint64_t> memory_available;
    std::optional<double> memory_percent;
    std::optional<double> gpu_percent;
    std::optional<std::string> gpu_name;
  };

  // Memory in use as a percentage; nothing when the total is zero or the available figure exceeds it.
  std::optional<double>
  memory_percent_from_bytes(std::uint64_t total, std::uint64_t available);

  /**
   * @brief JSON body of `GET /api/runtime/system`; unknown values are null.
   */
  nlohmann::json
  to_json(const snapshot_t &snapshot);

  /**
   * @brief Take a snapshot of the current load. Never blocks for a sampling interval.
   *
   * On platforms without a backend the result has `supported == false`.
   */
  snapshot_t
  sample();

}  // namespace system_load
