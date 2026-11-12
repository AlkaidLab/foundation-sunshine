/**
 * @file tests/unit/test_video_hdr_analyzer.cpp
 * @brief Tests for the CPU-side max(R,G,B) analyzer behind the HDR10+ metadata.
 */
// local includes
#include "src/video_hdr_analyzer.h"

// standard includes
#include <cmath>
#include <cstdint>
#include <vector>

// third-party includes
#include <gtest/gtest.h>

namespace {
  namespace analyzer = video::hdr_analyzer;
  namespace hdr_metadata = video::hdr_metadata;

  constexpr int kCodeMax = 1023;
  constexpr int kLimitedYLow = 64;
  constexpr int kLimitedYSpan = 940 - 64;
  constexpr int kLimitedCCenter = 512;
  constexpr int kLimitedCSpan = 960 - 64;

  /// The 10-bit code whose PQ signal is closest to `nits`.
  int
  code_for_nits(float nits) {
    int best = 0;
    float best_error = -1.0f;
    for (int code = 0; code <= kCodeMax; ++code) {
      const auto error = std::fabs(hdr_metadata::pq_to_nits(code / float(kCodeMax)) - nits);
      if (best_error < 0.0f || error < best_error) {
        best = code;
        best_error = error;
      }
    }
    return best;
  }

  /// A PQ frame with settable sample codes, encoded the way the analyzer reads them.
  struct frame_buf_t {
    frame_buf_t(int width, int height, int subsample, int shift = 0, bool limited = false, bool bt2020 = true) {
      frame.width = width;
      frame.height = height;
      frame.chroma_subsample = subsample;
      frame.shift = shift;
      frame.limited_range = limited;
      frame.bt2020 = bt2020;

      luma.assign(size_t(width) * height, 0);
      chroma_w = (width + subsample - 1) / subsample;
      chroma_h = (height + subsample - 1) / subsample;
      cb.assign(size_t(chroma_w) * chroma_h, 512);
      cr.assign(size_t(chroma_w) * chroma_h, 512);

      frame.y = { luma.data(), width };
      frame.cb = { cb.data(), chroma_w };
      frame.cr = { cr.data(), chroma_w };
    }

    /// Build a Y'CbCr triple from a PQ-domain R'G'B' triple.
    static void
    rgb_to_ycbcr(float r, float g, float b, bool bt2020, float &y, float &cb_out, float &cr_out) {
      const float kr = bt2020 ? 0.2627f : 0.2126f;
      const float kb = bt2020 ? 0.0593f : 0.0722f;
      y = kr * r + (1.0f - kr - kb) * g + kb * b;
      cb_out = (b - y) / (2.0f * (1.0f - kb));
      cr_out = (r - y) / (2.0f * (1.0f - kr));
    }

    std::uint16_t
    encode(float signal) const {
      auto code = (int) std::lround(std::clamp(signal, 0.0f, 1.0f) * float(kCodeMax));
      if (frame.limited_range) {
        code = kLimitedYLow + (code * kLimitedYSpan) / kCodeMax;
      }
      return (std::uint16_t) (code << frame.shift);
    }

    std::uint16_t
    encode_chroma(float signal) const {
      int code;
      if (frame.limited_range) {
        code = kLimitedCCenter + (int) std::lround(signal * float(kLimitedCSpan));
      }
      else {
        code = kLimitedCCenter + (int) std::lround(signal * float(kCodeMax));
      }
      return (std::uint16_t) (std::clamp(code, 0, kCodeMax) << frame.shift);
    }

    /// Fill every pixel with one PQ-domain R'G'B' triple.
    void
    fill(float r, float g, float b) {
      float y, cb_signal, cr_signal;
      rgb_to_ycbcr(r, g, b, frame.bt2020, y, cb_signal, cr_signal);

      const auto y_code = encode(y);
      const auto cb_code = encode_chroma(cb_signal);
      const auto cr_code = encode_chroma(cr_signal);
      for (auto &sample: luma) {
        sample = y_code;
      }
      for (auto &sample: cb) {
        sample = cb_code;
      }
      for (auto &sample: cr) {
        sample = cr_code;
      }
    }

