/**
 * @file src/amf/amf_avcodec_compat.cpp
 * @brief libavcodec AMF compatibility adapter for the standalone AMF encoder.
 */

#include "amf_avcodec_compat.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include <AMF/components/VideoEncoderAV1.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderVCE.h>

#include "src/logging.h"

namespace amf {

  namespace {

    struct avcodec_context_like_t {
      int64_t bit_rate = 0;
      int64_t rc_max_rate = 0;
      int64_t rc_buffer_size = 0;
      int gop_size = std::numeric_limits<int>::max();
      int async_depth = 16;
    };

    avcodec_context_like_t
    make_avcodec_context_like(const video::config_t &client_config) {
      avcodec_context_like_t ctx;
      ctx.bit_rate = static_cast<int64_t>(client_config.bitrate) * 1000;
      ctx.rc_max_rate = ctx.bit_rate;
      ctx.rc_buffer_size = amf_avcodec_compat::vbv_buffer_size(client_config.bitrate, client_config);
      return ctx;
    }

    int
    auto_rc_h264(const amf_config &config, const avcodec_context_like_t &ctx) {
      if (config.rc_mode) return *config.rc_mode;
      if (ctx.bit_rate > 0 && ctx.rc_max_rate == ctx.bit_rate) {
        return AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR;
      }
      return AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
    }

    int
    auto_rc_hevc(const amf_config &config, const avcodec_context_like_t &ctx) {
      if (config.rc_mode) return *config.rc_mode;
      if (ctx.bit_rate > 0 && ctx.rc_max_rate == ctx.bit_rate) {
        return AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR;
      }
      return AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
    }

    int
    auto_rc_av1(const amf_config &config, const avcodec_context_like_t &ctx) {
      if (config.rc_mode) return *config.rc_mode;
      if (ctx.bit_rate > 0 && ctx.rc_max_rate == ctx.bit_rate) {
        return AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR;
      }
      return AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;
    }

    void
    configure_h264(::amf::AMFComponent *encoder,
      const amf_config &config,
      const avcodec_context_like_t &ctx,
      const video::config_t &client_config,
      AMFRate framerate) {
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, (amf_int64) *config.quality_preset);

