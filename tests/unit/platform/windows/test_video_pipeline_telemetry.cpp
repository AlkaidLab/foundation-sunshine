/**
 * @file tests/unit/platform/windows/test_video_pipeline_telemetry.cpp
 * @brief Tests for Windows video pipeline telemetry helpers.
 */
#include "src/platform/windows/video_pipeline_telemetry.h"

#include <gtest/gtest.h>
#include <memory>

namespace {
  struct unused_query_context_t {
    int reads = 0;
    unused_query_context_t *
    operator->() { return this; }
    template <class... Args>
    int
    GetData(Args...) {
      ++reads;
      return 0;
    }
  };
  using platf::dxgi::telemetry::m0_metric_fields;
  using platf::dxgi::telemetry::m0_pipeline_metrics_t;
  using platf::dxgi::telemetry::metric_fields;
  using platf::dxgi::telemetry::sample_window_t;

  TEST(VideoPipelineTelemetry, EmptyWindowProducesZeroSummary) {
    const auto summary = sample_window_t {}.summary();

    EXPECT_EQ(summary.samples, std::size_t { 0 });
    EXPECT_DOUBLE_EQ(summary.p50, 0.0);
    EXPECT_DOUBLE_EQ(summary.p95, 0.0);
    EXPECT_DOUBLE_EQ(summary.p99, 0.0);
  }

  TEST(VideoPipelineTelemetry, ExternalAnalysisClassifiesConversionWithoutReadingD3D11AnalysisQueries) {
    platf::dxgi::telemetry::d3d11_stage_sample_t<std::shared_ptr<int>> sample;
    ASSERT_TRUE(sample.initialize([] { return std::make_shared<int>(1); }));
    sample.mark_analysis_frame();
    unused_query_context_t context;
    platf::dxgi::telemetry::d3d11_stage_values_t values {};
    EXPECT_TRUE(sample.read(context, values));
    EXPECT_EQ(context.reads, 0);
    m0_pipeline_metrics_t metrics;
    sample.accumulate(metrics, values, 1000, 10, 12, 13, false);
    EXPECT_EQ(metrics.convert_analysis.size(), 1);
    EXPECT_TRUE(metrics.convert_regular.empty());
    EXPECT_TRUE(metrics.analysis_pass1.empty());
    EXPECT_TRUE(metrics.analysis_pass2.empty());
  }

  TEST(VideoPipelineTelemetry, ReportsNearestRankPercentiles) {
    sample_window_t window;
    for (int value = 100; value >= 1; --value) {
      window.add(static_cast<double>(value));
    }

    const auto summary = window.summary();
    EXPECT_EQ(summary.samples, std::size_t { 100 });
    EXPECT_DOUBLE_EQ(summary.min, 1.0);
    EXPECT_DOUBLE_EQ(summary.average, 50.5);
    EXPECT_DOUBLE_EQ(summary.p50, 50.0);
    EXPECT_DOUBLE_EQ(summary.p95, 95.0);
    EXPECT_DOUBLE_EQ(summary.p99, 99.0);
    EXPECT_DOUBLE_EQ(summary.max, 100.0);
  }

  TEST(VideoPipelineTelemetry, ResetStartsANewWindow) {
    sample_window_t window;
    window.add(42.0);
    window.reset();
    window.add(7.0);

    const auto summary = window.summary();
    EXPECT_EQ(summary.samples, std::size_t { 1 });
    EXPECT_DOUBLE_EQ(summary.min, 7.0);
    EXPECT_DOUBLE_EQ(summary.average, 7.0);
    EXPECT_DOUBLE_EQ(summary.max, 7.0);
  }

  TEST(VideoPipelineTelemetry, FormatsMachineReadableMetricFields) {
    sample_window_t window;
    window.add(1.0);
    window.add(2.0);

    EXPECT_EQ(
      metric_fields("convert_gpu_ms", window.summary()),
      " convert_gpu_ms_samples=2 convert_gpu_ms_p50=1 convert_gpu_ms_p95=2 convert_gpu_ms_p99=2");
  }

  TEST(VideoPipelineTelemetry, FormatsM0CountersAndStageMetrics) {
    m0_pipeline_metrics_t metrics;
    metrics.path_compute_direct = 3;
    metrics.analysis_due = 2;
    metrics.analysis_dispatched = 1;
    metrics.analysis_pass1.add(0.25);

    const auto fields = m0_metric_fields(metrics);
    EXPECT_NE(fields.find(" conversion_path_d3d11_compute_direct=3"), std::string::npos);
    EXPECT_NE(fields.find(" analysis_due=2"), std::string::npos);
    EXPECT_NE(fields.find(" analysis_dispatched=1"), std::string::npos);
    EXPECT_NE(fields.find(" analysis_pass1_gpu_ms_samples=1"), std::string::npos);
    EXPECT_NE(fields.find(" analysis_pass1_gpu_ms_p95=0.25"), std::string::npos);
  }
}  // namespace