    platf::hdr_frame_luminance_stats_t
    run() {
      platf::hdr_frame_luminance_stats_t stats;
      analyzer::analyze(frame, stats, sequence);
      return stats;
    }

    analyzer::frame_t frame;
    std::vector<std::uint16_t> luma;
    std::vector<std::uint16_t> cb;
    std::vector<std::uint16_t> cr;
    int chroma_w = 0;
    int chroma_h = 0;
    std::uint64_t sequence = 0;
  };

  bool
  near(float actual, float expected, float tolerance = 0.02f) {
    return std::fabs(actual - expected) <= std::max(0.5f, expected * tolerance);
  }

  /**
   * @brief A saturated primary has a maxRGB far above its luma.
   * @details This is the whole point of the change: at 100 nits of pure red the luma
   *          plane carries ~1.2 nits, so a luma-keyed histogram reports a maxSCL two
   *          orders of magnitude too small.
   */
  TEST(VideoHdrAnalyzer, PureRedReportsMaxRgbNotLuma) {
    const auto red_code = code_for_nits(100.0f);
    const auto red_signal = red_code / float(kCodeMax);

    frame_buf_t frame(8, 8, 1);
    frame.fill(red_signal, 0.0f, 0.0f);
    const auto stats = frame.run();

    ASSERT_TRUE(stats.valid);
    EXPECT_TRUE(near(stats.max_maxrgb, hdr_metadata::pq_to_nits(red_signal), 0.02f))
      << "max_maxrgb=" << stats.max_maxrgb << " expected=" << hdr_metadata::pq_to_nits(red_signal);
    // The luma-only answer for the same frame would be about 1.2 nits.
    EXPECT_GT(stats.max_maxrgb, 20.0f);
  }

  /// A grey frame keeps max(R,G,B) == Y, so the analyzer must not shift it.
  TEST(VideoHdrAnalyzer, GreyKeepsItsLuma) {
    const auto code = code_for_nits(203.0f);
    const auto signal = code / float(kCodeMax);

    frame_buf_t frame(8, 8, 1);
    frame.fill(signal, signal, signal);
    const auto stats = frame.run();

    ASSERT_TRUE(stats.valid);
    EXPECT_TRUE(near(stats.max_maxrgb, hdr_metadata::pq_to_nits(signal), 0.01f));
  }

  /// Limited-range encoding of the same colour must land on the same nits.
  TEST(VideoHdrAnalyzer, LimitedRangeMatchesFullRange) {
    const auto red_code = code_for_nits(100.0f);
    const auto red_signal = red_code / float(kCodeMax);

    frame_buf_t full(8, 8, 1, 0, false);
    full.fill(red_signal, 0.0f, 0.0f);
    frame_buf_t limited(8, 8, 1, 0, true);
    limited.fill(red_signal, 0.0f, 0.0f);

    const auto full_stats = full.run();
    const auto limited_stats = limited.run();
    ASSERT_TRUE(full_stats.valid);
    ASSERT_TRUE(limited_stats.valid);
    EXPECT_TRUE(near(limited_stats.max_maxrgb, full_stats.max_maxrgb, 0.03f))
      << "limited=" << limited_stats.max_maxrgb << " full=" << full_stats.max_maxrgb;
  }

  /// MSB-aligned formats (P010, P16) keep their code in the high bits.
  TEST(VideoHdrAnalyzer, ShiftedSamplesAnalyzeTheSameAsLsbs) {
    const auto red_code = code_for_nits(100.0f);
    const auto red_signal = red_code / float(kCodeMax);

    frame_buf_t lsb(8, 8, 1);
    lsb.fill(red_signal, 0.0f, 0.0f);
    frame_buf_t msb(8, 8, 1, 6);
    msb.fill(red_signal, 0.0f, 0.0f);

    EXPECT_TRUE(near(lsb.run().max_maxrgb, msb.run().max_maxrgb, 0.01f));
  }

