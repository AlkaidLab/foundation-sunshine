/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <algorithm>
#include <array>
#include <iterator>
#include <atomic>
#include <bitset>
#include <functional>
#include <list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/hdr_dynamic_vivid_metadata.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// lib includes
#include "cbs.h"
#include "config.h"
#include "display_device/display_device.h"
#include "globals.h"
#include "hdr/dynamic_hdr_selection.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_encoder.h"
#include "amf/amf_encoder.h"
#include "platform/common.h"
#include "sync.h"
#include "video.h"
#include "video_dolby_vision.h"
#include "video_hdr_metadata.h"
#include "video_probe.h"

#ifdef _WIN32
extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}
  #include "platform/windows/display_device/windows_utils.h"
#endif

using namespace std::literals;
namespace video {

  namespace {
    std::mutex hdr_pipeline_status_mutex;
    std::map<std::uint64_t, hdr_pipeline_status_t> hdr_pipeline_statuses;
    std::atomic<std::uint64_t> next_hdr_pipeline_status_id { 1 };

    std::optional<std::string>
    capture_override_for_encoder_probe() {
#ifdef _WIN32
      // VDD shared-texture producer may not be ready (metadata mapping / KeyedMutex
      // not yet published) at encoder-probe time. Probing the real VDD backend
      // therefore tends to fail on cold start, even though runtime capture works
      // fine once the producer comes up. Fall back to ddx for the probe only;
      // this override is injected per-display via config_t::capture_backend_override
      // so it does not mutate the global config::video.capture used at runtime.
      if (config::video.capture == "vdd") {
        return std::string { "ddx" };
      }
#endif
      return std::nullopt;
    }

    /**
     * @brief Check if we can allow probing for the encoders.
     * @return True if there should be no issues with the probing, false if we should prevent it.
     */
    bool
    allow_encoder_probing() {
      const auto devices { display_device::enum_available_devices() };

      // If there are no devices, then either the API is not working correctly or OS does not support the lib.
      // Either way we should not block the probing in this case as we can't tell what's wrong.
      if (devices.empty()) {
        return true;
      }

      // Since Windows 11 24H2, it is possible that there will be no active devices present
      // for some reason (probably a bug). Trying to probe encoders in such a state locks/breaks the DXGI
      // and also the display device for Windows. So we must have at least 1 active device.
      const bool at_least_one_device_is_active = std::any_of(std::begin(devices), std::end(devices), [](const auto &device) {
        // If device has additional info, it is active.
        return device.second.device_state == display_device::device_state_e::active ||
               device.second.device_state == display_device::device_state_e::primary;
      });

      if (at_least_one_device_is_active) {
        return true;
      }

      BOOST_LOG(error) << "No display devices are active at the moment! Cannot probe the encoders.";
      last_encoder_probe_result = {
        probe_error_e::no_active_display,
        "No active display devices are available for capture.",
        "Turn on a physical display, enable a virtual display, or set Sunshine display/VDD options to Auto and try again."
      };
      return false;
    }
  }  // namespace

  std::uint64_t
  register_hdr_pipeline_status(const hdr_pipeline_status_t &status) {
    const auto id = next_hdr_pipeline_status_id.fetch_add(1, std::memory_order_relaxed);
    auto registered = status;
    registered.id = id;

    std::lock_guard lock { hdr_pipeline_status_mutex };
    hdr_pipeline_statuses[id] = std::move(registered);
    return id;
  }

  void
  update_hdr_pipeline_status(std::uint64_t id, const hdr_pipeline_status_t &status) {
    if (id == 0) {
      return;
    }

    auto updated = status;
    updated.id = id;
    std::lock_guard lock { hdr_pipeline_status_mutex };
    if (hdr_pipeline_statuses.contains(id)) {
      hdr_pipeline_statuses[id] = std::move(updated);
    }
  }

  void
  unregister_hdr_pipeline_status(std::uint64_t id) {
    if (id == 0) {
      return;
    }

    std::lock_guard lock { hdr_pipeline_status_mutex };
    hdr_pipeline_statuses.erase(id);
  }

  std::vector<hdr_pipeline_status_t>
  get_hdr_pipeline_statuses() {
    std::lock_guard lock { hdr_pipeline_status_mutex };
    std::vector<hdr_pipeline_status_t> statuses;
    statuses.reserve(hdr_pipeline_statuses.size());
    for (const auto &[id, status] : hdr_pipeline_statuses) {
      statuses.push_back(status);
    }
    return statuses;
  }

  int
  encoder_bitrate_from_total_bitrate(int total_bitrate_kbps, int fec_percentage) {
    if (fec_percentage > 0 && fec_percentage <= 80) {
      return total_bitrate_kbps * (100 - fec_percentage) / 100;
    }

    return total_bitrate_kbps;
  }

  int
  encoder_bitrate_for_total_request(int requested_total_bitrate_kbps, int max_total_bitrate_kbps, int fec_percentage) {
    auto capped_total_bitrate_kbps = requested_total_bitrate_kbps;
    if (max_total_bitrate_kbps > 0) {
      capped_total_bitrate_kbps = std::min(capped_total_bitrate_kbps, max_total_bitrate_kbps);
    }

    return encoder_bitrate_from_total_bitrate(capped_total_bitrate_kbps, fec_percentage);
  }

  int
  cap_initial_encoder_bitrate(int initial_encoder_bitrate_kbps, int max_total_bitrate_kbps, int fec_percentage) {
    if (max_total_bitrate_kbps <= 0) {
      return initial_encoder_bitrate_kbps;
    }

    return std::min(
      initial_encoder_bitrate_kbps,
      encoder_bitrate_from_total_bitrate(max_total_bitrate_kbps, fec_percentage)
    );
  }

  std::chrono::duration<double, std::milli>
  minimum_frame_time_for_vrr(int stream_fps, int minimum_fps_target) {
    if (minimum_fps_target > 0) {
      return std::chrono::duration<double, std::milli> { 1000.0 / minimum_fps_target };
    }

    return std::chrono::duration<double, std::milli> { 2000.0 / std::max(stream_fps, 1) };
  }

  input_activity_boost_policy_t
  make_input_activity_boost_policy(const input_activity_boost_config_t &config) {
    input_activity_boost_policy_t policy {};
    policy.configured =
      config.variable_refresh_rate &&
      config.enabled &&
      config.boost_fps > 0 &&
      config.window_ms > 0;

    if (!policy.configured) {
      return policy;
    }

    policy.fps = std::min(config.boost_fps, std::max(config.stream_fps, 1));
    policy.frame_time = std::chrono::duration<double, std::milli> { 1000.0 / policy.fps };
    policy.useful = config.minimum_fps_target == 0 || policy.fps > config.minimum_fps_target;

    return policy;
  }

  std::chrono::duration<double, std::milli>
  effective_minimum_frame_time(
    const std::chrono::duration<double, std::milli> &base_minimum_frame_time,
    const input_activity_boost_policy_t &input_activity_boost_policy,
    bool input_boost_active,
    int minimum_fps_target) {
    if (!input_boost_active || !input_activity_boost_policy.useful) {
      return base_minimum_frame_time;
    }

    if (minimum_fps_target > 0) {
      return std::min(base_minimum_frame_time, input_activity_boost_policy.frame_time);
    }

    return input_activity_boost_policy.frame_time;
  }

  void
  free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  void
  free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  void
  free_buffer(AVBufferRef *ref) {
    av_buffer_unref(&ref);
  }

  namespace nv {

    enum class profile_h264_e : int {
      high = 2,  ///< High profile
      high_444p = 3,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 0,  ///< Main profile
      main_10 = 1,  ///< Main 10 profile
      rext = 2,  ///< Rext profile
    };

  }  // namespace nv

  namespace qsv {

    enum class profile_h264_e : int {
      high = 100,  ///< High profile
      high_444p = 244,  ///< High 4:4:4 Predictive profile
    };

    enum class profile_hevc_e : int {
      main = 1,  ///< Main profile
      main_10 = 2,  ///< Main 10 profile
      rext = 4,  ///< RExt profile
    };

    enum class profile_av1_e : int {
      main = 1,  ///< Main profile
      high = 2,  ///< High profile
    };

  }  // namespace qsv

