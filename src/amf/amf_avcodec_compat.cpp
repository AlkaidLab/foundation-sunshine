/**
 * @file src/amf/amf_avcodec_compat.cpp
 * @brief libavcodec AMF compatibility adapter for the standalone AMF encoder.
 */

#include "amf_avcodec_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#include <AMF/components/VideoEncoderAV1.h>
#include <AMF/components/VideoEncoderHEVC.h>
#include <AMF/components/VideoEncoderVCE.h>

#include "src/logging.h"

namespace amf {

  namespace {

    constexpr int AVCODEC_ASYNC_DEPTH_DEFAULT = 16;
    constexpr int AVCODEC_LOOKAHEAD_DEPTH_MAX = 41;
    constexpr int AVCODEC_ASYNC_DEPTH_MAX = AVCODEC_LOOKAHEAD_DEPTH_MAX + 1;
    constexpr int AVCODEC_BACKPRESSURE_POLL_MAX = 20;
    constexpr int AVCODEC_SUBMIT_RETRY_MAX = 20;
    constexpr int AVCODEC_OUTPUT_POLL_MAX = 10;

    struct avcodec_context_like_t {
      int64_t bit_rate = 0;
      int64_t rc_max_rate = 0;
      int64_t rc_buffer_size = 0;
      int gop_size = std::numeric_limits<int>::max();
      int async_depth = AVCODEC_ASYNC_DEPTH_DEFAULT;
    };

    bool
    is_input_exhausted(AMF_RESULT res) {
      return res == AMF_INPUT_FULL || res == AMF_DECODER_NO_FREE_SURFACES;
    }

    template <class T>
    AMF_RESULT
    set_required_property(::amf::AMFComponent *encoder, const wchar_t *property, T value, const char *label) {
      auto res = encoder->SetProperty(property, value);
      if (res != AMF_OK) {
        BOOST_LOG(error) << "AMF: AVCodec compatibility failed to set " << label << ", error: " << res;
      }
      return res;
    }

    template <class T>
    void
    set_optional_property(::amf::AMFComponent *encoder, const wchar_t *property, T value, const char *label) {
      auto res = encoder->SetProperty(property, value);
      if (res != AMF_OK) {
        BOOST_LOG(warning) << "AMF: AVCodec compatibility ignored unsupported " << label << ", error: " << res;
      }
    }

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

