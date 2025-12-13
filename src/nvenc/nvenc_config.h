/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

namespace nvenc {

  enum class nvenc_two_pass {
    disabled,  ///< Single pass, the fastest and no extra vram
    quarter_resolution,  ///< Larger motion vectors being caught, faster and uses less extra vram
    full_resolution,  ///< Better overall statistics, slower and uses more extra vram
  };

  enum class nvenc_split_frame_encoding {
    disabled,  ///< Disable
    driver_decides,  ///< Let driver decide
    force_enabled,  ///< Force-enable
    two_strips,  ///< Force 2-strip split (requires 2+ NVENC engines)
    three_strips,  ///< Force 3-strip split (requires 3+ NVENC engines)
    four_strips,  ///< Force 4-strip split (requires 4+ NVENC engines)
  };

  enum class nvenc_lookahead_level {
    disabled = 0,  ///< Disable lookahead
    level_1 = 1,  ///< Lookahead level 1
    level_2 = 2,  ///< Lookahead level 2
    level_3 = 3,  ///< Lookahead level 3
    autoselect = 15,  ///< Let driver auto-select level
  };

  enum class nvenc_temporal_filter_level {
    disabled = 0,  ///< Disable temporal filter
    level_4 = 4,  ///< Temporal filter level 4
  };

  /**
   * @brief NVENC encoder configuration.
   */
  struct nvenc_config {
    // Quality preset from 1 to 7, higher is slower
    int quality_preset = 1;

    // Use optional preliminary pass for better motion vectors, bitrate distribution and stricter VBV(HRD), uses CUDA cores
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;

    // Percentage increase of VBV/HRD from the default single frame, allows low-latency variable bitrate
    int vbv_percentage_increase = 0;

    // Improves fades compression, uses CUDA cores
    bool weighted_prediction = false;

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    bool adaptive_quantization = false;

    // Enable temporal adaptive quantization (requires lookahead)
    bool enable_temporal_aq = false;

    // Don't use QP below certain value, limits peak image quality to save bitrate
    bool enable_min_qp = false;

    // Min QP value for H.264 when enable_min_qp is selected
    unsigned min_qp_h264 = 19;

    // Min QP value for HEVC when enable_min_qp is selected
    unsigned min_qp_hevc = 23;

    // Min QP value for AV1 when enable_min_qp is selected
    unsigned min_qp_av1 = 23;

    // Use CAVLC entropy coding in H.264 instead of CABAC, not relevant and here for historical reasons
    bool h264_cavlc = false;

    // Add filler data to encoded frames to stay at target bitrate, mainly for testing
    bool insert_filler_data = false;

    // Enable split-frame encoding if the gpu has multiple NVENC hardware clusters
    nvenc_split_frame_encoding split_frame_encoding = nvenc_split_frame_encoding::driver_decides;

    // Lookahead level (0-3, higher = better quality but more latency). Requires enable_lookahead=true
    nvenc_lookahead_level lookahead_level = nvenc_lookahead_level::disabled;

    // Lookahead depth (number of frames to look ahead, 0-32). 0 = disabled
    int lookahead_depth = 0;

    // Temporal filter level (reduces noise, improves compression). Requires frameIntervalP >= 5
    nvenc_temporal_filter_level temporal_filter_level = nvenc_temporal_filter_level::disabled;
  };

}  // namespace nvenc