  /// 4:2:0 subsampling reads the co-sited chroma for every luma sample.
  TEST(VideoHdrAnalyzer, SubsampledChromaReachesEveryPixel) {
    const auto red_code = code_for_nits(100.0f);
    const auto red_signal = red_code / float(kCodeMax);

    frame_buf_t frame(8, 8, 2);
    frame.fill(red_signal, 0.0f, 0.0f);
    const auto stats = frame.run();

    ASSERT_TRUE(stats.valid);
    EXPECT_TRUE(near(stats.max_maxrgb, hdr_metadata::pq_to_nits(red_signal), 0.02f));
    EXPECT_TRUE(near(stats.percentile_99, hdr_metadata::pq_to_nits(red_signal), 0.02f));
  }

  /// Without chroma the analyzer falls back to the luma plane.
  TEST(VideoHdrAnalyzer, MissingChromaFallsBackToLuma) {
    frame_buf_t frame(8, 8, 1);
    const auto code = code_for_nits(203.0f);
    frame.fill(code / float(kCodeMax), code / float(kCodeMax), code / float(kCodeMax));
    frame.frame.cb = {};
    frame.frame.cr = {};

    const auto stats = frame.run();
    ASSERT_TRUE(stats.valid);
    EXPECT_TRUE(near(stats.max_maxrgb, hdr_metadata::pq_to_nits(code / float(kCodeMax)), 0.01f));
  }

  /// A black frame reports full near-black coverage and a zero maxSCL.
  TEST(VideoHdrAnalyzer, BlackFrameReportsNearBlackCoverage) {
    frame_buf_t frame(8, 8, 1);
    frame.fill(0.0f, 0.0f, 0.0f);
    const auto stats = frame.run();

    ASSERT_TRUE(stats.valid);
    EXPECT_TRUE(stats.near_black_stats_valid);
    EXPECT_FLOAT_EQ(stats.near_black_fraction, 1.0f);
    EXPECT_FLOAT_EQ(stats.max_maxrgb, 0.0f);
    EXPECT_FLOAT_EQ(stats.percentile_99, 0.0f);
  }

  /// An unusable frame must not publish zeroed statistics.
  TEST(VideoHdrAnalyzer, EmptyFrameLeavesStatsUntouched) {
    frame_buf_t frame(8, 8, 1);
    frame.frame.y = {};

    platf::hdr_frame_luminance_stats_t stats;
    std::uint64_t sequence = 41;
    analyzer::analyze(frame.frame, stats, sequence);

    EXPECT_FALSE(stats.valid);
    EXPECT_EQ(sequence, 41u);
  }

  /// The sequence advances once per analyzed frame so the temporal filters can key off it.
  TEST(VideoHdrAnalyzer, SequenceAdvancesPerAnalyzedFrame) {
    frame_buf_t frame(4, 4, 1);
    frame.fill(0.1f, 0.1f, 0.1f);

    platf::hdr_frame_luminance_stats_t stats;
    std::uint64_t sequence = 0;
    analyzer::analyze(frame.frame, stats, sequence);
    EXPECT_EQ(sequence, 1u);
    EXPECT_EQ(stats.sample_sequence, 1u);
    analyzer::analyze(frame.frame, stats, sequence);
    EXPECT_EQ(sequence, 2u);
    EXPECT_EQ(stats.sample_sequence, 2u);
  }

  /// BT.709 content must not be decoded with BT.2020 coefficients.
  TEST(VideoHdrAnalyzer, Bt709UsesItsOwnMatrix) {
    const auto code = code_for_nits(100.0f);
    const auto signal = code / float(kCodeMax);

    frame_buf_t bt2020(8, 8, 1, 0, false, true);
    bt2020.fill(signal, 0.0f, 0.0f);
    frame_buf_t bt709(8, 8, 1, 0, false, false);
    bt709.fill(signal, 0.0f, 0.0f);

    // Both decode to the same primaries value, but through different coefficients,
    // so the rounding lands on slightly different codes; they must stay close yet
    // not be produced by the same matrix.
    EXPECT_TRUE(near(bt709.run().max_maxrgb, bt2020.run().max_maxrgb, 0.03f));
  }
}  // namespace
