/**
 * @file src/platform/windows/video_pipeline_telemetry.h
 * @brief Small, dependency-free helpers for Windows video pipeline telemetry.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace platf::dxgi::telemetry {
  struct sample_summary_t {
    std::size_t samples = 0;
    double min = 0.0;
    double average = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
  };

  /**
   * A bounded-by-reset sample window used for periodic telemetry summaries.
   *
   * The video pipeline resets these windows after each log interval, so retaining
   * the raw values is inexpensive and lets us report tail latency instead of only
   * min/average/max. Percentiles use the nearest-rank definition.
   */
  class sample_window_t {
  public:
    void
    add(double value) {
      values_.push_back(value);
      total_ += value;
    }

    [[nodiscard]] bool
    empty() const {
      return values_.empty();
    }

    [[nodiscard]] std::size_t
    size() const {
      return values_.size();
    }

    [[nodiscard]] sample_summary_t
    summary() const {
      sample_summary_t result;
      if (values_.empty()) {
        return result;
      }

      auto sorted = values_;
      std::sort(sorted.begin(), sorted.end());

      result.samples = sorted.size();
      result.min = sorted.front();
      result.average = total_ / static_cast<double>(sorted.size());
      result.p50 = nearest_rank(sorted, 50);
      result.p95 = nearest_rank(sorted, 95);
      result.p99 = nearest_rank(sorted, 99);
      result.max = sorted.back();
      return result;
    }

    void
    reset() {
      values_.clear();
      total_ = 0.0;
    }

  private:
    static double
    nearest_rank(const std::vector<double> &sorted, std::size_t percentile) {
      const auto rank = (percentile * sorted.size() + 99) / 100;
      return sorted[std::max<std::size_t>(1, rank) - 1];
    }

    std::vector<double> values_;
    double total_ = 0.0;
  };

  struct m0_pipeline_metrics_t {
    sample_window_t convert_analysis;
    sample_window_t convert_regular;
    sample_window_t surface_copy_analysis;
    sample_window_t surface_copy_regular;
    sample_window_t capture_copy;
    sample_window_t analysis_pass1;
    sample_window_t analysis_pass2;
    sample_window_t analysis_readback_copy;
    std::size_t path_ps = 0;
    std::size_t path_compute_direct = 0;
    std::size_t path_compute_scratch = 0;
    std::size_t analysis_due = 0;
    std::size_t analysis_dispatched = 0;
    std::size_t analysis_readback_not_ready = 0;

    [[nodiscard]] bool
    empty() const {
      return path_ps == 0 &&
             path_compute_direct == 0 &&
             path_compute_scratch == 0 &&
             analysis_due == 0;
    }

    void
    record_conversion_path(bool compute, bool direct) {
      if (!compute) {
        ++path_ps;
      }
      else if (direct) {
        ++path_compute_direct;
      }
      else {
        ++path_compute_scratch;
      }
    }

    void
    reset() {
      convert_analysis.reset();
      convert_regular.reset();
      surface_copy_analysis.reset();
      surface_copy_regular.reset();
      capture_copy.reset();
      analysis_pass1.reset();
      analysis_pass2.reset();
      analysis_readback_copy.reset();
      path_ps = 0;
      path_compute_direct = 0;
      path_compute_scratch = 0;
      analysis_due = 0;
      analysis_dispatched = 0;
      analysis_readback_not_ready = 0;
    }
  };

  enum class d3d11_stage_point_t : std::size_t {
    capture_copy_start,
    capture_copy_end,
    analysis_start,
    analysis_pass1_end,
    analysis_pass2_end,
    analysis_readback_end,
    count,
  };

  using d3d11_stage_values_t =
    std::array<std::uint64_t, static_cast<std::size_t>(d3d11_stage_point_t::count)>;

  template <class Query>
  class d3d11_stage_sample_t {
  public:
    template <class Factory>
    bool
    initialize(Factory &&factory) {
      for (auto &query : queries_) {
        query = factory();
        if (!query) {
          return false;
        }
      }
      return true;
    }

    template <class Context>
    void
    begin_capture_copy(Context &context) {
      capture_copy_ = true;
      mark(context, d3d11_stage_point_t::capture_copy_start);
    }

    template <class Context>
    void
    end_capture_copy(Context &context) {
      mark(context, d3d11_stage_point_t::capture_copy_end);
    }

    template <class Context>
    void
    begin_analysis(Context &context) {
      analysis_dispatched_ = true;
      mark(context, d3d11_stage_point_t::analysis_start);
    }

    template <class Context>
    void
    end_analysis_pass1(Context &context) {
      mark(context, d3d11_stage_point_t::analysis_pass1_end);
    }

    template <class Context>
    void
    end_analysis_pass2(Context &context) {
      mark(context, d3d11_stage_point_t::analysis_pass2_end);
    }

    template <class Context>
    void
    end_analysis_readback(Context &context) {
      mark(context, d3d11_stage_point_t::analysis_readback_end);
    }

    template <class Context>
    [[nodiscard]] bool
    read(Context &context, d3d11_stage_values_t &values) {
      if (capture_copy_ &&
          (!read(context, d3d11_stage_point_t::capture_copy_start, values) ||
            !read(context, d3d11_stage_point_t::capture_copy_end, values))) {
        return false;
      }
      if (analysis_dispatched_ &&
          (!read(context, d3d11_stage_point_t::analysis_start, values) ||
            !read(context, d3d11_stage_point_t::analysis_pass1_end, values) ||
            !read(context, d3d11_stage_point_t::analysis_pass2_end, values) ||
            !read(context, d3d11_stage_point_t::analysis_readback_end, values))) {
        return false;
      }
      return true;
    }

    void
    accumulate(
      m0_pipeline_metrics_t &metrics,
      const d3d11_stage_values_t &values,
      std::uint64_t frequency,
      std::uint64_t convert_start,
      std::uint64_t convert_end,
      std::uint64_t copy_end,
      bool surface_copy) const {
      auto &convert = analysis_dispatched_ ?
                        metrics.convert_analysis :
                        metrics.convert_regular;
      convert.add(delta_ms(convert_start, convert_end, frequency));
      if (surface_copy) {
        auto &copy = analysis_dispatched_ ?
                       metrics.surface_copy_analysis :
                       metrics.surface_copy_regular;
        copy.add(delta_ms(convert_end, copy_end, frequency));
      }
      if (capture_copy_) {
        metrics.capture_copy.add(delta_ms(
          value(values, d3d11_stage_point_t::capture_copy_start),
          value(values, d3d11_stage_point_t::capture_copy_end),
          frequency));
      }
      if (analysis_dispatched_) {
        const auto analysis_start = value(values, d3d11_stage_point_t::analysis_start);
        const auto pass1_end = value(values, d3d11_stage_point_t::analysis_pass1_end);
        const auto pass2_end = value(values, d3d11_stage_point_t::analysis_pass2_end);
        metrics.analysis_pass1.add(delta_ms(analysis_start, pass1_end, frequency));
        metrics.analysis_pass2.add(delta_ms(pass1_end, pass2_end, frequency));
        metrics.analysis_readback_copy.add(delta_ms(
          pass2_end,
          value(values, d3d11_stage_point_t::analysis_readback_end),
          frequency));
      }
    }

  private:
    static constexpr std::size_t
    index(d3d11_stage_point_t point) {
      return static_cast<std::size_t>(point);
    }

    static std::uint64_t
    value(const d3d11_stage_values_t &values, d3d11_stage_point_t point) {
      return values[index(point)];
    }

    static double
    delta_ms(std::uint64_t begin, std::uint64_t end, std::uint64_t frequency) {
      return static_cast<double>(end - begin) * 1000.0 /
             static_cast<double>(frequency);
    }

    template <class Context>
    void
    mark(Context &context, d3d11_stage_point_t point) {
      context->End(queries_[index(point)].get());
    }

    template <class Context>
    [[nodiscard]] bool
    read(
      Context &context,
      d3d11_stage_point_t point,
      d3d11_stage_values_t &values) {
      auto &result = values[index(point)];
      return context->GetData(
               queries_[index(point)].get(),
               &result,
               sizeof(result),
               0) == 0;
    }

    std::array<Query, static_cast<std::size_t>(d3d11_stage_point_t::count)> queries_;
    bool capture_copy_ = false;
    bool analysis_dispatched_ = false;
  };

  inline std::string
  metric_fields(std::string_view name, const sample_summary_t &summary) {
    std::ostringstream fields;
    fields << ' ' << name << "_samples=" << summary.samples
           << ' ' << name << "_p50=" << summary.p50
           << ' ' << name << "_p95=" << summary.p95
           << ' ' << name << "_p99=" << summary.p99;
    return fields.str();
  }

  inline std::string
  m0_metric_fields(const m0_pipeline_metrics_t &metrics) {
    std::ostringstream fields;
    fields << " conversion_path_ps=" << metrics.path_ps
           << " conversion_path_d3d11_compute_direct=" << metrics.path_compute_direct
           << " conversion_path_d3d11_compute_scratch=" << metrics.path_compute_scratch
           << " analysis_due=" << metrics.analysis_due
           << " analysis_dispatched=" << metrics.analysis_dispatched
           << " analysis_readback_not_ready=" << metrics.analysis_readback_not_ready
           << metric_fields("convert_analysis_gpu_ms", metrics.convert_analysis.summary())
           << metric_fields("convert_regular_gpu_ms", metrics.convert_regular.summary())
           << metric_fields("encoder_surface_copy_analysis_gpu_ms", metrics.surface_copy_analysis.summary())
           << metric_fields("encoder_surface_copy_regular_gpu_ms", metrics.surface_copy_regular.summary())
           << metric_fields("capture_or_bridge_copy_gpu_ms", metrics.capture_copy.summary())
           << metric_fields("analysis_pass1_gpu_ms", metrics.analysis_pass1.summary())
           << metric_fields("analysis_pass2_gpu_ms", metrics.analysis_pass2.summary())
           << metric_fields("analysis_readback_copy_gpu_ms", metrics.analysis_readback_copy.summary());
    return fields.str();
  }

  inline std::string
  m0_cpu_metric_fields(
    const sample_summary_t &acquire,
    const sample_summary_t &submit,
    const sample_summary_t &result_age,
    std::size_t analysis_skipped_busy) {
    std::ostringstream fields;
    fields << " samples=" << submit.samples
           << metric_fields("encoder_mutex_wait_cpu_ms", acquire)
           << metric_fields("video_submit_cpu_ms", submit)
           << metric_fields("analysis_result_age_frames", result_age)
           << " analysis_skipped_busy=" << analysis_skipped_busy;
    return fields.str();
  }
}  // namespace platf::dxgi::telemetry