    AMF_RESULT
    configure_h264(::amf::AMFComponent *encoder,
      const amf_config &config,
      const avcodec_context_like_t &ctx,
      const video::config_t &client_config,
      AMFRate framerate) {
      if (config.usage) set_optional_property(encoder, AMF_VIDEO_ENCODER_USAGE, (amf_int64) *config.usage, "H.264 usage");
      if (config.quality_preset) set_optional_property(encoder, AMF_VIDEO_ENCODER_QUALITY_PRESET, (amf_int64) *config.quality_preset, "H.264 quality_preset");

      const auto rc_mode = auto_rc_h264(config, ctx);
      auto res = set_required_property(encoder, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, (amf_int64) rc_mode, "H.264 rate_control_method");
      if (res != AMF_OK) return res;
      if (rc_mode == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_QUALITY_VBR && config.qvbr_quality_level) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level, "H.264 qvbr_quality_level");
      }
      if (config.high_motion_quality_boost_enable) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_HIGH_MOTION_QUALITY_BOOST_ENABLE, *config.high_motion_quality_boost_enable, "H.264 high_motion_quality_boost");
      }
      if (ctx.rc_buffer_size) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, ctx.rc_buffer_size, "H.264 vbv_buffer_size");
        if (res != AMF_OK) return res;
      }
      if (ctx.bit_rate) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_TARGET_BITRATE, ctx.bit_rate, "H.264 target_bitrate");
        if (res != AMF_OK) return res;
      }
      if (ctx.rc_max_rate) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_PEAK_BITRATE, ctx.rc_max_rate, "H.264 peak_bitrate");
        if (res != AMF_OK) return res;
      }

      res = set_required_property(encoder, AMF_VIDEO_ENCODER_FRAMERATE, framerate, "H.264 framerate");
      if (res != AMF_OK) return res;
      if (config.enforce_hrd) set_optional_property(encoder, AMF_VIDEO_ENCODER_ENFORCE_HRD, !!(*config.enforce_hrd), "H.264 enforce_hrd");
      res = set_required_property(encoder, AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64) ctx.gop_size, "H.264 idr_period");
      if (res != AMF_OK) return res;
      set_optional_property(encoder, AMF_VIDEO_ENCODER_DE_BLOCKING_FILTER, true, "H.264 deblocking_filter");
      if (config.preanalysis) set_optional_property(encoder, AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis), "H.264 preanalysis");
      if (config.vbaq) set_optional_property(encoder, AMF_VIDEO_ENCODER_ENABLE_VBAQ, !!(*config.vbaq), "H.264 vbaq");
      set_optional_property(encoder, AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64) 0, "H.264 b_pic_pattern");
      if (config.lowlatency_mode) set_optional_property(encoder, AMF_VIDEO_ENCODER_LOWLATENCY_MODE, !!(*config.lowlatency_mode), "H.264 lowlatency_mode");
      set_optional_property(encoder, AMF_VIDEO_ENCODER_QUERY_TIMEOUT, (amf_int64) 1, "H.264 query_timeout");
      if (config.intra_refresh_mbs) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT, (amf_int64) *config.intra_refresh_mbs, "H.264 intra_refresh_mbs");
      }
      if (client_config.slicesPerFrame > 1) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_SLICES_PER_FRAME, (amf_int64) client_config.slicesPerFrame, "H.264 slices_per_frame");
      }
      if (config.h264_coding_mode && *config.h264_coding_mode != AMF_VIDEO_ENCODER_UNDEFINED) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_CABAC_ENABLE, (amf_int64) *config.h264_coding_mode, "H.264 coding_mode");
      }
      return AMF_OK;
    }

    AMF_RESULT
    configure_hevc(::amf::AMFComponent *encoder,
      const amf_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      const avcodec_context_like_t &ctx,
      AMFRate framerate) {
      if (config.usage) set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_USAGE, (amf_int64) *config.usage, "HEVC usage");
      if (config.quality_preset) set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET, (amf_int64) *config.quality_preset, "HEVC quality_preset");

      const auto rc_mode = auto_rc_hevc(config, ctx);
      auto res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, (amf_int64) rc_mode, "HEVC rate_control_method");
      if (res != AMF_OK) return res;
      if (rc_mode == AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_QUALITY_VBR && config.qvbr_quality_level) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level, "HEVC qvbr_quality_level");
      }
      if (config.high_motion_quality_boost_enable) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_HIGH_MOTION_QUALITY_BOOST_ENABLE, *config.high_motion_quality_boost_enable, "HEVC high_motion_quality_boost");
      }
      if (ctx.rc_buffer_size) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, ctx.rc_buffer_size, "HEVC vbv_buffer_size");
        if (res != AMF_OK) return res;
      }
      if (ctx.bit_rate) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, ctx.bit_rate, "HEVC target_bitrate");
        if (res != AMF_OK) return res;
      }
      if (ctx.rc_max_rate) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, ctx.rc_max_rate, "HEVC peak_bitrate");
        if (res != AMF_OK) return res;
      }

      res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_FRAMERATE, framerate, "HEVC framerate");
      if (res != AMF_OK) return res;
      if (config.enforce_hrd) set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, !!(*config.enforce_hrd), "HEVC enforce_hrd");
      res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, (amf_int64) 1, "HEVC num_gops_per_idr");
      if (res != AMF_OK) return res;
      res = set_required_property(encoder, AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, (amf_int64) ctx.gop_size, "HEVC gop_size");
      if (res != AMF_OK) return res;
      if (config.preanalysis) set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis), "HEVC preanalysis");
      if (config.vbaq) set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, !!(*config.vbaq), "HEVC vbaq");
      if (config.lowlatency_mode) set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, !!(*config.lowlatency_mode), "HEVC lowlatency_mode");
      set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, (amf_int64) 1, "HEVC query_timeout");
      set_optional_property(
        encoder,
        AMF_VIDEO_ENCODER_HEVC_PROFILE,
        (amf_int64) (colorspace.bit_depth == 10 ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10 : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN),
        "HEVC profile");
      if (config.intra_refresh_mbs) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_INTRA_REFRESH_NUM_CTBS_PER_SLOT, (amf_int64) *config.intra_refresh_mbs, "HEVC intra_refresh_mbs");
      }
      if (client_config.slicesPerFrame > 1) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME, (amf_int64) client_config.slicesPerFrame, "HEVC slices_per_frame");
      }
      return AMF_OK;
    }

    AMF_RESULT
    configure_av1(::amf::AMFComponent *encoder,
      const amf_config &config,
      const avcodec_context_like_t &ctx,
      AMFRate framerate) {
      if (config.usage) set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_USAGE, (amf_int64) *config.usage, "AV1 usage");
      if (config.quality_preset) set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET, (amf_int64) *config.quality_preset, "AV1 quality_preset");

      const auto rc_mode = auto_rc_av1(config, ctx);
      auto res = set_required_property(encoder, AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, (amf_int64) rc_mode, "AV1 rate_control_method");
      if (res != AMF_OK) return res;
      if (rc_mode == AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_QUALITY_VBR && config.qvbr_quality_level) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_QVBR_QUALITY_LEVEL, (amf_int64) *config.qvbr_quality_level, "AV1 qvbr_quality_level");
      }
      if (config.high_motion_quality_boost_enable) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_HIGH_MOTION_QUALITY_BOOST, *config.high_motion_quality_boost_enable, "AV1 high_motion_quality_boost");
      }
      if (ctx.rc_buffer_size) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, ctx.rc_buffer_size, "AV1 vbv_buffer_size");
        if (res != AMF_OK) return res;
      }
      if (ctx.bit_rate) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, ctx.bit_rate, "AV1 target_bitrate");
        if (res != AMF_OK) return res;
      }
      if (ctx.rc_max_rate) {
        res = set_required_property(encoder, AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, ctx.rc_max_rate, "AV1 peak_bitrate");
        if (res != AMF_OK) return res;
      }

      res = set_required_property(encoder, AMF_VIDEO_ENCODER_AV1_FRAMERATE, framerate, "AV1 framerate");
      if (res != AMF_OK) return res;
      if (config.enforce_hrd) set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, !!(*config.enforce_hrd), "AV1 enforce_hrd");
      res = set_required_property(
        encoder,
        AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE,
        (amf_int64) AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS,
        "AV1 alignment_mode");
      if (res != AMF_OK) return res;
      res = set_required_property(encoder, AMF_VIDEO_ENCODER_AV1_GOP_SIZE, (amf_int64) ctx.gop_size, "AV1 gop_size");
      if (res != AMF_OK) return res;
      if (config.preanalysis) set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE, !!(*config.preanalysis), "AV1 preanalysis");
      if (config.av1_encoding_latency_mode) {
        set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, (amf_int64) *config.av1_encoding_latency_mode, "AV1 encoding_latency_mode");
      }
      set_optional_property(encoder, AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, (amf_int64) 1, "AV1 query_timeout");
      return AMF_OK;
    }

  }  // namespace

  void
  amf_avcodec_scheduler::configure(int max_in_queue, bool query_timeout) {
    hwsurfaces_in_queue_max = std::max(max_in_queue, 1);
    query_timeout_supported = query_timeout;
  }

  void
  amf_avcodec_scheduler::reset() {
    output_list.clear();
    hwsurfaces_in_queue = 0;
  }

  int
  amf_avcodec_scheduler::in_flight() const {
    return hwsurfaces_in_queue;
  }

  AMF_RESULT
  amf_avcodec_scheduler::query_once(::amf::AMFComponent *encoder, ::amf::AMFDataPtr &output_data) {
    output_data = nullptr;
    auto res = encoder->QueryOutput(&output_data);
    if (output_data) {
      decrement_in_flight();
    }
    return res;
  }

  void
  amf_avcodec_scheduler::sleep_if_needed(bool already_have_output) const {
    if (!query_timeout_supported || already_have_output) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void
  amf_avcodec_scheduler::decrement_in_flight() {
    if (hwsurfaces_in_queue > 0) {
      --hwsurfaces_in_queue;
    }
  }

  amf_avcodec_scheduler_result
  amf_avcodec_scheduler::submit_and_query(::amf::AMFComponent *encoder, const ::amf::AMFSurfacePtr &surface) {
    amf_avcodec_scheduler_result result;

    if (hwsurfaces_in_queue >= hwsurfaces_in_queue_max) {
      for (int wait = 0; wait < AVCODEC_BACKPRESSURE_POLL_MAX && hwsurfaces_in_queue >= hwsurfaces_in_queue_max; ++wait) {
        result.query_result = query_once(encoder, result.output_data);
        if (result.output_data) {
          output_list.push_back(result.output_data);
          result.output_data = nullptr;
          break;
        }
        sleep_if_needed(false);
      }
      if (hwsurfaces_in_queue >= hwsurfaces_in_queue_max) {
        result.submit_result = AMF_INPUT_FULL;
        result.input_exhausted = true;
        return result;
      }
    }

    result.submit_result = encoder->SubmitInput(surface);
    if (is_input_exhausted(result.submit_result)) {
      for (int retry = 0; retry < AVCODEC_SUBMIT_RETRY_MAX && is_input_exhausted(result.submit_result); ++retry) {
        ::amf::AMFDataPtr drain_data;
        result.query_result = query_once(encoder, drain_data);
        if (drain_data) {
          output_list.push_back(drain_data);
        }
        else {
          sleep_if_needed(false);
        }
        result.submit_result = encoder->SubmitInput(surface);
      }

      if (is_input_exhausted(result.submit_result)) {
        result.input_exhausted = true;
        return result;
      }
    }

    if (result.submit_result == AMF_NEED_MORE_INPUT) {
      result.submitted = true;
      result.need_more_input = true;
      ++hwsurfaces_in_queue;
      return result;
    }

    if (result.submit_result != AMF_OK) {
      return result;
    }

    result.submitted = true;
    ++hwsurfaces_in_queue;

    if (!output_list.empty()) {
      // Unlike libavcodec receive_packet(), Sunshine has already handed us the
      // current input frame. Submit it first, then return previously drained
      // output so we do not silently skip the current surface.
      result.output_data = output_list.front();
      output_list.pop_front();
      return result;
    }

    for (int poll = 0; poll < AVCODEC_OUTPUT_POLL_MAX; ++poll) {
      result.query_result = query_once(encoder, result.output_data);
      if (result.output_data || (result.query_result != AMF_REPEAT && result.query_result != AMF_NEED_MORE_INPUT)) {
        break;
      }
      sleep_if_needed(false);
    }

    return result;
  }

  amf_avcodec_compat_result
  amf_avcodec_compat::configure(::amf::AMFComponent *encoder,
    int video_format,
    const amf_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &colorspace) {
    auto ctx = make_avcodec_context_like(client_config);
    auto framerate = AMFConstructRate(client_config.framerate, 1);
    auto requested_async_depth = config.input_queue_size.value_or(ctx.async_depth);
    auto async_depth = std::clamp(requested_async_depth, 1, AVCODEC_ASYNC_DEPTH_MAX);

    if (async_depth != requested_async_depth) {
      BOOST_LOG(warning) << "AMF: AVCodec compatibility async_depth clamped from "
                         << requested_async_depth << " to " << async_depth;
    }

    if (config.pa_lookahead_depth) {
      const auto lookahead_depth = std::clamp(*config.pa_lookahead_depth, 0, AVCODEC_LOOKAHEAD_DEPTH_MAX);
      if (lookahead_depth != *config.pa_lookahead_depth) {
        BOOST_LOG(warning) << "AMF: AVCodec compatibility PA lookahead depth clamped from "
                           << *config.pa_lookahead_depth << " to " << lookahead_depth;
      }
      if (lookahead_depth >= async_depth) {
        async_depth = std::min(lookahead_depth + 1, AVCODEC_ASYNC_DEPTH_MAX);
        BOOST_LOG(warning) << "AMF: AVCodec compatibility async_depth raised to " << async_depth
                           << " to exceed PA lookahead depth " << lookahead_depth;
      }
    }

    AMF_RESULT configure_result = AMF_OK;
    switch (video_format) {
      case 0:
        configure_result = configure_h264(encoder, config, ctx, client_config, framerate);
        break;
      case 1:
        configure_result = configure_hevc(encoder, config, client_config, colorspace, ctx, framerate);
        break;
      default:
        configure_result = configure_av1(encoder, config, ctx, framerate);
        break;
    }

    BOOST_LOG(info) << "AMF: AVCodec compatibility layer loaded"
                    << " (async_depth=" << async_depth
                    << ", rc_buffer=" << ctx.rc_buffer_size << ")";

    return {
      async_depth,
      true,
      configure_result,
    };
  }

  int64_t
  amf_avcodec_compat::vbv_buffer_size(int bitrate_kbps, const video::config_t &client_config) {
    const auto bitrate = static_cast<int64_t>(bitrate_kbps) * 1000;
    const auto effective_fps = std::max(client_config.get_effective_framerate(), 1.0);
    return std::max<int64_t>(static_cast<int64_t>(bitrate / effective_fps), 1);
  }

}  // namespace amf