  util::Either<avcodec_buffer_t, int>
  dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int>
  vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int>
  cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int>
  vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  util::Either<avcodec_buffer_t, int>
  vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);

  class avcodec_software_encode_device_t: public platf::avcodec_encode_device_t {
  public:
    int
    convert(platf::img_t &img) override {
      // If we need to add aspect ratio padding, we need to scale into an intermediate output buffer
      bool requires_padding = (sw_frame->width != sws_output_frame->width || sw_frame->height != sws_output_frame->height);

      // Setup the input frame using the caller's img_t
      sws_input_frame->data[0] = img.data;
      sws_input_frame->linesize[0] = img.row_pitch;

      // Perform color conversion and scaling to the final size
      auto status = sws_scale_frame(sws.get(), requires_padding ? sws_output_frame.get() : sw_frame.get(), sws_input_frame.get());
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Couldn't scale frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      // If we require aspect ratio padding, copy the output frame into the final padded frame
      if (requires_padding) {
        auto fmt_desc = av_pix_fmt_desc_get((AVPixelFormat) sws_output_frame->format);
        auto planes = av_pix_fmt_count_planes((AVPixelFormat) sws_output_frame->format);
        for (int plane = 0; plane < planes; plane++) {
          auto shift_h = plane == 0 ? 0 : fmt_desc->log2_chroma_h;
          auto shift_w = plane == 0 ? 0 : fmt_desc->log2_chroma_w;
          auto offset = ((offsetW >> shift_w) * fmt_desc->comp[plane].step) + (offsetH >> shift_h) * sw_frame->linesize[plane];

          // Copy line-by-line to preserve leading padding for each row
          for (int line = 0; line < sws_output_frame->height >> shift_h; line++) {
            memcpy(sw_frame->data[plane] + offset + (line * sw_frame->linesize[plane]),
              sws_output_frame->data[plane] + (line * sws_output_frame->linesize[plane]),
              (size_t) (sws_output_frame->width >> shift_w) * fmt_desc->comp[plane].step);
          }
        }
      }

      // If frame is not a software frame, it means we still need to transfer from main memory
      // to vram memory
      if (frame->hw_frames_ctx) {
        auto status = av_hwframe_transfer_data(frame, sw_frame.get(), 0);
        if (status < 0) {
          char string[AV_ERROR_MAX_STRING_SIZE];
          BOOST_LOG(error) << "Failed to transfer image data to hardware frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
          return -1;
        }
      }

      return 0;
    }

    int
    set_frame(AVFrame *frame, AVBufferRef *hw_frames_ctx) override {
      this->frame = frame;

      // If it's a hwframe, allocate buffers for hardware
      if (hw_frames_ctx) {
        hw_frame.reset(frame);

        if (av_hwframe_get_buffer(hw_frames_ctx, frame, 0)) return -1;
      }
      else {
        sw_frame.reset(frame);
      }

      return 0;
    }

    void
    apply_colorspace() override {
      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);
      sws_setColorspaceDetails(sws.get(),
        sws_getCoefficients(SWS_CS_DEFAULT), 0,
        sws_getCoefficients(avcodec_colorspace.software_format), avcodec_colorspace.range - 1,
        0, 1 << 16, 1 << 16);
    }

    /**
     * When preserving aspect ratio, ensure that padding is black
     */
    void
    prefill() {
      auto frame = sw_frame ? sw_frame.get() : this->frame;
      av_frame_get_buffer(frame, 0);
      av_frame_make_writable(frame);
      ptrdiff_t linesize[4] = { frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->linesize[3] };
      av_image_fill_black(frame->data, linesize, (AVPixelFormat) frame->format, frame->color_range, frame->width, frame->height);
    }

    int
    init(int in_width, int in_height, AVFrame *frame, AVPixelFormat format, bool hardware) {
      // If the device used is hardware, yet the image resides on main memory
      if (hardware) {
        sw_frame.reset(av_frame_alloc());

        sw_frame->width = frame->width;
        sw_frame->height = frame->height;
        sw_frame->format = format;
      }
      else {
        this->frame = frame;
      }

      // Fill aspect ratio padding in the destination frame
      prefill();

      auto out_width = frame->width;
      auto out_height = frame->height;

      // Ensure aspect ratio is maintained
      auto scalar = std::fminf((float) out_width / in_width, (float) out_height / in_height);
      out_width = in_width * scalar;
      out_height = in_height * scalar;

      sws_input_frame.reset(av_frame_alloc());
      sws_input_frame->width = in_width;
      sws_input_frame->height = in_height;
      sws_input_frame->format = AV_PIX_FMT_BGR0;

      sws_output_frame.reset(av_frame_alloc());
      sws_output_frame->width = out_width;
      sws_output_frame->height = out_height;
      sws_output_frame->format = format;

      // Result is always positive
      offsetW = (frame->width - out_width) / 2;
      offsetH = (frame->height - out_height) / 2;

      sws.reset(sws_alloc_context());
      if (!sws) {
        return -1;
      }

      AVDictionary *options { nullptr };
      av_dict_set_int(&options, "srcw", sws_input_frame->width, 0);
      av_dict_set_int(&options, "srch", sws_input_frame->height, 0);
      av_dict_set_int(&options, "src_format", sws_input_frame->format, 0);
      av_dict_set_int(&options, "dstw", sws_output_frame->width, 0);
      av_dict_set_int(&options, "dsth", sws_output_frame->height, 0);
      av_dict_set_int(&options, "dst_format", sws_output_frame->format, 0);
      av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
      av_dict_set_int(&options, "threads", config::video.min_threads, 0);

      auto status = av_opt_set_dict(sws.get(), &options);
      av_dict_free(&options);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to set SWS options: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      status = sws_init_context(sws.get(), nullptr, nullptr);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to initialize SWS: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }

      return 0;
    }

    // Store ownership when frame is hw_frame
    avcodec_frame_t hw_frame;

    avcodec_frame_t sw_frame;
    avcodec_frame_t sws_input_frame;
    avcodec_frame_t sws_output_frame;
    sws_t sws;

    // Offset of input image to output frame in pixels
    int offsetW;
    int offsetH;
  };

  enum flag_e : uint32_t {
    DEFAULT = 0,  ///< Default flags
    PARALLEL_ENCODING = 1 << 1,  ///< Capture and encoding can run concurrently on separate threads
    H264_ONLY = 1 << 2,  ///< When HEVC is too heavy
    LIMITED_GOP_SIZE = 1 << 3,  ///< Some encoders don't like it when you have an infinite GOP_SIZE. e.g. VAAPI
    SINGLE_SLICE_ONLY = 1 << 4,  ///< Never use multiple slices. Older intel iGPU's ruin it for everyone else
    CBR_WITH_VBR = 1 << 5,  ///< Use a VBR rate control mode to simulate CBR
    RELAXED_COMPLIANCE = 1 << 6,  ///< Use FF_COMPLIANCE_UNOFFICIAL compliance mode
    NO_RC_BUF_LIMIT = 1 << 7,  ///< Don't set rc_buffer_size
    REF_FRAMES_INVALIDATION = 1 << 8,  ///< Support reference frames invalidation
    ALWAYS_REPROBE = 1 << 9,  ///< This is an encoder of last resort and we want to aggressively probe for a better one
    YUV444_SUPPORT = 1 << 10,  ///< Encoder may support 4:4:4 chroma sampling depending on hardware
    ASYNC_TEARDOWN = 1 << 11,  ///< Encoder supports async teardown on a different thread
  };

  class frame_timestamp_ring_t {
  public:
    void
    store(
      uint64_t frame_index,
      std::optional<std::chrono::steady_clock::time_point> timestamp,
      std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
      auto &entry = entries[frame_index % entries.size()];
      entry.frame_index = frame_index;
      entry.timestamp = timestamp;
      entry.pipeline_trace = std::move(pipeline_trace);
    }

    std::optional<std::chrono::steady_clock::time_point>
    lookup(uint64_t frame_index) const {
      const auto &entry = entries[frame_index % entries.size()];
      if (entry.frame_index != frame_index) {
        return std::nullopt;
      }
      return entry.timestamp;
    }

    std::optional<platf::frame_pipeline_trace_t>
    lookup_trace(uint64_t frame_index) const {
      const auto &entry = entries[frame_index % entries.size()];
      if (entry.frame_index != frame_index) {
        return std::nullopt;
      }
      return entry.pipeline_trace;
    }

  private:
    // Encoder output can lag submission; keep recent per-frame timing data without heap churn.
    struct entry_t {
      uint64_t frame_index = std::numeric_limits<uint64_t>::max();
      std::optional<std::chrono::steady_clock::time_point> timestamp;
      std::optional<platf::frame_pipeline_trace_t> pipeline_trace;
    };

    std::array<entry_t, 256> entries {};
  };

  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;
    avcodec_encode_session_t(
      avcodec_ctx_t &&avcodec_ctx,
      std::unique_ptr<platf::avcodec_encode_device_t> encode_device,
      uint16_t dynamic_metadata_target_peak_nits,
      int inject):
        avcodec_ctx { std::move(avcodec_ctx) },
        device { std::move(encode_device) },
        dynamic_metadata_target_peak_nits { dynamic_metadata_target_peak_nits },
        inject { inject } {}

    avcodec_encode_session_t(avcodec_encode_session_t &&other) noexcept = default;
    ~avcodec_encode_session_t() {
      // Flush any remaining frames in the encoder
      if (avcodec_send_frame(avcodec_ctx.get(), nullptr) == 0) {
        packet_raw_avcodec pkt;
        while (avcodec_receive_packet(avcodec_ctx.get(), pkt.av_packet) == 0);
      }

      // Order matters here because the context relies on the hwdevice still being valid
      avcodec_ctx.reset();
      device.reset();
    }

    // Ensure objects are destroyed in the correct order
    avcodec_encode_session_t &
    operator=(avcodec_encode_session_t &&other) {
      device = std::move(other.device);
      avcodec_ctx = std::move(other.avcodec_ctx);
      replacements = std::move(other.replacements);
      frame_timestamps = std::move(other.frame_timestamps);
      dynamic_metadata_temporal = std::move(other.dynamic_metadata_temporal);
      dynamic_metadata_target_peak_nits = other.dynamic_metadata_target_peak_nits;
      sps = std::move(other.sps);
      vps = std::move(other.vps);

      inject = other.inject;

      return *this;
    }

    int
    convert(platf::img_t &img) override {
      if (!device) return -1;
      return device->convert(img);
    }

    void
    request_idr_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
      }
    }

    void
    request_normal_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
      }
    }

    void
    invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      BOOST_LOG(error) << "Encoder doesn't support reference frame invalidation";
      request_idr_frame();
    }

    void
    set_bitrate(int bitrate_kbps) override {
      if (!avcodec_ctx) return;

      const auto adjusted_bitrate_kbps = encoder_bitrate_for_total_request(
        bitrate_kbps,
        config::video.max_bitrate,
        config::stream.fec_percentage
      );

      auto bitrate = static_cast<int64_t>(adjusted_bitrate_kbps) * 1000;  // Convert to bps

      // Update AVCodecContext fields (for software encoders and as fallback).
      // Note: dynamic bitrate changes for the AMF path are handled inside the
      // native amf_d3d11 encoder via amf_d3d11::set_bitrate(), so the legacy
      // FFmpeg-AMF reach-into-priv_data hack has been removed.
      avcodec_ctx->bit_rate = bitrate;
      avcodec_ctx->rc_max_rate = bitrate;
      avcodec_ctx->rc_min_rate = bitrate;

      BOOST_LOG(info) << "AVCodec encoder bitrate set to: " << adjusted_bitrate_kbps
                      << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                      << config::stream.fec_percentage << "%)";
    }

    void
    set_dynamic_param(const dynamic_param_t &param) override {
      if (!avcodec_ctx) return;

      switch (param.type) {
        case dynamic_param_type_e::RESOLUTION:
          // 分辨率变更需要重新初始化编码器
          BOOST_LOG(info) << "AVCodec encoder: Resolution change requested (requires encoder reinitialization)";
          break;
        case dynamic_param_type_e::FPS:
          // FPS变更需要重新配置编码器
          BOOST_LOG(info) << "AVCodec encoder: FPS change requested: " << param.value.float_value
                          << " fps (requires encoder reconfiguration)";
          break;
        case dynamic_param_type_e::BITRATE: {
          // 码率调整通过set_bitrate处理
          set_bitrate(param.value.int_value);
          break;
        }
        case dynamic_param_type_e::QP: {
          // 设置量化参数
          if (param.value.int_value >= 0 && param.value.int_value <= 51) {
            avcodec_ctx->qmin = param.value.int_value;
            avcodec_ctx->qmax = param.value.int_value;
            BOOST_LOG(info) << "AVCodec encoder QP changed to: " << param.value.int_value;
          }
          else {
            BOOST_LOG(warning) << "Invalid QP value: " << param.value.int_value << " (must be 0-51)";
          }
          break;
        }
        case dynamic_param_type_e::VBV_BUFFER_SIZE: {
          // 设置VBV缓冲区大小
          if (param.value.int_value > 0) {
            avcodec_ctx->rc_buffer_size = param.value.int_value * 1000;  // 转换为bps
            BOOST_LOG(info) << "AVCodec encoder VBV buffer size changed to: " << param.value.int_value << " Kbps";
          }
          break;
        }
        case dynamic_param_type_e::CLIENT_SDR_WHITE_NITS:
          device->set_client_sdr_white_nits(param.value.float_value);
          break;
        default:
          BOOST_LOG(warning) << "AVCodec encoder: Unsupported dynamic parameter type: " << (int) param.type;
          break;
      }
    }

    avcodec_ctx_t avcodec_ctx;
    std::unique_ptr<platf::avcodec_encode_device_t> device;

    std::vector<packet_raw_t::replace_t> replacements;
    frame_timestamp_ring_t frame_timestamps;

    // Scene-aware temporal state is session-local so a new stream cannot inherit
    // metadata history from the previous stream.
    hdr_metadata::dynamic_metadata_temporal_state_t dynamic_metadata_temporal;
    uint16_t dynamic_metadata_target_peak_nits = 1000;

    cbs::nal_t sps;
    cbs::nal_t vps;

    // inject sps/vps data into idr pictures
    int inject;
  };

  /**
   * Whether the HDR luminance analyzer can be trusted to produce samples for this
   * encode device: the user has not turned it off and the capture backend actually
   * implements it.
   */
  inline bool
  hdr_luminance_analysis_usable(bool device_supports_analysis) {
    return config::video.hdr_luminance_analysis != "off" && device_supports_analysis;
  }

  /**
   * Report a vivid_startup_gate_t transition. Shared so the two native encoder
   * paths cannot drift into describing the same decision differently in the log.
   */
  void
  log_vivid_gate_transition(
    const char *encoder_name,
    hdr_metadata::vivid_startup_gate_t::transition_e transition,
    const hdr_metadata::vivid_startup_gate_t &gate,
    const platf::hdr_frame_luminance_stats_t &stats) {
    using transition_e = hdr_metadata::vivid_startup_gate_t::transition_e;

    switch (transition) {
      case transition_e::ready:
        BOOST_LOG(info) << encoder_name << ": HDR Vivid startup guard ready after "
                        << gate.consecutive_samples()
                        << " independent samples; first encoded HLG frame will be IDR with Vivid"
                        << " (avg=" << stats.avg_maxrgb
                        << " nits, max=" << stats.max_maxrgb
                        << " nits, P10=" << stats.percentile_10_pq
                        << ", P90=" << stats.percentile_90_pq << ')';
        break;
      case transition_e::timed_out:
        BOOST_LOG(warning) << encoder_name << ": HDR Vivid startup guard timed out after "
                           << hdr_metadata::vivid_startup_gate_t::PREROLL_TIMEOUT.count()
                           << " ms; temporarily starting as plain HLG while analysis continues"
                           << " (samples=" << gate.consecutive_samples()
                           << '/' << hdr_metadata::vivid_startup_guard_t::REQUIRED_SAMPLES
                           << ", sequence=" << stats.sample_sequence
                           << ", valid=" << stats.valid
                           << ", avg=" << stats.avg_maxrgb
                           << " nits, max=" << stats.max_maxrgb << " nits)";
        break;
      case transition_e::recovered:
        BOOST_LOG(info) << encoder_name << ": HDR Vivid startup guard recovered after plain-HLG fallback; "
                        << "switching to Vivid at IDR"
                        << " (samples=" << gate.consecutive_samples()
                        << '/' << hdr_metadata::vivid_startup_guard_t::REQUIRED_SAMPLES
                        << ", sequence=" << stats.sample_sequence
                        << ", valid=" << stats.valid
                        << ", avg=" << stats.avg_maxrgb
                        << " nits, max=" << stats.max_maxrgb << " nits)";
        break;
      case transition_e::none:
        break;
    }
  }

  class nvenc_encode_session_t: public encode_session_t {
  public:
    nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device, int video_format):
        device(std::move(encode_device)),
        vivid_gate(
          device ? device->colorspace : sunshine_colorspace_t {},
          video_format,
          device && hdr_luminance_analysis_usable(device->hdr_luminance_analysis_available)) {
      if (vivid_gate.prerolling()) {
        BOOST_LOG(info) << "NVENC: holding HLG startup for stable HDR Vivid metadata ("
                        << hdr_metadata::vivid_startup_guard_t::REQUIRED_SAMPLES
                        << " independent samples, "
                        << hdr_metadata::vivid_startup_gate_t::PREROLL_TIMEOUT.count()
                        << " ms timeout)";
      }
    }

    /// Session-level Dolby Vision state; inert until the negotiated session
    /// enables it in make_nvenc_encode_session().
    dolby_vision::rpu_injector_t dolby_vision_;

    int
    convert(platf::img_t &img) override {
      if (!device) return -1;
      return device->convert(img);
    }

    void
    request_idr_frame() override {
      force_idr = true;
    }

    void
    request_normal_frame() override {
      force_idr = false;
    }

    void
    invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->nvenc) return;

      if (!device->nvenc->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    void
    set_bitrate(int bitrate_kbps) override {
      if (device && device->nvenc) {
        // 考虑FEC影响，调整编码码率
        // 当FEC百分比为X%时，实际编码码率需要调整为原始码率的(100-X)%
        const auto adjusted_bitrate_kbps = encoder_bitrate_for_total_request(
          bitrate_kbps,
          config::video.max_bitrate,
          config::stream.fec_percentage
        );

        device->nvenc->set_bitrate(adjusted_bitrate_kbps);
        BOOST_LOG(info) << "NVENC encoder bitrate changed to: " << adjusted_bitrate_kbps
                        << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                        << config::stream.fec_percentage << "%)";
      }
    }

    void
    set_dynamic_param(const dynamic_param_t &param) override {
      if (!device || !device->nvenc) return;

      switch (param.type) {
        case dynamic_param_type_e::RESOLUTION:
          // 分辨率变更需要重新初始化编码器，这里只记录日志
          BOOST_LOG(info) << "NVENC encoder: Resolution change requested (requires encoder reinitialization)";
          break;
        case dynamic_param_type_e::FPS:
          // FPS变更需要重新配置编码器
          BOOST_LOG(info) << "NVENC encoder: FPS change requested: " << param.value.float_value
                          << " fps (requires encoder reconfiguration)";
          break;
        case dynamic_param_type_e::BITRATE: {
          // 码率调整通过set_bitrate处理
          set_bitrate(param.value.int_value);
          break;
        }
        case dynamic_param_type_e::QP: {
          // NVENC的QP调整需要通过重新配置编码器
          BOOST_LOG(info) << "NVENC encoder QP change requested: " << param.value.int_value
                          << " (requires encoder reconfiguration)";
          break;
        }
        case dynamic_param_type_e::ADAPTIVE_QUANTIZATION: {
          // 自适应量化开关
          BOOST_LOG(info) << "NVENC encoder adaptive quantization change requested: " << param.value.bool_value;
          break;
        }
        case dynamic_param_type_e::MULTI_PASS: {
          // 多遍编码设置
          BOOST_LOG(info) << "NVENC encoder multi-pass change requested: " << param.value.int_value;
          break;
        }
        case dynamic_param_type_e::VBV_BUFFER_SIZE: {
          // VBV缓冲区大小
          BOOST_LOG(info) << "NVENC encoder VBV buffer size change requested: " << param.value.int_value << " Kbps";
          break;
        }
        case dynamic_param_type_e::CLIENT_SDR_WHITE_NITS:
          device->set_client_sdr_white_nits(param.value.float_value);
          break;
        default:
          BOOST_LOG(warning) << "NVENC encoder: Unsupported dynamic parameter type: " << (int) param.type;
          break;
      }
    }

    nvenc::nvenc_encoded_frame
    encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) return {};

      using decision_e = hdr_metadata::vivid_startup_gate_t::decision_e;
      const auto gated = vivid_gate.observe(device->hdr_luminance_stats, std::chrono::steady_clock::now());
      if (gated.transition != hdr_metadata::vivid_startup_gate_t::transition_e::none) {
        // The stream's metadata content changes here, so the client needs a fresh
        // IDR rather than a P frame that references pre-transition pictures.
        force_idr = true;
        log_vivid_gate_transition("NVENC", gated.transition, vivid_gate, device->hdr_luminance_stats);
      }
      if (gated.decision == decision_e::hold) {
        // Keep converting capture frames so the asynchronous GPU analyzer can
        // produce independent samples, but do not let the client see a plain-HLG
        // IDR followed by a mid-stream transition into HDR Vivid.
        return { {}, frame_index, false, false };
      }

      // Pass per-frame HDR luminance stats to NVENC for dynamic metadata injection
      if (gated.decision == decision_e::emit && device->hdr_luminance_stats.valid) {
        device->nvenc->set_luminance_stats(device->hdr_luminance_stats);
      }

      // Stage this frame's Dolby Vision RPU keyed by the submitted index; the
      // splice happens when the encoded access unit surfaces.
      dolby_vision_.stage(frame_index, device->hdr_luminance_stats);

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

    void
    track_frame_timestamp(
      uint64_t frame_index,
      std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
      std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
      frame_timestamps.store(frame_index, frame_timestamp, std::move(pipeline_trace));
    }

    std::optional<std::chrono::steady_clock::time_point>
    resolve_frame_timestamp(uint64_t frame_index) const {
      return frame_timestamps.lookup(frame_index);
    }

    std::optional<platf::frame_pipeline_trace_t>
    resolve_frame_trace(uint64_t frame_index) const {
      return frame_timestamps.lookup_trace(frame_index);
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    frame_timestamp_ring_t frame_timestamps;
    hdr_metadata::vivid_startup_gate_t vivid_gate;
    bool force_idr = false;
  };

  class amf_encode_session_t: public encode_session_t {
  public:
    amf_encode_session_t(std::unique_ptr<platf::amf_encode_device_t> encode_device, int video_format):
        device(std::move(encode_device)),
        vivid_gate(
          device ? device->colorspace : sunshine_colorspace_t {},
          video_format,
          device && hdr_luminance_analysis_usable(device->hdr_luminance_analysis_available)) {
      if (vivid_gate.prerolling()) {
        BOOST_LOG(info) << "AMF: holding HLG startup for stable HDR Vivid metadata ("
                        << hdr_metadata::vivid_startup_guard_t::REQUIRED_SAMPLES
                        << " independent samples, "
                        << hdr_metadata::vivid_startup_gate_t::PREROLL_TIMEOUT.count()
                        << " ms timeout)";
      }
    }

    /// Session-level Dolby Vision state; inert until the negotiated session
    /// enables it in make_amf_encode_session().
    dolby_vision::rpu_injector_t dolby_vision_;

    int
    convert(platf::img_t &img) override {
      if (!device) return -1;
      return device->convert(img);
    }

    void
    request_idr_frame() override {
      force_idr = true;
    }

    void
    request_normal_frame() override {
      force_idr = false;
    }

    void
    invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->amf) return;

      if (!device->amf->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    void
    set_bitrate(int bitrate_kbps) override {
      if (device && device->amf) {
        const auto adjusted_bitrate_kbps = encoder_bitrate_for_total_request(
          bitrate_kbps,
          config::video.max_bitrate,
          config::stream.fec_percentage
        );

        device->amf->set_bitrate(adjusted_bitrate_kbps);
        BOOST_LOG(info) << "AMF standalone encoder bitrate changed to: " << adjusted_bitrate_kbps
                        << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                        << config::stream.fec_percentage << "%)";
      }
    }

    void
    set_dynamic_param(const dynamic_param_t &param) override {
      if (!device || !device->amf) return;

      switch (param.type) {
        case dynamic_param_type_e::BITRATE:
          set_bitrate(param.value.int_value);
          break;
        case dynamic_param_type_e::CLIENT_SDR_WHITE_NITS:
          device->set_client_sdr_white_nits(param.value.float_value);
          break;
        default:
          break;
      }
    }

    amf::amf_encoded_frame
    encode_frame(uint64_t frame_index) {
      if (!device || !device->amf) return {};

      using decision_e = hdr_metadata::vivid_startup_gate_t::decision_e;
      const auto gated = vivid_gate.observe(device->hdr_luminance_stats, std::chrono::steady_clock::now());
      if (gated.transition != hdr_metadata::vivid_startup_gate_t::transition_e::none) {
        force_idr = true;
        log_vivid_gate_transition("AMF", gated.transition, vivid_gate, device->hdr_luminance_stats);
      }
      if (gated.decision == decision_e::hold) {
        // Same reasoning as NVENC: keep converting so the analyzer converges, but
        // do not let the client see plain HLG before the switch into Vivid.
        amf::amf_encoded_frame held;
        held.frame_index = frame_index;
        return held;
      }

      if (gated.decision == decision_e::emit && device->hdr_luminance_stats.valid) {
        device->amf->set_luminance_stats(device->hdr_luminance_stats);
      }

      // Stage this frame's Dolby Vision RPU keyed by the submitted index; the
      // splice happens when the encoded access unit surfaces.
      dolby_vision_.stage(frame_index, device->hdr_luminance_stats);

      auto result = device->amf->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

    void
    track_frame_timestamp(
      uint64_t frame_index,
      std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
      std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
      frame_timestamps.store(frame_index, frame_timestamp, std::move(pipeline_trace));
    }

    std::optional<std::chrono::steady_clock::time_point>
    resolve_frame_timestamp(uint64_t frame_index) const {
      return frame_timestamps.lookup(frame_index);
    }

    std::optional<platf::frame_pipeline_trace_t>
    resolve_frame_trace(uint64_t frame_index) const {
      return frame_timestamps.lookup_trace(frame_index);
    }

  private:
    std::unique_ptr<platf::amf_encode_device_t> device;
    frame_timestamp_ring_t frame_timestamps;
    hdr_metadata::vivid_startup_gate_t vivid_gate;
    bool force_idr = false;
  };

  struct sync_session_ctx_t {
    safe::signal_t *join_event;
    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::mail_raw_t::queue_t<packet_t> packets;
    safe::mail_raw_t::event_t<bool> idr_events;
    safe::mail_raw_t::event_t<hdr_info_t> hdr_events;
    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events;

    config_t config;
    int frame_nr;
    void *channel_data;
  };

  struct sync_session_t {
    sync_session_ctx_t *ctx;
    std::unique_ptr<encode_session_t> session;
  };

  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;
  using encode_e = platf::capture_e;

  struct captured_frame_t {
    std::shared_ptr<platf::img_t> image;
    bool is_replay {false};
  };

  using captured_frame_event_t = std::shared_ptr<safe::event_t<captured_frame_t>>;

  struct capture_ctx_t {
    captured_frame_event_t images;
    config_t config;
  };

  struct capture_thread_async_ctx_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;
    std::thread capture_thread;

    safe::signal_t reinit_event;
    const encoder_t *encoder_p;
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;
  };

  struct capture_thread_sync_ctx_t {
    encode_session_ctx_queue_t encode_session_ctx_queue { 30 };
  };

  int
  start_capture_sync(capture_thread_sync_ctx_t &ctx);
  void
  end_capture_sync(capture_thread_sync_ctx_t &ctx);
  int
  start_capture_async(capture_thread_async_ctx_t &ctx);
  void
  end_capture_async(capture_thread_async_ctx_t &ctx);

  // Keep a reference counter to ensure the capture thread only runs when other threads have a reference to the capture thread
  auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(start_capture_async, end_capture_async);
  auto capture_thread_sync = safe::make_shared<capture_thread_sync_ctx_t>(start_capture_sync, end_capture_sync);

#ifdef _WIN32
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12, platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv, platf::pix_fmt_e::yuv444p16),
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN  // flags
  };
