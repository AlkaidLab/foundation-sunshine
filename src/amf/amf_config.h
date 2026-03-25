/**
 * @file src/amf/amf_config.h
 * @brief Declarations for AMF encoder configuration.
 */
#pragma once

#include <cstdint>
#include <optional>

namespace amf {

  /**
   * @brief HDR metadata for AMF encoder.
   */
  struct amf_hdr_metadata {
    struct {
      uint16_t x;  // Normalized to 50,000
      uint16_t y;  // Normalized to 50,000
    } displayPrimaries[3];  // RGB order

    struct {
      uint16_t x;  // Normalized to 50,000
      uint16_t y;  // Normalized to 50,000
    } whitePoint;

    uint16_t maxDisplayLuminance;        // Nits
    uint16_t minDisplayLuminance;        // 1/10000th of a nit
    uint16_t maxContentLightLevel;       // Nits
    uint16_t maxFrameAverageLightLevel;  // Nits
  };

  /**
   * @brief AMF encoder configuration.
   * Integer values correspond directly to AMF SDK enum values.
   */
  struct amf_config {
    // Usage preset (AMF_VIDEO_ENCODER_USAGE_ENUM values)
    std::optional<int> usage;

    // Quality preset (AMF_VIDEO_ENCODER_QUALITY_PRESET_ENUM values)
    std::optional<int> quality_preset;

    // Rate control mode (AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_ENUM values)
    std::optional<int> rc_mode;

    // Pre-analysis enable
    std::optional<int> preanalysis;

    // VBAQ enable
    std::optional<int> vbaq;

    // H.264 entropy coding (0=CAVLC, 1=CABAC)
    int h264_cabac = 1;

    // Enforce HRD
    std::optional<int> enforce_hrd;

    // Number of LTR frames for RFI
    int max_ltr_frames = 1;

    // --- Pre-Analysis sub-system ---
    // PAQ mode (AMF_PA_PAQ_MODE_ENUM): 0=none, 1=CAQ
    std::optional<int> pa_paq_mode;
    // TAQ mode (AMF_PA_TAQ_MODE_ENUM): 0=none, 1=mode1, 2=mode2
    std::optional<int> pa_taq_mode;
    // CAQ strength (AMF_PA_CAQ_STRENGTH_ENUM): 0=low, 1=medium, 2=high
    std::optional<int> pa_caq_strength;
    // Lookahead buffer depth (0=disabled)
    std::optional<int> pa_lookahead_depth;
    // Scene change detection sensitivity (AMF_PA_SCENE_CHANGE_DETECTION_SENSITIVITY_ENUM): 0=low, 1=medium, 2=high
    std::optional<int> pa_scene_change_sensitivity;
    // High motion quality boost mode (AMF_PA_HIGH_MOTION_QUALITY_BOOST_MODE_ENUM): 0=none, 1=auto
    std::optional<int> pa_high_motion_quality_boost;
    // Initial QP after scene change (0-51, 0=auto)
    std::optional<int> pa_initial_qp_after_scene_change;
    // Activity type (AMF_PA_ACTIVITY_TYPE_ENUM): 0=Y, 1=YUV
    std::optional<int> pa_activity_type;

    // --- QVBR quality level ---
    // For QVBR rate control mode: quality level 1-51 (lower=better)
    std::optional<int> qvbr_quality_level;

    // --- AV1 Encoding Latency Mode ---
    // AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_ENUM: 0=none, 1=power saving RT, 2=RT, 3=lowest latency
    std::optional<int> av1_encoding_latency_mode;

    // --- AV1 Screen Content Tools ---
    std::optional<bool> av1_screen_content_tools;
    std::optional<bool> av1_palette_mode;
    std::optional<bool> av1_force_integer_mv;

    // --- Intra Refresh ---
    // H.264: number of MBs per slot; HEVC: number of CTBs per slot; AV1: mode enum
    std::optional<int> intra_refresh_mbs;
    // AV1-specific: intra refresh mode (AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE_ENUM)
    std::optional<int> av1_intra_refresh_mode;
    // AV1-specific: number of stripes for intra refresh
    std::optional<int> av1_intra_refresh_stripes;

    // --- Statistics feedback ---
    bool enable_statistics_feedback = false;
    bool enable_psnr_feedback = false;
    bool enable_ssim_feedback = false;

    // --- High Motion Quality Boost (encoder-level, separate from PA) ---
    std::optional<bool> high_motion_quality_boost_enable;
  };

}  // namespace amf