      const auto rc_mode = auto_rc_h264(config, ctx);
      encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, (amf_int64) rc_mode);
      if (rc_mode == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_QUALITY_VBR && config.qvbr_quality_level) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level);
      }
      if (config.high_motion_quality_boost_enable) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE, *config.high_motion_quality_boost_enable);
      }
      if (ctx.rc_buffer_size) encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, ctx.rc_buffer_size);
      if (ctx.bit_rate) encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, ctx.bit_rate);
      if (rc_mode == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR && ctx.bit_rate) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, ctx.bit_rate);
      }
      if (ctx.rc_max_rate) encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, ctx.rc_max_rate);

      encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, framerate);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64) ctx.gop_size);
      encoder->SetProperty(AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, true);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.vbaq) encoder->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, !!(*config.vbaq));
      encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64) 0);
      if (config.lowlatency_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, !!(*config.lowlatency_mode));
      encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64) 1);
      if (config.intra_refresh_mbs) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT, (amf_int64) *config.intra_refresh_mbs);
      }
      if (client_config.slicesPerFrame > 1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_SLICES_PER_FRAME, (amf_int64) client_config.slicesPerFrame);
      }
      if (config.h264_coding_mode && *config.h264_coding_mode != AMF_VIDEO_ENCODER_UNDEFINED) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_CABAC_ENABLE, (amf_int64) *config.h264_coding_mode);
      }
    }

    void
    configure_hevc(::amf::AMFComponent *encoder,
      const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      const avcodec_context_like_t &ctx,
      AMFRate framerate) {
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, (amf_int64) *config.quality_preset);

      const auto rc_mode = auto_rc_hevc(config, ctx);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, (amf_int64) rc_mode);
      if (rc_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_QUALITY_VBR && config.qvbr_quality_level) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level);
      }
      if (config.high_motion_quality_boost_enable) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_HIGH_MOTION_QUALITY_BOOST_ENABLE, *config.high_motion_quality_boost_enable);
      }
      if (ctx.rc_buffer_size) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, ctx.rc_buffer_size);
      if (ctx.bit_rate) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, ctx.bit_rate);
      if (rc_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR && ctx.bit_rate) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, ctx.bit_rate);
      }
      if (ctx.rc_max_rate) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, ctx.rc_max_rate);

      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, framerate);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64) 1);
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, (amf_int64) ctx.gop_size);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.vbaq) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, !!(*config.vbaq));
      if (config.lowlatency_mode) encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, !!(*config.lowlatency_mode));
      encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, (amf_int64) 1);
      encoder->SetProperty(
        AMF_VIDEO_ENCODER_HEVC_PROFILE,
        (amf_int64) (colorspace.bit_depth == 10 ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10 : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN));
      if (config.intra_refresh_mbs) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INTRA_REFRESH_NUM_CTBS_PER_SLOT, (amf_int64) *config.intra_refresh_mbs);
      }
      if (client_config.slicesPerFrame > 1) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME, (amf_int64) client_config.slicesPerFrame);
      }
    }

    void
    configure_av1(::amf::AMFComponent *encoder,
      const amf_config &config,
      const avcodec_context_like_t &ctx,
      AMFRate framerate) {
      if (config.usage) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, (amf_int64) *config.usage);
      if (config.quality_preset) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, (amf_int64) *config.quality_preset);

      const auto rc_mode = auto_rc_av1(config, ctx);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, (amf_int64) rc_mode);
      if (rc_mode == AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_QUALITY_VBR && config.qvbr_quality_level) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level);
      }
      if (config.high_motion_quality_boost_enable) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_HIGH_MOTION_QUALITY_BOOST, *config.high_motion_quality_boost_enable);
      }
      if (ctx.rc_buffer_size) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, ctx.rc_buffer_size);
      if (ctx.bit_rate) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, ctx.bit_rate);
      if (rc_mode == AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR && ctx.bit_rate) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, ctx.bit_rate);
      }
      if (ctx.rc_max_rate) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, ctx.rc_max_rate);

      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, framerate);
      if (config.enforce_hrd) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, !!(*config.enforce_hrd));
      encoder->SetProperty(
        AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE,
        (amf_int64) AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, (amf_int64) ctx.gop_size);
      if (config.preanalysis) encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis));
      if (config.av1_encoding_latency_mode) {
        encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) *config.av1_encoding_latency_mode);
      }
      encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, (amf_int64) 1);
    }

  }  // namespace

  amf_avcodec_compat_result
  amf_avcodec_compat::configure(::amf::AMFComponent *encoder,
    int video_format,
    const amf_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &colorspace) {
    auto ctx = make_avcodec_context_like(client_config);
    auto framerate = AMFConstructRate(client_config.framerate, 1);
    auto async_depth = config.input_queue_size.value_or(ctx.async_depth);

    if (config.pa_lookahead_depth && *config.pa_lookahead_depth >= async_depth) {
      async_depth = *config.pa_lookahead_depth + 1;
      BOOST_LOG(warning) << "AMF: AVCodec compatibility async_depth raised to " << async_depth
                         << " to exceed PA lookahead depth " << *config.pa_lookahead_depth;
    }

    switch (video_format) {
      case 0:
        configure_h264(encoder, config, ctx, client_config, framerate);
        break;
      case 1:
        configure_hevc(encoder, config, client_config, colorspace, ctx, framerate);
        break;
      default:
        configure_av1(encoder, config, ctx, framerate);
        break;
    }

    BOOST_LOG(info) << "AMF: AVCodec compatibility layer loaded"
                    << " (async_depth=" << async_depth
                    << ", rc_buffer=" << ctx.rc_buffer_size << ")";

    return {
      async_depth,
      true,
    };
  }

  int64_t
  amf_avcodec_compat::vbv_buffer_size(int bitrate_kbps, const video::config_t &client_config) {
    const auto bitrate = static_cast<int64_t>(bitrate_kbps) * 1000;
    const auto effective_fps = std::max(client_config.get_effective_framerate(), 1.0);
    return std::max<int64_t>(static_cast<int64_t>(bitrate / effective_fps), 1);
  }

}  // namespace amf
