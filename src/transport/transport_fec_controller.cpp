#include "transport_fec_controller.h"

#include <algorithm>
#include <array>
#include <vector>

namespace transport {
  namespace {
    struct replay_t {
      bool enough = true;
      bool representable = true;
      bool target_met = true;
      std::size_t windows = 0, failures = 0;
      std::size_t worst_windows = 1, worst_failures = 0;
      std::array<std::pair<std::size_t, std::size_t>, 32> risks {};
      std::size_t risk_count = 0;
    };

    struct cached_geometry_t {
      // Ordered block (total, parity) pairs and block count. Encoded percent
      // is deliberately absent: equal actual layouts have equal raw risks.
      std::array<std::uint16_t, 9> key {};
      std::size_t windows = 0, failures = 0;
      bool occupied = false;
    };

    bool
    lower_risk(std::size_t failures, std::size_t windows, std::size_t other_failures, std::size_t other_windows) {
      return failures * other_windows < other_failures * windows;
    }
  }  // namespace

  fec_selection_t
  select_fec(const protection_trace_t &trace, const frame_policy_t &policy,
    const fec_selection_config_t &config, const fec_selection_context_t &context) {
    fec_selection_t result;
    const std::array<unsigned, 3> current { policy.fec_base, policy.fec_key, policy.fec_recovery };
    for (std::size_t i = 0; i < current.size(); ++i) result.classes[i].percentage = current[i];
    if (!trace.valid || trace.connection_epoch != policy.connection_epoch || trace.sampled_at_us < 0 ||
        trace.settled_until_us < 0 || trace.settled_until_us > trace.sampled_at_us || trace.last_feedback_us < 0 ||
        trace.last_feedback_us > trace.sampled_at_us || config.feedback_timeout_us <= 0 ||
        trace.sampled_at_us - trace.last_feedback_us > config.feedback_timeout_us ||
        trace.samples.size() > 4096 || config.minimum_windows < 128 || config.minimum_windows > 4096 ||
        config.minimum_parity > 255 || config.decrease_stability_us <= 0 ||
        context.clean_covered_us < 0 || context.since_last_change_us < 0 ||
        policy.basis != budget_basis_e::normalized ||
        std::any_of(current.begin(), current.end(), [](auto value) { return value > 100; }) || std::any_of(config.maximum_replay_failure_ppm.begin(), config.maximum_replay_failure_ppm.end(), [](auto value) { return value > 1000000; })) return result;
    const auto count = trace.samples.size();
    std::vector<std::size_t> unknown(count + 1), gaps(count + 1), erasures(count + 1);
    std::array<std::array<bool, 4093>, 3> frames {};
    std::array<std::array<bool, 1024>, 3> blocks {};
    for (std::size_t i = 0; i < count; ++i) {
      const auto &sample = trace.samples[i];
      const auto &packet = sample.sent;
      const auto &shape = packet.protection;
      if (!sample.commit_ordinal || packet.send_time_us < 0 || packet.send_time_us > trace.settled_until_us ||
          (i && (packet.extended_sequence <= trace.samples[i - 1].sent.extended_sequence ||
                  sample.commit_ordinal <= trace.samples[i - 1].commit_ordinal ||
                  packet.send_time_us < trace.samples[i - 1].sent.send_time_us)) ||
          sample.status > packet_status_e::unknown || shape.frame_class > protection_class_e::recovery ||
          packet.kind > packet_kind_e::probe || shape.data_shards > 1023 ||
          (shape.data_shards && (shape.total_shards < shape.data_shards || shape.total_shards > 1023 ||
                                  (shape.total_shards > shape.data_shards && shape.total_shards > 255) ||
                                  shape.shard_index >= shape.total_shards || shape.block_index >= 4 ||
                                  packet.kind != (shape.shard_index < shape.data_shards ? packet_kind_e::data : packet_kind_e::fec))) ||
          (!shape.data_shards && (shape.total_shards || shape.shard_index || shape.block_index ||
                                   shape.frame_class != protection_class_e::base || shape.frame_data_shards || shape.frame_blocks))) return result;
      if (shape.frame_data_shards || shape.frame_blocks) {
        if (!shape.frame_blocks || shape.frame_blocks > 4 || shape.frame_data_shards < shape.frame_blocks ||
            shape.frame_data_shards > 4092 || shape.block_index >= shape.frame_blocks ||
            shape.data_shards != shape.frame_data_shards / shape.frame_blocks +
                                   (shape.block_index < shape.frame_data_shards % shape.frame_blocks)) return result;
      }
      const bool known = shape.frame_data_shards && (packet.kind == packet_kind_e::data || packet.kind == packet_kind_e::fec) &&
                         (sample.status == packet_status_e::received || sample.status == packet_status_e::missing);
      unknown[i + 1] = unknown[i] + !known;
      gaps[i + 1] = gaps[i] + (i && sample.commit_ordinal - trace.samples[i - 1].commit_ordinal != 1);
      erasures[i + 1] = erasures[i] + (known && (sample.status == packet_status_e::missing || sample.late_correction));
      if (known) {
        const auto index = static_cast<std::size_t>(shape.frame_class);
        frames[index][shape.frame_data_shards] = true;
        blocks[index][shape.data_shards] = true;
      }
    }
    result.valid = true;
    // At most 3 classes * 32 shapes * (current + 9 candidates) = 960 distinct
    // geometries. The held/downstep layout is one of those. This fixed table
    // is private to the call; feedback, clocks, epochs and policy changes can
    // never reuse observations from a previous decision.
    constexpr std::size_t cache_capacity = 2048;
    std::vector<cached_geometry_t> cache;
    const auto replay_geometry = [&](const fec_frame_t &layout) {
      std::array<std::uint16_t, 9> key {};
      key.back() = static_cast<std::uint16_t>(layout.block_count);
      std::uint64_t hash = 14695981039346656037ULL;
      for (std::size_t b = 0; b < layout.block_count; ++b) {
        key[2 * b] = layout.blocks[b].total_shards();
        key[2 * b + 1] = layout.blocks[b].parity_shards;
      }
      for (const auto value : key) {
        hash ^= value;
        hash *= 1099511628211ULL;
      }
      if (cache.empty()) {
        cache.resize(cache_capacity);
        result.geometry_cache_bytes = cache.size() * sizeof(cached_geometry_t);
      }
      std::size_t slot = hash % cache_capacity;
      std::size_t examined = 0;
      while (examined < cache_capacity && cache[slot].occupied) {
        if (cache[slot].key == key) {
          ++result.geometry_cache_hits;
          return std::pair { cache[slot].failures, cache[slot].windows };
        }
        slot = (slot + 1) % cache_capacity;
        ++examined;
      }
      const auto length = layout.data_shards() + layout.parity_shards();
      std::size_t windows = 0, failures = 0;
      for (std::size_t begin = 0; begin + length <= count; ++begin) {
        const auto end = begin + length;
        if (unknown[end] != unknown[begin] || gaps[end] != gaps[begin + 1]) continue;
        ++windows;
        auto offset = begin;
        bool failed = false;
        for (std::size_t b = 0; b < layout.block_count; ++b) {
          const auto &block = layout.blocks[b];
          const auto block_end = offset + block.total_shards();
          failed |= erasures[block_end] - erasures[offset] > block.parity_shards;
          offset = block_end;
        }
        failures += failed;
      }
      ++result.geometries_evaluated;
      if (examined < cache_capacity) cache[slot] = { key, windows, failures, true };
      return std::pair { failures, windows };
    };
    constexpr std::array<unsigned, 9> candidates { 0, 5, 10, 15, 20, 25, 30, 40, 50 };
    for (std::size_t frame_class = 0; frame_class < 3; ++frame_class) {
      auto &selection = result.classes[frame_class];
      selection.block_shapes = std::count(blocks[frame_class].begin(), blocks[frame_class].end(), true);
      selection.frame_shapes = std::count(frames[frame_class].begin(), frames[frame_class].end(), true);
      if (!selection.frame_shapes || selection.frame_shapes > 32) continue;
      std::array<std::uint16_t, 32> data_sizes {};
      std::size_t shapes = 0;
      for (std::size_t data = 1; data < frames[frame_class].size(); ++data) {
        if (frames[frame_class][data]) data_sizes[shapes++] = static_cast<std::uint16_t>(data);
      }
      const auto replay = [&](unsigned percentage) {
        replay_t evaluation;
        for (std::size_t shape = 0; shape < shapes; ++shape) {
          const auto data = data_sizes[shape];
          const auto layout = plan_fec_frame(data, percentage, config.minimum_parity);
          if (!layout) {
            evaluation.representable = false;
            evaluation.target_met = false;
            evaluation.enough = false;
            return evaluation;
          }
          // An unprotected fallback is replayed as such, never as recovery.
          if (layout->fec_skipped) evaluation.representable = false;
          const auto [failures, windows] = replay_geometry(*layout);
          evaluation.risks[evaluation.risk_count++] = { failures, windows };
          evaluation.enough &= windows >= config.minimum_windows;
          evaluation.target_met &= windows && failures * 1000000ULL <= windows * config.maximum_replay_failure_ppm[frame_class];
          evaluation.windows += windows;
          evaluation.failures += failures;
          if (windows && lower_risk(evaluation.worst_failures, evaluation.worst_windows, failures, windows)) {
            evaluation.worst_failures = failures;
            evaluation.worst_windows = windows;
          }
        }
        evaluation.target_met &= evaluation.representable && evaluation.enough;
        return evaluation;
      };
      const auto budget_allows = [&](unsigned percentage) {
        auto budget = policy.budget;
        auto ratios = current;
        ratios[frame_class] = percentage;
        budget.fec_numerator = *std::max_element(ratios.begin(), ratios.end());
        budget.fec_denominator = 100;
        const auto allocation = allocate_budget(budget);
        return allocation && allocation->encoder_kbps > 0;
      };
      const auto baseline = replay(current[frame_class]);
      if (!baseline.enough) continue;
      unsigned proposed = current[frame_class];
      auto chosen = baseline;
      bool met_target = false;
      bool any_evidence = false;
      for (const auto percentage : candidates) {
        auto evaluation = replay(percentage);
        if (!evaluation.enough || !evaluation.representable || !budget_allows(percentage)) continue;
        any_evidence = true;
        if (evaluation.target_met) {
          proposed = percentage;
          chosen = std::move(evaluation);
          met_target = true;
          break;
        }
        // Best effort must improve the worst frame shape without regressing
        // any observed shape relative to the current actual policy. Ties keep
        // the lowest percentage because the candidates are ordered by cost.
        bool no_regression = evaluation.risk_count == baseline.risk_count;
        for (std::size_t i = 0; no_regression && i < evaluation.risk_count; ++i) {
          const auto [f, n] = evaluation.risks[i];
          const auto [old_f, old_n] = baseline.risks[i];
          no_regression &= !lower_risk(old_f, old_n, f, n);
        }
        if (percentage > current[frame_class] && no_regression &&
            lower_risk(evaluation.worst_failures, evaluation.worst_windows, chosen.worst_failures, chosen.worst_windows)) {
          proposed = percentage;
          chosen = std::move(evaluation);
        }
      }
      if (!any_evidence) {
        selection.result = fec_selection_result_e::infeasible;
        result.infeasible = true;
        continue;
      }
      if (met_target && proposed < current[frame_class]) {
        proposed = current[frame_class];
        if (erasures.back() == 0 && context.clean_covered_us >= config.decrease_stability_us &&
            context.since_last_change_us >= config.decrease_stability_us) {
          auto lower = std::lower_bound(candidates.begin(), candidates.end(), current[frame_class]);
          if (lower != candidates.begin()) --lower;
          proposed = *lower;
        }
        // Replay the actual downstep/held policy, not the lower ideal candidate.
        chosen = replay(proposed);
      }
      if (!chosen.enough || !chosen.representable || !budget_allows(proposed)) {
        selection.result = fec_selection_result_e::infeasible;
        result.infeasible = true;
        continue;
      }
      selection.percentage = proposed;
      selection.replay_windows = chosen.windows;
      selection.replay_failures = chosen.failures;
      selection.worst_frame_failure_ppm = static_cast<std::uint32_t>(
        (chosen.worst_failures * 1000000ULL + chosen.worst_windows - 1) / chosen.worst_windows);
      selection.target_met = chosen.target_met;
      if (!selection.target_met) {
        result.infeasible = true;
        selection.result = proposed != current[frame_class] ? fec_selection_result_e::best_effort : fec_selection_result_e::infeasible;
      }
      else
        selection.result = proposed != current[frame_class] ? fec_selection_result_e::changed : fec_selection_result_e::unchanged;
      result.changed |= proposed != current[frame_class];
    }
    return result;
  }
}  // namespace transport