#elif !defined(__APPLE__)
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
  #ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
  #else
      AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_CUDA,
  #endif
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
  #ifdef _WIN32
      dxgi_init_avcodec_hardware_input_buffer
  #else
      cuda_init_avcodec_hardware_input_buffer
  #endif
      ),
    {
      // Common options
      {
        { "delay"s, 0 },
        { "forced-idr"s, 1 },
        { "zerolatency"s, 1 },
        { "surfaces"s, 1 },
        { "cbr_padding"s, false },
        { "preset"s, &config::video.nv_legacy.preset },
        { "tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY },
        { "rc"s, NV_ENC_PARAMS_RC_CBR },
        { "multipass"s, &config::video.nv_legacy.multipass },
        { "aq"s, &config::video.nv_legacy.aq },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      // Common options
      {
        { "delay"s, 0 },
        { "forced-idr"s, 1 },
        { "zerolatency"s, 1 },
        { "surfaces"s, 1 },
        { "cbr_padding"s, false },
        { "preset"s, &config::video.nv_legacy.preset },
        { "tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY },
        { "rc"s, NV_ENC_PARAMS_RC_CBR },
        { "multipass"s, &config::video.nv_legacy.multipass },
        { "aq"s, &config::video.nv_legacy.aq },
      },
      {
        // SDR-specific options
        { "profile"s, (int) nv::profile_hevc_e::main },
      },
      {
        // HDR-specific options
        { "profile"s, (int) nv::profile_hevc_e::main_10 },
      },
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {
        { "delay"s, 0 },
        { "forced-idr"s, 1 },
        { "zerolatency"s, 1 },
        { "surfaces"s, 1 },
        { "cbr_padding"s, false },
        { "preset"s, &config::video.nv_legacy.preset },
        { "tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY },
        { "rc"s, NV_ENC_PARAMS_RC_CBR },
        { "coder"s, &config::video.nv_legacy.h264_coder },
        { "multipass"s, &config::video.nv_legacy.multipass },
        { "aq"s, &config::video.nv_legacy.aq },
      },
      {
        // SDR-specific options
        { "profile"s, (int) nv::profile_h264_e::high },
      },
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING
  };
#endif

#ifdef _WIN32
  encoder_t quicksync {
    "quicksync"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_QSV,
      AV_PIX_FMT_QSV,
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX, AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer),
    {
      // Common options
      {
        { "preset"s, &config::video.qsv.qsv_preset },
        { "forced_idr"s, 1 },
        { "async_depth"s, 1 },
        { "low_delay_brc"s, 1 },
        { "low_power"s, 1 },
      },
      {
        // SDR-specific options
        { "profile"s, (int) qsv::profile_av1_e::main },
      },
      {
        // HDR-specific options
        { "profile"s, (int) qsv::profile_av1_e::main },
      },
      {
        // YUV444 SDR-specific options
        { "profile"s, (int) qsv::profile_av1_e::high },
      },
      {
        // YUV444 HDR-specific options
        { "profile"s, (int) qsv::profile_av1_e::high },
      },
      {},  // Fallback options
      "av1_qsv"s,
    },
    {
      // Common options
      {
        { "preset"s, &config::video.qsv.qsv_preset },
        { "forced_idr"s, 1 },
        { "async_depth"s, 1 },
        { "low_delay_brc"s, 1 },
        { "low_power"s, 1 },
        { "recovery_point_sei"s, 0 },
        { "pic_timing_sei"s, 0 },
      },
      {
        // SDR-specific options
        { "profile"s, (int) qsv::profile_hevc_e::main },
      },
      {
        // HDR-specific options
        { "profile"s, (int) qsv::profile_hevc_e::main_10 },
      },
      {
        // YUV444 SDR-specific options
        { "profile"s, (int) qsv::profile_hevc_e::rext },
      },
      {
        // YUV444 HDR-specific options
        { "profile"s, (int) qsv::profile_hevc_e::rext },
      },
      {
        // Fallback options
        { "low_power"s, []() { return config::video.qsv.qsv_slow_hevc ? 0 : 1; } },
      },
      "hevc_qsv"s,
    },
    {
      // Common options
      {
        { "preset"s, &config::video.qsv.qsv_preset },
        { "cavlc"s, &config::video.qsv.qsv_cavlc },
        { "forced_idr"s, 1 },
        { "async_depth"s, 1 },
        { "low_delay_brc"s, 1 },
        { "low_power"s, 1 },
        { "recovery_point_sei"s, 0 },
        { "vcm"s, 1 },
        { "pic_timing_sei"s, 0 },
        { "max_dec_frame_buffering"s, 1 },
      },
      {
        // SDR-specific options
        { "profile"s, (int) qsv::profile_h264_e::high },
      },
      {},  // HDR-specific options
      {
        // YUV444 SDR-specific options
        { "profile"s, (int) qsv::profile_h264_e::high_444p },
      },
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        { "low_power"s, 0 },  // Some old/low-end Intel GPUs don't support low power encoding
      },
      "h264_qsv"s,
    },
    PARALLEL_ENCODING | CBR_WITH_VBR | RELAXED_COMPLIANCE | NO_RC_BUF_LIMIT | YUV444_SUPPORT
  };

  encoder_t amdvce {
    "amdvce"sv,
    std::make_unique<encoder_platform_formats_amf>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12, platf::pix_fmt_e::p010,
      platf::pix_fmt_e::unknown, platf::pix_fmt_e::unknown),
    {
      {},  // Common options (handled by AMF directly)
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      {},
      {},
      {},
      {},
      {},
      {},
      "hevc_amf"s,
    },
    {
      {},
      {},
      {},
      {},
      {},
      {},
      "h264_amf"s,
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION
  };

  // NOTE: The legacy FFmpeg-based AMF encoder (encoder_t amdvce_legacy) was
  // removed. The native AMF path (`amdvce`, src/amf/amf_d3d11.cpp) is strictly
  // superior (HDR, RFI, dynamic bitrate without reaching into FFmpeg internals)
  // and was already the preferred entry. Keeping the legacy path required the
  // fragile AMFEncoderContext_Partial reflection over libavcodec's amfenc.h
  // private struct, which would silently break on any FFmpeg ABI change.
#endif

  encoder_t software {
    "software"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_NONE, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10,
      AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10,
      nullptr),
    {
      // libsvtav1 takes different presets than libx264/libx265.
      // We set an infinite GOP length, use a low delay prediction structure,
      // force I frames to be key frames, and set max bitrate to default to work
      // around a FFmpeg bug with CBR mode.
      {
        { "svtav1-params"s, "keyint=-1:pred-struct=1:force-key-frames=1:mbr=0"s },
        { "preset"s, &config::video.sw.svtav1_preset },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options

#ifdef ENABLE_BROKEN_AV1_ENCODER
           // Due to bugs preventing on-demand IDR frames from working and very poor
           // real-time encoding performance, we do not enable libsvtav1 by default.
           // It is only suitable for testing AV1 until the IDR frame issue is fixed.
      "libsvtav1"s,
#else
      {},
#endif
    },
    {
      // x265's Info SEI is so long that it causes the IDR picture data to be
      // kicked to the 2nd packet in the frame, breaking Moonlight's parsing logic.
      // It also looks like gop_size isn't passed on to x265, so we have to set
      // 'keyint=-1' in the parameters ourselves.
      {
        { "forced-idr"s, 1 },
        { "x265-params"s, "info=0:keyint=-1"s },
        { "preset"s, &config::video.sw.sw_preset },
        { "tune"s, &config::video.sw.sw_tune },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx265"s,
    },
    {
      // Common options
      {
        { "preset"s, &config::video.sw.sw_preset },
        { "tune"s, &config::video.sw.sw_tune },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx264"s,
    },
    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT
  };

#ifdef __linux__
  encoder_t vaapi {
    "vaapi"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VAAPI,
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
      vaapi_init_avcodec_hardware_input_buffer),
    {
      // Common options
      {
        { "async_depth"s, 1 },
        { "idr_interval"s, std::numeric_limits<int>::max() },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vaapi"s,
    },
    {
      // Common options
      {
        { "async_depth"s, 1 },
        { "sei"s, 0 },
        { "idr_interval"s, std::numeric_limits<int>::max() },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vaapi"s,
    },
    {
      // Common options
      {
        { "async_depth"s, 1 },
        { "sei"s, 0 },
        { "idr_interval"s, std::numeric_limits<int>::max() },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vaapi"s,
    },
    // RC buffer size will be set in platform code if supported
    LIMITED_GOP_SIZE | PARALLEL_ENCODING | NO_RC_BUF_LIMIT
  };
#endif

#ifdef __APPLE__
  encoder_t videotoolbox {
    "videotoolbox"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VIDEOTOOLBOX,
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
      vt_init_avcodec_hardware_input_buffer),
    {
      // Common options
      {
        { "allow_sw"s, &config::video.vt.vt_allow_sw },
        { "require_sw"s, &config::video.vt.vt_require_sw },
        { "realtime"s, &config::video.vt.vt_realtime },
        { "prio_speed"s, 1 },
        { "max_ref_frames"s, 1 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_videotoolbox"s,
    },
    {
      // Common options
      {
        { "allow_sw"s, &config::video.vt.vt_allow_sw },
        { "require_sw"s, &config::video.vt.vt_require_sw },
        { "realtime"s, &config::video.vt.vt_realtime },
        { "prio_speed"s, 1 },
        { "max_ref_frames"s, 1 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_videotoolbox"s,
    },
    {
      // Common options
      {
        { "allow_sw"s, &config::video.vt.vt_allow_sw },
        { "require_sw"s, &config::video.vt.vt_require_sw },
        { "realtime"s, &config::video.vt.vt_realtime },
        { "prio_speed"s, 1 },
        { "max_ref_frames"s, 1 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        { "flags"s, "-low_delay" },
      },
      "h264_videotoolbox"s,
    },
    DEFAULT
  };
#endif

  // Vulkan encoder - cross-platform (Windows and Linux)
#if !defined(__APPLE__)
  encoder_t vulkan {
    "vulkan"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VULKAN, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VULKAN,
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
      vulkan_init_avcodec_hardware_input_buffer),
    {
      // Common options for AV1
      {
        { "forced_idr"s, 1 },
        { "async_depth"s, 1 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vulkan"s,
    },
    {
      // Common options for HEVC (if supported)
      {
        { "forced_idr"s, 1 },
        { "async_depth"s, 1 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vulkan"s,
    },
    {
      // Common options for H.264 (if supported)
      {
        { "forced_idr"s, 1 },
        { "async_depth"s, 1 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vulkan"s,
    },
    PARALLEL_ENCODING
  };
#endif

  static const std::vector<encoder_t *> encoders {
#ifndef __APPLE__
    &nvenc,
    &vulkan,  // Vulkan encoder (cross-platform)
#endif
#ifdef _WIN32
    &quicksync,
    &amdvce,
#endif
#ifdef __linux__
    &vaapi,
#endif
#ifdef __APPLE__
    &videotoolbox,
#endif
    &software
  };

  static encoder_t *chosen_encoder;
  static std::atomic<const encoder_t *> active_encoder_for_status { nullptr };
  int active_hevc_mode;
  int active_av1_mode;
  bool last_encoder_probe_supported_ref_frames_invalidation = false;
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};
  probe_result_t last_encoder_probe_result {
    probe_error_e::none,
    "Encoder probe succeeded.",
    {}
  };

  std::string
  active_encoder_name() {
    const auto *encoder = active_encoder_for_status.load(std::memory_order_acquire);
    return encoder ? std::string { encoder->name } : std::string {};
  }

  bool
  active_encoder_supports_dynamic_sdr_white() {
    const auto *encoder = active_encoder_for_status.load(std::memory_order_acquire);
    if (!encoder) {
      return false;
    }

    return dynamic_cast<const encoder_platform_formats_nvenc *>(encoder->platform_formats.get()) != nullptr ||
           dynamic_cast<const encoder_platform_formats_amf *>(encoder->platform_formats.get()) != nullptr;
  }

  bool
  is_valid_client_sdr_white_nits(float nits) {
    return std::isfinite(nits) && nits >= 50.0f && nits <= 1000.0f;
  }

  void
  reset_display(std::shared_ptr<platf::display_t> &disp, const platf::mem_type_e &type, const std::string &display_name, const config_t &config) {
    // We try this twice, in case we still get an error on reinitialization
    for (int x = 0; x < 2; ++x) {
      disp.reset();
      disp = platf::display(type, display_name, config);
      if (disp) {
        BOOST_LOG(debug) << "[reset_display] 成功重置显示器: " << display_name;
        break;
      }
      BOOST_LOG(debug) << "[reset_display] 显示器创建失败 (尝试 " << (x + 1) << "/2): " << display_name;
      // The capture code depends on us to sleep between failures
      std::this_thread::sleep_for(200ms);
    }
  }

  /**
   * @brief Update the list of display names before or during a stream.
   * @details This will attempt to keep `current_display_index` pointing at the same display.
   * @param dev_type The encoder device type used for display lookup.
   * @param display_names The list of display names to repopulate.
   * @param current_display_index The current display index or -1 if not yet known.
   */
  void
  refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index) {
    // It is possible that the output display name may be empty even if it wasn't before (device disconnected)
    const auto output_name { display_device::get_display_name(config::video.output_name) };
    std::string current_display_name;

    // If we have a current display index, let's start with that
    if (current_display_index >= 0 && current_display_index < display_names.size()) {
      current_display_name = display_names.at(current_display_index);
    }

    // Refresh the display names
    auto old_display_names = std::move(display_names);
    display_names = platf::display_names(dev_type);

    // If we now have no displays, let's put the old display array back and fail
    if (display_names.empty() && !old_display_names.empty()) {
      BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
      display_names = std::move(old_display_names);
      return;
    }
    else if (display_names.empty()) {
      display_names.emplace_back(output_name);
    }

    // We now have a new display name list, so reset the index back to 0
    current_display_index = 0;

    // If we had a name previously, let's try to find it in the new list
    if (!current_display_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == current_display_name) {
          current_display_index = x;
          return;
        }
      }

      // The old display was removed, so we'll start back at the first display again
      BOOST_LOG(warning) << "Previous active display ["sv << current_display_name << "] is no longer present"sv;
    }
    else {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == output_name) {
          current_display_index = x;
          return;
        }
      }
    }
  }

  void
  captureThread(
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
    sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
    safe::signal_t &reinit_event,
    const encoder_t &encoder) {
    std::vector<capture_ctx_t> capture_ctxs;

    auto fg = util::fail_guard([&]() {
      capture_ctx_queue->stop();

      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
        capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
        capture_ctx.images->stop();
      }
    });

    auto switch_display_event = mail::man->event<int>(mail::switch_display);
    auto active_display_event = mail::man->event<std::string>(mail::active_display);

    // Wait for the initial capture context or a request to stop the queue
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));

    // Get all the monitor names now, rather than at boot, to
    // get the most up-to-date list available monitors
    std::vector<std::string> display_names;
    int display_p = -1;
    refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

    // Use client-specified display_name if provided, otherwise use the selected display
    std::string target_display_name;
    const auto &config = capture_ctxs.front().config;
    if (!config.display_name.empty()) {
      // config.display_name may be a device ID (e.g., {xxx-xxx-xxx}) rather than display name (e.g., \\.\DISPLAY1)
      // Try to convert device ID to display name first
      std::string resolved_display_name = display_device::get_display_name(config.display_name);
      if (resolved_display_name.empty()) {
        // If conversion failed, use the original value (might already be a display name)
        resolved_display_name = config.display_name;
      }

      // Try to find the display in the list
      bool found = false;
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == resolved_display_name) {
          display_p = x;
          target_display_name = resolved_display_name;
          found = true;
          BOOST_LOG(info) << "Using client-specified display: " << target_display_name;
          break;
        }
      }
      if (!found) {
        BOOST_LOG(warning) << "Client-specified display [" << config.display_name << "] (resolved: " << resolved_display_name << ") not found, using default display";
        target_display_name = display_names[display_p];
      }
    }
    else {
      target_display_name = display_names[display_p];
    }

    auto disp = platf::display(encoder.platform_formats->dev_type, target_display_name, config);
    if (!disp) {
      return;
    }
    active_display_event->raise(target_display_name);
    display_wp = disp;

    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);
    std::shared_ptr<platf::img_t> latest_captured_img;

    auto append_pending_capture_contexts = [&](const std::shared_ptr<platf::img_t> &initial_img = {}) -> bool {
      while (capture_ctx_queue->peek()) {
        auto capture_ctx = capture_ctx_queue->pop();
        if (!capture_ctx) {
          return false;
        }

        // 同一个捕获线程中的会话共享当前显示器。手动切换显示器后加入的会话
        // 必须继承当前目标，避免后续重新初始化跳回它启动时选择的显示器。
        capture_ctx->config.display_name = target_display_name;
        if (initial_img) {
          // 锁屏或静态桌面可能长时间没有新的 Desktop Duplication 帧。
          // 新会话必须先取得当前画面，不能一直编码初始化用的黑色占位帧。
          capture_ctx->images->raise(captured_frame_t {
            .image = initial_img,
            .is_replay = true,
          });
        }
        BOOST_LOG(debug) << "Attached streaming session to shared capture display ["sv << target_display_name
                         << "], reused latest frame: "sv << (initial_img ? "yes"sv : "no"sv);
        capture_ctxs.emplace_back(std::move(*capture_ctx));
      }

      return true;
    };

    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    const std::chrono::seconds trim_timeot = 3s;
    auto trim_imgs = [&]() {
      // count allocated and used within current pool
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // remember the timestamp of currently used count
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // decide whether to trim allocated unused above the currently used count
      // based on last used timestamp and universal timeout
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // trim allocated unused above the newly decided trim target
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
        // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();
            to_trim -= 1;
            if (to_trim == 0) break;
          }
        }
        // forget timestamps that no longer relevant
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    auto pull_free_image_callback = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
      img_out.reset();
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          // trim allocated but unused portion of the pool based on timeouts
          trim_imgs();
          img_out->frame_timestamp.reset();
          img_out->pipeline_trace.reset();
          return true;
        }
        else {
          // sleep and retry if image pool is full
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }
          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        // 先接入新会话，再分发本次真实画面。若本次只是捕获超时，则给新会话
        // 补发上一张真实画面，保证它与现有会话看到同一个活动显示器内容。
        if (!append_pending_capture_contexts(frame_captured ? std::shared_ptr<platf::img_t> {} : latest_captured_img)) {
          return false;
        }

        if (frame_captured) {
          latest_captured_img = img;
          for (auto &capture_ctx : capture_ctxs) {
            capture_ctx.images->raise(captured_frame_t {
              .image = img,
            });
          }
        }

        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);

      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit: {
          reinit_event.raise(true);

          // Some classes of images contain references to the display --> display won't delete unless img is deleted
          latest_captured_img.reset();
          for (auto &img : imgs) {
            img.reset();
          }

          // display_wp is modified in this thread only
          // Wait for the other shared_ptr's of display to be destroyed.
          // New displays will only be created in this thread.
          while (display_wp->use_count() != 1) {
            // Free images that weren't consumed by the encoders. These can reference the display and prevent
            // the ref count from reaching 1. We do this here rather than on the encoder thread to avoid race
            // conditions where the encoding loop might free a good frame after reinitializing if we capture
            // a new frame here before the encoder has finished reinitializing.
            KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
              if (!capture_ctx->images->running()) {
                capture_ctx = capture_ctxs.erase(capture_ctx);
                continue;
              }

              while (capture_ctx->images->peek()) {
                capture_ctx->images->pop();
              }

              ++capture_ctx;
            });

            std::this_thread::sleep_for(20ms);
          }

          // 等待旧显示器释放期间，最后一个旧会话可能退出，同时新会话已经加入队列。
          // 先接入新上下文；若仍无会话则安全结束捕获线程，不能访问空容器的 front()。
          if (!append_pending_capture_contexts() || capture_ctxs.empty()) {
            return;
          }

          while (capture_ctx_queue->running()) {
            // Release the display before reenumerating displays, since some capture backends
            // only support a single display session per device/application.
            disp.reset();

            // Refresh display names since a display removal might have caused the reinitialization
            refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

            // Process any pending display switch with the new list of displays
            bool user_switched = false;
            if (switch_display_event->peek()) {
              display_p = std::clamp(*switch_display_event->pop(), 0, (int) display_names.size() - 1);
              user_switched = true;
            }

            // Use client-specified display_name if provided (only for auto-reinit, not manual switch)
            auto &config = capture_ctxs.front().config;
            target_display_name = display_names[display_p];
            if (!user_switched && !config.display_name.empty()) {
              // config.display_name may be a device ID - convert to display name
              std::string resolved_display_name = display_device::get_display_name(config.display_name);
              if (resolved_display_name.empty()) {
                resolved_display_name = config.display_name;
              }

              // Try to find the display in the list
              bool found = false;
              for (int x = 0; x < display_names.size(); ++x) {
                if (display_names[x] == resolved_display_name) {
                  display_p = x;
                  target_display_name = resolved_display_name;
                  found = true;
                  break;
                }
              }
              if (!found) {
                BOOST_LOG(warning) << "Client-specified display [" << config.display_name << "] (resolved: " << resolved_display_name << ") not found, using default display";
              }
            }

            if (user_switched) {
              for (auto &capture_ctx : capture_ctxs) {
                capture_ctx.config.display_name = target_display_name;
              }
            }

            // reset_display() will sleep between retries
            reset_display(disp, encoder.platform_formats->dev_type, target_display_name, config);
            if (disp) {
              active_display_event->raise(target_display_name);
              break;
            }
          }
          if (!disp) {
            return;
          }

          display_wp = disp;

          reinit_event.reset();
          continue;
        }
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
          return;
      }
    }
  }

  /**
   * @brief Update HDR Vivid and HDR10+ side data before encoding a frame.
   *
   * HDR Vivid receives its GB/T 46269.1-2025 content-domain statistics after
   * the recommended 32-frame mean. HDR10+ keeps its independent EMA path.
   *
   * @param frame The AVFrame with pre-allocated dynamic HDR side data
   * @param hdr10plus_stats Temporally-smoothed HDR10+ luminance statistics
   * @param vivid_metadata Filtered HDR Vivid statistics in normalized PQ space
   * @param max_display_luminance Mapped client display peak luminance in nits
   */
  void
  update_hdr_dynamic_metadata(
    AVFrame *frame,
    const platf::hdr_frame_luminance_stats_t &hdr10plus_stats,
    const hdr_metadata::vivid_metadata_t &vivid_metadata,
    uint16_t max_display_luminance) {
    if (!frame) return;

    // Update HDR Vivid (CUVA) dynamic metadata
    auto vivid_sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_VIVID);
    if (vivid_sd && vivid_metadata.valid) {
      auto *vivid = reinterpret_cast<AVDynamicHDRVivid *>(vivid_sd->data);
      if (vivid && vivid->num_windows > 0) {
        auto &params = vivid->params[0];

        params.minimum_maxrgb = av_make_q(vivid_metadata.minimum_maxrgb_pq, hdr_metadata::pq_u12_den);
        params.average_maxrgb = av_make_q(vivid_metadata.average_maxrgb_pq, hdr_metadata::pq_u12_den);
        params.variance_maxrgb = av_make_q(vivid_metadata.variance_maxrgb_pq, hdr_metadata::pq_u12_den);
        params.maximum_maxrgb = av_make_q(vivid_metadata.maximum_maxrgb_pq, hdr_metadata::pq_u12_den);

        const auto target_display_pq =
          hdr_metadata::target_display_pq_u12(static_cast<float>(max_display_luminance));
        for (int i = 0; i < 2; i++) {
          params.tm_params[i].targeted_system_display_maximum_luminance =
            av_make_q(target_display_pq, hdr_metadata::pq_u12_den);
        }
      }
    }

    // Update HDR10+ dynamic metadata
    auto hdr10plus_sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (hdr10plus_sd && hdr10plus_stats.valid) {
      auto *hdr10plus = reinterpret_cast<AVDynamicHDRPlus *>(hdr10plus_sd->data);
      if (hdr10plus && hdr10plus->num_windows > 0) {
        // The 99th percentile is what maxSCL reports; see hdr10plus_from_luminance().
        const auto frame_metadata = hdr_metadata::hdr10plus_from_luminance(
          hdr10plus_stats.percentile_99,
          hdr10plus_stats.avg_maxrgb,
          max_display_luminance,
          hdr10plus_stats.distribution_maxrgb);
        if (frame_metadata.valid) {
          auto &params = hdr10plus->params[0];
          const auto maxscl = av_make_q(
            frame_metadata.maxscl, hdr_metadata::hdr10plus_normalized_scale);
          params.maxscl[0] = maxscl;
          params.maxscl[1] = maxscl;
          params.maxscl[2] = maxscl;
          params.average_maxrgb = av_make_q(
            frame_metadata.average_maxrgb, hdr_metadata::hdr10plus_normalized_scale);
          hdr10plus->targeted_system_display_maximum_luminance = av_make_q(
            frame_metadata.targeted_system_display_maximum_luminance, 1);
          params.num_distribution_maxrgb_percentiles =
            static_cast<uint8_t>(hdr_metadata::hdr10plus_percentages.size());
          for (size_t i = 0; i < hdr_metadata::hdr10plus_percentages.size(); ++i) {
            params.distribution_maxrgb[i].percentage = hdr_metadata::hdr10plus_percentages[i];
            params.distribution_maxrgb[i].percentile = av_make_q(
              frame_metadata.distribution_maxrgb[i], hdr_metadata::hdr10plus_normalized_scale);
          }
        }
      }
    }
  }

  void
  apply_client_target_luminance(SS_HDR_METADATA &metadata, const config_t &client_config) {
    const auto pipeline = client_config.effective_frame_pipeline_policy();
    if (platf::postprocess_produces_hdr_output(pipeline, client_config.pre_encode_filter)) {
      // The synthetic backend has already rendered against this session's
      // fixed mastering peak. Replacing it with the client's panel peak would
      // make the signalled mastering metadata disagree with the actual pixels.
      return;
    }

    const auto &capabilities = client_config.hdr_capabilities;
    if (!capabilities.reported) {
      return;
    }

    metadata.maxDisplayLuminance = static_cast<std::uint16_t>(std::lround(capabilities.max_nits));
    // The field is uint16_t in 1/10000-nit units: clamp before the cast so a
    // client reporting min_nits >= 6.5536 cannot wrap into a bogus floor.
    metadata.minDisplayLuminance = static_cast<std::uint16_t>(std::clamp(
      static_cast<std::uint32_t>(std::lround(static_cast<double>(capabilities.min_nits) * 10000.0)),
      0u, 0xFFFFu));
  }

  bool
  synthetic_hdr_source_active(const config_t &config) {
    const auto pipeline = config.effective_frame_pipeline_policy();
    return platf::postprocess_produces_hdr_output(
      pipeline,
      config.pre_encode_filter);
  }

  bool
  get_effective_hdr_metadata(
    platf::display_t *display,
    const config_t &config,
    SS_HDR_METADATA &metadata) {
    if (!synthetic_hdr_source_active(config)) {
      return display->get_hdr_metadata(metadata);
    }

    std::memset(&metadata, 0, sizeof(metadata));
    // Synthetic HDR is mastered in Rec. 2020 with D65. Its mastering peak is
    // the same session-stable peak passed to the vendor backend, not a value
    // queried from the deliberately SDR source display.
    metadata.displayPrimaries[0].x = static_cast<std::uint16_t>(0.708f * 50000.0f);
    metadata.displayPrimaries[0].y = static_cast<std::uint16_t>(0.292f * 50000.0f);
    metadata.displayPrimaries[1].x = static_cast<std::uint16_t>(0.170f * 50000.0f);
    metadata.displayPrimaries[1].y = static_cast<std::uint16_t>(0.797f * 50000.0f);
    metadata.displayPrimaries[2].x = static_cast<std::uint16_t>(0.131f * 50000.0f);
    metadata.displayPrimaries[2].y = static_cast<std::uint16_t>(0.046f * 50000.0f);
    metadata.whitePoint.x = static_cast<std::uint16_t>(0.3127f * 50000.0f);
    metadata.whitePoint.y = static_cast<std::uint16_t>(0.3290f * 50000.0f);

    const auto peak_nits = static_cast<std::uint16_t>(std::lround(std::clamp(
      config.pre_encode_filter_config.peak_nits,
      1.0f,
      10000.0f)));
    metadata.maxDisplayLuminance = peak_nits;
    metadata.minDisplayLuminance = 1;  // 0.0001 nit units
    metadata.maxFullFrameLuminance = peak_nits;
    metadata.maxContentLightLevel = peak_nits;
    // TrueHDR's frame-average luminance is not known at session setup. Zero is
    // the HDR10 "unknown" value and avoids suggesting a full frame at MaxCLL.
    metadata.maxFrameAverageLightLevel = 0;
    return true;
  }

  int
  encode_avcodec(
    int64_t frame_nr,
    avcodec_encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
    const auto submitted_frame_index = static_cast<uint64_t>(frame_nr);
    if (pipeline_trace) {
      pipeline_trace->encode_submit = std::chrono::steady_clock::now();
    }
    session.frame_timestamps.store(submitted_frame_index, frame_timestamp, std::move(pipeline_trace));

    auto &frame = session.device->frame;
    frame->pts = frame_nr;

    auto &ctx = session.avcodec_ctx;

    auto &sps = session.sps;
    auto &vps = session.vps;

    // Update per-frame dynamic metadata. HDR10+ and HDR Vivid intentionally use
    // their own temporal models because their standards define different fields.
    {
      auto &raw_stats = session.device->hdr_luminance_stats;
      if (raw_stats.valid) {
        const auto filtered = session.dynamic_metadata_temporal.update(raw_stats);

        update_hdr_dynamic_metadata(
          frame,
          filtered.hdr10plus_stats,
          filtered.vivid,
          session.dynamic_metadata_target_peak_nits);
      }
    }

    // send the frame to the encoder
    auto ret = avcodec_send_frame(ctx.get(), frame);
    if (ret < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
      BOOST_LOG(error) << "Could not send a frame for encoding: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, ret);

      return -1;
    }

    while (ret >= 0) {
      auto packet = std::make_unique<packet_raw_avcodec>();
      auto av_packet = packet.get()->av_packet;

      ret = avcodec_receive_packet(ctx.get(), av_packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
      }
      else if (ret < 0) {
        return ret;
      }

      if (av_packet->flags & AV_PKT_FLAG_KEY) {
        BOOST_LOG(debug) << "Frame "sv << frame_nr << ": IDR Keyframe (AV_FRAME_FLAG_KEY)"sv;
      }

      if ((frame->flags & AV_FRAME_FLAG_KEY) && !(av_packet->flags & AV_PKT_FLAG_KEY)) {
        BOOST_LOG(error) << "Encoder did not produce IDR frame when requested!"sv;
      }

      if (session.inject) {
        if (session.inject == 1) {
          auto h264 = cbs::make_sps_h264(ctx.get(), av_packet);

          sps = std::move(h264.sps);
        }
        else {
          auto hevc = cbs::make_sps_hevc(ctx.get(), av_packet);

          sps = std::move(hevc.sps);
          vps = std::move(hevc.vps);

          session.replacements.emplace_back(
            std::string_view((char *) std::begin(vps.old), vps.old.size()),
            std::string_view((char *) std::begin(vps._new), vps._new.size()));
        }

        session.inject = 0;

        session.replacements.emplace_back(
          std::string_view((char *) std::begin(sps.old), sps.old.size()),
          std::string_view((char *) std::begin(sps._new), sps._new.size()));
      }

      if (av_packet && av_packet->pts >= 0) {
        const auto encoded_frame_index = static_cast<uint64_t>(av_packet->pts);
        packet->frame_timestamp = session.frame_timestamps.lookup(encoded_frame_index);
        auto encoded_trace = session.frame_timestamps.lookup_trace(encoded_frame_index);
        if (encoded_trace) {
          encoded_trace->packet_ready = std::chrono::steady_clock::now();
          packet->pipeline_trace = std::move(encoded_trace);
        }
      }

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      packets->raise(std::move(packet));
    }

    return 0;
  }

  int
  encode_nvenc(
    int64_t frame_nr,
    nvenc_encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
    const auto submitted_frame_index = static_cast<uint64_t>(frame_nr);
    if (pipeline_trace) {
      pipeline_trace->encode_submit = std::chrono::steady_clock::now();
    }
    session.track_frame_timestamp(submitted_frame_index, frame_timestamp, std::move(pipeline_trace));

    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      // Empty data with valid frame_index means encoder needs more input (NV_ENC_ERR_NEED_MORE_INPUT).
      // This is not an error - just return success and continue with next frame.
      if (encoded_frame.frame_index == static_cast<uint64_t>(frame_nr)) {
        BOOST_LOG(debug) << "NvENC: frame " << frame_nr << " buffered, waiting for more input";
        return 0;
      }
      BOOST_LOG(error) << "NvENC returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "NvENC frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    // The RPU rides with the access unit it was generated for, matched by the
    // encoder's own frame index round trip — never by callback order.
    session.dolby_vision_.inject(encoded_frame.frame_index, encoded_frame.data);

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = session.resolve_frame_timestamp(encoded_frame.frame_index);
    auto encoded_trace = session.resolve_frame_trace(encoded_frame.frame_index);
    if (encoded_trace) {
      encoded_trace->packet_ready = std::chrono::steady_clock::now();
      packet->pipeline_trace = std::move(encoded_trace);
    }
    packets->raise(std::move(packet));

    return 0;
  }

  int
  encode_amf(
    int64_t frame_nr,
    amf_encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
    const auto submitted_frame_index = static_cast<uint64_t>(frame_nr);
    if (pipeline_trace) {
      pipeline_trace->encode_submit = std::chrono::steady_clock::now();
    }
    session.track_frame_timestamp(submitted_frame_index, frame_timestamp, std::move(pipeline_trace));

    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.fatal) {
      // Encoder is in unrecoverable state (device lost or repeated failures);
      // propagate fatal so the session reinitializes instead of silently
      // producing no video (which causes the client to time out and reconnect repeatedly).
      BOOST_LOG(error) << "AMF: encoder in unrecoverable state, requesting reinit";
      return -1;
    }
    if (encoded_frame.data.empty()) {
      if (encoded_frame.frame_index == static_cast<uint64_t>(frame_nr)) {
        BOOST_LOG(debug) << "AMF: frame " << frame_nr << " buffered, waiting for more input";
        return 0;
      }
      BOOST_LOG(error) << "AMF returned empty packet";
      return -1;
    }

    if (encoded_frame.frame_index > static_cast<uint64_t>(frame_nr)) {
      // AMF returned a frame from the future — truly unexpected
      BOOST_LOG(error) << "AMF frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }
    else if (encoded_frame.frame_index < static_cast<uint64_t>(frame_nr)) {
      // Normal pipeline latency: encoder buffered one frame and is returning a previous frame
      BOOST_LOG(debug) << "AMF pipeline lag: submitted " << frame_nr << ", got " << encoded_frame.frame_index;
    }

    // AMF may return an older frame than the one submitted; the RPU splice
    // keys on the encoder's own output index, so pipeline lag cannot mismatch.
    session.dolby_vision_.inject(encoded_frame.frame_index, encoded_frame.data);

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = session.resolve_frame_timestamp(encoded_frame.frame_index);
    auto encoded_trace = session.resolve_frame_trace(encoded_frame.frame_index);
    if (encoded_trace) {
      encoded_trace->packet_ready = std::chrono::steady_clock::now();
      packet->pipeline_trace = std::move(encoded_trace);
    }
    packets->raise(std::move(packet));

    return 0;
  }

  int
  encode(
    int64_t frame_nr,
    encode_session_t &session,
    safe::mail_raw_t::queue_t<packet_t> &packets,
    void *channel_data,
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp,
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace) {
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp, std::move(pipeline_trace));
    }
    else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp, std::move(pipeline_trace));
    }
    else if (auto amf_session = dynamic_cast<amf_encode_session_t *>(&session)) {
      return encode_amf(frame_nr, *amf_session, packets, channel_data, frame_timestamp, std::move(pipeline_trace));
    }

    return -1;
  }

  std::unique_ptr<avcodec_encode_session_t>
  make_avcodec_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::avcodec_encode_device_t> encode_device) {
    auto platform_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
    if (!platform_formats) {
      return nullptr;
    }

    bool hardware = platform_formats->avcodec_base_dev_type != AV_HWDEVICE_TYPE_NONE;

    auto &video_format = encoder.codec_from_config(config);
    if (!video_format[encoder_t::PASSED] || !disp->is_codec_supported(video_format.name, config)) {
      BOOST_LOG(error) << encoder.name << ": "sv << video_format.name << " mode not supported"sv;
      return nullptr;
    }

    if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE]) {
      BOOST_LOG(error) << video_format.name << ": dynamic range not supported"sv;
      return nullptr;
    }

    if (config.chromaSamplingType == 1 && !video_format[encoder_t::YUV444]) {
      BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 not supported"sv;
      return nullptr;
    }

    auto codec = avcodec_find_encoder_by_name(video_format.name.c_str());
    if (!codec) {
      BOOST_LOG(error) << "Couldn't open ["sv << video_format.name << ']';

      return nullptr;
    }

    auto colorspace = encode_device->colorspace;
    auto sw_fmt = (colorspace.bit_depth == 8 && config.chromaSamplingType == 0)  ? platform_formats->avcodec_pix_fmt_8bit :
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 1)  ? platform_formats->avcodec_pix_fmt_yuv444_8bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 0) ? platform_formats->avcodec_pix_fmt_10bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 1) ? platform_formats->avcodec_pix_fmt_yuv444_10bit :
                                                                                   AV_PIX_FMT_NONE;

    // Allow up to 1 retry to apply the set of fallback options.
    //
    // Note: If we later end up needing multiple sets of
    // fallback options, we may need to allow more retries
    // to try applying each set.
    avcodec_ctx_t ctx;
    for (int retries = 0; retries < 2; retries++) {
      ctx.reset(avcodec_alloc_context3(codec));
      ctx->width = config.width;
      ctx->height = config.height;

      // Use fractional framerate if available (for NTSC support)
      if (config.frameRateNum > 0 && config.frameRateDen > 0) {
        ctx->time_base = AVRational { config.frameRateDen, config.frameRateNum };
        ctx->framerate = AVRational { config.frameRateNum, config.frameRateDen };
        BOOST_LOG(debug) << "Using fractional framerate: " << config.frameRateNum << "/" << config.frameRateDen
                         << " (" << config.get_effective_framerate() << "fps)";
      }
      else {
        ctx->time_base = AVRational { 1, config.framerate };
        ctx->framerate = AVRational { config.framerate, 1 };
      }

      switch (config.videoFormat) {
        case 0:
          // 10-bit h264 encoding is not supported by our streaming protocol
          assert(!config.dynamicRange);
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_H264_HIGH_444_PREDICTIVE : AV_PROFILE_H264_HIGH;
          break;

        case 1:
          if (config.chromaSamplingType == 1) {
            // HEVC uses the same RExt profile for both 8 and 10 bit YUV 4:4:4 encoding
            ctx->profile = AV_PROFILE_HEVC_REXT;
          }
          else {
            ctx->profile = config.dynamicRange ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
          }
          break;

        case 2:
          // AV1 supports both 8 and 10 bit encoding with the same Main profile
          // but YUV 4:4:4 sampling requires High profile
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_AV1_HIGH : AV_PROFILE_AV1_MAIN;
          break;
      }

      // B-frames delay decoder output, so never use them
      ctx->max_b_frames = 0;

      // Use an infinite GOP length since I-frames are generated on demand
      ctx->gop_size = encoder.flags & LIMITED_GOP_SIZE ?
                        std::numeric_limits<std::int16_t>::max() :
                        std::numeric_limits<int>::max();

      ctx->keyint_min = std::numeric_limits<int>::max();

      // Some client decoders have limits on the number of reference frames
      if (config.numRefFrames) {
        if (video_format[encoder_t::REF_FRAMES_RESTRICT]) {
          ctx->refs = config.numRefFrames;
        }
        else {
          BOOST_LOG(warning) << "Client requested reference frame limit, but encoder doesn't support it!"sv;
        }
      }

      // We forcefully reset the flags to avoid clash on reuse of AVCodecContext
      ctx->flags = 0;
      ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;

      ctx->flags2 |= AV_CODEC_FLAG2_FAST;

      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

      ctx->color_range = avcodec_colorspace.range;
      ctx->color_primaries = avcodec_colorspace.primaries;
      ctx->color_trc = avcodec_colorspace.transfer_function;
      ctx->colorspace = avcodec_colorspace.matrix;

      // Used by cbs::make_sps_hevc
      ctx->sw_pix_fmt = sw_fmt;

      if (hardware) {
        avcodec_buffer_t encoding_stream_context;

        ctx->pix_fmt = platform_formats->avcodec_dev_pix_fmt;

        // Create the base hwdevice context
        auto buf_or_error = platform_formats->init_avcodec_hardware_input_buffer(encode_device.get());
        if (buf_or_error.has_right()) {
          return nullptr;
        }
        encoding_stream_context = std::move(buf_or_error.left());

        // If this encoder requires derivation from the base, derive the desired type
        if (platform_formats->avcodec_derived_dev_type != AV_HWDEVICE_TYPE_NONE) {
          avcodec_buffer_t derived_context;

          // Allow the hwdevice to prepare for this type of context to be derived
          if (encode_device->prepare_to_derive_context(platform_formats->avcodec_derived_dev_type)) {
            return nullptr;
          }

          auto err = av_hwdevice_ctx_create_derived(&derived_context, platform_formats->avcodec_derived_dev_type, encoding_stream_context.get(), 0);
          if (err) {
            char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
            BOOST_LOG(error) << "Failed to derive device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

            return nullptr;
          }

          encoding_stream_context = std::move(derived_context);
        }

        // Initialize avcodec hardware frames
        {
          avcodec_buffer_t frame_ref { av_hwframe_ctx_alloc(encoding_stream_context.get()) };

          auto frame_ctx = (AVHWFramesContext *) frame_ref->data;
          frame_ctx->format = ctx->pix_fmt;
          frame_ctx->sw_format = sw_fmt;
          frame_ctx->height = ctx->height;
          frame_ctx->width = ctx->width;
          frame_ctx->initial_pool_size = 0;

          // Allow the hwdevice to modify hwframe context parameters
          encode_device->init_hwframes(frame_ctx);

          if (auto err = av_hwframe_ctx_init(frame_ref.get()); err < 0) {
            return nullptr;
          }

          ctx->hw_frames_ctx = av_buffer_ref(frame_ref.get());
        }

        ctx->slices = config.slicesPerFrame;
      }
      else /* software */ {
        ctx->pix_fmt = sw_fmt;

        // Clients will request for the fewest slices per frame to get the
        // most efficient encode, but we may want to provide more slices than
        // requested to ensure we have enough parallelism for good performance.
        ctx->slices = std::max(config.slicesPerFrame, config::video.min_threads);
      }

      if (encoder.flags & SINGLE_SLICE_ONLY) {
        ctx->slices = 1;
      }

      ctx->thread_type = FF_THREAD_SLICE;
      ctx->thread_count = ctx->slices;

      AVDictionary *options { nullptr };
      auto handle_option = [&options, &config](const encoder_t::option_t &option) {
        std::visit(
          util::overloaded {
            [&](int v) {
              av_dict_set_int(&options, option.name.c_str(), v, 0);
            },
            [&](int *v) {
              av_dict_set_int(&options, option.name.c_str(), *v, 0);
            },
            [&](std::optional<int> *v) {
              if (*v) {
                av_dict_set_int(&options, option.name.c_str(), **v, 0);
              }
            },
            [&](const std::function<int()> &v) {
              av_dict_set_int(&options, option.name.c_str(), v(), 0);
            },
            [&](const std::string &v) {
              av_dict_set(&options, option.name.c_str(), v.c_str(), 0);
            },
            [&](std::string *v) {
              if (!v->empty()) {
                av_dict_set(&options, option.name.c_str(), v->c_str(), 0);
              }
            },
            [&](const std::function<const std::string(const config_t &cfg)> &v) {
              av_dict_set(&options, option.name.c_str(), v(config).c_str(), 0);
            } },
          option.value);
      };

      // Apply common options, then format-specific overrides
      for (auto &option : video_format.common_options) {
        handle_option(option);
      }
      for (auto &option : (config.dynamicRange ? video_format.hdr_options : video_format.sdr_options)) {
        handle_option(option);
      }
      if (config.chromaSamplingType == 1) {
        for (auto &option : (config.dynamicRange ? video_format.hdr444_options : video_format.sdr444_options)) {
          handle_option(option);
        }
      }
      if (retries > 0) {
        for (auto &option : video_format.fallback_options) {
          handle_option(option);
        }
      }

      auto bitrate = config.bitrate * 1000;
      BOOST_LOG(info) << "Streaming bitrate is " << bitrate;
      ctx->rc_max_rate = bitrate;
      ctx->bit_rate = bitrate;

      if (encoder.flags & CBR_WITH_VBR) {
        // Ensure rc_max_bitrate != bit_rate to force VBR mode
        ctx->bit_rate--;
      }
      else {
        ctx->rc_min_rate = bitrate;
      }

      if (encoder.flags & RELAXED_COMPLIANCE) {
        ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
      }

      if (!(encoder.flags & NO_RC_BUF_LIMIT)) {
        // Use effective framerate for VBV buffer calculation (supports NTSC fractional framerates)
        double effective_fps = config.get_effective_framerate();

        if (!hardware && (ctx->slices > 1 || config.videoFormat == 1)) {
          // Use a larger rc_buffer_size for software encoding when slices are enabled,
          // because libx264 can severely degrade quality if the buffer is too small.
          // libx265 encounters this issue more frequently, so always scale the
          // buffer by 1.5x for software HEVC encoding.
          ctx->rc_buffer_size = static_cast<int>(bitrate / (effective_fps * 10 / 15));
        }
        else {
          ctx->rc_buffer_size = static_cast<int>(bitrate / effective_fps);

#ifndef __APPLE__
          if (encoder.name == "nvenc" && config::video.nv_legacy.vbv_percentage_increase > 0) {
            ctx->rc_buffer_size += ctx->rc_buffer_size * config::video.nv_legacy.vbv_percentage_increase / 100;
          }
#endif
        }
      }

      // Allow the encoding device a final opportunity to set/unset or override any options
      encode_device->init_codec_options(ctx.get(), &options);

      if (auto status = avcodec_open2(ctx.get(), codec, &options)) {
        char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };

        if (!video_format.fallback_options.empty() && retries == 0) {
          BOOST_LOG(info)
            << "Retrying with fallback configuration options for ["sv << video_format.name << "] after error: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          continue;
        }
        else {
          BOOST_LOG(error)
            << "Could not open codec ["sv
            << video_format.name << "]: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          return nullptr;
        }
      }

      // Successfully opened the codec
      break;
    }

    avcodec_frame_t frame { av_frame_alloc() };
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->chroma_location = ctx->chroma_sample_location;

    uint16_t dynamic_metadata_target_peak_nits = 1000;

    // Attach HDR metadata to the AVFrame
    // Both PQ (ST 2084) and HLG (ARIB STD-B67) can carry HDR metadata.
    // PQ uses absolute luminance and requires static metadata (MDCV, CLL).
    // HLG uses scene-referred relative luminance but benefits from HDR Vivid (CUVA)
    // dynamic metadata for enhanced tone mapping on capable displays, where the
    // codec defines a carriage for it.
    if (colorspace_is_hdr(colorspace)) {
      // Single source of truth for which dynamic formats this transfer function allows
      // and this codec can actually carry, shared with the native NVENC path so the two
      // cannot drift apart.
      const auto dynamic_hdr_formats = hdr_metadata::formats_for(colorspace, config.videoFormat);

      SS_HDR_METADATA hdr_metadata;
      bool has_metadata = get_effective_hdr_metadata(disp, config, hdr_metadata);

      if (has_metadata) {
        apply_client_target_luminance(hdr_metadata, config);
        dynamic_metadata_target_peak_nits = hdr_metadata::resolve_target_display_luminance(
          config.hdr_capabilities.reported,
          config.hdr_capabilities.max_nits,
          hdr_metadata.maxDisplayLuminance);
        // Attach static HDR metadata (Mastering Display Color Volume + Content Light Level)
        // Required for PQ, optional but beneficial for HLG with HDR Vivid
        auto mdm = av_mastering_display_metadata_create_side_data(frame.get());

        mdm->display_primaries[0][0] = av_make_q(hdr_metadata.displayPrimaries[0].x, 50000);
        mdm->display_primaries[0][1] = av_make_q(hdr_metadata.displayPrimaries[0].y, 50000);
        mdm->display_primaries[1][0] = av_make_q(hdr_metadata.displayPrimaries[1].x, 50000);
        mdm->display_primaries[1][1] = av_make_q(hdr_metadata.displayPrimaries[1].y, 50000);
        mdm->display_primaries[2][0] = av_make_q(hdr_metadata.displayPrimaries[2].x, 50000);
        mdm->display_primaries[2][1] = av_make_q(hdr_metadata.displayPrimaries[2].y, 50000);

        mdm->white_point[0] = av_make_q(hdr_metadata.whitePoint.x, 50000);
        mdm->white_point[1] = av_make_q(hdr_metadata.whitePoint.y, 50000);

        mdm->min_luminance = av_make_q(hdr_metadata.minDisplayLuminance, 10000);
        mdm->max_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);

        mdm->has_luminance = hdr_metadata.maxDisplayLuminance != 0 ? 1 : 0;
        mdm->has_primaries = hdr_metadata.displayPrimaries[0].x != 0 ? 1 : 0;

        if (hdr_metadata.maxContentLightLevel != 0 || hdr_metadata.maxFrameAverageLightLevel != 0) {
          auto clm = av_content_light_metadata_create_side_data(frame.get());

          clm->MaxCLL = hdr_metadata.maxContentLightLevel;
          clm->MaxFALL = hdr_metadata.maxFrameAverageLightLevel;
        }

        // HDR10+ dynamic metadata - PQ only (Samsung ST 2094-40, uses absolute luminance)
        if (dynamic_hdr_formats.hdr10plus) {
          auto hdr10plus = av_dynamic_hdr_plus_create_side_data(frame.get());
          if (hdr10plus) {
            // Set default values for HDR10+
            hdr10plus->itu_t_t35_country_code = 0xB5;  // USA
            hdr10plus->application_version = hdr_metadata::hdr10plus_application_version;
            hdr10plus->num_windows = 1;  // Single processing window covering entire frame

            // Initialize the first (and only) processing window
            auto &params = hdr10plus->params[0];
            params.window_upper_left_corner_x = av_make_q(0, 1);
            params.window_upper_left_corner_y = av_make_q(0, 1);
            params.window_lower_right_corner_x = av_make_q(1, 1);
            params.window_lower_right_corner_y = av_make_q(1, 1);

            // Set center of elliptical pixel selector to center of frame
            params.center_of_ellipse_x = static_cast<uint16_t>(config.width / 2);
            params.center_of_ellipse_y = static_cast<uint16_t>(config.height / 2);
            params.rotation_angle = 0;  // 0 degrees
            params.semimajor_axis_internal_ellipse = static_cast<uint16_t>(config.width / 2);
            params.semimajor_axis_external_ellipse = static_cast<uint16_t>(config.width / 2);
            params.semiminor_axis_external_ellipse = static_cast<uint16_t>(config.height / 2);
            params.overlap_process_option = AV_HDR_PLUS_OVERLAP_PROCESS_WEIGHTED_AVERAGING;

            // Set maxscl (maximum of R, G, B) to 1.0 (full brightness)
            params.maxscl[0] = av_make_q(1, 1);
            params.maxscl[1] = av_make_q(1, 1);
            params.maxscl[2] = av_make_q(1, 1);

            // Set average maxRGB to 1.0
            params.average_maxrgb = av_make_q(1, 1);

            // Initialize percentile distribution (simplified)
            params.num_distribution_maxrgb_percentiles = 0;  // No percentiles for simplified metadata

            // Set fraction brightness to 0 (no bright pixels)
            params.fraction_bright_pixels = av_make_q(0, 1);

            // Set tone mapping curve to linear (no adjustment)
            params.tone_mapping_flag = 0;
            params.knee_point_x = av_make_q(0, 1);
            params.knee_point_y = av_make_q(0, 1);
            params.num_bezier_curve_anchors = 0;

            hdr10plus->targeted_system_display_maximum_luminance = av_make_q(
              dynamic_metadata_target_peak_nits,
              1);
            hdr10plus->targeted_system_display_actual_peak_luminance_flag = 0;
            hdr10plus->mastering_display_actual_peak_luminance_flag = 0;

            BOOST_LOG(debug) << "Added HDR10+ dynamic metadata to frame";
          }
        }

        // HDR Vivid dynamic metadata (GB/T 46269.1-2025) - both PQ and HLG.
        //
        // NOTE: this side data currently cannot reach the bitstream on the avcodec path.
        // FFmpeg ships a serializer for HDR10+ (av_dynamic_hdr_plus_to_t35) but has no
        // CUVA counterpart: libavcodec/dynamic_hdr_vivid.c defines only
        // ff_parse_itu_t_t35_to_dynamic_hdr_vivid, i.e. parsing for decode. So no FFmpeg
        // encoder turns AV_FRAME_DATA_DYNAMIC_HDR_VIVID into an SEI/OBU today.
        //
        // We still attach and maintain it so the metadata is correct the moment a
        // serializer exists, but HDR Vivid output is in practice only produced by the
        // native NVENC path (see nvenc_base.cpp, which hand-writes the T.35 payload).
        // Encoders routed through avcodec (QSV, AMF, software) will not emit it.
        //
        // Field values are deliberately left zero-initialized rather than filled with
        // invented defaults: update_hdr_dynamic_metadata() populates them from real
        // analyzer statistics once the 32-frame filter reports valid output, and a
        // fabricated "average 0.5 / maximum 1.0" frame is worse than an absent one.
        if (dynamic_hdr_formats.vivid) {
          auto vivid = av_dynamic_hdr_vivid_create_side_data(frame.get());
          if (vivid) {
            vivid->system_start_code = 0x01;
            vivid->num_windows = 0x01;  // Fixed at one for system_start_code 0x01

            auto &params = vivid->params[0];

            // Statistics mode only: no tone mapping curve, no saturation mapping.
            params.tone_mapping_mode_flag = 0;
            params.tone_mapping_param_num = 0;
            params.color_saturation_mapping_flag = 0;
            params.color_saturation_num = 0;

            const auto target_display_pq =
              hdr_metadata::target_display_pq_u12(static_cast<float>(dynamic_metadata_target_peak_nits));
            for (int i = 0; i < 2; i++) {
              auto &tm_params = params.tm_params[i];
              tm_params.targeted_system_display_maximum_luminance =
                av_make_q(target_display_pq, hdr_metadata::pq_u12_den);
              tm_params.base_enable_flag = 0;
            }

            BOOST_LOG(debug) << "Attached HDR Vivid side data to frame"
                             << (colorspace_is_hlg(colorspace) ? " (HLG mode)" : " (PQ mode)")
                             << " - note: no FFmpeg encoder serializes it yet";
          }
        }
      }
      else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }

    std::unique_ptr<platf::avcodec_encode_device_t> encode_device_final;

    if (!encode_device->data) {
      auto software_encode_device = std::make_unique<avcodec_software_encode_device_t>();

      if (software_encode_device->init(width, height, frame.get(), sw_fmt, hardware)) {
        return nullptr;
      }
      software_encode_device->colorspace = colorspace;
      software_encode_device->video_format = config.videoFormat;

      encode_device_final = std::move(software_encode_device);
    }
    else {
      encode_device_final = std::move(encode_device);
    }

    if (encode_device_final->set_frame(frame.release(), ctx->hw_frames_ctx)) {
      return nullptr;
    }

    encode_device_final->apply_colorspace();

    auto session = std::make_unique<avcodec_encode_session_t>(
      std::move(ctx),
      std::move(encode_device_final),
      dynamic_metadata_target_peak_nits,

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - (int) video_format[encoder_t::VUI_PARAMETERS]) * (1 + config.videoFormat) : 0);

    return session;
  }

  /**
   * The L6 the RPU carries mirrors the host display's RAW mastering metadata,
   * deliberately not run through apply_client_target_luminance(): the RPU
   * describes the content, and the client display's peak belongs to display
   * adaptation, which the terminal Dolby engine already knows (docs §3.3).
   */
  std::optional<dolby_vision::session_config_t>
  dolby_vision_config_for_session(platf::display_t *disp, const config_t &client_config) {
    SS_HDR_METADATA raw;
    if (!get_effective_hdr_metadata(disp, client_config, raw)) {
      return std::nullopt;
    }

    const auto clamp_u16 = [](uint16_t value, uint16_t low, uint16_t high) {
      return std::clamp(value, low, high);
    };

    dolby_vision::session_config_t config;
    config.source_mastering_peak_nits = clamp_u16(raw.maxDisplayLuminance, 1, 10000);
    config.mastering_min_nits_x10000 = clamp_u16(raw.minDisplayLuminance, 1, 10000);
    config.max_cll_nits = clamp_u16(raw.maxContentLightLevel, 0, 10000);
    config.max_fall_nits = clamp_u16(raw.maxFrameAverageLightLevel, 0, 10000);
    return config;
  }

  std::unique_ptr<nvenc_encode_session_t>
  make_nvenc_encode_session(platf::display_t *disp, const config_t &client_config, std::unique_ptr<platf::nvenc_encode_device_t> encode_device, bool is_probe = false) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace, is_probe)) {
      return nullptr;
    }

    // Set HDR metadata for NVENC encoder if HDR is enabled (both PQ and HLG)
    // PQ needs mastering display + content light level SEI for proper absolute luminance mapping.
    // HLG benefits from these SEI for HDR Vivid tone mapping on the decoder side.
    if (colorspace_is_hdr(encode_device->colorspace) && encode_device->nvenc) {
      SS_HDR_METADATA hdr_metadata;
      if (get_effective_hdr_metadata(disp, client_config, hdr_metadata)) {
        apply_client_target_luminance(hdr_metadata, client_config);
        nvenc::nvenc_hdr_metadata nvenc_metadata;
        // Copy display primaries (RGB order)
        for (int i = 0; i < 3; i++) {
          nvenc_metadata.displayPrimaries[i].x = hdr_metadata.displayPrimaries[i].x;
          nvenc_metadata.displayPrimaries[i].y = hdr_metadata.displayPrimaries[i].y;
        }
        nvenc_metadata.whitePoint.x = hdr_metadata.whitePoint.x;
        nvenc_metadata.whitePoint.y = hdr_metadata.whitePoint.y;
        nvenc_metadata.maxDisplayLuminance = hdr_metadata.maxDisplayLuminance;
        nvenc_metadata.minDisplayLuminance = hdr_metadata.minDisplayLuminance;
        nvenc_metadata.targetDisplayLuminance = hdr_metadata::resolve_target_display_luminance(
          client_config.hdr_capabilities.reported,
          client_config.hdr_capabilities.max_nits,
          hdr_metadata.maxDisplayLuminance);
        nvenc_metadata.maxContentLightLevel = hdr_metadata.maxContentLightLevel;
        nvenc_metadata.maxFrameAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel;
        encode_device->nvenc->set_hdr_metadata(nvenc_metadata);
        BOOST_LOG(info) << "NVENC: HDR metadata set - max luminance: " << nvenc_metadata.maxDisplayLuminance
                        << " nits, mode: " << (colorspace_is_hlg(encode_device->colorspace) ? "HLG" : "PQ");
      }
    }

    // The device moves into the session; sample its gates beforehand.
    const bool dv_analysis_usable = encode_device && hdr_luminance_analysis_usable(encode_device->hdr_luminance_analysis_available);
    const auto dv_format = static_cast<hdr::dynamic_hdr_format_e>(client_config.dynamic_hdr_format);
    const bool dv_negotiated = dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_81 ||
                               dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_84;
    // 8.1 rides PQ, 8.4 rides HLG: the RPU metadata domain is the same
    // (profile84.md Phase 0), only the required base-layer transfer differs.
    const bool dv_transfer_ok =
      encode_device &&
      ((dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_81 && colorspace_is_pq(encode_device->colorspace)) ||
        (dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_84 && colorspace_is_hlg(encode_device->colorspace)));
    auto session = std::make_unique<nvenc_encode_session_t>(std::move(encode_device), client_config.videoFormat);

    // Contract note: a negotiated-DV session that cannot carry the RPU still
    // serves a valid HDR10-compatible HEVC base layer; the client's decoder
    // routing must tolerate RPU absence and fall back. These warnings are the
    // diagnostics for exactly that path -- do not promise the RPU here.
    if (dv_negotiated && !is_probe) {
      // Dynamic L1 is the whole point of the pipeline; without analyzer
      // output the stream would carry nothing but a static template.
      if (!dv_analysis_usable) {
        BOOST_LOG(warning) << "NVENC: Dolby Vision negotiated but luminance analysis is unavailable; "
                              "streaming without RPU"sv;
      }
      else if (!dv_transfer_ok) {
        BOOST_LOG(warning) << "NVENC: Dolby Vision negotiated but the final colorspace does not match the "
                              "negotiated profile; streaming without RPU"sv;
      }
      else if (const auto dv_config = dolby_vision_config_for_session(disp, client_config);
               !dv_config || !session->dolby_vision_.configure(*dv_config)) {
        BOOST_LOG(warning) << "NVENC: Dolby Vision negotiated but no usable mastering metadata; "
                              "streaming without RPU"sv;
      }
      else if (dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_84) {
        BOOST_LOG(info) << "NVENC: Dolby Vision Profile 8.4 active (HLG base layer, mastering peak "
                        << dv_config->source_mastering_peak_nits << " nits)"sv;
      }
      else {
        BOOST_LOG(info) << "NVENC: Dolby Vision Profile 8.1 active (mastering peak "
                        << dv_config->source_mastering_peak_nits << " nits)"sv;
      }
    }

    return session;
  }

  std::unique_ptr<amf_encode_session_t>
  make_amf_encode_session(platf::display_t *disp, const config_t &client_config, std::unique_ptr<platf::amf_encode_device_t> encode_device, bool is_probe = false) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace, is_probe)) {
      return nullptr;
    }

    // Set HDR metadata for AMF encoder if HDR is enabled
    if (colorspace_is_hdr(encode_device->colorspace) && encode_device->amf) {
      SS_HDR_METADATA hdr_metadata;
      if (get_effective_hdr_metadata(disp, client_config, hdr_metadata)) {
        apply_client_target_luminance(hdr_metadata, client_config);
        amf::amf_hdr_metadata amf_metadata;
        for (int i = 0; i < 3; i++) {
          amf_metadata.displayPrimaries[i].x = hdr_metadata.displayPrimaries[i].x;
          amf_metadata.displayPrimaries[i].y = hdr_metadata.displayPrimaries[i].y;
        }
        amf_metadata.whitePoint.x = hdr_metadata.whitePoint.x;
        amf_metadata.whitePoint.y = hdr_metadata.whitePoint.y;
        amf_metadata.maxDisplayLuminance = hdr_metadata.maxDisplayLuminance;
        amf_metadata.minDisplayLuminance = hdr_metadata.minDisplayLuminance;
        amf_metadata.targetDisplayLuminance = hdr_metadata::resolve_target_display_luminance(
          client_config.hdr_capabilities.reported,
          client_config.hdr_capabilities.max_nits,
          hdr_metadata.maxDisplayLuminance);
        amf_metadata.maxContentLightLevel = hdr_metadata.maxContentLightLevel;
        amf_metadata.maxFrameAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel;
        encode_device->amf->set_hdr_metadata(amf_metadata);
        BOOST_LOG(info) << "AMF: HDR metadata set - max luminance: " << amf_metadata.maxDisplayLuminance
                        << " nits, mode: " << (colorspace_is_hlg(encode_device->colorspace) ? "HLG" : "PQ");
      }
    }

    // The device moves into the session; sample its gates beforehand.
    const bool dv_analysis_usable = encode_device && hdr_luminance_analysis_usable(encode_device->hdr_luminance_analysis_available);
    const auto dv_format = static_cast<hdr::dynamic_hdr_format_e>(client_config.dynamic_hdr_format);
    const bool dv_negotiated = dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_81 ||
                               dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_84;
    // 8.1 rides PQ, 8.4 rides HLG: the RPU metadata domain is the same
    // (profile84.md Phase 0), only the required base-layer transfer differs.
    const bool dv_transfer_ok =
      encode_device &&
      ((dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_81 && colorspace_is_pq(encode_device->colorspace)) ||
        (dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_84 && colorspace_is_hlg(encode_device->colorspace)));
    auto session = std::make_unique<amf_encode_session_t>(std::move(encode_device), client_config.videoFormat);

    // Contract note: a negotiated-DV session that cannot carry the RPU still
    // serves a valid HDR10-compatible HEVC base layer; the client's decoder
    // routing must tolerate RPU absence and fall back (docs dolby_vision_
    // profile81.md SS 5.3). These warnings are the diagnostics for exactly
    // that path -- do not promise the RPU here.
    if (dv_negotiated && !is_probe) {
      if (!dv_analysis_usable) {
        BOOST_LOG(warning) << "AMF: Dolby Vision negotiated but luminance analysis is unavailable; "
                              "streaming without RPU"sv;
      }
      else if (!dv_transfer_ok) {
        BOOST_LOG(warning) << "AMF: Dolby Vision negotiated but the final colorspace does not match the "
                              "negotiated profile; streaming without RPU"sv;
      }
      else if (const auto dv_config = dolby_vision_config_for_session(disp, client_config);
               !dv_config || !session->dolby_vision_.configure(*dv_config)) {
        BOOST_LOG(warning) << "AMF: Dolby Vision negotiated but no usable mastering metadata; "
                              "streaming without RPU"sv;
      }
      else if (dv_format == hdr::dynamic_hdr_format_e::dolby_vision_profile_84) {
        BOOST_LOG(info) << "AMF: Dolby Vision Profile 8.4 active (HLG base layer, mastering peak "
                        << dv_config->source_mastering_peak_nits << " nits)"sv;
      }
      else {
        BOOST_LOG(info) << "AMF: Dolby Vision Profile 8.1 active (mastering peak "
                        << dv_config->source_mastering_peak_nits << " nits)"sv;
      }
    }

    return session;
  }

  std::unique_ptr<encode_session_t>
  make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device, bool is_probe = false) {
    auto effective_config = config;
    effective_config.bitrate = cap_initial_encoder_bitrate(
      config.bitrate,
      config::video.max_bitrate,
      config::stream.fec_percentage
    );
    if (!is_probe && effective_config.bitrate != config.bitrate) {
      BOOST_LOG(info) << "Capping initial encoder bitrate from " << config.bitrate
                      << " Kbps to " << effective_config.bitrate
                      << " Kbps (host maximum total bitrate: " << config::video.max_bitrate
                      << " Kbps, FEC: " << config::stream.fec_percentage << "%)";
    }

    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      // The RPU splice needs to grow the encoded access unit, which only the
      // native NVENC/AMF paths own end to end; an AVPacket cannot be resized
      // in place. The stream stays a valid HDR10-compatible HEVC base layer,
      // so the client is served — just without the Dolby Vision metadata.
      if (!is_probe &&
          (effective_config.dynamic_hdr_format == static_cast<int>(hdr::dynamic_hdr_format_e::dolby_vision_profile_81) ||
            effective_config.dynamic_hdr_format == static_cast<int>(hdr::dynamic_hdr_format_e::dolby_vision_profile_84))) {
        BOOST_LOG(warning) << "Dolby Vision negotiated but an avcodec-family encoder was selected; "
                              "streaming HDR10 without RPU"sv;
      }
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, effective_config, width, height, std::move(avcodec_encode_device));
    }
    else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(disp, effective_config, std::move(nvenc_encode_device), is_probe);
    }
    else if (dynamic_cast<platf::amf_encode_device_t *>(encode_device.get())) {
      auto amf_encode_device = boost::dynamic_pointer_cast<platf::amf_encode_device_t>(std::move(encode_device));
      return make_amf_encode_session(disp, effective_config, std::move(amf_encode_device), is_probe);
    }

    return nullptr;
  }

  /**
   * @brief Get NTSC framerate for a given integer framerate.
   * @details NTSC framerates are slightly lower than integer framerates:
   *          120 -> 119.88 (120000/1001)
   *          60 -> 59.94 (60000/1001)
   *          30 -> 29.97 (30000/1001)
   *          24 -> 23.976 (24000/1001)
   * @param fps Integer framerate
   * @param num Output numerator
   * @param den Output denominator
   * @return true if NTSC framerate is available for this fps
   */
  bool
  get_ntsc_framerate(int fps, int &num, int &den) {
    // NTSC framerate pattern: fps * 1000 / 1001
    // Only support common framerates that have NTSC equivalents
    static const int supported_fps[] = { 24, 30, 48, 60, 120, 144, 240 };
    for (int supported : supported_fps) {
      if (fps == supported) {
        num = fps * 1000;
        den = 1001;
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Create encode session with NTSC framerate fallback.
   * @details If the initial framerate fails, try NTSC framerate (e.g., 120 -> 119.88fps).
   * @param disp Display device
   * @param encoder Encoder to use
   * @param config Configuration (may be modified if NTSC fallback is used)
   * @param width Frame width
   * @param height Frame height
   * @param make_encode_device_func Function to create encode device
   * @return Encode session or nullptr on failure
   */
  std::unique_ptr<encode_session_t>
  make_encode_session_with_ntsc_fallback(
    platf::display_t *disp,
    const encoder_t &encoder,
    config_t &config,
    int width,
    int height,
    std::function<std::unique_ptr<platf::encode_device_t>()> make_encode_device_func) {
    // First try with original framerate
    auto encode_device = make_encode_device_func();
    if (!encode_device) {
      return nullptr;
    }

    auto session = make_encode_session(disp, encoder, config, width, height, std::move(encode_device));
    if (session) {
      return session;
    }

    // If failed, try NTSC framerate fallback
    int ntsc_num, ntsc_den;
    if (get_ntsc_framerate(config.framerate, ntsc_num, ntsc_den)) {
      BOOST_LOG(info) << "Encoder initialization failed at " << config.framerate << "fps, "
                      << "trying NTSC framerate " << ntsc_num << "/" << ntsc_den
                      << " (" << (double) ntsc_num / ntsc_den << "fps)";

      config.frameRateNum = ntsc_num;
      config.frameRateDen = ntsc_den;

      // Create new encode device with NTSC framerate
      encode_device = make_encode_device_func();
      if (!encode_device) {
        BOOST_LOG(warning) << "Failed to create encode device with NTSC framerate";
        // Reset to integer framerate
        config.frameRateNum = 0;
        config.frameRateDen = 1;
        return nullptr;
      }

      session = make_encode_session(disp, encoder, config, width, height, std::move(encode_device));
      if (session) {
        BOOST_LOG(info) << "Successfully initialized encoder with NTSC framerate "
                        << (double) ntsc_num / ntsc_den << "fps";
        return session;
      }

      // Reset to integer framerate if NTSC also failed
      config.frameRateNum = 0;
      config.frameRateDen = 1;
      BOOST_LOG(warning) << "NTSC framerate fallback also failed";
    }

    return nullptr;
  }

  void
  encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    captured_frame_event_t images,
    config_t &config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data,
    std::optional<safe::mail_raw_t::event_t<dynamic_param_t>> dynamic_param_events) {
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return;
    }

    // As a workaround for NVENC hangs and to generally speed up encoder reinit,
    // we will complete the encoder teardown in a separate thread if supported.
    // This will move expensive processing off the encoder thread to allow us
    // to restart encoding as soon as possible. For cases where the NVENC driver
    // hang occurs, this thread may probably never exit, but it will allow
    // streaming to continue without requiring a full restart of Sunshine.
    auto fail_guard = util::fail_guard([&encoder, &session] {
      if (encoder.flags & ASYNC_TEARDOWN) {
        std::thread encoder_teardown_thread { [session = std::move(session)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          session.reset();
          BOOST_LOG(info) << "Async encoder teardown complete";
        } };
        encoder_teardown_thread.detach();
      }
    });

    // Set the base minimum frame time based on client-requested target framerate or minimum_fps_target.
    // This can be temporarily reduced later if VRR input activity boost is active.
    const auto base_minimum_frame_time = minimum_frame_time_for_vrr(config.framerate, config::video.minimum_fps_target);
    if (config::video.minimum_fps_target > 0) {
      BOOST_LOG(info) << "Minimum frame time set to "sv << base_minimum_frame_time.count() << "ms, based on minimum_fps_target "sv << config::video.minimum_fps_target << " fps."sv;
    }
    else {
      BOOST_LOG(info) << "Minimum frame time set to "sv << base_minimum_frame_time.count() << "ms, based on client-requested target framerate "sv << config.framerate << "."sv;
    }

    const auto input_activity_boost_policy = make_input_activity_boost_policy({
      config::video.variable_refresh_rate,
      config::video.input_activity_boost,
      config.framerate,
      config::video.minimum_fps_target,
      config::video.input_activity_boost_fps,
      config::video.input_activity_boost_window_ms,
    });
    const auto input_activity_boost_window = std::chrono::milliseconds { config::video.input_activity_boost_window_ms };

    if (input_activity_boost_policy.useful) {
      BOOST_LOG(info) << "Input activity boost enabled: floor="sv
                      << input_activity_boost_policy.fps
                      << " fps, window="sv << config::video.input_activity_boost_window_ms << " ms"sv;
    }
    else if (input_activity_boost_policy.configured) {
      BOOST_LOG(info) << "Input activity boost configured but not enabled because it would not raise the current VRR minimum cadence."sv;
    }

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
    auto dynamic_param_events_ptr = dynamic_param_events.value_or(mail::man->event<dynamic_param_t>(mail::dynamic_param_change));
    auto input_activity_event = mail->event<std::chrono::steady_clock::time_point>(mail::input_activity);
    auto input_boost_until = std::chrono::steady_clock::time_point::min();

    auto consume_input_activity = [&]() {
      while (input_activity_event->peek()) {
        if (auto activity = input_activity_event->pop(0ms)) {
          input_boost_until = std::max(input_boost_until, *activity + input_activity_boost_window);
        }
      }
    };

    using image_pop_result_t = decltype(images->pop(0ms));
    const auto input_activity_poll_interval = std::chrono::duration<double, std::milli> { 5ms };
    auto pop_image_interruptible = [&](const std::chrono::duration<double, std::milli> &wait_time, bool allow_input_preemption) -> image_pop_result_t {
      if (!allow_input_preemption || wait_time <= input_activity_poll_interval) {
        return images->pop(wait_time);
      }

      auto remaining_wait = wait_time;
      while (images->running() && remaining_wait > 0ms) {
        auto slice_wait = std::min(remaining_wait, input_activity_poll_interval);
        if (auto img = images->pop(slice_wait)) {
          return img;
        }

        consume_input_activity();
        if (std::chrono::steady_clock::now() < input_boost_until) {
          return {};
        }

        remaining_wait -= slice_wait;
      }

      return {};
    };

    {
      // Load a dummy image into the AVFrame to ensure we have something to encode
      // even if we timeout waiting on the first frame. This is a relatively large
      // allocation which can be freed immediately after convert(), so we do this
      // in a separate scope.
      auto dummy_img = disp->alloc_img();
      if (!dummy_img || disp->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
        return;
      }
    }

    while (true) {
      // Break out of the encoding loop if any of the following are true:
      // a) The stream is ending
      // b) Sunshine is quitting
      // c) The capture side is waiting to reinit and we've encoded at least one frame
      //
      // If we have to reinit before we have received any captured frames, we will encode
      // the blank dummy frame just to let Moonlight know that we're alive.
      if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
        break;
      }

      bool requested_idr_frame = false;

      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_events->peek()) {
        requested_idr_frame = true;
        idr_events->pop();
      }

      // 处理动态参数调整
      while (dynamic_param_events_ptr->peek()) {
        if (auto param = dynamic_param_events_ptr->pop(0ms)) {
          BOOST_LOG(info) << "Applying dynamic parameter change: type=" << (int) param->type;
          if (param->type == dynamic_param_type_e::CLIENT_SDR_WHITE_NITS) {
            // Keep the latest value in the video-thread-owned config. If the
            // encoder is recreated after a display/capture reinit, device
            // construction will apply this value again.
            config.hdr_capabilities.sdr_white_nits = param->value.float_value;
          }
          session->set_dynamic_param(*param);
        }
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      consume_input_activity();
      auto input_boost_active = input_activity_boost_policy.useful && std::chrono::steady_clock::now() < input_boost_until;
      auto effective_frame_time = effective_minimum_frame_time(
        base_minimum_frame_time,
        input_activity_boost_policy,
        input_boost_active,
        config::video.minimum_fps_target);

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
      std::optional<platf::frame_pipeline_trace_t> pipeline_trace;
      bool has_new_frame = false;

      // Encode at a minimum FPS to avoid image quality issues with static content
      // When variable_refresh_rate is enabled, only encode when we have a new frame
      if (!requested_idr_frame || images->peek()) {
        if (auto frame = pop_image_interruptible(effective_frame_time, input_activity_boost_policy.useful && !input_boost_active)) {
          auto &img = frame->image;
          if (!frame->is_replay) {
            frame_timestamp = img->frame_timestamp;
            pipeline_trace = img->pipeline_trace.value_or(platf::frame_pipeline_trace_t {});
            if (!pipeline_trace->capture_ready) {
              pipeline_trace->capture_ready = frame_timestamp;
            }
            pipeline_trace->convert_begin = std::chrono::steady_clock::now();
          }
          if (session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            // Don't exit permanently — break to let the outer reinit loop handle recovery
            break;
          }
          if (pipeline_trace) {
            pipeline_trace->convert_end = std::chrono::steady_clock::now();
          }
          has_new_frame = true;
        }
        else if (!images->running()) {
          break;
        }
      }

      consume_input_activity();
      input_boost_active = input_activity_boost_policy.useful && std::chrono::steady_clock::now() < input_boost_until;

      // While streaming check to see if the mouse is present and enable Mouse Keys to force the cursor to appear.
      // Run this BEFORE the VRR early-continue so a KVM switch on a static screen still recovers the cursor
      // even when no new frame would be encoded.
      platf::enable_mouse_keys();

      // If variable refresh rate is enabled, skip encoding when no new frame is available
      // This allows the stream framerate to match the render framerate for VRR support
      // However, if minimum_fps_target is set, or input activity boost is active, we still encode
      // to maintain a temporary minimum FPS floor for better visual input feedback.
      if (config::video.variable_refresh_rate && !has_new_frame && !requested_idr_frame) {
        // Only skip if minimum_fps_target is 0 (disabled) and input activity boost is inactive.
        if (config::video.minimum_fps_target == 0 && !input_boost_active) {
          continue;
        }
        // If minimum_fps_target is set or boost is active, we'll encode anyway to maintain minimum FPS.
      }

      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp, std::move(pipeline_trace))) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        // Don't exit permanently — break to let the outer reinit loop handle recovery
        break;
      }

      session->request_normal_frame();
    }
  }

  input::touch_port_t
  make_port(platf::display_t *display, const config_t &config) {
    float wd = display->width;
    float hd = display->height;

    float wt = config.width;
    float ht = config.height;

    auto scalar = std::fminf(wt / wd, ht / hd);

    auto w2 = scalar * wd;
    auto h2 = scalar * hd;

    auto offsetX = (config.width - w2) * 0.5f;
    auto offsetY = (config.height - h2) * 0.5f;

    return input::touch_port_t {
      {
        display->offset_x,
        display->offset_y,
        config.width,
        config.height,
      },
      display->env_width,
      display->env_height,
      offsetX,
      offsetY,
      1.0f / scalar,
    };
  }

  /**
   * @brief Disable the pre-encode filter when the opened display cannot satisfy
   *        its preconditions, keeping the wire signal consistent with the pixels
   *        actually produced (rtx_hdr_stream_implementation.md §5.3
   *        source_display_not_sdr / §6.3 capability-probe degradation).
   */
  void
  strip_unusable_pre_encode_filter(platf::display_t &disp, config_t &config) {
    if (config.pre_encode_filter == platf::pre_encode_filter_e::none) {
      return;
    }
    if (!disp.supports_pre_encode_filter()) {
      BOOST_LOG(warning) << "Pre-encode filter is not supported by this capture/encode path; disabling RTX HDR for this session"sv;
    }
    else if (disp.is_hdr()) {
      BOOST_LOG(warning) << "Source display is already in HDR mode (source_display_not_sdr); disabling RTX HDR for this session"sv;
    }
    else {
      return;
    }
    config.pre_encode_filter = platf::pre_encode_filter_e::none;
    config.frame_pipeline_policy = platf::resolve_frame_pipeline_policy(config.dynamicRange, false);
    config.frame_pipeline_policy_resolved = true;
  }

  std::unique_ptr<platf::encode_device_t>
  make_encode_device(platf::display_t &disp, const encoder_t &encoder, const config_t &config) {
    std::unique_ptr<platf::encode_device_t> result;

    auto colorspace = colorspace_from_client_config(config, disp.is_hdr());

    platf::pix_fmt_e pix_fmt;
    if (config.chromaSamplingType == 1) {
      // YUV 4:4:4
      if (!(encoder.flags & YUV444_SUPPORT)) {
        // Encoder can't support YUV 4:4:4 regardless of hardware capabilities
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv444_10bit :
                  encoder.platform_formats->pix_fmt_yuv444_8bit;
    }
    else {
      // YUV 4:2:0
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_10bit :
                  encoder.platform_formats->pix_fmt_8bit;
    }

    {
      auto encoder_name = encoder.codec_from_config(config).name;

      auto color_coding = colorspace.colorspace == colorspace_e::bt2020    ? "HDR (Rec. 2020 + SMPTE 2084 PQ)" :
                          colorspace.colorspace == colorspace_e::bt2020hlg ? "HDR (Rec. 2020 + HLG)" :
                          colorspace.colorspace == colorspace_e::rec601    ? "SDR (Rec. 601)" :
                          colorspace.colorspace == colorspace_e::rec709    ? "SDR (Rec. 709)" :
                          colorspace.colorspace == colorspace_e::bt2020sdr ? "SDR (Rec. 2020)" :
                                                                             "unknown";

      BOOST_LOG(info) << "Creating encoder " << logging::bracket(encoder_name)
                      << ", Color coding: " << color_coding
                      << ", Color depth: " << colorspace.bit_depth << "-bit"
                      << ", Color range: " << (colorspace.full_range ? "JPEG" : "MPEG");
    }

    if (dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get())) {
      result = disp.make_avcodec_encode_device(pix_fmt);
    }
    else if (dynamic_cast<const encoder_platform_formats_nvenc *>(encoder.platform_formats.get())) {
      result = disp.make_nvenc_encode_device(pix_fmt);
    }
    else if (dynamic_cast<const encoder_platform_formats_amf *>(encoder.platform_formats.get())) {
      result = disp.make_amf_encode_device(pix_fmt);
    }

    if (result) {
      result->colorspace = colorspace;
      result->video_format = config.videoFormat;
    }

    return result;
  }

  std::optional<sync_session_t>
  make_synced_session(platf::display_t *disp, const encoder_t &encoder, platf::img_t &img, sync_session_ctx_t &ctx) {
    sync_session_t encode_session;

    encode_session.ctx = &ctx;

    // absolute mouse coordinates require that the dimensions of the screen are known
    ctx.touch_port_events->raise(make_port(disp, ctx.config));

    // Create encode device with NTSC framerate fallback support
    strip_unusable_pre_encode_filter(*disp, ctx.config);
    auto make_encode_device_func = [&]() {
      return make_encode_device(*disp, encoder, ctx.config);
    };

    auto session = make_encode_session_with_ntsc_fallback(
      disp, encoder, ctx.config, img.width, img.height, make_encode_device_func);
    if (!session) {
      return std::nullopt;
    }

    // The encode colorspace is fully determined by the client config and the
    // display's HDR state — building a throwaway encode device here used to
    // spin up a whole encoder pipeline (including a CUDA interop context for
    // 4:4:4) just to read that value back.
    const auto colorspace = colorspace_from_client_config(ctx.config, disp->is_hdr());

    // Update client with our current HDR display state
    hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (colorspace_is_hdr(colorspace)) {
      if (get_effective_hdr_metadata(disp, ctx.config, hdr_info->metadata)) {
        hdr_info->enabled = true;
      }
      else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }
    ctx.hdr_events->raise(std::move(hdr_info));

    // Load the initial image to prepare for encoding
    if (session->convert(img)) {
      BOOST_LOG(error) << "Could not convert initial image"sv;
      return std::nullopt;
    }

    encode_session.session = std::move(session);

    return encode_session;
  }

  encode_e
  encode_run_sync(
    std::vector<std::unique_ptr<sync_session_ctx_t>> &synced_session_ctxs,
    encode_session_ctx_queue_t &encode_session_ctx_queue,
    std::vector<std::string> &display_names,
    int &display_p) {
    const auto &encoder = *chosen_encoder;

    std::shared_ptr<platf::display_t> disp;

    auto switch_display_event = mail::man->event<int>(mail::switch_display);
    auto active_display_event = mail::man->event<std::string>(mail::active_display);
    std::string active_display_name;

    if (synced_session_ctxs.empty()) {
      auto ctx = encode_session_ctx_queue.pop();
      if (!ctx) {
        return encode_e::ok;
      }

      synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*ctx)));
    }

    while (encode_session_ctx_queue.running()) {
      // Refresh display names since a display removal might have caused the reinitialization
      refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

      // Process any pending display switch with the new list of displays
      bool user_switched = false;
      if (switch_display_event->peek()) {
        display_p = std::clamp(*switch_display_event->pop(), 0, (int) display_names.size() - 1);
        user_switched = true;
      }

      // Use client-specified display_name if provided (only for auto-reinit, not manual switch)
      auto &config = synced_session_ctxs.front()->config;
      std::string target_display_name = display_names[display_p];
      if (!user_switched && !config.display_name.empty()) {
        // config.display_name may be a device ID - convert to display name
        std::string resolved_display_name = display_device::get_display_name(config.display_name);
        if (resolved_display_name.empty()) {
          resolved_display_name = config.display_name;
        }

        // Try to find the display in the list
        bool found = false;
        for (int x = 0; x < display_names.size(); ++x) {
          if (display_names[x] == resolved_display_name) {
            display_p = x;
            target_display_name = resolved_display_name;
            found = true;
            break;
          }
        }
        if (!found) {
          BOOST_LOG(warning) << "Client-specified display [" << config.display_name << "] (resolved: " << resolved_display_name << ") not found, using default display";
        }
      }

      if (user_switched) {
        for (auto &ctx : synced_session_ctxs) {
          ctx->config.display_name = target_display_name;
        }
      }

      // reset_display() will sleep between retries
      reset_display(disp, encoder.platform_formats->dev_type, target_display_name, config);
      if (disp) {
        active_display_name = target_display_name;
        active_display_event->raise(target_display_name);
        break;
      }
    }

    if (!disp) {
      return encode_e::error;
    }

    auto img = disp->alloc_img();
    if (!img || disp->dummy_img(img.get())) {
      return encode_e::error;
    }

    std::vector<sync_session_t> synced_sessions;
    for (auto &ctx : synced_session_ctxs) {
      auto synced_session = make_synced_session(disp.get(), encoder, *img, *ctx);
      if (!synced_session) {
        return encode_e::error;
      }

      synced_sessions.emplace_back(std::move(*synced_session));
    }

    auto ec = platf::capture_e::ok;
    while (encode_session_ctx_queue.running()) {
      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        while (encode_session_ctx_queue.peek()) {
          auto encode_session_ctx = encode_session_ctx_queue.pop();
          if (!encode_session_ctx) {
            return false;
          }

          // Synchronous sessions share the display opened above. Keep the
          // active target in newly joined contexts so the next reinit does not
          // restore their stale launch-time display selection.
          encode_session_ctx->config.display_name = active_display_name;
          synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*encode_session_ctx)));

          auto encode_session = make_synced_session(disp.get(), encoder, *img, *synced_session_ctxs.back());
          if (!encode_session) {
            ec = platf::capture_e::error;
            return false;
          }

          synced_sessions.emplace_back(std::move(*encode_session));
        }

        KITTY_WHILE_LOOP(auto pos = std::begin(synced_sessions), pos != std::end(synced_sessions), {
          auto ctx = pos->ctx;
          if (ctx->shutdown_event->peek()) {
            // Let waiting thread know it can delete shutdown_event
            ctx->join_event->raise(true);

            pos = synced_sessions.erase(pos);
            synced_session_ctxs.erase(std::find_if(std::begin(synced_session_ctxs), std::end(synced_session_ctxs), [&ctx_p = ctx](auto &ctx) {
              return ctx.get() == ctx_p;
            }));

            if (synced_sessions.empty()) {
              return false;
            }

            continue;
          }

          if (ctx->idr_events->peek()) {
            pos->session->request_idr_frame();
            ctx->idr_events->pop();
          }

          std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
          std::optional<platf::frame_pipeline_trace_t> pipeline_trace;
          if (img) {
            frame_timestamp = img->frame_timestamp;
          }

          if (frame_captured) {
            pipeline_trace = img->pipeline_trace.value_or(platf::frame_pipeline_trace_t {});
            if (!pipeline_trace->capture_ready) {
              pipeline_trace->capture_ready = frame_timestamp;
            }
            pipeline_trace->convert_begin = std::chrono::steady_clock::now();
          }

          if (frame_captured && pos->session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          if (frame_captured) {
            pipeline_trace->convert_end = std::chrono::steady_clock::now();
          }

          if (encode(ctx->frame_nr++, *pos->session, ctx->packets, ctx->channel_data, frame_timestamp, std::move(pipeline_trace))) {
            BOOST_LOG(error) << "Could not encode video packet"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          pos->session->request_normal_frame();

          ++pos;
        })

        if (switch_display_event->peek()) {
          ec = platf::capture_e::reinit;
          return false;
        }

        return true;
      };

      auto pull_free_image_callback = [&img](std::shared_ptr<platf::img_t> &img_out) -> bool {
        img_out = img;
        img_out->frame_timestamp.reset();
        img_out->pipeline_trace.reset();
        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);
      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return ec != platf::capture_e::ok ? ec : status;
      }
    }

    return encode_e::ok;
  }

  void
  captureThreadSync() {
    auto ref = capture_thread_sync.ref();

    std::vector<std::unique_ptr<sync_session_ctx_t>> synced_session_ctxs;

    auto &ctx = ref->encode_session_ctx_queue;
    auto lg = util::fail_guard([&]() {
      ctx.stop();

      for (auto &ctx : synced_session_ctxs) {
        ctx->shutdown_event->raise(true);
        ctx->join_event->raise(true);
      }

      for (auto &ctx : ctx.unsafe()) {
        ctx.shutdown_event->raise(true);
        ctx.join_event->raise(true);
      }
    });

    // Encoding and capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    std::vector<std::string> display_names;
    int display_p = -1;
    while (encode_run_sync(synced_session_ctxs, ctx, display_names, display_p) == encode_e::reinit) {}
  }

  void
  capture_async(
    safe::mail_t mail,
    config_t &config,
    void *channel_data,
    std::optional<safe::mail_raw_t::event_t<dynamic_param_t>> dynamic_param_events) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<captured_frame_event_t::element_type>();
    auto lg = util::fail_guard([&]() {
      images->stop();
      shutdown_event->raise(true);
    });

    auto ref = capture_thread_async.ref();
    if (!ref) {
      return;
    }

    ref->capture_ctx_queue->raise(capture_ctx_t { images, config });

    if (!ref->capture_ctx_queue->running()) {
      return;
    }

    int frame_nr = 1;

    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);
    auto idr_events = mail->event<bool>(mail::idr);
    auto resolution_change_event = mail->event<std::pair<std::uint32_t, std::uint32_t>>(mail::resolution_change);

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    // Cache window capture mode check outside the loop
    const bool is_window_capture = (config::video.capture_target == "window");

    // Track display dimensions for resolution change detection
    int last_display_width = 0;
    int last_display_height = 0;

    // Track initial scale ratio (encoding resolution / display resolution)
    // Used to maintain consistent scaling when display resolution changes
    float initial_scale_x = 1.0f;
    float initial_scale_y = 1.0f;

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (ref->reinit_event.peek()) {
        BOOST_LOG(debug) << "[Display] Reinit event detected, waiting for display ready...";
        std::this_thread::sleep_for(20ms);
        continue;
      }

      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto lg = ref->display_wp.lock();
        if (ref->display_wp->expired()) {
          BOOST_LOG(verbose) << "[Display] Display object expired, waiting for reinit...";
          // std::this_thread::sleep_for(20ms);
          continue;
        }
        display = ref->display_wp->lock();
      }

      // Detect display resolution changes (e.g., rotation causing width/height swap)
      // For WGC window capture, display->width/height is monitor resolution, not window size
      const int current_width = display->width;
      const int current_height = display->height;

      // Helper lambda to compute even-aligned resolution with minimum 64
      auto compute_aligned_resolution = [](int dimension, float scale) {
        return std::max(64, (static_cast<int>(dimension * scale) + 1) & ~1);
      };

      // Initialize cached display dimensions on first iteration
      if (last_display_width == 0 && last_display_height == 0) {
        last_display_width = current_width;
        last_display_height = current_height;

        // Check if display orientation matches client request
        const bool display_is_portrait = (current_height > current_width);
        const bool client_wants_landscape = (config.width > config.height);
        const bool orientation_mismatch = !is_window_capture &&
                                          (display_is_portrait == client_wants_landscape);

        if (orientation_mismatch) {
          // When orientation mismatches, client width maps to display height and vice versa
          initial_scale_x = static_cast<float>(config.width) / current_height;
          initial_scale_y = static_cast<float>(config.height) / current_width;
          BOOST_LOG(info) << "Display orientation mismatch: display="
                          << current_width << "x" << current_height
                          << ", client=" << config.width << "x" << config.height
                          << " -> using display resolution";
        }
        else {
          initial_scale_x = static_cast<float>(config.width) / current_width;
          initial_scale_y = static_cast<float>(config.height) / current_height;
          BOOST_LOG(info) << "Initial display: " << current_width << "x" << current_height
                          << ", encoding: " << config.width << "x" << config.height
                          << ", scale: " << initial_scale_x << "x" << initial_scale_y;
        }

        config.width = compute_aligned_resolution(current_width, initial_scale_x);
        config.height = compute_aligned_resolution(current_height, initial_scale_y);

        resolution_change_event->raise(std::make_pair(
          static_cast<std::uint32_t>(current_width),
          static_cast<std::uint32_t>(current_height)));

        if (orientation_mismatch) {
          idr_events->raise(true);
        }
      }
      else if (!is_window_capture &&
               (current_width != last_display_width || current_height != last_display_height)) {
        const bool is_rotation = (last_display_width == current_height && last_display_height == current_width);

        BOOST_LOG(info) << "Display resolution changed: "
                        << last_display_width << "x" << last_display_height << " -> "
                        << current_width << "x" << current_height
                        << (is_rotation ? " (rotation)" : "");

        last_display_width = current_width;
        last_display_height = current_height;

        if (!config::video.dynamic_resolution_follow_display) {
          // Toggle off: keep the originally negotiated stream resolution and let the
          // encoder's scaler adapt. Avoids sending SS_RESOLUTION_CHANGE, which legacy
          // Moonlight clients (e.g. PSVita port) don't implement and would freeze on.
          BOOST_LOG(info) << "dynamic_resolution_follow_display=false: keeping stream at "
                          << config.width << "x" << config.height << " (scaler will adapt)";
        }
        else {
          if (is_rotation) {
            std::swap(initial_scale_x, initial_scale_y);
          }

          config.width = compute_aligned_resolution(current_width, initial_scale_x);
          config.height = compute_aligned_resolution(current_height, initial_scale_y);

          BOOST_LOG(info) << "New encoding resolution: " << config.width << "x" << config.height
                          << " (scale: " << initial_scale_x << "x" << initial_scale_y << ")";

          resolution_change_event->raise(std::make_pair(
            static_cast<std::uint32_t>(current_width),
            static_cast<std::uint32_t>(current_height)));

          idr_events->raise(true);
          std::this_thread::sleep_for(100ms);
        }
      }

      auto &encoder = *chosen_encoder;

      strip_unusable_pre_encode_filter(*display, config);
      auto encode_device = make_encode_device(*display, encoder, config);
      if (!encode_device) {
        return;
      }

      // Absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      // Update client with our current HDR display state
      hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
      if (colorspace_is_hdr(encode_device->colorspace)) {
        if (get_effective_hdr_metadata(display.get(), config, hdr_info->metadata)) {
          hdr_info->enabled = true;
        }
        else {
          BOOST_LOG(error) << "Couldn't get display HDR metadata when colorspace indicates it should have one";
        }
      }
      hdr_event->raise(std::move(hdr_info));

      encode_run(
        frame_nr,
        mail, images,
        config, display,
        std::move(encode_device),
        ref->reinit_event, *ref->encoder_p,
        channel_data, dynamic_param_events);
    }
  }

  void
  capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data,
    std::optional<safe::mail_raw_t::event_t<dynamic_param_t>> dynamic_param_events) {
    auto idr_events = mail->event<bool>(mail::idr);

    idr_events->raise(true);
    if (chosen_encoder->flags & PARALLEL_ENCODING) {
      capture_async(std::move(mail), config, channel_data, dynamic_param_events);
    }
    else {
      safe::signal_t join_event;
      auto ref = capture_thread_sync.ref();
      ref->encode_session_ctx_queue.raise(sync_session_ctx_t {
        &join_event,
        mail->event<bool>(mail::shutdown),
        mail::man->queue<packet_t>(mail::video_packets),
        std::move(idr_events),
        mail->event<hdr_info_t>(mail::hdr),
        mail->event<input::touch_port_t>(mail::touch_port),
        config,
        1,
        channel_data,
      });

      // Wait for join signal
      join_event.view();
    }
  }

  enum validate_flag_e {
    VUI_PARAMS = 0x01,  ///< VUI parameters
  };

  int
  validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config) {
    auto encode_device = make_encode_device(*disp, encoder, config);
    if (!encode_device) {
      return -1;
    }

    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device), true);
    if (!session) {
      return -1;
    }

    {
      // Image buffers are large, so we use a separate scope to free it immediately after convert()
      auto img = disp->alloc_img();
      if (!img || disp->dummy_img(img.get()) || session->convert(*img)) {
        return -1;
      }
    }

    session->request_idr_frame();

    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto encode_start = std::chrono::steady_clock::now();
    while (!packets->peek()) {
      if (encode(1, *session, packets, nullptr, {}, {})) {
        return -1;
      }
      // Timeout protection: if encoding takes more than 5 seconds, it's likely hung
      if (std::chrono::steady_clock::now() - encode_start > std::chrono::seconds(5)) {
        BOOST_LOG(error) << "validate_config: encode timed out (5s), encoder may be incompatible with current settings";
        return -1;
      }
    }

    auto packet = packets->pop();
    if (!packet->is_idr()) {
      BOOST_LOG(error) << "First packet type is not an IDR frame"sv;

      return -1;
    }

    int flag = 0;

    // This check only applies for H.264 and HEVC
    if (config.videoFormat <= 1) {
      if (auto packet_avcodec = dynamic_cast<packet_raw_avcodec *>(packet.get())) {
        if (cbs::validate_sps(packet_avcodec->av_packet, config.videoFormat ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264)) {
          flag |= VUI_PARAMS;
        }
      }
      else {
        // Don't check it for non-avcodec encoders.
        flag |= VUI_PARAMS;
      }
    }

    return flag;
  }

  /**
   * @brief Validate encoder configuration, with optional NTSC framerate fallback.
   * @details If the integer framerate fails, try NTSC framerate (e.g., 120 -> 119.88fps).
   * @param disp Display device
   * @param encoder Encoder to test
   * @param config Configuration to test
   * @param try_ntsc_fallback Whether to try NTSC framerate if integer framerate fails
   * @return Validation flags on success, -1 on failure
   */
  int
  validate_config_with_fallback(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, config_t &config, bool try_ntsc_fallback = true) {
    // First try with the original framerate
    auto result = validate_config(disp, encoder, config);
    if (result >= 0) {
      return result;
    }

    // If failed and NTSC fallback is enabled, try NTSC framerate
    if (try_ntsc_fallback) {
      int ntsc_num, ntsc_den;
      if (get_ntsc_framerate(config.framerate, ntsc_num, ntsc_den)) {
        BOOST_LOG(info) << "Integer framerate " << config.framerate << "fps failed, trying NTSC framerate "
                        << ntsc_num << "/" << ntsc_den << " (" << (double) ntsc_num / ntsc_den << "fps)";

        config.frameRateNum = ntsc_num;
        config.frameRateDen = ntsc_den;

        result = validate_config(disp, encoder, config);
        if (result >= 0) {
          BOOST_LOG(info) << "NTSC framerate " << (double) ntsc_num / ntsc_den << "fps succeeded";
          return result;
        }

        // Reset to integer framerate if NTSC also failed
        config.frameRateNum = 0;
        config.frameRateDen = 1;
        BOOST_LOG(warning) << "NTSC framerate fallback also failed";
      }
    }

    return -1;
  }

  bool
  validate_encoder(
    encoder_t &encoder,
    bool expect_failure,
    const std::optional<std::string> &probe_capture_override,
    const std::string &probe_display_name) {
    std::shared_ptr<platf::display_t> disp;
    const auto configured_capture_backend = config::video.capture;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] failed"sv;
    });

    if (probe_capture_override) {
      BOOST_LOG(info) << "Temporarily using capture backend ["sv << *probe_capture_override
                      << "] for encoder probe while configured capture backend is ["sv
                      << configured_capture_backend << "]"sv;
    }

    // Quick GPU compatibility check: skip encoders that definitely won't work on this GPU
    // This optimization prevents testing encoders on incompatible hardware (e.g., NVIDIA NVENC on AMD GPU)
    // Extract GPU vendor from encoder name or check against known incompatible combinations
    if (encoder.name.find("nvenc") != std::string::npos || encoder.name.find("cuda") != std::string::npos) {
      // NVIDIA encoders - would need NVIDIA GPU
      // We'll let the actual validation fail naturally, but at a fast level
    }
    else if (encoder.name.find("quicksync") != std::string::npos || encoder.name.find("qsv") != std::string::npos) {
      // Intel QuickSync - would need Intel GPU
      // We'll let the actual validation fail naturally
    }

    auto test_hevc = active_hevc_mode >= 2 || (active_hevc_mode == 0 && !(encoder.flags & H264_ONLY));
    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));

    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();

    // First, test encoder viability
    // Note: videoFormat starts at 0 (H.264), will be changed to 1 (HEVC) or 2 (AV1) later if needed
    config_t config_max_ref_frames { 1920, 1080, 60, 1000, 1, 1, 1, 0, 0, 0, 0 };
    config_t config_autoselect { 1920, 1080, 60, 1000, 1, 1, 0, 0, 0, 0, 0 };
    if (probe_capture_override) {
      config_max_ref_frames.capture_backend_override = *probe_capture_override;
      config_autoselect.capture_backend_override = *probe_capture_override;
    }

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, config_autoselect);
    if (!disp) {
      return false;
    }
    if (!disp->is_codec_supported(encoder.h264.name, config_autoselect)) {
      fg.disable();
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] is not supported on this GPU"sv;
      return false;
    }

    // If we're expecting failure, use the autoselect ref config first since that will always succeed
    // if the encoder is available.
    auto max_ref_frames_h264 = expect_failure ? -1 : validate_config(disp, encoder, config_max_ref_frames);
    auto autoselect_h264 = max_ref_frames_h264 >= 0 ? max_ref_frames_h264 : validate_config(disp, encoder, config_autoselect);
    if (autoselect_h264 < 0) {
      return false;
    }
    else if (expect_failure) {
      // We expected failure, but actually succeeded. Do the max_ref_frames probe we skipped.
      max_ref_frames_h264 = validate_config(disp, encoder, config_max_ref_frames);
    }

    std::vector<std::pair<validate_flag_e, encoder_t::flag_e>> packet_deficiencies {
      { VUI_PARAMS, encoder_t::VUI_PARAMETERS },
    };

    for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
      encoder.h264[encoder_flag] = (max_ref_frames_h264 & validate_flag && autoselect_h264 & validate_flag);
    }

    encoder.h264[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_h264 >= 0;
    encoder.h264[encoder_t::PASSED] = true;

    if (test_hevc) {
      config_max_ref_frames.videoFormat = 1;
      config_autoselect.videoFormat = 1;

      if (disp->is_codec_supported(encoder.hevc.name, config_autoselect)) {
        auto max_ref_frames_hevc = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // HEVC to also succeed with max ref frames specified if HEVC is supported.
        auto autoselect_hevc = (max_ref_frames_hevc >= 0 || max_ref_frames_h264 >= 0) ?
                                 max_ref_frames_hevc :
                                 validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.hevc[encoder_flag] = (max_ref_frames_hevc & validate_flag && autoselect_hevc & validate_flag);
        }

        encoder.hevc[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_hevc >= 0;
        encoder.hevc[encoder_t::PASSED] = max_ref_frames_hevc >= 0 || autoselect_hevc >= 0;
      }
      else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.hevc.name << "] is not supported on this GPU"sv;
        encoder.hevc.capabilities.reset();
      }
    }
    else {
      // Clear all cap bits for HEVC if we didn't probe it
      encoder.hevc.capabilities.reset();
    }

    if (test_av1) {
      config_max_ref_frames.videoFormat = 2;
      config_autoselect.videoFormat = 2;

      if (disp->is_codec_supported(encoder.av1.name, config_autoselect)) {
        auto max_ref_frames_av1 = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // AV1 to also succeed with max ref frames specified if AV1 is supported.
        auto autoselect_av1 = (max_ref_frames_av1 >= 0 || max_ref_frames_h264 >= 0) ?
                                max_ref_frames_av1 :
                                validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.av1[encoder_flag] = (max_ref_frames_av1 & validate_flag && autoselect_av1 & validate_flag);
        }

        encoder.av1[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_av1 >= 0;
        encoder.av1[encoder_t::PASSED] = max_ref_frames_av1 >= 0 || autoselect_av1 >= 0;
      }
      else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.av1.name << "] is not supported on this GPU"sv;
        encoder.av1.capabilities.reset();
      }
    }
    else {
      // Clear all cap bits for AV1 if we didn't probe it
      encoder.av1.capabilities.reset();
    }

    // Test HDR and YUV444 support
    {
#ifdef _WIN32
      const bool is_rdp_session = !is_running_as_system_user && display_device::w_utils::is_any_rdp_session_active();
#else
      const bool is_rdp_session = false;
#endif

      // H.264 is special because encoders may support YUV 4:4:4 without supporting 10-bit color depth
      if (encoder.flags & YUV444_SUPPORT) {
        config_t config_h264_yuv444 { 1920, 1080, 60, 1000, 1, 1, 0, 0, 0, 0, 1 };
        encoder.h264[encoder_t::YUV444] = disp->is_codec_supported(encoder.h264.name, config_h264_yuv444) &&
                                          validate_config(disp, encoder, config_h264_yuv444) >= 0;
      }
      else {
        encoder.h264[encoder_t::YUV444] = false;
      }

      // HDR is not supported with H.264
      encoder.h264[encoder_t::DYNAMIC_RANGE] = false;

      // Skip HDR testing in RDP/virtual display environments
      if (is_rdp_session) {
        BOOST_LOG(info) << "Skipping HDR testing in RDP environment";
        encoder.hevc[encoder_t::DYNAMIC_RANGE] = false;
        encoder.av1[encoder_t::DYNAMIC_RANGE] = false;
      }
      else {
        config_t generic_hdr_config = { 1920, 1080, 60, 1000, 1, 1, 0, 3, 1, 1, 0 };
        if (probe_capture_override) {
          generic_hdr_config.capture_backend_override = *probe_capture_override;
        }

        // Reset the display since we're switching from SDR to HDR
        reset_display(disp, encoder.platform_formats->dev_type, probe_display_name, generic_hdr_config);
        if (!disp) {
          return false;
        }

        auto test_hdr_and_yuv444 = [&](auto &flag_map, int video_format) {
          if (!flag_map[encoder_t::PASSED]) {
            flag_map[encoder_t::DYNAMIC_RANGE] = false;
            flag_map[encoder_t::YUV444] = false;
            return;
          }

          auto config = generic_hdr_config;
          config.videoFormat = video_format;
          auto encoder_codec_name = encoder.codec_from_config(config).name;

          // Test 4:4:4 HDR first. If 4:4:4 is supported, 4:2:0 should also be supported.
          if (encoder.flags & YUV444_SUPPORT) {
            config.chromaSamplingType = 1;
            if (disp->is_codec_supported(encoder_codec_name, config) &&
                validate_config(disp, encoder, config) >= 0) {
              flag_map[encoder_t::DYNAMIC_RANGE] = true;
              flag_map[encoder_t::YUV444] = true;
              return;
            }
          }
          flag_map[encoder_t::YUV444] = false;

          // Test 4:2:0 HDR
          config.chromaSamplingType = 0;
          flag_map[encoder_t::DYNAMIC_RANGE] = disp->is_codec_supported(encoder_codec_name, config) &&
                                               validate_config(disp, encoder, config) >= 0;
        };

        test_hdr_and_yuv444(encoder.hevc, 1);
        test_hdr_and_yuv444(encoder.av1, 2);
      }
    }

    encoder.h264[encoder_t::VUI_PARAMETERS] = encoder.h264[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];
    encoder.hevc[encoder_t::VUI_PARAMETERS] = encoder.hevc[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];

    if (!encoder.h264[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": h264 missing sps->vui parameters"sv;
    }
    if (encoder.hevc[encoder_t::PASSED] && !encoder.hevc[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": hevc missing sps->vui parameters"sv;
    }

    fg.disable();
    return true;
  }

  int
  probe_encoders(std::optional<probe_target_t> target) {
    last_encoder_probe_result = {
      probe_error_e::none,
      "Encoder probe succeeded.",
      {}
    };

    if (!allow_encoder_probing()) {
      active_encoder_for_status.store(nullptr, std::memory_order_release);
      // Error already logged
      return -1;
    }
    auto encoder_list = encoders;

    // If we already have a good encoder, check to see if another probe is required
    if (!target && chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      BOOST_LOG(info) << "Using cached encoder validation results";
      active_encoder_for_status.store(chosen_encoder, std::memory_order_release);
      return 0;
    }

    const auto probe_capture_override = capture_override_for_encoder_probe();
    const auto configured_output_name = target ? target->output_name : config::video.output_name;
    const bool target_requires_exact_resolution = target && target->policy == probe_target_policy_e::exact;
    const auto configured_display_name = display_device::get_display_name(configured_output_name);
    if (target_requires_exact_resolution && configured_display_name.empty()) {
      last_encoder_probe_result = {
        probe_error_e::no_active_display,
        "The requested display is not connected or active for encoder probing.",
        "Connect or enable the selected display, then try again."
      };
      BOOST_LOG(error) << "Requested output ["sv << configured_output_name
                       << "] could not be resolved for encoder probing"sv;
      return -1;
    }
    auto probe_display_name = configured_display_name;
    if (probe_capture_override) {
      // The Windows implementation enumerates all DXGI capture-ready outputs
      // regardless of memory type, so one pass serves every encoder candidate.
      const auto capture_ready_displays = encoder_list.empty() ?
                                            std::vector<std::string> {} :
                                            platf::display_names(encoder_list.front()->platform_formats->dev_type);
      const bool exact_target_unavailable = target_requires_exact_resolution &&
                                            std::ranges::find(capture_ready_displays, configured_display_name) == capture_ready_displays.end();
      if (exact_target_unavailable) {
        last_encoder_probe_result = {
          probe_error_e::no_active_display,
          "The requested display is not available to the encoder probe capture backend.",
          "Connect or enable the selected display, then try again."
        };
        BOOST_LOG(error) << "Requested output ["sv << configured_output_name
                         << "] is unavailable to temporary capture backend ["sv
                         << *probe_capture_override << "]"sv;
        return -1;
      }
      probe_display_name = select_encoder_probe_display(configured_display_name, capture_ready_displays);
      if (target && target->policy != probe_target_policy_e::backend_autoselect &&
          !configured_output_name.empty() && configured_display_name.empty()) {
        BOOST_LOG(warning) << "Configured output ["sv << configured_output_name
                           << "] could not be resolved for temporary capture backend ["sv
                           << *probe_capture_override
                           << "]; encoder probing will use backend display auto-selection"sv;
      }
      else if (target && target->policy != probe_target_policy_e::backend_autoselect &&
               !configured_display_name.empty() && probe_display_name.empty()) {
        BOOST_LOG(warning) << "Configured output ["sv << configured_display_name
                           << "] is unavailable to temporary capture backend ["sv
                           << *probe_capture_override
                           << "]; encoder probing will use backend display auto-selection"sv;
      }
    }
    else if (target && target->policy == probe_target_policy_e::vdd_compatible && configured_display_name.empty()) {
      last_encoder_probe_result = {
        probe_error_e::no_active_display,
        "The requested display is not connected or active for encoder probing.",
        "Connect or enable the selected display, then try again."
      };
      BOOST_LOG(error) << "Requested output ["sv << configured_output_name
                       << "] could not be resolved for encoder probing"sv;
      return -1;
    }

    // Restart encoder selection
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;
    active_encoder_for_status.store(nullptr, std::memory_order_release);
    active_hevc_mode = config::video.hevc_mode;
    active_av1_mode = config::video.av1_mode;
    last_encoder_probe_supported_ref_frames_invalidation = false;

    auto adjust_encoder_constraints = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 on this system"sv;
        active_hevc_mode = 0;
      }
      else if (active_hevc_mode == 2 && !encoder->hevc[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC on this system"sv;
        active_hevc_mode = 0;
      }

      if (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 on this system"sv;
        active_av1_mode = 0;
      }
      else if (active_av1_mode == 2 && !encoder->av1[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 on this system"sv;
        active_av1_mode = 0;
      }
    };

    if (!config::video.encoder.empty()) {
      // If there is a specific encoder specified, use it if it passes validation
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        if (encoder->name == config::video.encoder) {
          // Remove the encoder from the list entirely if it fails validation
          if (!validate_encoder(
                *encoder,
                previous_encoder && previous_encoder != encoder,
                probe_capture_override,
                probe_display_name)) {
            pos = encoder_list.erase(pos);
            break;
          }

          // We will return an encoder here even if it fails one of the codec requirements specified by the user
          adjust_encoder_constraints(encoder);

          chosen_encoder = encoder;
          break;
        }

        pos++;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder matching ["sv << config::video.encoder << ']';
      }
    }

    BOOST_LOG(info) << "Testing for available encoders - Errors during this phase can be ignored (测试可用编码器 - 此阶段的错误可以忽略)";

    // If we haven't found an encoder yet, but we want one with specific codec support, search for that now.
    if (chosen_encoder == nullptr && (active_hevc_mode >= 2 || active_av1_mode >= 2)) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // Remove the encoder from the list entirely if it fails validation
        if (!validate_encoder(
              *encoder,
              previous_encoder && previous_encoder != encoder,
              probe_capture_override,
              probe_display_name)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // Skip it if it doesn't support the specified codec at all
        if ((active_hevc_mode >= 2 && !encoder->hevc[encoder_t::PASSED]) ||
            (active_av1_mode >= 2 && !encoder->av1[encoder_t::PASSED])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) ||
            (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE])) {
          pos++;
          continue;
        }

        chosen_encoder = encoder;
        break;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder that meets HEVC/AV1 requirements"sv;
      }
    }

    // If no encoder was specified or the specified encoder was unusable, keep trying
    // the remaining encoders until we find one that passes validation.
    if (chosen_encoder == nullptr) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // If we've used a previous encoder and it's not this one, we expect this encoder to
        // fail to validate. It will use a slightly different order of checks to more quickly
        // eliminate failing encoders.
        if (!validate_encoder(
              *encoder,
              previous_encoder && previous_encoder != encoder,
              probe_capture_override,
              probe_display_name)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // We will return an encoder here even if it fails one of the codec requirements specified by the user
        adjust_encoder_constraints(encoder);

        chosen_encoder = encoder;
        break;
      });
    }

    if (chosen_encoder == nullptr) {
      const auto output_display_name { display_device::get_display_name(configured_output_name) };
      BOOST_LOG(error) << "Unable to find display or encoder during startup."sv;
      if (!config::video.encoder.empty()) {
        last_encoder_probe_result = {
          probe_error_e::configured_encoder_unavailable,
          "The configured video encoder is not available on this system.",
          "Set the video encoder to Auto, update the GPU driver, or choose an encoder supported by the active GPU."
        };
      }
      else if (config::video.hevc_mode >= 2 || config::video.av1_mode >= 2) {
        last_encoder_probe_result = {
          probe_error_e::codec_requirements_unmet,
          "No working encoder satisfies the requested HEVC or AV1 requirements.",
          "Set HEVC/AV1 support to Auto or Disabled, or use H.264 and try again."
        };
      }
      else {
        last_encoder_probe_result = {
          probe_error_e::no_working_encoder,
          "Sunshine could not find a working display capture path and video encoder.",
          "Check that a display is connected or VDD is active, set GPU/display/encoder options to Auto, and update the GPU driver."
        };
      }
      if (!config::video.adapter_name.empty() || !output_display_name.empty()) {
        BOOST_LOG(error) << "Please ensure your manually chosen GPU and monitor are connected and powered on."sv;
      }
      else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      return -1;
    }

    BOOST_LOG(info) << "Ignore any errors, Encoder testing completed (忽略任何错误，编码器测试完成)";

    auto &encoder = *chosen_encoder;
    active_encoder_for_status.store(chosen_encoder, std::memory_order_release);

    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                       encoder.av1[encoder_t::YUV444];

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = (encoder_t::flag_e) x;
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

    if (encoder.av1[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  av1 ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = (encoder_t::flag_e) x;
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.av1[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found AV1 encoder: "sv << encoder.av1.name << " ["sv << encoder.name << ']';
    }

    if (active_hevc_mode == 0) {
      active_hevc_mode = encoder.hevc[encoder_t::PASSED] ? (encoder.hevc[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    if (active_av1_mode == 0) {
      active_av1_mode = encoder.av1[encoder_t::PASSED] ? (encoder.av1[encoder_t::DYNAMIC_RANGE] ? 3 : 2) : 1;
    }

    return 0;
  }

  // Linux only declaration
  typedef int (*vaapi_init_avcodec_hardware_input_buffer_fn)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  util::Either<avcodec_buffer_t, int>
  vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    // If an egl hwdevice
    if (encode_device->data) {
      if (((vaapi_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }

      return hw_device_buf;
    }

    auto render_device = config::video.adapter_name.empty() ? nullptr : config::video.adapter_name.c_str();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VAAPI, render_device, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VAAPI device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int>
  cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1 /* AV_CUDA_USE_PRIMARY_CONTEXT */);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a CUDA device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int>
  vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VideoToolbox device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  util::Either<avcodec_buffer_t, int>
  vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a Vulkan device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef _WIN32
}

void
do_nothing(void *) {}

namespace video {
  util::Either<avcodec_buffer_t, int>
  dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t ctx_buf { av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA) };
    auto ctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) ctx_buf->data)->hwctx;

    std::fill_n((std::uint8_t *) ctx, sizeof(AVD3D11VADeviceContext), 0);

    auto device = (ID3D11Device *) encode_device->data;

    device->AddRef();
    ctx->device = device;

    ctx->lock_ctx = (void *) 1;
    ctx->lock = do_nothing;
    ctx->unlock = do_nothing;

    auto err = av_hwdevice_ctx_init(ctx_buf.get());
    if (err) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return ctx_buf;
  }
#endif

  int
  start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.encoder_p = chosen_encoder;
    capture_thread_ctx.reinit_event.reset();

    capture_thread_ctx.capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::thread {
      captureThread,
      capture_thread_ctx.capture_ctx_queue,
      std::ref(capture_thread_ctx.display_wp),
      std::ref(capture_thread_ctx.reinit_event),
      std::ref(*capture_thread_ctx.encoder_p)
    };

    return 0;
  }
  void
  end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.capture_ctx_queue->stop();

    capture_thread_ctx.capture_thread.join();
  }

  int
  start_capture_sync(capture_thread_sync_ctx_t &ctx) {
    std::thread { &captureThreadSync }.detach();
    return 0;
  }
  void
  end_capture_sync(capture_thread_sync_ctx_t &ctx) {}

  platf::mem_type_e
  map_base_dev_type(AVHWDeviceType type) {
    switch (type) {
      case AV_HWDEVICE_TYPE_D3D11VA:
        return platf::mem_type_e::dxgi;
      case AV_HWDEVICE_TYPE_VAAPI:
        return platf::mem_type_e::vaapi;
      case AV_HWDEVICE_TYPE_CUDA:
        return platf::mem_type_e::cuda;
      case AV_HWDEVICE_TYPE_NONE:
        return platf::mem_type_e::system;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return platf::mem_type_e::videotoolbox;
      case AV_HWDEVICE_TYPE_VULKAN:
        return platf::mem_type_e::vulkan;
      default:
        return platf::mem_type_e::unknown;
    }

    return platf::mem_type_e::unknown;
  }

  platf::pix_fmt_e
  map_pix_fmt(AVPixelFormat fmt) {
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return platf::pix_fmt_e::ayuv;
      case AV_PIX_FMT_XV30:
        return platf::pix_fmt_e::y410;
      case AV_PIX_FMT_YUV420P10:
        return platf::pix_fmt_e::yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return platf::pix_fmt_e::yuv420p;
      case AV_PIX_FMT_NV12:
        return platf::pix_fmt_e::nv12;
      case AV_PIX_FMT_P010:
        return platf::pix_fmt_e::p010;
      default:
        return platf::pix_fmt_e::unknown;
    }

    return platf::pix_fmt_e::unknown;
  }

}  // namespace video
