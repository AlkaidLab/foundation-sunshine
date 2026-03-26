/**
 * @file src/amf/amf_d3d11.cpp
 * @brief Implementation of standalone AMF encoder with D3D11 texture input.
 */

#include "amf_d3d11.h"

#include <chrono>
#include <thread>

#include <AMF/components/ColorSpace.h>
#include <AMF/components/PreAnalysis.h>
#include <AMF/components/VideoEncoderAV1.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderVCE.h>
#include <AMF/core/Surface.h>

#include "src/config.h"
#include "src/logging.h"

namespace amf {

  // AMF DLL function types
  typedef AMF_RESULT(AMF_CDECL_CALL *AMFInit_Fn)(amf_uint64 version, ::amf::AMFFactory **ppFactory);
  typedef AMF_RESULT(AMF_CDECL_CALL *AMFQueryVersion_Fn)(amf_uint64 *pVersion);

  amf_d3d11::amf_d3d11(ID3D11Device *d3d_device):
      device(d3d_device) {
  }

  amf_d3d11::~amf_d3d11() {
    destroy_encoder();
  }

  bool
  amf_d3d11::init_amf_library() {
    if (factory) return true;

    amf_dll = LoadLibraryA(AMF_DLL_NAMEA);
    if (!amf_dll) {
      BOOST_LOG(error) << "AMF: failed to load " << AMF_DLL_NAMEA;
      return false;
    }

    auto amf_query_version = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(amf_dll, AMF_QUERY_VERSION_FUNCTION_NAME));
    auto amf_init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(amf_dll, AMF_INIT_FUNCTION_NAME));

    if (!amf_query_version || !amf_init) {
      BOOST_LOG(error) << "AMF: missing entry points in " << AMF_DLL_NAMEA;
      FreeLibrary(amf_dll);
      amf_dll = nullptr;
      return false;
    }

    amf_uint64 version = 0;
    if (amf_query_version(&version) != AMF_OK) {
      BOOST_LOG(error) << "AMF: failed to query runtime version";
      FreeLibrary(amf_dll);
      amf_dll = nullptr;
      return false;
    }

    BOOST_LOG(info) << "AMF runtime version: "
                    << AMF_GET_MAJOR_VERSION(version) << "."
                    << AMF_GET_MINOR_VERSION(version) << "."
                    << AMF_GET_SUBMINOR_VERSION(version) << "."
                    << AMF_GET_BUILD_VERSION(version);

    if (amf_init(AMF_FULL_VERSION, &factory) != AMF_OK || !factory) {
      BOOST_LOG(error) << "AMF: AMFInit failed";
      FreeLibrary(amf_dll);
      amf_dll = nullptr;
      return false;
    }

    return true;
  }

  AMF_SURFACE_FORMAT
  amf_d3d11::get_amf_format(platf::pix_fmt_e buffer_format, int bit_depth) {
    switch (buffer_format) {
      case platf::pix_fmt_e::nv12:
        return AMF_SURFACE_NV12;
      case platf::pix_fmt_e::p010:
        return AMF_SURFACE_P010;
      default:
        return (bit_depth == 10) ? AMF_SURFACE_P010 : AMF_SURFACE_NV12;
    }
  }

  const wchar_t *
  amf_d3d11::get_codec_id() {
    switch (video_format) {
      case 0:
        return AMFVideoEncoderVCE_AVC;
      case 1:
        return AMFVideoEncoder_HEVC;
      case 2:
        return AMFVideoEncoder_AV1;
      default:
        return AMFVideoEncoderVCE_AVC;
    }
  }

  bool
  amf_d3d11::set_ltr_property(const wchar_t *name, int64_t value) {
    auto res = encoder->SetProperty(name, value);
    if (res != AMF_OK) {
      BOOST_LOG(warning) << "AMF: failed to set LTR property, error: " << res;
      return false;
    }
    return true;
  }

  // Helper to set a codec-specific property with the right prefix
  template<typename T>
  void
  amf_d3d11::set_codec_property(const wchar_t *h264_name, const wchar_t *hevc_name, const wchar_t *av1_name, T value) {
    const wchar_t *name = (video_format == 0) ? h264_name :
                          (video_format == 1) ? hevc_name : av1_name;
    if (name) {
      encoder->SetProperty(name, value);
    }
  }

  bool
  amf_d3d11::configure_encoder(const amf_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &colorspace) {
    auto bitrate = static_cast<int64_t>(client_config.bitrate) * 1000;
    auto framerate = AMFConstructRate(client_config.framerate, 1);

    if (video_format == 0) {
      // H.264
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, (amf_int64) *config.quality_preset);
      if (config.rc_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, (amf_int64) *config.rc_mode);
      encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, framerate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64) 0);
      encoder->SetProperty(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, true);
      encoder->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, (amf_int64)(config.h264_cabac ? AMF_VIDEO_ENCODER_CABAC : AMF_VIDEO_ENCODER_CALV));
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.vbaq) encoder->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, !!(*config.vbaq));
      encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64) 0);
      encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
      encoder->SetProperty(AMF_VIDEO_ENCODER_INPUT_QUEUE_SIZE, (amf_int64) 1);
      encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64) 50);

      // LTR for RFI
      max_ltr_frames = config.max_ltr_frames;
      if (max_ltr_frames > 0) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_MAX_LTR_FRAMES, (amf_int64) max_ltr_frames);
        encoder->SetProperty(AMF_VIDEO_ENCODER_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_LTR_MODE_RESET_UNUSED);
      }

      // QVBR quality level
      if (config.qvbr_quality_level) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level);
      }

      // High motion quality boost
      if (config.high_motion_quality_boost_enable) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE, *config.high_motion_quality_boost_enable);
      }

      // Intra refresh
      if (config.intra_refresh_mbs) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT, (amf_int64) *config.intra_refresh_mbs);
      }

      // Statistics feedback
      if (config.enable_statistics_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_STATISTICS_FEEDBACK, true);
      }
      if (config.enable_psnr_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_PSNR_FEEDBACK, true);
      }
      if (config.enable_ssim_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_SSIM_FEEDBACK, true);
      }
    }
    else if (video_format == 1) {
      // HEVC
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, (amf_int64) *config.quality_preset);
      if (config.rc_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, (amf_int64) *config.rc_mode);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, framerate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64) 1);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, (amf_int64) 0);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.vbaq) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, !!(*config.vbaq));
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_QUEUE_SIZE, (amf_int64) 1);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, (amf_int64) 50);

      if (colorspace.bit_depth == 10) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10);
      }
      else {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
      }

      max_ltr_frames = config.max_ltr_frames;
      if (max_ltr_frames > 0) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_LTR_FRAMES, (amf_int64) max_ltr_frames);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_LTR_MODE_RESET_UNUSED);
      }

      // QVBR quality level
      if (config.qvbr_quality_level) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level);
      }

      // High motion quality boost
      if (config.high_motion_quality_boost_enable) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HIGH_MOTION_QUALITY_BOOST_ENABLE, *config.high_motion_quality_boost_enable);
      }

      // Intra refresh
      if (config.intra_refresh_mbs) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INTRA_REFRESH_NUM_CTBS_PER_SLOT, (amf_int64) *config.intra_refresh_mbs);
      }

      // Statistics feedback
      if (config.enable_statistics_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_STATISTICS_FEEDBACK, true);
      }
      if (config.enable_psnr_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PSNR_FEEDBACK, true);
      }
      if (config.enable_ssim_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_SSIM_FEEDBACK, true);
      }
    }
    else {
      // AV1
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, (amf_int64) *config.quality_preset);
      if (config.rc_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, (amf_int64) *config.rc_mode);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, framerate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, (amf_int64) 0);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INPUT_QUEUE_SIZE, (amf_int64) 1);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, (amf_int64) 50);
      if (config.av1_encoding_latency_mode) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) *config.av1_encoding_latency_mode);
      }
      else {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY);
      }

      // AV1 Screen Content Tools
      if (config.av1_screen_content_tools) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_SCREEN_CONTENT_TOOLS, *config.av1_screen_content_tools);
      }
      if (config.av1_palette_mode) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PALETTE_MODE, *config.av1_palette_mode);
      }
      if (config.av1_force_integer_mv) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INTEGER_MV, *config.av1_force_integer_mv);
      }

      // AV1 high motion quality boost
      if (config.high_motion_quality_boost_enable) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_HIGH_MOTION_QUALITY_BOOST, *config.high_motion_quality_boost_enable);
      }

      // AV1 AQ mode (Content Adaptive Quantization)
      if (config.pa_paq_mode) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_AQ_MODE, (amf_int64) *config.pa_paq_mode);
      }

      max_ltr_frames = config.max_ltr_frames;
      if (max_ltr_frames > 0) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_LTR_FRAMES, (amf_int64) max_ltr_frames);
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_LTR_MODE_RESET_UNUSED);
      }

      // QVBR quality level
      if (config.qvbr_quality_level) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level);
      }

      // Intra refresh
      if (config.av1_intra_refresh_mode) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE, (amf_int64) *config.av1_intra_refresh_mode);
        if (config.av1_intra_refresh_stripes) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRAREFRESH_STRIPES, (amf_int64) *config.av1_intra_refresh_stripes);
        }
      }

      // Statistics feedback
      if (config.enable_statistics_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_STATISTICS_FEEDBACK, true);
      }
      if (config.enable_psnr_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PSNR_FEEDBACK, true);
      }
      if (config.enable_ssim_feedback) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_SSIM_FEEDBACK, true);
      }
    }

    // Color space properties
    if (video_format == 0) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_FULL_RANGE_COLOR, colorspace.full_range);
    }
    else if (video_format == 1) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE, (amf_int64)(colorspace.full_range ? AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_FULL : AMF_VIDEO_ENCODER_HEVC_NOMINAL_RANGE_STUDIO));
    }
    else {
      // AV1: amf_bool type
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FULL_RANGE_COLOR, colorspace.full_range);
    }

    // Color properties for bitstream metadata.
    // Only set OUTPUT properties, matching FFmpeg's approach.
    // Do NOT set INPUT_COLOR_xxx — setting them may trigger AMF's internal color converter.
    amf_int64 amf_primaries;
    amf_int64 amf_transfer;
    amf_int64 amf_color_profile;

    switch (colorspace.colorspace) {
      case video::colorspace_e::rec601:
        amf_primaries = AMF_COLOR_PRIMARIES_SMPTE170M;
        amf_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE170M;
        amf_color_profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
        break;
      case video::colorspace_e::rec709:
        amf_primaries = AMF_COLOR_PRIMARIES_BT709;
        amf_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709;
        amf_color_profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
        break;
      case video::colorspace_e::bt2020sdr:
        amf_primaries = AMF_COLOR_PRIMARIES_BT2020;
        amf_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT2020_10;
        amf_color_profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
        break;
      case video::colorspace_e::bt2020:
        amf_primaries = AMF_COLOR_PRIMARIES_BT2020;
        amf_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084;
        amf_color_profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
        break;
      case video::colorspace_e::bt2020hlg:
        amf_primaries = AMF_COLOR_PRIMARIES_BT2020;
        amf_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
        amf_color_profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
        break;
      default:
        amf_primaries = AMF_COLOR_PRIMARIES_BT709;
        amf_transfer = AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709;
        amf_color_profile = colorspace.full_range ? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709 : AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
        break;
    }

    auto amf_bit_depth = (amf_int64)((colorspace.bit_depth == 10) ? AMF_COLOR_BIT_DEPTH_10 : AMF_COLOR_BIT_DEPTH_8);

    if (video_format == 0) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_COLOR_BIT_DEPTH, amf_bit_depth);
      encoder->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, amf_color_profile);
      encoder->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC, amf_transfer);
      encoder->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, amf_primaries);
    }
    else if (video_format == 1) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, amf_bit_depth);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE, amf_color_profile);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC, amf_transfer);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, amf_primaries);
    }
    else {
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, amf_bit_depth);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, amf_color_profile);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC, amf_transfer);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES, amf_primaries);
    }

    // Save statistics feedback state for encode_frame()
    statistics_enabled = config.enable_statistics_feedback;
    psnr_enabled = config.enable_psnr_feedback;
    ssim_enabled = config.enable_ssim_feedback;

    // Pre-Analysis sub-system properties (set on encoder when PA is enabled)
    if (config.preanalysis && *config.preanalysis) {
      if (config.pa_paq_mode) {
        encoder->SetProperty(AMF_PA_PAQ_MODE, (amf_int64) *config.pa_paq_mode);
      }
      if (config.pa_taq_mode) {
        encoder->SetProperty(AMF_PA_TAQ_MODE, (amf_int64) *config.pa_taq_mode);
      }
      if (config.pa_caq_strength) {
        encoder->SetProperty(AMF_PA_CAQ_STRENGTH, (amf_int64) *config.pa_caq_strength);
      }
      if (config.pa_lookahead_depth) {
        encoder->SetProperty(AMF_PA_LOOKAHEAD_BUFFER_DEPTH, (amf_int64) *config.pa_lookahead_depth);
      }
      if (config.pa_scene_change_sensitivity) {
        encoder->SetProperty(AMF_PA_SCENE_CHANGE_DETECTION_SENSITIVITY, (amf_int64) *config.pa_scene_change_sensitivity);
      }
      if (config.pa_high_motion_quality_boost) {
        encoder->SetProperty(AMF_PA_HIGH_MOTION_QUALITY_BOOST_MODE, (amf_int64) *config.pa_high_motion_quality_boost);
      }
      if (config.pa_initial_qp_after_scene_change) {
        encoder->SetProperty(AMF_PA_INITIAL_QP_AFTER_SCENE_CHANGE, (amf_int64) *config.pa_initial_qp_after_scene_change);
      }
      if (config.pa_activity_type) {
        encoder->SetProperty(AMF_PA_ACTIVITY_TYPE, (amf_int64) *config.pa_activity_type);
      }
    }

    // Low-latency mode (matching FFmpeg's "latency"=1 option)
    if (video_format == 0) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
    }
    else if (video_format == 1) {
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
    }

    return true;
  }

  bool
  amf_d3d11::create_encoder(const amf_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &colorspace,
    platf::pix_fmt_e buffer_format) {
    // Determine video format from client config
    video_format = client_config.videoFormat;
    current_config = client_config;

    // Initialize AMF library
    if (!init_amf_library()) return false;

    // Create AMF context
    auto res = factory->CreateContext(&context);
    if (res != AMF_OK || !context) {
      BOOST_LOG(error) << "AMF: CreateContext failed, error: " << res;
      return false;
    }

    // Set surface cache size to match FFmpeg's hwcontext_amf initialization
    context->SetProperty(L"DeviceSurfaceCacheSize", (amf_int64) 50);

    // Initialize D3D11 in AMF context with DX11_1 (matching FFmpeg)
    res = context->InitDX11(device, AMF_DX11_1);
    if (res != AMF_OK) {
      BOOST_LOG(error) << "AMF: InitDX11 failed, error: " << res;
      return false;
    }

    // Create encoder component
    res = factory->CreateComponent(context, get_codec_id(), &encoder);
    if (res != AMF_OK || !encoder) {
      BOOST_LOG(error) << "AMF: CreateComponent failed for codec " << video_format << ", error: " << res;
      return false;
    }

    // Configure encoder properties (before Init)
    if (!configure_encoder(config, client_config, colorspace)) {
      return false;
    }

    // Initialize encoder
    auto amf_format = get_amf_format(buffer_format, colorspace.bit_depth);
    surface_format = amf_format;
    encode_width = client_config.width;
    encode_height = client_config.height;
    res = encoder->Init(amf_format, client_config.width, client_config.height);
    if (res != AMF_OK && config.rc_mode) {
      // Init failed with custom RC mode - retry without it (driver may not support it)
      BOOST_LOG(warning) << "AMF: Init failed with rc_mode=" << *config.rc_mode << ", retrying with default RC";
      encoder->Terminate();
      encoder = nullptr;
      res = factory->CreateComponent(context, get_codec_id(), &encoder);
      if (res == AMF_OK && encoder) {
        auto config_fallback = config;
        config_fallback.rc_mode = std::nullopt;
        if (configure_encoder(config_fallback, client_config, colorspace)) {
          res = encoder->Init(amf_format, client_config.width, client_config.height);
        }
      }
    }
    if (res != AMF_OK) {
      BOOST_LOG(error) << "AMF: encoder Init failed, error: " << res;
      return false;
    }

    // Create input texture for the rendering pipeline to write to.
    // Must match the YUV format that the shader pipeline outputs (NV12/P010).
    DXGI_FORMAT dxgi_fmt;
    switch (buffer_format) {
      case platf::pix_fmt_e::nv12:
        dxgi_fmt = DXGI_FORMAT_NV12;
        break;
      case platf::pix_fmt_e::p010:
        dxgi_fmt = DXGI_FORMAT_P010;
        break;
      default:
        dxgi_fmt = (colorspace.bit_depth == 10) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        break;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = client_config.width;
    desc.Height = client_config.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = dxgi_fmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    auto hr = device->CreateTexture2D(&desc, nullptr, &input_texture);
    if (FAILED(hr)) {
      BOOST_LOG(error) << "AMF: failed to create input texture, HRESULT: 0x" << std::hex << hr;
      return false;
    }



    // Clamp effective LTR slots to what the encoder actually reserves
    effective_ltr_slots = (max_ltr_frames > 0) ? std::min(max_ltr_frames, MAX_LTR_SLOTS) : 0;

    // Reset LTR state
    for (auto &valid : ltr_slots_valid) valid = false;
    for (auto &fi : ltr_slot_frame_index) fi = 0;
    current_ltr_slot = 0;
    rfi_pending = false;

    auto codec_name = (video_format == 0) ? "H.264" :
                      (video_format == 1) ? "HEVC" :
                      (video_format == 2) ? "AV1" : "Unknown";
    BOOST_LOG(info) << "AMF: standalone " << codec_name << " encoder created ("
                    << client_config.width << "x" << client_config.height << " @ "
                    << client_config.framerate << "fps, LTR=" << max_ltr_frames << ")";
    return true;
  }

  void
  amf_d3d11::destroy_encoder() {
    pending_output = nullptr;
    if (encoder) {
      encoder->Terminate();
      encoder = nullptr;
    }
    if (context) {
      context->Terminate();
      context = nullptr;
    }
    if (input_texture) {
      input_texture->Release();
      input_texture = nullptr;
    }

    if (amf_dll) {
      FreeLibrary(amf_dll);
      amf_dll = nullptr;
    }
    factory = nullptr;
  }

  amf_encoded_frame
  amf_d3d11::encode_frame(uint64_t frame_index, bool force_idr) {
    amf_encoded_frame result;
    result.frame_index = frame_index;

    if (!encoder || !input_texture) return result;

    // Set the texture array index via private data, as FFmpeg does.
    // AMF uses this GUID to determine which slice of a texture array to encode.
    static const GUID AMFTextureArrayIndexGUID = { 0x28115527, 0xe7c3, 0x4b66, { 0x99, 0xd3, 0x4f, 0x2a, 0xe6, 0xb4, 0x7f, 0xaf } };
    int array_index = 0;
    input_texture->SetPrivateData(AMFTextureArrayIndexGUID, sizeof(array_index), &array_index);

    // Wrap the D3D11 texture as AMF surface (zero-copy)
    ::amf::AMFSurfacePtr surface;
    auto res = context->CreateSurfaceFromDX11Native(input_texture, &surface, nullptr);
    if (res != AMF_OK || !surface) {
      BOOST_LOG(error) << "AMF: CreateSurfaceFromDX11Native failed, error: " << res;
      return result;
    }

    // Set crop to actual frame dimensions (hw surfaces can be vertically aligned by 16)
    surface->SetCrop(0, 0, encode_width, encode_height);

    // Set per-frame properties
    if (force_idr) {
      if (video_format == 0) {
        surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
        surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
        surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
      }
      else if (video_format == 1) {
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
      }
      else {
        surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
        surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
      }

      // After IDR, mark LTR slot 0 for RFI baseline
      if (effective_ltr_slots > 0) {
        if (video_format == 0) {
          surface->SetProperty(AMF_VIDEO_ENCODER_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) 0);
        }
        else if (video_format == 1) {
          surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) 0);
        }
        else {
          surface->SetProperty(AMF_VIDEO_ENCODER_AV1_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) 0);
        }
        ltr_slots_valid[0] = true;
        ltr_slot_frame_index[0] = frame_index;
        current_ltr_slot = 1 % effective_ltr_slots;
      }
    }
    else if (rfi_pending && effective_ltr_slots > 0) {
      // After RFI: force reference to the saved LTR frame
      int64_t ltr_bitfield = 1LL << last_rfi_ltr_index;

      if (video_format == 0) {
        surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_LTR_REFERENCE_BITFIELD, ltr_bitfield);
      }
      else if (video_format == 1) {
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_LTR_REFERENCE_BITFIELD, ltr_bitfield);
      }
      else {
        surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_LTR_REFERENCE_BITFIELD, ltr_bitfield);
      }

      rfi_pending = false;
      result.after_ref_frame_invalidation = true;
    }
    else if (effective_ltr_slots > 0 && (frame_index % LTR_MARK_INTERVAL) == 0) {
      // Periodically mark current frame as LTR for future RFI use
      // Only mark every LTR_MARK_INTERVAL frames to avoid limiting encoder reference freedom
      // Only use slots < effective_ltr_slots (clamped to max_ltr_frames)
      if (video_format == 0) {
        surface->SetProperty(AMF_VIDEO_ENCODER_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) current_ltr_slot);
      }
      else if (video_format == 1) {
        surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) current_ltr_slot);
      }
      else {
        surface->SetProperty(AMF_VIDEO_ENCODER_AV1_MARK_CURRENT_WITH_LTR_INDEX, (amf_int64) current_ltr_slot);
      }
      ltr_slots_valid[current_ltr_slot] = true;
      ltr_slot_frame_index[current_ltr_slot] = frame_index;
      current_ltr_slot = (current_ltr_slot + 1) % effective_ltr_slots;
    }

    // Submit input — retry with output draining if input queue is full (like FFmpeg)
    res = encoder->SubmitInput(surface);
    if (res == AMF_INPUT_FULL) {
      // Drain output to free up space in the encoder queue, then retry
      for (int retry = 0; retry < 20 && res == AMF_INPUT_FULL; ++retry) {
        ::amf::AMFDataPtr drain_data;
        auto drain_res = encoder->QueryOutput(&drain_data);
        if (drain_data) {
          // Stash the output for later retrieval
          pending_output = drain_data;
        }
        if (drain_res != AMF_OK && !drain_data) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        res = encoder->SubmitInput(surface);
      }
      if (res == AMF_INPUT_FULL) {
        BOOST_LOG(warning) << "AMF: SubmitInput still AMF_INPUT_FULL after retries, dropping frame " << frame_index;
        return result;
      }
    }
    if (res != AMF_OK) {
      BOOST_LOG(error) << "AMF: SubmitInput failed, error: " << res;
      return result;
    }

    // Query output — if we already drained output during SubmitInput retry, use that
    ::amf::AMFDataPtr output_data;
    if (pending_output) {
      output_data = pending_output;
      pending_output = nullptr;
    }
    else {
      // Poll with retry: encoder may need a moment after SubmitInput
      for (int poll = 0; poll < 10; ++poll) {
        res = encoder->QueryOutput(&output_data);
        if (output_data || (res != AMF_REPEAT && res != AMF_NEED_MORE_INPUT)) {
          break;
        }
        // QUERY_TIMEOUT is set to 1ms, but if driver doesn't support it, sleep manually
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!output_data) {
        // Encoder needs more input or no output yet (pipeline filling)
        return result;
      }
    }

    // Extract encoded bitstream
    ::amf::AMFBufferPtr buffer(output_data);
    if (!buffer) {
      BOOST_LOG(error) << "AMF: output is not a buffer";
      return result;
    }

    auto data_ptr = static_cast<uint8_t *>(buffer->GetNative());
    auto data_size = buffer->GetSize();
    result.data.assign(data_ptr, data_ptr + data_size);

    // Check if output frame is IDR
    amf_int64 output_type = 0;
    if (video_format == 0) {
      if (output_data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &output_type) == AMF_OK) {
        result.idr = (output_type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
      }
    }
    else if (video_format == 1) {
      if (output_data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &output_type) == AMF_OK) {
        result.idr = (output_type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR);
      }
    }
    else {
      if (output_data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &output_type) == AMF_OK) {
        result.idr = (output_type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY);
      }
    }

    // Statistics feedback logging (only if enabled and at debug level)
    if (statistics_enabled) {
      amf_int64 avg_qp = 0;
      const wchar_t *avg_qp_prop = (video_format == 0) ? AMF_VIDEO_ENCODER_STATISTIC_AVERAGE_QP :
                                   (video_format == 1) ? AMF_VIDEO_ENCODER_HEVC_STATISTIC_AVERAGE_QP :
                                                         AMF_VIDEO_ENCODER_AV1_STATISTIC_AVERAGE_Q_INDEX;
      if (output_data->GetProperty(avg_qp_prop, &avg_qp) == AMF_OK) {
        BOOST_LOG(debug) << "AMF: frame " << frame_index << " avg_qp=" << avg_qp << " size=" << data_size;
      }
    }
    if (psnr_enabled) {
      double psnr_y = 0;
      const wchar_t *psnr_prop = (video_format == 0) ? AMF_VIDEO_ENCODER_STATISTIC_PSNR_Y :
                                 (video_format == 1) ? AMF_VIDEO_ENCODER_HEVC_STATISTIC_PSNR_Y :
                                                       AMF_VIDEO_ENCODER_AV1_STATISTIC_PSNR_Y;
      if (output_data->GetProperty(psnr_prop, &psnr_y) == AMF_OK) {
        BOOST_LOG(debug) << "AMF: frame " << frame_index << " PSNR_Y=" << psnr_y;
      }
    }
    if (ssim_enabled) {
      double ssim_y = 0;
      const wchar_t *ssim_prop = (video_format == 0) ? AMF_VIDEO_ENCODER_STATISTIC_SSIM_Y :
                                 (video_format == 1) ? AMF_VIDEO_ENCODER_HEVC_STATISTIC_SSIM_Y :
                                                       AMF_VIDEO_ENCODER_AV1_STATISTIC_SSIM_Y;
      if (output_data->GetProperty(ssim_prop, &ssim_y) == AMF_OK) {
        BOOST_LOG(debug) << "AMF: frame " << frame_index << " SSIM_Y=" << ssim_y;
      }
    }

    return result;
  }

  bool
  amf_d3d11::invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) {
    if (!encoder || effective_ltr_slots <= 0) return false;

    // Find a valid LTR slot whose frame was marked BEFORE the invalidation range.
    // This ensures we reference a frame that predates the corrupted frames.
    int best_ltr = -1;
    uint64_t best_frame = 0;
    for (int i = 0; i < effective_ltr_slots; i++) {
      if (ltr_slots_valid[i] && ltr_slot_frame_index[i] < first_frame) {
        if (best_ltr < 0 || ltr_slot_frame_index[i] > best_frame) {
          best_ltr = i;
          best_frame = ltr_slot_frame_index[i];
        }
      }
    }

    if (best_ltr < 0) {
      BOOST_LOG(warning) << "AMF: RFI failed, no valid LTR frame before frame " << first_frame;
      return false;
    }

    // Invalidate all LTR slots that overlap the invalidation range
    for (int i = 0; i < effective_ltr_slots; i++) {
      if (ltr_slots_valid[i] && ltr_slot_frame_index[i] >= first_frame && ltr_slot_frame_index[i] <= last_frame) {
        ltr_slots_valid[i] = false;
      }
    }

    last_rfi_ltr_index = best_ltr;
    rfi_pending = true;

    BOOST_LOG(info) << "AMF: RFI pending, using LTR index " << best_ltr
                    << " (frame " << best_frame << ") for invalidated frames " << first_frame << "-" << last_frame;
    return true;
  }

  void
  amf_d3d11::set_bitrate(int bitrate_kbps) {
    if (!encoder) return;

    auto bitrate = static_cast<int64_t>(bitrate_kbps) * 1000;
    AMF_RESULT res;

    if (video_format == 0) {
      res = encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitrate);
    }
    else if (video_format == 1) {
      res = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitrate);
    }
    else {
      res = encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitrate);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bitrate);
    }

    if (res == AMF_OK) {
      BOOST_LOG(info) << "AMF: bitrate dynamically changed to " << bitrate_kbps << " Kbps";
    }
    else {
      BOOST_LOG(warning) << "AMF: set_bitrate failed, error: " << res;
    }
  }

  void
  amf_d3d11::set_hdr_metadata(const std::optional<amf_hdr_metadata> &metadata) {
    if (!encoder || !context) return;

    if (metadata && video_format >= 0) {
      // Create AMFBuffer containing AMFHDRMetadata
      ::amf::AMFBufferPtr hdr_buffer;
      auto res = context->AllocBuffer(::amf::AMF_MEMORY_HOST, sizeof(AMFHDRMetadata), &hdr_buffer);
      if (res != AMF_OK || !hdr_buffer) {
        BOOST_LOG(warning) << "AMF: failed to allocate HDR metadata buffer";
        return;
      }

      auto *amf_hdr = static_cast<AMFHDRMetadata *>(hdr_buffer->GetNative());
      // Display primaries: both normalized to 50,000
      amf_hdr->redPrimary[0] = metadata->displayPrimaries[0].x;
      amf_hdr->redPrimary[1] = metadata->displayPrimaries[0].y;
      amf_hdr->greenPrimary[0] = metadata->displayPrimaries[1].x;
      amf_hdr->greenPrimary[1] = metadata->displayPrimaries[1].y;
      amf_hdr->bluePrimary[0] = metadata->displayPrimaries[2].x;
      amf_hdr->bluePrimary[1] = metadata->displayPrimaries[2].y;
      amf_hdr->whitePoint[0] = metadata->whitePoint.x;
      amf_hdr->whitePoint[1] = metadata->whitePoint.y;
      // maxMasteringLuminance: AMF expects nits * 10000, SS_HDR_METADATA provides nits
      amf_hdr->maxMasteringLuminance = static_cast<amf_uint32>(metadata->maxDisplayLuminance) * 10000;
      // minMasteringLuminance: both in 1/10000th of a nit
      amf_hdr->minMasteringLuminance = metadata->minDisplayLuminance;
      amf_hdr->maxContentLightLevel = metadata->maxContentLightLevel;
      amf_hdr->maxFrameAverageLightLevel = metadata->maxFrameAverageLightLevel;

      // Set HDR metadata on encoder
      if (video_format == 0) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_INPUT_HDR_METADATA, hdr_buffer);
      }
      else if (video_format == 1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA, hdr_buffer);
      }
      else {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA, hdr_buffer);
      }

      BOOST_LOG(info) << "AMF: HDR metadata set (max luminance: " << metadata->maxDisplayLuminance << " nits)";
    }
  }

  void *
  amf_d3d11::get_input_texture() {
    return input_texture;
  }

  std::unique_ptr<amf_d3d11>
  create_amf_d3d11(ID3D11Device *d3d_device) {
    if (!d3d_device) return nullptr;

    auto enc = std::make_unique<amf_d3d11>(d3d_device);
    return enc;
  }

}  // namespace amf
