/**
 * @file src/amf/amf_d3d11.cpp
 * @brief Implementation of standalone AMF encoder with D3D11 texture input.
 */

#include "amf_d3d11.h"

#include "amf_avcodec_compat.h"

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
#include "src/utility.h"

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
    avcodec_compat_profile = config.avcodec_compat;
    user_configured_rate_control = config.rc_mode.has_value();
    hwsurfaces_in_queue_max = HWSURFACES_IN_QUEUE_DEFAULT;

    auto configure_multi_hw_instance = [&](const wchar_t *multi_hw_property,
                                           const wchar_t *sav_property,
                                           const wchar_t *hw_instances_cap,
                                           const wchar_t *sav_support_cap) {
      if (!config.multi_hw_instance_encode) return;

      const bool enabled = *config.multi_hw_instance_encode;
      amf_int64 hw_instances = 0;
      const bool hw_cap_known = hw_instances_cap && encoder->GetProperty(hw_instances_cap, &hw_instances) == AMF_OK;
      amf_bool sav_supported = false;
      const bool sav_cap_known = sav_support_cap && encoder->GetProperty(sav_support_cap, &sav_supported) == AMF_OK;

      auto set_optional_property = [&](const wchar_t *property, bool value, const char *label) {
        if (!property) return;
        auto property_res = encoder->SetProperty(property, value);
        if (property_res != AMF_OK) {
          BOOST_LOG(warning) << "AMF: failed to " << (value ? "enable " : "disable ")
                             << label << ", error: " << property_res;
        }
      };

      if (enabled && hw_cap_known && hw_instances <= 1 && (!sav_property || (sav_cap_known && !sav_supported))) {
        BOOST_LOG(info) << "AMF: multi-HW instance encode requested, but this codec reports "
                        << hw_instances << " hardware encoder instance(s)";
        return;
      }

      if (!enabled || !hw_cap_known || hw_instances > 1) {
        set_optional_property(multi_hw_property, enabled, "multi-HW instance encode");
      }
      if (!enabled || !sav_cap_known || sav_supported) {
        set_optional_property(sav_property, enabled, "Smart Access Video");
      }
    };

    auto apply_standalone_feature_overlay = [&]() {
      max_ltr_frames = config.max_ltr_frames;
      if (max_ltr_frames > 0) {
        if (video_format == 0) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_MAX_LTR_FRAMES, (amf_int64) max_ltr_frames);
          encoder->SetProperty(AMF_VIDEO_ENCODER_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_LTR_MODE_RESET_UNUSED);
        }
        else if (video_format == 1) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_MAX_LTR_FRAMES, (amf_int64) max_ltr_frames);
          encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_LTR_MODE_RESET_UNUSED);
        }
        else {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_MAX_LTR_FRAMES, (amf_int64) max_ltr_frames);
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_LTR_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_LTR_MODE_RESET_UNUSED);
        }
      }

      if (video_format == 2) {
        if (config.av1_screen_content_tools) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_SCREEN_CONTENT_TOOLS, *config.av1_screen_content_tools);
        }
        if (config.av1_palette_mode) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PALETTE_MODE, *config.av1_palette_mode);
        }
        if (config.av1_force_integer_mv) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INTEGER_MV, *config.av1_force_integer_mv);
        }
        if (config.av1_intra_refresh_mode) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE, (amf_int64) *config.av1_intra_refresh_mode);
          if (config.av1_intra_refresh_stripes) {
            encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRAREFRESH_STRIPES, (amf_int64) *config.av1_intra_refresh_stripes);
          }
        }
        if (client_config.slicesPerFrame > 1) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TILES_PER_FRAME, (amf_int64) client_config.slicesPerFrame);
        }
      }

      if (config.enable_statistics_feedback) {
        set_codec_property(
          AMF_VIDEO_ENCODER_STATISTICS_FEEDBACK,
          AMF_VIDEO_ENCODER_HEVC_STATISTICS_FEEDBACK,
          AMF_VIDEO_ENCODER_AV1_STATISTICS_FEEDBACK,
          true);
      }
      if (config.enable_psnr_feedback) {
        set_codec_property(
          AMF_VIDEO_ENCODER_PSNR_FEEDBACK,
          AMF_VIDEO_ENCODER_HEVC_PSNR_FEEDBACK,
          AMF_VIDEO_ENCODER_AV1_PSNR_FEEDBACK,
          true);
      }
      if (config.enable_ssim_feedback) {
        set_codec_property(
          AMF_VIDEO_ENCODER_SSIM_FEEDBACK,
          AMF_VIDEO_ENCODER_HEVC_SSIM_FEEDBACK,
          AMF_VIDEO_ENCODER_AV1_SSIM_FEEDBACK,
          true);
      }
    };

    auto apply_avcodec_sav_latency = [&]() {
      if (!config.multi_hw_instance_encode || !*config.multi_hw_instance_encode) return;

      if (video_format == 0) {
        if (!config.lowlatency_mode || *config.lowlatency_mode) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        }
      }
      else if (video_format == 1) {
        if (!config.lowlatency_mode) {
          encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
        }
      }
      else {
        if (!config.av1_encoding_latency_mode) {
          encoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
            (amf_int64) AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY);
        }
      }
    };

    if (avcodec_compat_profile) {
      auto compat = amf_avcodec_compat::configure(encoder, video_format, config, client_config, colorspace);
      if (compat.result != AMF_OK) {
        return false;
      }
      hwsurfaces_in_queue_max = compat.hwsurfaces_in_queue_max;
      user_configured_rate_control = compat.manages_rate_control;

      if (video_format == 0) {
        configure_multi_hw_instance(
          nullptr,
          AMF_VIDEO_ENCODER_ENABLE_SMART_ACCESS_VIDEO,
          AMF_VIDEO_ENCODER_CAP_NUM_OF_HW_INSTANCES,
          AMF_VIDEO_ENCODER_CAP_SUPPORT_SMART_ACCESS_VIDEO);
      }
      else if (video_format == 1) {
        configure_multi_hw_instance(
          AMF_VIDEO_ENCODER_HEVC_MULTI_HW_INSTANCE_ENCODE,
          AMF_VIDEO_ENCODER_HEVC_ENABLE_SMART_ACCESS_VIDEO,
          AMF_VIDEO_ENCODER_HEVC_CAP_NUM_OF_HW_INSTANCES,
          AMF_VIDEO_ENCODER_HEVC_CAP_SUPPORT_SMART_ACCESS_VIDEO);
      }
      else {
        configure_multi_hw_instance(
          AMF_VIDEO_ENCODER_AV1_MULTI_HW_INSTANCE_ENCODE,
          AMF_VIDEO_ENCODER_AV1_ENABLE_SMART_ACCESS_VIDEO,
          AMF_VIDEO_ENCODER_AV1_CAP_NUM_OF_HW_INSTANCES,
          AMF_VIDEO_ENCODER_AV1_CAP_SUPPORT_SMART_ACCESS_VIDEO);
      }

      apply_avcodec_sav_latency();
      apply_standalone_feature_overlay();
    }
    else if (video_format == 0) {
      // H.264
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, (amf_int64) *config.quality_preset);
      if (config.rc_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, (amf_int64) *config.rc_mode);
      encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
      if (user_configured_rate_control) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
        encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bitrate);
      }
      encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, framerate);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64) 0);
      encoder->SetProperty(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, true);
      encoder->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, (amf_int64)(config.h264_cabac ? AMF_VIDEO_ENCODER_CABAC : AMF_VIDEO_ENCODER_CALV));
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.vbaq) encoder->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, !!(*config.vbaq));
      encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64) 0);
      // LOWLATENCY_MODE and INPUT_QUEUE_SIZE: only set when user opts in.
      // Matches FFmpeg amfenc behavior (FFmpeg never forces these properties).
      // Forcing them to true/1 has been observed to expose latent AMD driver
      // bugs (see AlkaidLab/foundation-sunshine#666 freeze on RDNA4 26.5.x).
      if (config.lowlatency_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, !!(*config.lowlatency_mode));
      if (config.input_queue_size) encoder->SetProperty(AMF_VIDEO_ENCODER_INPUT_QUEUE_SIZE, (amf_int64) *config.input_queue_size);
      configure_multi_hw_instance(
        nullptr,
        AMF_VIDEO_ENCODER_ENABLE_SMART_ACCESS_VIDEO,
        AMF_VIDEO_ENCODER_CAP_NUM_OF_HW_INSTANCES,
        AMF_VIDEO_ENCODER_CAP_SUPPORT_SMART_ACCESS_VIDEO);
      encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64) 1);

      // LTR for RFI (Reference Frame Invalidation, weak-network recovery).
      //
      // Disabled by default (max_ltr_frames == 0) to match FFmpeg amfenc behavior:
      // FFmpeg's libavcodec/amfenc.c never sets MAX_LTR_FRAMES / LTR_MODE, so static
      // screen regions are not pinned to a baseline LTR frame and never accumulate
      // color-block artifacts.
      //
      // Trade-off when the user opts in (amd_ltr_frames >= 1):
      //   + On lossy links, client-side reference invalidation can recover by
      //     sending a P-frame referencing a known-good LTR slot instead of a full
      //     IDR. IDRs are 10-20x larger than P-frames and themselves more likely
      //     to be lost on weak networks, which can cascade into an "IDR storm".
      //   - Static regions may inherit the IDR-time quantization noise of the
      //     baseline LTR slot until motion forces a fresh intra block.
      //
      // The slot rotation / IDR-baseline preservation logic below (PR #630) only
      // takes effect when LTR is opted in.
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

      // Slices per frame
      if (client_config.slicesPerFrame > 1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_SLICES_PER_FRAME, (amf_int64) client_config.slicesPerFrame);
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
      if (user_configured_rate_control) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bitrate);
      }
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, framerate);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64) 1);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, (amf_int64) 60);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.vbaq) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, !!(*config.vbaq));
      if (config.lowlatency_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, !!(*config.lowlatency_mode));
      if (config.input_queue_size) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INPUT_QUEUE_SIZE, (amf_int64) *config.input_queue_size);
      configure_multi_hw_instance(
        AMF_VIDEO_ENCODER_HEVC_MULTI_HW_INSTANCE_ENCODE,
        AMF_VIDEO_ENCODER_HEVC_ENABLE_SMART_ACCESS_VIDEO,
        AMF_VIDEO_ENCODER_HEVC_CAP_NUM_OF_HW_INSTANCES,
        AMF_VIDEO_ENCODER_HEVC_CAP_SUPPORT_SMART_ACCESS_VIDEO);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, (amf_int64) 1);

      if (colorspace.bit_depth == 10) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10);
      }
      else {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, (amf_int64) AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
      }

      // LTR for RFI - see H.264 block above for detailed trade-off rationale.
      // Disabled by default; opt-in via amd_ltr_frames config.
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

      // Slices per frame
      if (client_config.slicesPerFrame > 1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME, (amf_int64) client_config.slicesPerFrame);
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
      if (user_configured_rate_control) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitrate);
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bitrate);
      }
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, framerate);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, (amf_int64) AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, (amf_int64) 0);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      // INPUT_QUEUE_SIZE / ENCODING_LATENCY_MODE: only set when user opts in.
      // Matches FFmpeg amfenc behavior (never auto-forces LOWEST_LATENCY).
      // See AlkaidLab/foundation-sunshine#666 for the RDNA4 freeze that
      // motivated stopping aggressive defaults.
      if (config.input_queue_size) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INPUT_QUEUE_SIZE, (amf_int64) *config.input_queue_size);
      configure_multi_hw_instance(
        AMF_VIDEO_ENCODER_AV1_MULTI_HW_INSTANCE_ENCODE,
        AMF_VIDEO_ENCODER_AV1_ENABLE_SMART_ACCESS_VIDEO,
        AMF_VIDEO_ENCODER_AV1_CAP_NUM_OF_HW_INSTANCES,
        AMF_VIDEO_ENCODER_AV1_CAP_SUPPORT_SMART_ACCESS_VIDEO);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, (amf_int64) 1);
      if (config.av1_encoding_latency_mode) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) *config.av1_encoding_latency_mode);
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

      // LTR for RFI - see H.264 block above for detailed trade-off rationale.
      // Disabled by default; opt-in via amd_ltr_frames config.
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

      // Tiles per frame
      if (client_config.slicesPerFrame > 1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TILES_PER_FRAME, (amf_int64) client_config.slicesPerFrame);
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

    // NOTE: LOWLATENCY_MODE is intentionally NOT forced here.
    //
    // Previously this block hard-coded AMF_VIDEO_ENCODER_(HEVC_)LOWLATENCY_MODE = true
    // for both H264 and HEVC (AV1 was never forced). This:
    //   1) Silently overrode the per-codec `config.lowlatency_mode` opt-in
    //      we set earlier in configure_*_encoder().
    //   2) Diverged from FFmpeg amfenc behavior, which only writes this
    //      property when the user passes `-latency 1` (default -1 = leave
    //      property unset, driver default = false).
    //   3) Triggered a firmware freeze on AMD RDNA4 (RX 9070/9070 XT) with
    //      Adrenalin 26.5.x on HEVC: video stalls while audio keeps flowing,
    //      toggling HDR (which forces encoder reinit) temporarily recovers.
    //      AV1 was unaffected precisely because no AV1 branch existed here.
    //
    // LOWLATENCY_MODE is now controlled solely by `config.lowlatency_mode`
    // (WebUI: amd_lowlatency_mode). Combined with USAGE = ULTRA_LOW_LATENCY,
    // the encoder pipeline still achieves low latency without the firmware
    // bug path. Users who want the aggressive mode can opt in explicitly.

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

    // Init fallback chain: some driver/hardware combinations reject specific
    // properties (especially PreAnalysis with high quality_preset on older VCN).
    // Try progressively disabling problematic features instead of failing the
    // whole session, which previously caused "server keeps restarting" loops.
    // The fallback is *cumulative*: once a feature is disabled in one step it
    // stays disabled in subsequent steps, so we don't accidentally re-enable
    // the failing feature on a later retry.
    auto config_fallback = config;
    auto try_with_fallback = [&](const char *what, auto mutator) {
      if (res == AMF_OK) return;
      BOOST_LOG(warning) << "AMF: Init failed (error " << res << "), retrying without " << what;
      if (encoder) {
        encoder->Terminate();
        encoder = nullptr;
      }
      auto recreate_res = factory->CreateComponent(context, get_codec_id(), &encoder);
      if (recreate_res != AMF_OK || !encoder) {
        res = recreate_res;
        return;
      }
      mutator(config_fallback);
      if (!configure_encoder(config_fallback, client_config, colorspace)) {
        res = AMF_FAIL;
        return;
      }
      res = encoder->Init(amf_format, client_config.width, client_config.height);
    };

    if (config.multi_hw_instance_encode && *config.multi_hw_instance_encode) {
      try_with_fallback("multi-HW instance encode", [](amf_config &c) { c.multi_hw_instance_encode = std::nullopt; });
    }
    if (config.preanalysis && *config.preanalysis) {
      try_with_fallback("PreAnalysis", [](amf_config &c) { c.preanalysis = false; });
    }
    if (config.rc_mode) {
      try_with_fallback("custom rc_mode", [](amf_config &c) { c.rc_mode = std::nullopt; });
    }
    if (config.quality_preset) {
      try_with_fallback("quality_preset", [](amf_config &c) { c.quality_preset = std::nullopt; });
    }

    if (res != AMF_OK) {
      BOOST_LOG(error) << "AMF: encoder Init failed after fallbacks, error: " << res;
      return false;
    }

    // Derive runtime watchdog threshold from framerate so the fatal-error
    // signal fires after roughly 1s of wall-clock time regardless of fps.
    // Floor at 30 to give the PreAnalysis lookahead pipeline time to fill
    // at startup without false-positive reinit.
    {
      int fps = client_config.framerate > 0 ? client_config.framerate : 60;
      max_consecutive_failures = std::max(30, fps);
    }

    // Check if driver supports QUERY_TIMEOUT by reading back the property (FFmpeg pattern)
    {
      const wchar_t *qt_prop = (video_format == 0) ? AMF_VIDEO_ENCODER_QUERY_TIMEOUT :
                               (video_format == 1) ? AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT :
                                                     AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT;
      amf_int64 qt_val = 0;
      auto qt_res = encoder->GetProperty(qt_prop, &qt_val);
      query_timeout_supported = (qt_res == AMF_OK && qt_val > 0);
      BOOST_LOG(info) << "AMF: QUERY_TIMEOUT " << (query_timeout_supported ? "supported" : "not supported") << " (value=" << qt_val << ")";
      avcodec_scheduler.configure(hwsurfaces_in_queue_max, query_timeout_supported);
      if (avcodec_compat_profile) {
        BOOST_LOG(info) << "AMF: AVCodec compatibility scheduler ready"
                        << " (hwsurfaces_in_queue_max=" << hwsurfaces_in_queue_max
                        << ", query_timeout=" << (query_timeout_supported ? "yes" : "no") << ")";
      }
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
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

    auto hr = device->CreateTexture2D(&desc, nullptr, &input_texture);
    if (FAILED(hr)) {
      BOOST_LOG(info) << "AMF: input texture UAV bind unavailable, falling back to render-target input, HRESULT: 0x"
                      << std::hex << hr;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET;
      hr = device->CreateTexture2D(&desc, nullptr, &input_texture);
    }
    if (FAILED(hr)) {
      BOOST_LOG(error) << "AMF: failed to create input texture, HRESULT: 0x" << std::hex << hr;
      return false;
    }



    // Clamp effective LTR slots to what the encoder actually reserves.
    // When max_ltr_frames == 0 (default), the entire LTR/RFI subsystem becomes
    // a no-op: the IDR baseline marking, slot rotation, and invalidate handling
    // below all gate on `effective_ltr_slots > 0`. The fallback for client-side
    // invalidate_ref_frames in this case is force_idr=true (see video.cpp).
    effective_ltr_slots = (max_ltr_frames > 0) ? std::min(max_ltr_frames, MAX_LTR_SLOTS) : 0;

    // Reset LTR state
    for (auto &valid : ltr_slots_valid) valid = false;
    for (auto &fi : ltr_slot_frame_index) fi = 0;
    current_ltr_slot = 0;
    rfi_pending = false;
    hwsurfaces_in_queue = 0;
    avcodec_scheduler.reset();
    consecutive_submit_failures = 0;
    consecutive_empty_outputs = 0;

    auto codec_name = (video_format == 0) ? "H.264" :
                      (video_format == 1) ? "HEVC" :
                      (video_format == 2) ? "AV1" : "Unknown";
    BOOST_LOG(info) << "AMF: standalone " << codec_name << " encoder created ("
                    << client_config.width << "x" << client_config.height << " @ "
                    << client_config.framerate << "fps, LTR=" << max_ltr_frames
                    << ", slices=" << client_config.slicesPerFrame << ")";
    return true;
  }

  void
  amf_d3d11::destroy_encoder() {
    pending_outputs.clear();
    frame_rfi_flags.clear();
    hwsurfaces_in_queue = 0;
    avcodec_scheduler.reset();
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
      // Check if the D3D11 device is lost (TDR, driver crash, etc.)
      if (device) {
        auto removed_reason = device->GetDeviceRemovedReason();
        if (removed_reason != S_OK) {
          BOOST_LOG(error) << "AMF: D3D11 device lost, reason: 0x" << util::hex(removed_reason).to_string_view();
        }
      }
      return result;
    }

    // Set crop to actual frame dimensions (hw surfaces can be vertically aligned by 16)
    surface->SetCrop(0, 0, encode_width, encode_height);
    surface->SetPts(static_cast<amf_pts>(frame_index));

    // Set per-frame properties
    bool frame_after_ref_frame_invalidation = false;
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
        // Slot 0 is reserved as the permanent IDR baseline. Periodic rotation
        // begins at slot 1 (or stays at 0 when only a single slot exists, in
        // which case the baseline must be sacrificed for fresher anchors).
        current_ltr_slot = (effective_ltr_slots > 1) ? 1 : 0;
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
      frame_after_ref_frame_invalidation = true;
    }
    else if (effective_ltr_slots > 0 && (frame_index % LTR_MARK_INTERVAL) == 0) {
      // Periodically mark current frame as LTR for future RFI use.
      // Rotate through slots 1..N-1 so the IDR baseline in slot 0 stays valid
      // even if every recent periodic anchor lands inside a loss burst. With a
      // single slot configured, fall back to overwriting slot 0.
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
      if (effective_ltr_slots > 1) {
        current_ltr_slot++;
        if (current_ltr_slot >= effective_ltr_slots) {
          current_ltr_slot = 1;  // wrap, skipping the reserved slot 0
        }
      }
      // else: stays at 0 (single-slot fallback)
    }

    frame_rfi_flags[frame_index] = frame_after_ref_frame_invalidation;

    ::amf::AMFDataPtr output_data;
    if (avcodec_compat_profile) {
      auto compat = avcodec_scheduler.submit_and_query(encoder, surface);
      res = compat.submit_result;

      if (compat.input_exhausted) {
        BOOST_LOG(warning) << "AMF: AVCodec scheduler SubmitInput still "
                           << (res == AMF_INPUT_FULL ? "AMF_INPUT_FULL" : "AMF_DECODER_NO_FREE_SURFACES")
                           << " after retries, dropping frame " << frame_index
                           << " (in_flight=" << avcodec_scheduler.in_flight() << ")";
        frame_rfi_flags.erase(frame_index);
        if (++consecutive_submit_failures >= max_consecutive_failures) {
          BOOST_LOG(error) << "AMF: " << consecutive_submit_failures
                           << " consecutive frames with AVCodec scheduler input exhaustion, signaling reinit";
          result.fatal = true;
        }
        return result;
      }

      if (compat.need_more_input) {
        consecutive_submit_failures = 0;
        return result;
      }

      if (res != AMF_OK) {
        BOOST_LOG(error) << "AMF: AVCodec scheduler SubmitInput failed, error: " << res;
        frame_rfi_flags.erase(frame_index);
        if (device) {
          auto removed_reason = device->GetDeviceRemovedReason();
          if (removed_reason != S_OK) {
            BOOST_LOG(error) << "AMF: D3D11 device lost after SubmitInput, reason: 0x" << util::hex(removed_reason).to_string_view();
            result.fatal = true;
            return result;
          }
        }
        if (++consecutive_submit_failures >= max_consecutive_failures) {
          BOOST_LOG(error) << "AMF: " << consecutive_submit_failures << " consecutive AVCodec scheduler SubmitInput failures, signaling reinit";
          result.fatal = true;
        }
        return result;
      }

      consecutive_submit_failures = 0;
      output_data = compat.output_data;
      if (!output_data) {
        if (++consecutive_empty_outputs >= max_consecutive_failures) {
          BOOST_LOG(error) << "AMF: " << consecutive_empty_outputs << " consecutive frames with no AVCodec scheduler output, signaling reinit";
          result.fatal = true;
        }
        return result;
      }
    }
    else {
    // FFmpeg-style proactive backpressure: if we already have many surfaces
    // in flight, drain one output BEFORE SubmitInput to avoid AMF_INPUT_FULL
    // entirely. This eliminates the tight retry spin in the common overrun
    // path (4K144 HDR, transient VCN stall, DXGI scheduling jitter) which on
    // some AMD GPUs has been observed to wedge the pipeline until the client
    // disconnects (frame frozen, audio still flowing).
    if (hwsurfaces_in_queue >= hwsurfaces_in_queue_max) {
      ::amf::AMFDataPtr drain_data;
      encoder->QueryOutput(&drain_data);
      if (drain_data) {
        pending_outputs.push_back(drain_data);
        --hwsurfaces_in_queue;
      }
    }

    // Submit input — retry with output draining if input queue is still full (like FFmpeg).
    //
    // AMF SubmitInput return values we explicitly handle (per AMF SimpleEncoder sample):
    //   AMF_OK                          — submitted, count it.
    //   AMF_INPUT_FULL                  — encoder queue full, drain output + retry.
    //   AMF_DECODER_NO_FREE_SURFACES    — surface pool exhausted, semantically equivalent
    //                                     to INPUT_FULL on the input side; treat the same.
    //   AMF_NEED_MORE_INPUT             — frame was absorbed but no output yet (e.g. PA
    //                                     lookahead warming up). Treat as success (count
    //                                     it as in-flight) but skip the output poll loop.
    //   anything else                   — real error.
    res = encoder->SubmitInput(surface);
    if (res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES) {
      // Drain output to free up space in the encoder queue, then retry
      for (int retry = 0; retry < 20 && (res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES); ++retry) {
        ::amf::AMFDataPtr drain_data;
        auto drain_res = encoder->QueryOutput(&drain_data);
        if (drain_data) {
          // Stash the output for later retrieval
          pending_outputs.push_back(drain_data);
          --hwsurfaces_in_queue;
        }
        if (drain_res != AMF_OK && !drain_data) {
          if (!query_timeout_supported) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }
        res = encoder->SubmitInput(surface);
      }
      if (res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES) {
        BOOST_LOG(warning) << "AMF: SubmitInput still " << (res == AMF_INPUT_FULL ? "AMF_INPUT_FULL" : "AMF_DECODER_NO_FREE_SURFACES")
                           << " after retries, dropping frame " << frame_index
                           << " (in_flight=" << hwsurfaces_in_queue << ")";
        frame_rfi_flags.erase(frame_index);
        // Treat sustained INPUT_FULL exhaustion as a submit failure for the
        // watchdog: if the pipeline stays jammed for ~1s of frames the upper
        // layer will reinit instead of silently producing no output forever.
        if (++consecutive_submit_failures >= max_consecutive_failures) {
          BOOST_LOG(error) << "AMF: " << consecutive_submit_failures
                           << " consecutive frames with INPUT_FULL exhaustion, signaling reinit";
          result.fatal = true;
        }
        return result;
      }
    }
    if (res == AMF_NEED_MORE_INPUT) {
      // Frame consumed but encoder isn't producing output yet (typical during
      // pre-analysis / lookahead warm-up). Per AMF SimpleEncoder sample this
      // is a normal "do nothing" case — NOT an error. Count the surface as
      // in-flight so backpressure stays accurate, but skip output polling.
      consecutive_submit_failures = 0;
      ++hwsurfaces_in_queue;
      return result;
    }
    if (res != AMF_OK) {
      BOOST_LOG(error) << "AMF: SubmitInput failed, error: " << res;
      frame_rfi_flags.erase(frame_index);
      // Check if the D3D11 device is lost (TDR, driver crash, etc.)
      if (device) {
        auto removed_reason = device->GetDeviceRemovedReason();
        if (removed_reason != S_OK) {
          BOOST_LOG(error) << "AMF: D3D11 device lost after SubmitInput, reason: 0x" << util::hex(removed_reason).to_string_view();
          result.fatal = true;  // Device gone — must reinit, no point retrying
          return result;
        }
      }
      if (++consecutive_submit_failures >= max_consecutive_failures) {
        BOOST_LOG(error) << "AMF: " << consecutive_submit_failures << " consecutive SubmitInput failures, signaling reinit";
        result.fatal = true;
      }
      return result;
    }
    consecutive_submit_failures = 0;
    ++hwsurfaces_in_queue;

    // Query output — if we already drained output during SubmitInput retry, use that
    if (!pending_outputs.empty()) {
      output_data = pending_outputs.front();
      pending_outputs.pop_front();
      // hwsurfaces_in_queue was already decremented when this output was drained
    }
    else {
      // Poll with retry: encoder may need a moment after SubmitInput
      for (int poll = 0; poll < 10; ++poll) {
        res = encoder->QueryOutput(&output_data);
        if (output_data || (res != AMF_REPEAT && res != AMF_NEED_MORE_INPUT)) {
          break;
        }
        // Only sleep manually if driver doesn't support QUERY_TIMEOUT;
        // when supported, QueryOutput() blocks internally for up to 1ms
        if (!query_timeout_supported) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
      if (!output_data) {
        // Encoder needs more input or no output yet (pipeline filling).
        // Track this in case the pipeline gets stuck (driver hang, PA stall, etc.)
        if (++consecutive_empty_outputs >= max_consecutive_failures) {
          BOOST_LOG(error) << "AMF: " << consecutive_empty_outputs << " consecutive frames with no encoder output, signaling reinit";
          result.fatal = true;
        }
        return result;
      }
      --hwsurfaces_in_queue;
    }
    }
    consecutive_empty_outputs = 0;

    auto output_pts = output_data->GetPts();
    if (output_pts >= 0) {
      result.frame_index = static_cast<uint64_t>(output_pts);
    }
    auto rfi_flag = frame_rfi_flags.find(result.frame_index);
    if (rfi_flag != frame_rfi_flags.end()) {
      result.after_ref_frame_invalidation = rfi_flag->second;
      frame_rfi_flags.erase(rfi_flag);
    }
    while (frame_rfi_flags.size() > 256) {
      frame_rfi_flags.erase(frame_rfi_flags.begin());
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
    auto vbv_size = avcodec_compat_profile ? amf_avcodec_compat::vbv_buffer_size(bitrate_kbps, current_config) : bitrate;
    AMF_RESULT res;

    if (video_format == 0) {
      res = encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
      if (user_configured_rate_control) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
        encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_size);
      }
    }
    else if (video_format == 1) {
      res = encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
      if (user_configured_rate_control) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_size);
      }
    }
    else {
      res = encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitrate);
      if (user_configured_rate_control) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitrate);
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_size);
      }
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
