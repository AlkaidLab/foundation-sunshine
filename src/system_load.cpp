/**
 * @file src/system_load.cpp
 * @brief Pure helpers behind the system load route: CPU delta, GPU engine counters, JSON.
 */
#include "system_load.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace system_load {

  namespace {
    constexpr std::string_view luid_tag = "luid_0x";
    constexpr std::string_view phys_tag = "_phys_";
    constexpr std::string_view engtype_tag = "_engtype_";

    double
    clamp_percent(double value) {
      return std::clamp(value, 0.0, 100.0);
    }

    template <typename T>
    bool
    parse_number(std::string_view text, int base, T &out) {
      if (text.empty()) {
        return false;
      }
      auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out, base);
      return ec == std::errc {} && end == text.data() + text.size();
    }

    nlohmann::json
    optional_json(const std::optional<double> &value) {
      return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
    }
  }  // namespace

  std::optional<double>
  cpu_percent_from_times(const cpu_times_t &prev, const cpu_times_t &now) {
    if (now.idle < prev.idle || now.kernel < prev.kernel || now.user < prev.user) {
      return std::nullopt;
    }

    const std::uint64_t idle = now.idle - prev.idle;
    const std::uint64_t busy_and_idle = (now.kernel - prev.kernel) + (now.user - prev.user);
    if (busy_and_idle == 0) {
      return std::nullopt;
    }

    const double idle_share = static_cast<double>(idle) / static_cast<double>(busy_and_idle);
    return clamp_percent(100.0 * (1.0 - idle_share));
  }

  std::optional<gpu_engine_id_t>
  parse_gpu_engine_instance(std::string_view name) {
    // The luid part may open the name; every other part follows an underscore.
    const auto luid_pos = name.find(luid_tag);
    if (luid_pos != std::string_view::npos && luid_pos > 0 && name[luid_pos - 1] != '_') {
      return std::nullopt;
    }
    const auto phys_pos = name.find(phys_tag);
    const auto engtype_pos = name.find(engtype_tag);
    if (luid_pos == std::string_view::npos || phys_pos == std::string_view::npos || engtype_pos == std::string_view::npos) {
      return std::nullopt;
    }
    if (!(luid_pos < phys_pos && phys_pos < engtype_pos)) {
      return std::nullopt;
    }

    // luid_0x<high>_0x<low>
    const auto luid_text = name.substr(luid_pos + luid_tag.size(), phys_pos - (luid_pos + luid_tag.size()));
    const auto separator = luid_text.find("_0x");
    if (separator == std::string_view::npos) {
      return std::nullopt;
    }

    gpu_engine_id_t id {};
    if (!parse_number(luid_text.substr(0, separator), 16, id.luid_high) ||
        !parse_number(luid_text.substr(separator + 3), 16, id.luid_low)) {
      return std::nullopt;
    }

    // phys_<n> runs to the next "_eng_" or to the engtype tag when the engine index is absent.
    const auto phys_start = phys_pos + phys_tag.size();
    auto phys_end = name.find("_eng_", phys_start);
    if (phys_end == std::string_view::npos || phys_end > engtype_pos) {
      phys_end = engtype_pos;
    }
    if (!parse_number(name.substr(phys_start, phys_end - phys_start), 10, id.phys)) {
      return std::nullopt;
    }

    // eng_<n> sits between phys and engtype; the same engine type can exist several times (two video encoders are two engines).
    id.eng = 0;
    const auto eng_pos = name.find("_eng_", phys_start);
    if (eng_pos != std::string_view::npos && eng_pos < engtype_pos) {
      const auto eng_start = eng_pos + 5;
      if (!parse_number(name.substr(eng_start, engtype_pos - eng_start), 10, id.eng)) {
        return std::nullopt;
      }
    }

    const auto engtype = name.substr(engtype_pos + engtype_tag.size());
    if (engtype.empty()) {
      return std::nullopt;
    }
    id.engtype = std::string(engtype);
    return id;
  }

  std::optional<gpu_load_t>
  aggregate_gpu_adapters(const std::vector<gpu_engine_sample_t> &samples) {
    using adapter_key_t = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
    std::map<adapter_key_t, std::map<std::pair<std::uint32_t, std::string>, double>> sums;

    for (const auto &sample : samples) {
      if (!std::isfinite(sample.value)) {
        continue;
      }
      const auto id = parse_gpu_engine_instance(sample.name);
      if (!id) {
        continue;
      }
      sums[{ id->luid_high, id->luid_low, id->phys }][{ id->eng, id->engtype }] += std::max(sample.value, 0.0);
    }

    std::optional<gpu_load_t> best;
    for (const auto &[key, per_type] : sums) {
      double adapter_load = 0.0;
      for (const auto &[engine, sum] : per_type) {
        adapter_load = std::max(adapter_load, sum);
      }
      adapter_load = clamp_percent(adapter_load);
      if (!best || adapter_load > best->percent) {
        best = gpu_load_t { adapter_load, std::get<0>(key), std::get<1>(key) };
      }
    }
    return best;
  }

  std::optional<double>
  aggregate_gpu_engines(const std::vector<gpu_engine_sample_t> &samples) {
    if (const auto load = aggregate_gpu_adapters(samples)) {
      return load->percent;
    }
    return std::nullopt;
  }

  std::optional<double>
  memory_percent_from_bytes(std::uint64_t total, std::uint64_t available) {
    if (total == 0 || available > total) {
      return std::nullopt;
    }
    return clamp_percent(100.0 * static_cast<double>(total - available) / static_cast<double>(total));
  }

  nlohmann::json
  to_json(const snapshot_t &snapshot) {
    nlohmann::json out;
    out["supported"] = snapshot.supported;
    out["cpu_percent"] = optional_json(snapshot.cpu_percent);
    out["memory_total"] = snapshot.memory_total ? nlohmann::json(*snapshot.memory_total) : nlohmann::json(nullptr);
    out["memory_available"] = snapshot.memory_available ? nlohmann::json(*snapshot.memory_available) : nlohmann::json(nullptr);
    out["memory_percent"] = optional_json(snapshot.memory_percent);
    out["gpu_percent"] = optional_json(snapshot.gpu_percent);
    out["gpu_name"] = snapshot.gpu_name ? nlohmann::json(*snapshot.gpu_name) : nlohmann::json(nullptr);
    return out;
  }

}  // namespace system_load
