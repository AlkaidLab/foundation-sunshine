/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <functional>
#include <future>
#include <list>
#include <mutex>
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

// AMF SDK headers for direct encoder access (Windows only)
#ifdef _WIN32
  #include <AMF/components/Component.h>
  #include <AMF/components/VideoEncoderAV1.h>
  #include <AMF/components/VideoEncoderHEVC.h>
  #include <AMF/components/VideoEncoderVCE.h>
  #include <AMF/core/Interface.h>
  #include <AMF/core/PropertyStorage.h>
  #include <cstring>  // for strstr

// Forward declaration of FFmpeg's internal AMFEncoderContext structure
// This structure layout must match FFmpeg's libavcodec/amfenc.h
// We only need the first few fields to access the encoder pointer
struct AMFEncoderContext_Partial {
  void *avclass;  // AVClass pointer
  void *device_ctx_ref;  // AVBufferRef pointer
  amf::AMFComponent *encoder;  // AMF encoder object
};
#endif

// lib includes
#include "cbs.h"
#include "config.h"
#include "display_device/display_device.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_encoder.h"
#include "amf/amf_encoder.h"
#include "platform/common.h"
#include "stream_quality.h"
#include "sync.h"
#include "video.h"
#include "video_bitrate.h"

#ifdef _WIN32
extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}
  #include "platform/windows/display_device/windows_utils.h"
#endif

using namespace std::literals;
namespace video {

  namespace {
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

    int
    clamp_fec_percentage(int fec_percentage) {
      return std::clamp(fec_percentage, 0, 100);
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
      return false;
    }
  }  // namespace

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

  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;
    avcodec_encode_session_t(avcodec_ctx_t &&avcodec_ctx, std::unique_ptr<platf::avcodec_encode_device_t> encode_device, int inject):
        avcodec_ctx { std::move(avcodec_ctx) }, device { std::move(encode_device) }, inject { inject } {}

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

      auto adjusted_bitrate_kbps = dynamic_encoder_bitrate_kbps(bitrate_kbps, current_fec_percentage);
      auto bitrate = static_cast<int64_t>(adjusted_bitrate_kbps) * 1000;  // Convert to bps

      // Update AVCodecContext fields (for software encoders and as fallback)
      avcodec_ctx->bit_rate = bitrate;
      avcodec_ctx->rc_max_rate = bitrate;
      avcodec_ctx->rc_min_rate = bitrate;

#ifdef _WIN32
      // For AMF encoders, directly call AMF SDK to change bitrate dynamically
      // AMF_VIDEO_ENCODER_TARGET_BITRATE, AMF_VIDEO_ENCODER_PEAK_BITRATE, and
      // AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE are documented as "Dynamic properties -
      // can be set at any time" in AMF SDK
      const AVCodec *codec = avcodec_ctx->codec;
      if (codec && codec->name && avcodec_ctx->priv_data && strstr(codec->name, "_amf")) {
        auto *amf_ctx = reinterpret_cast<AMFEncoderContext_Partial *>(avcodec_ctx->priv_data);
        if (amf_ctx && amf_ctx->encoder) {
          // VBV buffer size: 1 second worth of data at the target bitrate
          int64_t vbv_buffer_size = bitrate;
          AMF_RESULT res = AMF_OK;

          // Set properties based on codec type
          if (strstr(codec->name, "h264_amf")) {
            res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv_buffer_size);
            if (res == AMF_OK) res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate);
            if (res == AMF_OK) res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate);
          }
          else if (strstr(codec->name, "hevc_amf")) {
            res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv_buffer_size);
            if (res == AMF_OK) res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bitrate);
            if (res == AMF_OK) res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bitrate);
          }
          else if (strstr(codec->name, "av1_amf")) {
            res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv_buffer_size);
            if (res == AMF_OK) res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bitrate);
            if (res == AMF_OK) res = amf_ctx->encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bitrate);
          }

          if (res == AMF_OK) {
            BOOST_LOG(info) << "AMF encoder bitrate dynamically changed to: " << adjusted_bitrate_kbps
                            << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                            << current_fec_percentage << "%)";
            return;
          }
          BOOST_LOG(warning) << "AMF SetProperty for bitrate failed with error: " << res;
        }
      }
#endif

      BOOST_LOG(info) << "AVCodec encoder bitrate set to: " << adjusted_bitrate_kbps
                      << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                      << current_fec_percentage << "%)";
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
        case dynamic_param_type_e::FEC_PERCENTAGE: {
          current_fec_percentage = clamp_fec_percentage(param.value.int_value);
          BOOST_LOG(info) << "AVCodec encoder FEC percentage changed to: " << current_fec_percentage << "%";
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
        case dynamic_param_type_e::CHROMA_SAMPLING:
          BOOST_LOG(info) << "AVCodec encoder: Chroma sampling change requested: "
                          << param.value.int_value
                          << " (profile-tier fallback, requires encoder reinitialization)";
          break;
        case dynamic_param_type_e::DYNAMIC_RANGE:
          BOOST_LOG(info) << "AVCodec encoder: Dynamic range change requested: "
                          << param.value.int_value
                          << " (profile-tier fallback, requires encoder reinitialization)";
          break;
        default:
          BOOST_LOG(warning) << "AVCodec encoder: Unsupported dynamic parameter type: " << (int) param.type;
          break;
      }
    }

    const char *
    encoder_backend_name() const override {
      return avcodec_ctx && avcodec_ctx->codec && avcodec_ctx->codec->name ?
               avcodec_ctx->codec->name :
               "AVCodec";
    }

    frame_interest::backend_caps_t
    frame_interest_caps() const override {
      return {
        .adaptive_quantization = true,
      };
    }

    avcodec_ctx_t avcodec_ctx;
    std::unique_ptr<platf::avcodec_encode_device_t> device;

    std::vector<packet_raw_t::replace_t> replacements;
    int current_fec_percentage = clamp_fec_percentage(config::stream.fec_percentage);

    cbs::nal_t sps;
    cbs::nal_t vps;

    // inject sps/vps data into idr pictures
    int inject;
  };

  class nvenc_encode_session_t: public encode_session_t {
  public:
    nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

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
        auto adjusted_bitrate_kbps = dynamic_encoder_bitrate_kbps(bitrate_kbps, current_fec_percentage);
        device->nvenc->set_bitrate(adjusted_bitrate_kbps);
        BOOST_LOG(info) << "NVENC encoder bitrate changed to: " << adjusted_bitrate_kbps
                        << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                        << current_fec_percentage << "%)";
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
          // Runtime FPS changes are applied by the stream pacing loop. Do not
          // reconfigure NVENC per feedback window; that creates visible stalls.
          BOOST_LOG(info) << "NVENC encoder: FPS change requested: " << param.value.float_value
                          << " fps (pacing-only, encoder not reconfigured)";
          break;
        case dynamic_param_type_e::BITRATE: {
          // 码率调整通过set_bitrate处理
          set_bitrate(param.value.int_value);
          break;
        }
        case dynamic_param_type_e::FEC_PERCENTAGE: {
          current_fec_percentage = clamp_fec_percentage(param.value.int_value);
          BOOST_LOG(info) << "NVENC encoder FEC percentage changed to: " << current_fec_percentage << "%";
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
        case dynamic_param_type_e::CHROMA_SAMPLING:
          BOOST_LOG(info) << "NVENC encoder: Chroma sampling change requested: "
                          << param.value.int_value
                          << " (profile-tier fallback, requires encoder reinitialization)";
          break;
        case dynamic_param_type_e::DYNAMIC_RANGE:
          BOOST_LOG(info) << "NVENC encoder: Dynamic range change requested: "
                          << param.value.int_value
                          << " (profile-tier fallback, requires encoder reinitialization)";
          break;
        default:
          BOOST_LOG(warning) << "NVENC encoder: Unsupported dynamic parameter type: " << (int) param.type;
          break;
      }
    }

    const char *
    encoder_backend_name() const override {
      return "NVENC";
    }

    frame_interest::backend_caps_t
    frame_interest_caps() const override {
      return device && device->nvenc ? device->nvenc->frame_interest_caps() : frame_interest::backend_caps_t {};
    }

    void
    set_frame_interest(const frame_interest::map_t &map, std::uint32_t intent_flags) override {
      if (device && device->nvenc) {
        device->nvenc->set_frame_interest(map, intent_flags);
      }
    }

    nvenc::nvenc_encoded_frame
    encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) return {};

      // Pass per-frame HDR luminance stats to NVENC for dynamic metadata injection
      if (device->hdr_luminance_stats.valid) {
        device->nvenc->set_luminance_stats(device->hdr_luminance_stats);
      }

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    int current_fec_percentage = clamp_fec_percentage(config::stream.fec_percentage);
    bool force_idr = false;
  };

  class amf_encode_session_t: public encode_session_t {
  public:
    amf_encode_session_t(std::unique_ptr<platf::amf_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

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
        auto adjusted_bitrate_kbps = dynamic_encoder_bitrate_kbps(bitrate_kbps, current_fec_percentage);
        device->amf->set_bitrate(adjusted_bitrate_kbps);
        BOOST_LOG(info) << "AMF standalone encoder bitrate changed to: " << adjusted_bitrate_kbps
                        << " Kbps (requested: " << bitrate_kbps << " Kbps, FEC: "
                        << current_fec_percentage << "%)";
      }
    }

    void
    set_dynamic_param(const dynamic_param_t &param) override {
      if (!device || !device->amf) return;

      switch (param.type) {
        case dynamic_param_type_e::BITRATE:
          set_bitrate(param.value.int_value);
          break;
        case dynamic_param_type_e::FEC_PERCENTAGE:
          current_fec_percentage = clamp_fec_percentage(param.value.int_value);
          BOOST_LOG(info) << "AMF standalone encoder FEC percentage changed to: " << current_fec_percentage << "%";
          break;
        case dynamic_param_type_e::RESOLUTION:
          BOOST_LOG(info) << "AMF standalone encoder: Resolution change requested (profile-tier fallback, requires encoder reinitialization)";
          break;
        case dynamic_param_type_e::FPS:
          BOOST_LOG(info) << "AMF standalone encoder: FPS change requested: " << param.value.float_value
                          << " fps (requires encoder reconfiguration)";
          break;
        case dynamic_param_type_e::CHROMA_SAMPLING:
          BOOST_LOG(info) << "AMF standalone encoder: Chroma sampling change requested: "
                          << param.value.int_value
                          << " (profile-tier fallback, requires encoder reinitialization)";
          break;
        case dynamic_param_type_e::DYNAMIC_RANGE:
          BOOST_LOG(info) << "AMF standalone encoder: Dynamic range change requested: "
                          << param.value.int_value
                          << " (profile-tier fallback, requires encoder reinitialization)";
          break;
        default:
          break;
      }
    }

    const char *
    encoder_backend_name() const override {
      return "AMF";
    }

    frame_interest::backend_caps_t
    frame_interest_caps() const override {
      return device && device->amf ? device->amf->frame_interest_caps() : frame_interest::backend_caps_t {};
    }

    void
    set_frame_interest(const frame_interest::map_t &map, std::uint32_t intent_flags) override {
      if (device && device->amf) {
        device->amf->set_frame_interest(map, intent_flags);
      }
    }

    amf::amf_encoded_frame
    encode_frame(uint64_t frame_index) {
      if (!device || !device->amf) return {};

      auto result = device->amf->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::amf_encode_device_t> device;
    int current_fec_percentage = clamp_fec_percentage(config::stream.fec_percentage);
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
    frame_interest_feedback_fn_t frame_interest_feedback;
  };

  struct sync_session_t {
    sync_session_ctx_t *ctx;
    std::unique_ptr<encode_session_t> session;
  };

  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;
  using encode_e = platf::capture_e;

  struct capture_ctx_t {
    img_event_t images;
    config_t config;
  };

  struct capture_thread_async_state_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;

    safe::signal_t reinit_event;
    const encoder_t *encoder_p;
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;
  };

  struct capture_thread_async_ctx_t {
    std::shared_ptr<capture_thread_async_state_t> state;
    std::thread capture_thread;
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

  // Legacy FFmpeg-based AMF encoder (fallback)
  encoder_t amdvce_legacy {
    "amdvce_legacy"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12, AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE, AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer),
    {
      // Common options
      {
        { "filler_data"s, false },
        { "forced_idr"s, 1 },
        { "latency"s, "lowest_latency"s },
        { "async_depth"s, 1 },
        { "skip_frame"s, 0 },
        { "log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         } },
        { "preencode"s, &config::video.amd.amd_preanalysis },
        { "quality"s, &config::video.amd.amd_quality_av1 },
        { "rc"s, &config::video.amd.amd_rc_av1 },
        { "usage"s, &config::video.amd.amd_usage_av1 },
        { "enforce_hrd"s, &config::video.amd.amd_enforce_hrd },
        // AV1 optimization options (no latency impact)
        { "high_motion_quality_boost_enable"s, true },
        { "pa_paq_mode"s, "caq"s },
        { "pa_taq_mode"s, 2 },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      // Common options
      {
        { "filler_data"s, false },
        { "forced_idr"s, 1 },
        { "latency"s, 1 },
        { "async_depth"s, 1 },
        { "skip_frame"s, 0 },
        { "log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         } },
        { "gops_per_idr"s, 1 },
        { "header_insertion_mode"s, "idr"s },
        { "preencode"s, &config::video.amd.amd_preanalysis },
        { "quality"s, &config::video.amd.amd_quality_hevc },
        { "rc"s, &config::video.amd.amd_rc_hevc },
        { "usage"s, &config::video.amd.amd_usage_hevc },
        { "vbaq"s, &config::video.amd.amd_vbaq },
        { "enforce_hrd"s, &config::video.amd.amd_enforce_hrd },
        { "level"s, [](const config_t &cfg) {
           auto size = cfg.width * cfg.height;
           // For 4K and below, try to use level 5.1 or 5.2 if possible
           if (size <= 8912896) {
             if (size * cfg.framerate <= 534773760) {
               return "5.1"s;
             }
             else if (size * cfg.framerate <= 1069547520) {
               return "5.2"s;
             }
           }
           return "auto"s;
         } },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_amf"s,
    },
    {
      // Common options
      {
        { "filler_data"s, false },
        { "forced_idr"s, 1 },
        { "latency"s, 1 },
        { "async_depth"s, 1 },
        { "frame_skipping"s, 0 },
        { "log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         } },
        { "preencode"s, &config::video.amd.amd_preanalysis },
        { "quality"s, &config::video.amd.amd_quality_h264 },
        { "rc"s, &config::video.amd.amd_rc_h264 },
        { "usage"s, &config::video.amd.amd_usage_h264 },
        { "vbaq"s, &config::video.amd.amd_vbaq },
        { "enforce_hrd"s, &config::video.amd.amd_enforce_hrd },
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        { "usage"s, 2 /* AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY */ },  // Workaround for https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/410
      },
      "h264_amf"s,
    },
    PARALLEL_ENCODING
  };
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
    &amdvce_legacy,
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
  static std::mutex encoder_probe_mutex;
  int active_hevc_mode;
  int active_av1_mode;
  bool last_encoder_probe_supported_ref_frames_invalidation = false;
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};

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
      BOOST_LOG(info) << "[Display] Capture thread fail-guard running"
                      << " queueRunning=" << (capture_ctx_queue->running() ? 1 : 0)
                      << " activeContexts=" << capture_ctxs.size();
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
    BOOST_LOG(info) << "[Display] Capture thread started"
                    << " queueRunning=" << (capture_ctx_queue->running() ? 1 : 0);

    // Wait for the initial capture context or a request to stop the queue
    BOOST_LOG(info) << "[Display] Capture thread waiting for initial context";
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      BOOST_LOG(warning) << "[Display] Capture thread exited before initial context"
                         << " queueRunning=" << (capture_ctx_queue->running() ? 1 : 0)
                         << " reinitPending=" << (reinit_event.peek() ? 1 : 0);
      return;
    }
    BOOST_LOG(info) << "[Display] Capture thread received initial context"
                    << " requested=" << initial_capture_ctx->config.width << 'x' << initial_capture_ctx->config.height
                    << '@' << initial_capture_ctx->config.framerate
                    << " preferCursorPlane=" << (initial_capture_ctx->config.preferCursorPlane ? 1 : 0)
                    << " displayName="
                    << (initial_capture_ctx->config.display_name.empty() ? "<default>" : initial_capture_ctx->config.display_name);
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

    const auto display_init_started = std::chrono::steady_clock::now();
    BOOST_LOG(info) << "[Display] Capture display init begin"
                    << " target=" << target_display_name
                    << " preferCursorPlane=" << (config.preferCursorPlane ? 1 : 0)
                    << " requested=" << config.width << 'x' << config.height
                    << '@' << config.framerate
                    << " startupFps=" << config.startupFramerate;
    auto disp = platf::display(encoder.platform_formats->dev_type, target_display_name, config);
    if (!disp) {
      return;
    }
    BOOST_LOG(info) << "[Display] Capture display created"
                    << " target=" << target_display_name
                    << " actual=" << disp->width << 'x' << disp->height
                    << " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - display_init_started).count();
    display_wp = disp;
    BOOST_LOG(info) << "[Display] Capture display published"
                    << " target=" << target_display_name
                    << " actual=" << disp->width << 'x' << disp->height
                    << " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - display_init_started).count();

    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);

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

    std::optional<bool> last_capture_cursor;
    std::size_t last_cursor_plane_sessions = 0;
    std::size_t last_legacy_video_cursor_sessions = 0;
    auto drain_pending_capture_contexts = [&]() {
      while (capture_ctx_queue->peek()) {
        capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
      }
    };
    auto should_capture_cursor = [&]() {
      const bool any_cursor_plane_session = std::any_of(
        std::begin(capture_ctxs),
        std::end(capture_ctxs),
        [](const auto &capture_ctx) {
          return capture_ctx.config.preferCursorPlane;
        });
      return display_cursor && !config.preferCursorPlane && !any_cursor_plane_session;
    };
    auto refresh_capture_cursor = [&](bool &capture_cursor) {
      const bool next_capture_cursor = should_capture_cursor();
      if (!last_capture_cursor || *last_capture_cursor != next_capture_cursor) {
        const auto cursor_plane_sessions = std::count_if(
          std::begin(capture_ctxs),
          std::end(capture_ctxs),
          [](const auto &capture_ctx) {
            return capture_ctx.config.preferCursorPlane;
          });
        const auto legacy_video_cursor_sessions = capture_ctxs.size() - cursor_plane_sessions;
        BOOST_LOG(info) << "Capture cursor burn-in " << (next_capture_cursor ? "enabled" : "disabled")
                        << " displayCursor=" << display_cursor
                        << " cursorPlaneSessions=" << cursor_plane_sessions
                        << " legacyVideoCursorSessions=" << legacy_video_cursor_sessions
                        << " activeCaptureContexts=" << capture_ctxs.size();
        last_capture_cursor = next_capture_cursor;
        last_cursor_plane_sessions = cursor_plane_sessions;
        last_legacy_video_cursor_sessions = legacy_video_cursor_sessions;
      }
      capture_cursor = next_capture_cursor;
    };

    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;
      bool capture_cursor = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        const bool frame_captured_with_cursor = capture_cursor;
        drain_pending_capture_contexts();
        refresh_capture_cursor(capture_cursor);
        const bool drop_burned_cursor_frame_for_cursor_plane =
          frame_captured && frame_captured_with_cursor && !capture_cursor;
        static bool logged_cursor_plane_burn_drop = false;

        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }

          if (frame_captured) {
            if (drop_burned_cursor_frame_for_cursor_plane && capture_ctx->config.preferCursorPlane) {
              if (!logged_cursor_plane_burn_drop) {
                logged_cursor_plane_burn_drop = true;
                BOOST_LOG(info) << "Dropping cursor-burned frame for cursor-plane client";
              }
              ++capture_ctx;
              continue;
            }
            capture_ctx->images->raise(img);
          }

          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      drain_pending_capture_contexts();
      refresh_capture_cursor(capture_cursor);
      BOOST_LOG(debug) << "Final video cursor decision before backend capture"
                       << " backend=async"
                       << " preferCursorPlane=" << ((config.preferCursorPlane || last_cursor_plane_sessions > 0) ? 1 : 0)
                       << " finalVideoCursorEnabled=" << (capture_cursor ? 1 : 0)
                       << " cursorPlaneSessions=" << last_cursor_plane_sessions
                       << " legacyVideoCursorSessions=" << last_legacy_video_cursor_sessions;
      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &capture_cursor);

      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit: {
          reinit_event.raise(true);

          // Some classes of images contain references to the display --> display won't delete unless img is deleted
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
            const auto &config = capture_ctxs.front().config;
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

            // reset_display() will sleep between retries
            const auto reinit_started = std::chrono::steady_clock::now();
            reset_display(disp, encoder.platform_formats->dev_type, target_display_name, config);
            if (disp) {
              BOOST_LOG(info) << "[Display] Capture display reinit completed"
                              << " target=" << target_display_name
                              << " actual=" << disp->width << 'x' << disp->height
                              << " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - reinit_started).count();
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
   * @brief Temporal EMA (Exponential Moving Average) state for HDR luminance stats.
   * Prevents frame-to-frame brightness jitter/flicker in tone mapping by smoothing
   * the raw per-frame GPU statistics over time.
   */
  struct hdr_luminance_ema_t {
    float min_maxrgb = 0.0f;
    float max_maxrgb = 0.0f;
    float avg_maxrgb = 0.0f;
    float percentile_95 = 0.0f;
    float percentile_99 = 0.0f;
    bool initialized = false;

    /// EMA smoothing factor: 0.15 = responsive to changes while avoiding flicker.
    /// Lower α = more smoothing (less flicker, slower adaptation).
    /// Scene cuts are handled by fast-tracking when the change exceeds a threshold.
    static constexpr float ALPHA = 0.15f;
    static constexpr float SCENE_CUT_THRESHOLD = 3.0f;  // Ratio threshold for scene cut detection

    /**
     * @brief Apply EMA smoothing to raw per-frame stats.
     * On first frame or scene cuts (>3x luminance change), snaps to current value.
     * Otherwise applies exponential smoothing: smoothed = α·current + (1-α)·previous.
     */
    void
    update(const platf::hdr_frame_luminance_stats_t &raw) {
      if (!raw.valid) return;

      if (!initialized) {
        // First frame: snap to current values
        min_maxrgb = raw.min_maxrgb;
        max_maxrgb = raw.max_maxrgb;
        avg_maxrgb = raw.avg_maxrgb;
        percentile_95 = raw.percentile_95;
        percentile_99 = raw.percentile_99;
        initialized = true;
        return;
      }

      // Scene cut detection: if peak luminance changes dramatically, snap immediately
      float ratio = (max_maxrgb > 1.0f) ? raw.max_maxrgb / max_maxrgb : SCENE_CUT_THRESHOLD + 1.0f;
      float alpha = (ratio > SCENE_CUT_THRESHOLD || ratio < 1.0f / SCENE_CUT_THRESHOLD)
                    ? 1.0f  // Scene cut: snap to new values
                    : ALPHA; // Normal: smooth transition

      min_maxrgb = alpha * raw.min_maxrgb + (1.0f - alpha) * min_maxrgb;
      max_maxrgb = alpha * raw.max_maxrgb + (1.0f - alpha) * max_maxrgb;
      avg_maxrgb = alpha * raw.avg_maxrgb + (1.0f - alpha) * avg_maxrgb;
      percentile_95 = alpha * raw.percentile_95 + (1.0f - alpha) * percentile_95;
      percentile_99 = alpha * raw.percentile_99 + (1.0f - alpha) * percentile_99;
    }
  };

  /**
   * @brief Update per-frame HDR dynamic metadata with smoothed GPU-computed luminance stats.
   *
   * Called before each avcodec_send_frame() to inject accurate per-frame
   * maxRGB statistics into HDR Vivid and HDR10+ side data.
   * Uses P95 percentile for peak luminance (more stable than raw max) and
   * EMA-smoothed values to prevent frame-to-frame flicker.
   *
   * @param frame The AVFrame with pre-allocated dynamic HDR side data
   * @param ema Temporally-smoothed luminance statistics
   * @param max_display_luminance Display peak luminance in nits (from EDID)
   */
  void
  update_hdr_dynamic_metadata(AVFrame *frame, const hdr_luminance_ema_t &ema, uint16_t max_display_luminance) {
    if (!ema.initialized || !frame) return;

    float peak_nits = max_display_luminance > 0 ? static_cast<float>(max_display_luminance) : 1000.0f;

    // Use P95 as the "effective peak" for metadata — more stable than raw max,
    // avoids single-pixel HDR highlights distorting global tone mapping
    float effective_max = ema.percentile_95;

    // Update HDR Vivid (CUVA) dynamic metadata
    auto vivid_sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_VIVID);
    if (vivid_sd) {
      auto *vivid = reinterpret_cast<AVDynamicHDRVivid *>(vivid_sd->data);
      if (vivid && vivid->num_windows > 0) {
        auto &params = vivid->params[0];

        // Normalize to [0, 1] range relative to peak luminance
        // CUVA spec uses Q4.12 representation via AVRational with denominator 4095
        float min_norm = std::clamp(ema.min_maxrgb / peak_nits, 0.0f, 1.0f);
        float avg_norm = std::clamp(ema.avg_maxrgb / peak_nits, 0.0f, 1.0f);
        float max_norm = std::clamp(effective_max / peak_nits, 0.0f, 1.0f);

        // Variance: spread between P99 and min, normalized
        float variance_norm = std::clamp((ema.percentile_99 - ema.min_maxrgb) / peak_nits, 0.0f, 1.0f);

        params.minimum_maxrgb = av_make_q(static_cast<int>(min_norm * 4095), 4095);
        params.average_maxrgb = av_make_q(static_cast<int>(avg_norm * 4095), 4095);
        params.variance_maxrgb = av_make_q(static_cast<int>(variance_norm * 4095), 4095);
        params.maximum_maxrgb = av_make_q(static_cast<int>(max_norm * 4095), 4095);

        // Update targeted display luminance in tone mapping params
        for (int i = 0; i < 2; i++) {
          params.tm_params[i].targeted_system_display_maximum_luminance = av_make_q(max_display_luminance, 1);
        }
      }
    }

    // Update HDR10+ dynamic metadata
    auto hdr10plus_sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (hdr10plus_sd) {
      auto *hdr10plus = reinterpret_cast<AVDynamicHDRPlus *>(hdr10plus_sd->data);
      if (hdr10plus && hdr10plus->num_windows > 0) {
        auto &params = hdr10plus->params[0];

        // HDR10+ maxscl: use P95 for stability
        float max_norm = std::clamp(effective_max / peak_nits, 0.0f, 1.0f);
        float avg_norm = std::clamp(ema.avg_maxrgb / peak_nits, 0.0f, 1.0f);

        params.maxscl[0] = av_make_q(static_cast<int>(max_norm * 100000), 100000);
        params.maxscl[1] = av_make_q(static_cast<int>(max_norm * 100000), 100000);
        params.maxscl[2] = av_make_q(static_cast<int>(max_norm * 100000), 100000);
        params.average_maxrgb = av_make_q(static_cast<int>(avg_norm * 100000), 100000);

        hdr10plus->targeted_system_display_maximum_luminance = av_make_q(max_display_luminance, 1);
      }
    }
  }

  // Per-session EMA state for temporal smoothing of HDR luminance stats
  static thread_local hdr_luminance_ema_t hdr_ema_state;

  const char *
  cursor_probe_backend_name(platf::img_t::cursor_probe_backend_e backend) {
    switch (backend) {
      case platf::img_t::cursor_probe_backend_e::ddx_ram:
        return "DDX-RAM";
      case platf::img_t::cursor_probe_backend_e::ddx_vram:
        return "DDX-VRAM";
      case platf::img_t::cursor_probe_backend_e::wgc_ram:
        return "WGC-RAM";
      case platf::img_t::cursor_probe_backend_e::wgc_vram:
        return "WGC-VRAM";
      case platf::img_t::cursor_probe_backend_e::amd:
        return "AMD";
      case platf::img_t::cursor_probe_backend_e::unknown:
      default:
        return "unknown";
    }
  }

  void
  log_cursor_encoder_input_probe(const config_t &config, const platf::img_t &img, const char *path) {
    static std::atomic_uint samples { 0 };
    const auto sample = ++samples;
    const auto &probe = img.cursor_probe;
    if (!probe.active && !config.preferCursorPlane && sample > 20) {
      return;
    }
    if (sample > 80 && !probe.crop_changed) {
      return;
    }
    if (sample > 48 && !probe.crop_changed && !probe.mouse_update) {
      return;
    }
    if (sample > 16 &&
        !probe.crop_changed &&
        (!probe.mouse_update || (sample % 16) != 0)) {
      return;
    }

    BOOST_LOG(info) << "Cursor encoder-input proof"
                    << " runtime=" << config.cursorProbeRuntimeId
                    << " path=" << (path ? path : "unknown")
                    << " backend=" << cursor_probe_backend_name(probe.backend)
                    << " preferCursorPlane=" << (config.preferCursorPlane ? 1 : 0)
                    << " finalVideoCursorEnabled=" << (probe.final_video_cursor_enabled ? 1 : 0)
                    << " captureCursor=" << (probe.capture_cursor ? 1 : 0)
                    << " sourcePresent=" << (probe.source_present ? 1 : 0)
                    << " mouseUpdate=" << (probe.mouse_update ? 1 : 0)
                    << " pointerVisible=" << (probe.pointer_visible ? 1 : 0)
                    << " cursorRect=" << probe.x << ',' << probe.y << ',' << probe.w << ',' << probe.h
                    << " hash=" << probe.hash
                    << " cropChanged=" << (probe.crop_changed ? 1 : 0)
                    << " captureSample=" << probe.sample_index;
  }

  void
  apply_frame_interest_to_encoder(encode_session_t &session,
                                  const platf::img_t &img,
                                  const config_t &config,
                                  void *channel_data,
                                  frame_interest_feedback_fn_t frame_interest_feedback) {
    auto map = img.interest_map;
    if (map.frame_width <= 0) {
      map.frame_width = img.width > 0 ? img.width : config.width;
    }
    if (map.frame_height <= 0) {
      map.frame_height = img.height > 0 ? img.height : config.height;
    }
    if (config.width > 0 && config.height > 0 &&
        map.frame_width > 0 && map.frame_height > 0 &&
        (map.frame_width != config.width || map.frame_height != config.height)) {
      map = frame_interest::scale_to_frame(map, config.width, config.height);
    }

    auto intent_flags = config.lowBitrateClarityIntentFlags;
    const bool runtime_dynamic_interest =
      config.qualityCeilingBitrate > config.bitrate ||
      config.qualityCeilingFramerate > config.framerate ||
      config.lowBitrateClarityIntentFlags != 0;
    intent_flags = frame_interest::encoder_qp_delta_interest_flags(intent_flags, runtime_dynamic_interest);
    if ((intent_flags & stream_quality::clarity_intent_temporal_layers) != 0 &&
        map.temporal_policy == frame_interest::temporal_policy_e::none) {
      map.temporal_policy =
        (intent_flags & stream_quality::clarity_intent_discardable_enhancement) != 0 ?
          frame_interest::temporal_policy_e::base_with_discardable_enhancement :
          frame_interest::temporal_policy_e::base_only;
    }

    frame_interest::finalize(map);
    if (frame_interest_feedback && map.valid) {
      const auto frame_area = map.frame_width > 0 && map.frame_height > 0 ?
                                static_cast<std::uint64_t>(map.frame_width) *
                                  static_cast<std::uint64_t>(map.frame_height) :
                                0;
      frame_interest_feedback(channel_data, {
        .frame_area = frame_area,
        .dirty_area = static_cast<std::uint64_t>(std::max<std::int64_t>(0, frame_interest::total_dirty_area(map))),
        .full_frame_dirty = frame_interest::has_full_frame_dirty_region(map),
      });
    }
    session.set_frame_interest(map, intent_flags);

    if (!map.valid || intent_flags == 0) {
      return;
    }

    const auto caps = session.frame_interest_caps();
    const auto decision = frame_interest::decide_backend(map, caps, intent_flags);
    const bool accepted = decision.roi_accepted ||
                          decision.dirty_rects_accepted ||
                          decision.move_rects_accepted ||
                          decision.temporal_layers_accepted;
    const bool fallback = decision.roi_fallback ||
                          decision.dirty_rects_fallback ||
                          decision.move_rects_fallback ||
                          decision.temporal_layers_fallback;
    if (!accepted && !fallback) {
      return;
    }

    static auto last_backend_log = std::chrono::steady_clock::time_point {};
    const auto now = std::chrono::steady_clock::now();
    if (last_backend_log.time_since_epoch().count() != 0 && now - last_backend_log < 1000ms) {
      return;
    }
    last_backend_log = now;

    if (accepted) {
      BOOST_LOG(info) << "Frame interest metadata available encoder="
                      << session.encoder_backend_name() << " "
                      << frame_interest::summarize_decision(decision)
                      << " " << frame_interest::summarize_backend_caps(caps)
                      << " " << frame_interest::summarize_map(map);
    }
    if (fallback) {
      BOOST_LOG(info) << "Frame interest backend fallback encoder="
                      << session.encoder_backend_name() << " "
                      << frame_interest::summarize_decision(decision)
                      << " " << frame_interest::summarize_backend_caps(caps)
                      << " " << frame_interest::summarize_map(map);
    }
  }

  int
  encode_avcodec(int64_t frame_nr, avcodec_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto &frame = session.device->frame;
    frame->pts = frame_nr;

    auto &ctx = session.avcodec_ctx;

    auto &sps = session.sps;
    auto &vps = session.vps;

    // Update per-frame HDR dynamic metadata with GPU-computed luminance stats
    // Apply temporal EMA smoothing to prevent brightness jitter between frames
    {
      auto &raw_stats = session.device->hdr_luminance_stats;
      if (raw_stats.valid) {
        hdr_ema_state.update(raw_stats);

        uint16_t max_lum = 1000;
        auto mdm_sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
        if (mdm_sd) {
          auto *mdm = reinterpret_cast<AVMasteringDisplayMetadata *>(mdm_sd->data);
          if (mdm && mdm->has_luminance) {
            max_lum = static_cast<uint16_t>(av_q2d(mdm->max_luminance));
          }
        }
        update_hdr_dynamic_metadata(frame, hdr_ema_state, max_lum);
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

      if (av_packet && av_packet->pts == frame_nr) {
        packet->frame_timestamp = frame_timestamp;
      }

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      packets->raise(std::move(packet));
    }

    return 0;
  }

  int
  encode_nvenc(int64_t frame_nr, nvenc_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
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

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  int
  encode_amf(int64_t frame_nr, amf_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
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

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "AMF frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  int
  encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp);
    }
    else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);
    }
    else if (auto amf_session = dynamic_cast<amf_encode_session_t *>(&session)) {
      return encode_amf(frame_nr, *amf_session, packets, channel_data, frame_timestamp);
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

      auto bitrate = ((config::video.max_bitrate > 0) ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate) * 1000;
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

    // Attach HDR metadata to the AVFrame
    // Both PQ (ST 2084) and HLG (ARIB STD-B67) can carry HDR metadata.
    // PQ uses absolute luminance and requires static metadata (MDCV, CLL).
    // HLG uses scene-referred relative luminance but benefits from HDR Vivid (CUVA)
    // dynamic metadata for enhanced tone mapping on capable displays.
    if (colorspace_is_hdr(colorspace)) {
      SS_HDR_METADATA hdr_metadata;
      bool has_metadata = disp->get_hdr_metadata(hdr_metadata);

      if (has_metadata) {
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
        if (colorspace_is_pq(colorspace)) {
          auto hdr10plus = av_dynamic_hdr_plus_create_side_data(frame.get());
          if (hdr10plus) {
            // Set default values for HDR10+
            hdr10plus->itu_t_t35_country_code = 0xB5;  // USA
            hdr10plus->application_version = 0;
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
            params.maxscl[3] = av_make_q(0, 1);  // Unused

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

            // Set targeted system display maximum luminance from static metadata
            hdr10plus->targeted_system_display_maximum_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);
            hdr10plus->targeted_system_display_actual_peak_luminance_flag = 0;
            hdr10plus->mastering_display_actual_peak_luminance_flag = 0;

            BOOST_LOG(debug) << "Added HDR10+ dynamic metadata to frame";
          }
        }

        // HDR Vivid (CUVA HDR / T/UWA 3.137) dynamic metadata - both PQ and HLG
        // HDR Vivid supports both transfer functions:
        //   - PQ mode: absolute luminance tone mapping
        //   - HLG mode: scene-referred relative luminance tone mapping
        // The CUVA metadata is carried as ITU-T T.35 registered SEI/OBU, independent
        // of the underlying transfer function.
        auto vivid = av_dynamic_hdr_vivid_create_side_data(frame.get());
        if (vivid) {
          // Set default values for HDR Vivid
          vivid->system_start_code = 0x01;
          vivid->num_windows = 0x01;  // Single processing window

          // Initialize the first (and only) processing window
          auto &params = vivid->params[0];

          // Initialize maxrgb values (simplified - use full range)
          params.minimum_maxrgb = av_make_q(0, 4095);
          params.average_maxrgb = av_make_q(2047, 4095);  // 0.5
          params.variance_maxrgb = av_make_q(0, 4095);
          params.maximum_maxrgb = av_make_q(4095, 4095);  // 1.0

          // Initialize tone mapping parameters (simplified - no tone mapping)
          params.tone_mapping_mode_flag = 0;
          params.tone_mapping_param_num = 0;

          // Initialize color saturation mapping (disabled)
          params.color_saturation_mapping_flag = 0;
          params.color_saturation_num = 0;
          for (int j = 0; j < 8; j++) {
            params.color_saturation_gain[j] = av_make_q(128, 128);  // 1.0 (no adjustment)
          }

          // Initialize tone mapping params structure (even if not used)
          for (int i = 0; i < 2; i++) {
            auto &tm_params = params.tm_params[i];
            tm_params.targeted_system_display_maximum_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);
            tm_params.base_enable_flag = 0;
            tm_params.base_param_m_p = av_make_q(0, 16383);
            tm_params.base_param_m_m = av_make_q(0, 10);
            tm_params.base_param_m_a = av_make_q(0, 1023);
            tm_params.base_param_m_b = av_make_q(0, 1023);
            tm_params.base_param_m_n = av_make_q(0, 10);
            tm_params.base_param_k1 = 0;
            tm_params.base_param_k2 = 0;
            tm_params.base_param_k3 = 0;
            tm_params.base_param_Delta_enable_mode = 0;
            tm_params.base_param_Delta = av_make_q(0, 127);
            tm_params.three_Spline_enable_flag = 0;
            tm_params.three_Spline_num = 0;
            // Initialize three spline parameters
            for (int j = 0; j < 2; j++) {
              auto &spline = tm_params.three_spline[j];
              spline.th_mode = 0;
              spline.th_enable_mb = av_make_q(0, 255);
              spline.th_enable = av_make_q(0, 4095);
              spline.th_delta1 = av_make_q(0, 1023);
              spline.th_delta2 = av_make_q(0, 1023);
              spline.enable_strength = av_make_q(0, 255);
            }
          }

          BOOST_LOG(debug) << "Added HDR Vivid dynamic metadata to frame"
                           << (colorspace_is_hlg(colorspace) ? " (HLG mode)" : " (PQ mode)");
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

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - (int) video_format[encoder_t::VUI_PARAMETERS]) * (1 + config.videoFormat) : 0);

    return session;
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
      if (disp->get_hdr_metadata(hdr_metadata)) {
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
        nvenc_metadata.maxContentLightLevel = hdr_metadata.maxContentLightLevel;
        nvenc_metadata.maxFrameAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel;
        encode_device->nvenc->set_hdr_metadata(nvenc_metadata);
        BOOST_LOG(info) << "NVENC: HDR metadata set - max luminance: " << nvenc_metadata.maxDisplayLuminance
                        << " nits, mode: " << (colorspace_is_hlg(encode_device->colorspace) ? "HLG" : "PQ");
      }
    }

    return std::make_unique<nvenc_encode_session_t>(std::move(encode_device));
  }

  std::unique_ptr<amf_encode_session_t>
  make_amf_encode_session(platf::display_t *disp, const config_t &client_config, std::unique_ptr<platf::amf_encode_device_t> encode_device, bool is_probe = false) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace, is_probe)) {
      return nullptr;
    }

    // Set HDR metadata for AMF encoder if HDR is enabled
    if (colorspace_is_hdr(encode_device->colorspace) && encode_device->amf) {
      SS_HDR_METADATA hdr_metadata;
      if (disp->get_hdr_metadata(hdr_metadata)) {
        amf::amf_hdr_metadata amf_metadata;
        for (int i = 0; i < 3; i++) {
          amf_metadata.displayPrimaries[i].x = hdr_metadata.displayPrimaries[i].x;
          amf_metadata.displayPrimaries[i].y = hdr_metadata.displayPrimaries[i].y;
        }
        amf_metadata.whitePoint.x = hdr_metadata.whitePoint.x;
        amf_metadata.whitePoint.y = hdr_metadata.whitePoint.y;
        amf_metadata.maxDisplayLuminance = hdr_metadata.maxDisplayLuminance;
        amf_metadata.minDisplayLuminance = hdr_metadata.minDisplayLuminance;
        amf_metadata.maxContentLightLevel = hdr_metadata.maxContentLightLevel;
        amf_metadata.maxFrameAverageLightLevel = hdr_metadata.maxFrameAverageLightLevel;
        encode_device->amf->set_hdr_metadata(amf_metadata);
        BOOST_LOG(info) << "AMF: HDR metadata set - max luminance: " << amf_metadata.maxDisplayLuminance
                        << " nits, mode: " << (colorspace_is_hlg(encode_device->colorspace) ? "HLG" : "PQ");
      }
    }

    return std::make_unique<amf_encode_session_t>(std::move(encode_device));
  }

  std::unique_ptr<encode_session_t>
  make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device, bool is_probe = false) {
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, config, width, height, std::move(avcodec_encode_device));
    }
    else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(disp, config, std::move(nvenc_encode_device), is_probe);
    }
    else if (dynamic_cast<platf::amf_encode_device_t *>(encode_device.get())) {
      auto amf_encode_device = boost::dynamic_pointer_cast<platf::amf_encode_device_t>(std::move(encode_device));
      return make_amf_encode_session(disp, config, std::move(amf_encode_device), is_probe);
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
    img_event_t images,
    config_t &config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data,
    std::optional<dynamic_param_change_event_t> dynamic_param_events,
    frame_interest_feedback_fn_t frame_interest_feedback,
    input_activity_fn_t input_activity,
    startup_pacing_fn_t startup_pacing) {
    const auto encoder_init_started = std::chrono::steady_clock::now();
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return;
    }
    BOOST_LOG(info) << "Startup timeline server stage=video-encoder-ready"
                    << " encoder=" << session->encoder_backend_name()
                    << " requested=" << config.width << 'x' << config.height
                    << '@' << config.framerate
                    << " startupFps=" << config.startupFramerate
                    << " elapsedMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - encoder_init_started)
                         .count();

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

    // Keep sending low-rate duplicate frames for static/VRR scenes so clients do not
    // interpret "no desktop changes" as a dead video stream.
    const auto initial_keepalive_fps = stream_quality::static_frame_keepalive_fps(
      config.framerate,
      config::video.variable_refresh_rate,
      config::video.minimum_fps_target);
    std::chrono::duration<double, std::milli> minimum_frame_time { 1000.0 / initial_keepalive_fps };
    if (config::video.minimum_fps_target > 0) {
      BOOST_LOG(info) << "Minimum frame time set to "sv << minimum_frame_time.count() << "ms, based on minimum_fps_target "sv << config::video.minimum_fps_target << " fps."sv;
    }
    else if (config::video.variable_refresh_rate) {
      BOOST_LOG(info) << "Minimum frame time set to "sv << minimum_frame_time.count()
                      << "ms, based on VRR static keepalive "sv << initial_keepalive_fps
                      << " fps for client-requested target framerate "sv << config.framerate << "."sv;
    }
    else {
      BOOST_LOG(info) << "Minimum frame time set to "sv << minimum_frame_time.count() << "ms, based on client-requested target framerate "sv << config.framerate << "."sv;
    }

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto resolution_change_event = mail->event<std::pair<std::uint32_t, std::uint32_t>>(mail::resolution_change);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
    auto dynamic_param_events_ptr = dynamic_param_events.value_or(mail::man->queue<dynamic_param_t>(mail::dynamic_param_change));
    auto target_frame_time = std::chrono::duration<double, std::milli> { 1000.0 / std::max(1, config.framerate) };
    auto effective_target_frame_time = target_frame_time;
    std::optional<std::chrono::steady_clock::time_point> startup_next_emit_time;
    bool startup_pacing_active = false;
    std::optional<std::chrono::steady_clock::time_point> next_encode_sample_time;
    std::uint64_t stale_frame_drop_count = 0;
    std::uint64_t pacing_frame_drop_count = 0;
    auto last_stale_frame_drop_log = std::chrono::steady_clock::now();
    std::shared_ptr<platf::img_t> last_keepalive_img;
    std::uint64_t new_frame_encode_count = 0;
    std::uint64_t keepalive_encode_count = 0;
    std::uint64_t keepalive_reconvert_count = 0;
    std::uint64_t pop_wait_us = 0;
    std::uint64_t startup_sleep_us = 0;
    std::uint64_t stale_scan_us = 0;
    std::uint64_t convert_us = 0;
    std::uint64_t cursor_probe_us = 0;
    std::uint64_t frame_interest_us = 0;
    std::uint64_t mouse_keys_us = 0;
    std::uint64_t encode_us = 0;
    auto last_encode_loop_log = std::chrono::steady_clock::now();
    auto last_static_frame_mode = stream_quality::static_frame_mode_e::idle;
    auto add_elapsed_us = [](std::uint64_t &accumulator,
                             std::chrono::steady_clock::time_point started) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
      if (elapsed.count() > 0) {
        accumulator += static_cast<std::uint64_t>(elapsed.count());
      }
    };
    auto refresh_frame_interest_intent = [&config] {
      const auto content_type = std::clamp(config.contentType, 0, 3);
      const auto clarity_plan = stream_quality::plan_low_bitrate_clarity({
        .width = config.width,
        .height = config.height,
        .fps = config.framerate,
        .video_bitrate_kbps = config.bitrate,
        .video_format = config.videoFormat,
        .chroma_sampling_type = config.chromaSamplingType,
        .content_type = static_cast<stream_quality::content_type_e>(content_type),
      });

      config.lowBitrateClarityIntentFlags = clarity_plan.intent_flags;
      config.lowBitrateTargetQp = clarity_plan.target_qp;
      config.lowBitrateSharpenAlpha = clarity_plan.sharpen_alpha;
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
      bool restart_for_runtime_resolution = false;
      while (dynamic_param_events_ptr->peek()) {
        if (auto param = dynamic_param_events_ptr->pop(0ms)) {
          BOOST_LOG(info) << "Applying dynamic parameter change: type=" << (int) param->type;
          if (param->valid && param->type == dynamic_param_type_e::RESOLUTION) {
            constexpr int min_runtime_dimension = 64;
            constexpr int max_runtime_dimension = 16384;
            const int target_width = param->value.int_array_value[0];
            const int target_height = param->value.int_array_value[1];
            if (target_width < min_runtime_dimension ||
                target_height < min_runtime_dimension ||
                target_width > max_runtime_dimension ||
                target_height > max_runtime_dimension) {
              BOOST_LOG(warning) << "Ignoring invalid runtime encoder resolution: "
                                 << target_width << "x" << target_height;
              continue;
            }

            if (target_width != config.width || target_height != config.height) {
              const int previous_width = config.width;
              const int previous_height = config.height;
              config.width = target_width;
              config.height = target_height;
              refresh_frame_interest_intent();
              resolution_change_event->raise(std::make_pair(
                static_cast<std::uint32_t>(target_width),
                static_cast<std::uint32_t>(target_height)));
              idr_events->raise(true);
              restart_for_runtime_resolution = true;
              BOOST_LOG(info) << "Runtime encoder output scale change queued: "
                              << previous_width << "x" << previous_height
                              << " -> " << target_width << "x" << target_height
                              << " noDisplayReconfig=1 encoderReinit=1";
            }
            else {
              BOOST_LOG(info) << "Runtime encoder output scale unchanged: "
                              << target_width << "x" << target_height;
            }
            continue;
          }
          if (param->valid && param->type == dynamic_param_type_e::FPS && param->value.float_value >= 1.0f) {
            target_frame_time = std::chrono::duration<double, std::milli> { 1000.0 / param->value.float_value };
            effective_target_frame_time = target_frame_time;
            config.framerate = std::max(1, static_cast<int>(std::lround(param->value.float_value)));
            next_encode_sample_time.reset();
            startup_next_emit_time.reset();
            const auto keepalive_fps = stream_quality::static_frame_keepalive_fps(
              config.framerate,
              config::video.variable_refresh_rate,
              config::video.minimum_fps_target,
              last_static_frame_mode);
            minimum_frame_time = std::chrono::duration<double, std::milli> { 1000.0 / keepalive_fps };
            refresh_frame_interest_intent();
            BOOST_LOG(info) << "Encode pacing target changed to " << param->value.float_value
                            << " fps (" << target_frame_time.count()
                            << "ms), static keepalive=" << keepalive_fps
                            << " fps (" << minimum_frame_time.count() << "ms)";
          }
          else if (param->valid && param->type == dynamic_param_type_e::BITRATE && param->value.int_value > 0) {
            config.bitrate = param->value.int_value;
            refresh_frame_interest_intent();
          }
          else if (param->valid && param->type == dynamic_param_type_e::CHROMA_SAMPLING &&
                   (param->value.int_value == 0 || param->value.int_value == 1)) {
            config.chromaSamplingType = param->value.int_value;
            refresh_frame_interest_intent();
          }
          else if (param->valid && param->type == dynamic_param_type_e::DYNAMIC_RANGE &&
                   param->value.int_value >= 0) {
            config.dynamicRange = param->value.int_value;
            refresh_frame_interest_intent();
          }
          session->set_dynamic_param(*param);
        }
      }
      if (restart_for_runtime_resolution) {
        break;
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
      bool has_new_frame = false;
      bool has_keepalive_frame = false;
      effective_target_frame_time = startup_pacing ?
                                      startup_pacing(channel_data, config.framerate, target_frame_time) :
                                      target_frame_time;
      startup_pacing_active = effective_target_frame_time > target_frame_time + 100us;
      if (!startup_pacing_active) {
        startup_next_emit_time.reset();
      }

      // Encode at a minimum FPS to avoid image quality issues with static content
      // When variable_refresh_rate is enabled, only encode when we have a new frame
      const bool static_keepalive_input_active = input_activity && input_activity(channel_data);
      const bool static_keepalive_cursor_plane_active = config.preferCursorPlane;
      const auto static_frame_mode = stream_quality::static_frame_mode_for_input_activity(
        static_keepalive_input_active,
        static_keepalive_cursor_plane_active);
      if (static_frame_mode != last_static_frame_mode) {
        const auto keepalive_fps = stream_quality::static_frame_keepalive_fps(
          config.framerate,
          config::video.variable_refresh_rate,
          config::video.minimum_fps_target,
          static_frame_mode);
        minimum_frame_time = std::chrono::duration<double, std::milli> { 1000.0 / keepalive_fps };
        BOOST_LOG(info) << "Static frame keepalive mode changed to "
                        << (static_frame_mode == stream_quality::static_frame_mode_e::interactive_input ?
                              "interactive-input" : "idle")
                        << ": " << keepalive_fps << " fps ("
                        << minimum_frame_time.count() << "ms)"
                        << " inputActive=" << (static_keepalive_input_active ? 1 : 0)
                        << " preferCursorPlane=" << (static_keepalive_cursor_plane_active ? 1 : 0);
        last_static_frame_mode = static_frame_mode;
      }
      if (!requested_idr_frame || images->peek()) {
        const auto pop_started = std::chrono::steady_clock::now();
        auto img = images->pop(minimum_frame_time);
        add_elapsed_us(pop_wait_us, pop_started);
        if (img) {
          std::uint32_t dropped_stale_frames = 0;
          if (startup_pacing_active && startup_next_emit_time) {
            const auto now = std::chrono::steady_clock::now();
            if (now < *startup_next_emit_time) {
              const auto sleep_started = std::chrono::steady_clock::now();
              std::this_thread::sleep_until(*startup_next_emit_time);
              add_elapsed_us(startup_sleep_us, sleep_started);
            }
          }
          const auto stale_scan_started = std::chrono::steady_clock::now();
          while (!requested_idr_frame && images->peek()) {
            if (auto newer_img = images->pop(0ms)) {
              img = std::move(newer_img);
              ++dropped_stale_frames;
            }
            else {
              break;
            }
          }
          add_elapsed_us(stale_scan_us, stale_scan_started);
          if (dropped_stale_frames > 0) {
            stale_frame_drop_count += dropped_stale_frames;
            const auto now = std::chrono::steady_clock::now();
            if (now - last_stale_frame_drop_log >= 1000ms) {
              BOOST_LOG(info) << "Video encode dropped " << stale_frame_drop_count
                              << " stale captured frames to catch up to latest frame";
              stale_frame_drop_count = 0;
              last_stale_frame_drop_log = now;
            }
          }
          frame_timestamp = img->frame_timestamp;
          const auto sample_time = frame_timestamp.value_or(std::chrono::steady_clock::now());
          if (!requested_idr_frame) {
            if (!next_encode_sample_time) {
              next_encode_sample_time = sample_time;
            }

            // Capture is already paced by the host display and often produces
            // rates that are close to, but not equal to, the client target
            // (for example 75 Hz for 60 FPS, or 144 Hz for 120 FPS).  A
            // simple "wait one target interval after the previous encoded
            // frame" aliases those sources into half-rate output.  Track a
            // target sample timeline instead, so only true surplus frames are
            // skipped over time.
            constexpr auto source_jitter_slack = 750us;
            if (sample_time + source_jitter_slack < *next_encode_sample_time) {
              ++pacing_frame_drop_count;
              continue;
            }
          }
          if (startup_pacing_active) {
            const auto now = std::chrono::steady_clock::now();
            const auto effective_target_duration =
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(effective_target_frame_time);
            startup_next_emit_time = (startup_next_emit_time && *startup_next_emit_time > now) ?
                                       *startup_next_emit_time + effective_target_duration :
                                       now + effective_target_duration;
          }

          const auto convert_started = std::chrono::steady_clock::now();
          const int convert_result = session->convert(*img);
          add_elapsed_us(convert_us, convert_started);
          if (convert_result) {
            BOOST_LOG(error) << "Could not convert image"sv;
            // Don't exit permanently — break to let the outer reinit loop handle recovery
            break;
          }
          const auto cursor_probe_started = std::chrono::steady_clock::now();
          log_cursor_encoder_input_probe(config, *img, "async");
          add_elapsed_us(cursor_probe_us, cursor_probe_started);
          const auto frame_interest_started = std::chrono::steady_clock::now();
          apply_frame_interest_to_encoder(*session, *img, config, channel_data, frame_interest_feedback);
          add_elapsed_us(frame_interest_us, frame_interest_started);
          last_keepalive_img = img;
          has_new_frame = true;
        }
        else if (!images->running()) {
          break;
        }
        else if (last_keepalive_img) {
          const auto convert_started = std::chrono::steady_clock::now();
          const int convert_result = session->convert(*last_keepalive_img);
          add_elapsed_us(convert_us, convert_started);
          if (convert_result) {
            BOOST_LOG(error) << "Could not reconvert last image for static keepalive"sv;
            break;
          }
          has_keepalive_frame = true;
          ++keepalive_reconvert_count;
        }
        else {
          has_keepalive_frame = true;
        }
      }

      // While streaming check to see if the mouse is present and enable Mouse Keys to force the cursor to appear.
      // Run this BEFORE the VRR early-continue so a KVM switch on a static screen still recovers the cursor
      // even when no new frame would be encoded.
      const auto mouse_keys_started = std::chrono::steady_clock::now();
      platf::enable_mouse_keys();
      add_elapsed_us(mouse_keys_us, mouse_keys_started);

      // If variable refresh rate is enabled, skip encoding when no new frame is available.
      // Keepalive frames still encode to avoid stale static streams.
      if (config::video.variable_refresh_rate && !has_new_frame && !has_keepalive_frame && !requested_idr_frame) {
        if (config::video.minimum_fps_target == 0) {
          continue;
        }
      }

      const auto encode_started = std::chrono::steady_clock::now();
      const int encode_result = encode(frame_nr++, *session, packets, channel_data, frame_timestamp);
      add_elapsed_us(encode_us, encode_started);
      if (encode_result) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        // Don't exit permanently — break to let the outer reinit loop handle recovery
        break;
      }
      if (has_new_frame) {
        ++new_frame_encode_count;
        const auto encoded_sample_time = frame_timestamp.value_or(std::chrono::steady_clock::now());
        const auto target_frame_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(effective_target_frame_time);
        if (!next_encode_sample_time || requested_idr_frame) {
          next_encode_sample_time = encoded_sample_time + target_frame_duration;
        }
        else {
          do {
            *next_encode_sample_time += target_frame_duration;
          } while (*next_encode_sample_time <= encoded_sample_time);
        }
      }
      else if (has_keepalive_frame) {
        ++keepalive_encode_count;
      }
      {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_encode_loop_log >= 1000ms) {
          BOOST_LOG(info) << "Video encode loop runtime summary: newFrames="
                          << new_frame_encode_count
                          << " keepaliveFrames=" << keepalive_encode_count
                          << " keepaliveReconvert=" << keepalive_reconvert_count
                          << " pacingDrops=" << pacing_frame_drop_count
                          << " targetFrameMs=" << target_frame_time.count()
                          << " effectiveFrameMs=" << effective_target_frame_time.count()
                          << " startupPacing=" << (startup_pacing_active ? 1 : 0)
                          << " timingUs{pop=" << pop_wait_us
                          << ",startupSleep=" << startup_sleep_us
                          << ",stale=" << stale_scan_us
                          << ",convert=" << convert_us
                          << ",cursorProbe=" << cursor_probe_us
                          << ",frameInterest=" << frame_interest_us
                          << ",mouseKeys=" << mouse_keys_us
                          << ",encode=" << encode_us
                          << "}"
                          << " nextFrame=" << frame_nr
                          << " imagesRunning=" << (images->running() ? 1 : 0)
                          << " reinit=" << (reinit_event.peek() ? 1 : 0)
                          << " shutdown=" << (shutdown_event->peek() ? 1 : 0);
          new_frame_encode_count = 0;
          keepalive_encode_count = 0;
          keepalive_reconvert_count = 0;
          pacing_frame_drop_count = 0;
          pop_wait_us = 0;
          startup_sleep_us = 0;
          stale_scan_us = 0;
          convert_us = 0;
          cursor_probe_us = 0;
          frame_interest_us = 0;
          mouse_keys_us = 0;
          encode_us = 0;
          last_encode_loop_log = now;
        }
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
    auto make_encode_device_func = [&]() {
      return make_encode_device(*disp, encoder, ctx.config);
    };

    auto session = make_encode_session_with_ntsc_fallback(
      disp, encoder, ctx.config, img.width, img.height, make_encode_device_func);
    if (!session) {
      return std::nullopt;
    }

    // Get encode device colorspace for HDR metadata (need to create a temporary device)
    auto encode_device = make_encode_device(*disp, encoder, ctx.config);
    if (!encode_device) {
      return std::nullopt;
    }

    // Update client with our current HDR display state
    hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (colorspace_is_hdr(encode_device->colorspace)) {
      if (disp->get_hdr_metadata(hdr_info->metadata)) {
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
    log_cursor_encoder_input_probe(ctx.config, img, "sync-initial");
    apply_frame_interest_to_encoder(*session, img, ctx.config, ctx.channel_data, ctx.frame_interest_feedback);

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
      const auto &config = synced_session_ctxs.front()->config;
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

      // reset_display() will sleep between retries
      reset_display(disp, encoder.platform_formats->dev_type, target_display_name, config);
      if (disp) {
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
    std::optional<bool> last_capture_cursor;
    std::size_t last_cursor_plane_sessions = 0;
    std::size_t last_legacy_video_cursor_sessions = 0;
    while (encode_session_ctx_queue.running()) {
      const bool any_session_prefers_cursor_plane = std::any_of(
        std::begin(synced_session_ctxs),
        std::end(synced_session_ctxs),
        [](const auto &synced_ctx) {
          return synced_ctx && synced_ctx->config.preferCursorPlane;
        });
      bool capture_cursor = display_cursor && !any_session_prefers_cursor_plane;
      if (!last_capture_cursor || *last_capture_cursor != capture_cursor) {
        const auto cursor_plane_sessions = std::count_if(
          std::begin(synced_session_ctxs),
          std::end(synced_session_ctxs),
          [](const auto &synced_ctx) {
            return synced_ctx && synced_ctx->config.preferCursorPlane;
          });
        const auto legacy_video_cursor_sessions = synced_session_ctxs.size() - cursor_plane_sessions;
        BOOST_LOG(info) << "Capture cursor burn-in " << (capture_cursor ? "enabled" : "disabled")
                        << " displayCursor=" << display_cursor
                        << " cursorPlaneSessions=" << cursor_plane_sessions
                        << " legacyVideoCursorSessions=" << legacy_video_cursor_sessions
                        << " activeSyncSessions=" << synced_session_ctxs.size();
        last_capture_cursor = capture_cursor;
        last_cursor_plane_sessions = cursor_plane_sessions;
        last_legacy_video_cursor_sessions = legacy_video_cursor_sessions;
      }

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        const bool frame_captured_with_cursor = capture_cursor;
        while (encode_session_ctx_queue.peek()) {
          auto encode_session_ctx = encode_session_ctx_queue.pop();
          if (!encode_session_ctx) {
            return false;
          }

          synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*encode_session_ctx)));

          auto encode_session = make_synced_session(disp.get(), encoder, *img, *synced_session_ctxs.back());
          if (!encode_session) {
            ec = platf::capture_e::error;
            return false;
          }

          synced_sessions.emplace_back(std::move(*encode_session));
        }

        const bool any_session_prefers_cursor_plane_after_sync = std::any_of(
          std::begin(synced_session_ctxs),
          std::end(synced_session_ctxs),
          [](const auto &synced_ctx) {
            return synced_ctx && synced_ctx->config.preferCursorPlane;
          });
        const bool drop_burned_cursor_frame_for_cursor_plane =
          frame_captured && frame_captured_with_cursor && any_session_prefers_cursor_plane_after_sync;
        static bool logged_cursor_plane_sync_burn_drop = false;

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

          if (frame_captured) {
            if (drop_burned_cursor_frame_for_cursor_plane && ctx->config.preferCursorPlane) {
              if (!logged_cursor_plane_sync_burn_drop) {
                logged_cursor_plane_sync_burn_drop = true;
                BOOST_LOG(info) << "Dropping first cursor-burned sync frame for cursor-plane client";
              }
              pos->session->request_normal_frame();
              ++pos;
              continue;
            }
            if (pos->session->convert(*img)) {
              BOOST_LOG(error) << "Could not convert image"sv;
              ctx->shutdown_event->raise(true);

              continue;
            }
            log_cursor_encoder_input_probe(ctx->config, *img, "sync");
            apply_frame_interest_to_encoder(*pos->session,
                                            *img,
                                            ctx->config,
                                            ctx->channel_data,
                                            ctx->frame_interest_feedback);
          }

          std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
          if (img) {
            frame_timestamp = img->frame_timestamp;
          }

          if (encode(ctx->frame_nr++, *pos->session, ctx->packets, ctx->channel_data, frame_timestamp)) {
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
        return true;
      };

      BOOST_LOG(debug) << "Final video cursor decision before backend capture"
                       << " backend=sync"
                       << " preferCursorPlane=" << (last_cursor_plane_sessions > 0 ? 1 : 0)
                       << " finalVideoCursorEnabled=" << (capture_cursor ? 1 : 0)
                       << " cursorPlaneSessions=" << last_cursor_plane_sessions
                       << " legacyVideoCursorSessions=" << last_legacy_video_cursor_sessions;
      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &capture_cursor);
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
    std::optional<dynamic_param_change_event_t> dynamic_param_events,
    frame_interest_feedback_fn_t frame_interest_feedback,
    input_activity_fn_t input_activity,
    startup_pacing_fn_t startup_pacing) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<img_event_t::element_type>();
    auto lg = util::fail_guard([&]() {
      images->stop();
      shutdown_event->raise(true);
    });

    auto ref = capture_thread_async.ref();
    if (!ref) {
      BOOST_LOG(error) << "[Display] capture_async missing capture-thread ref";
      return;
    }

    auto state = ref->state;
    if (!state || !state->capture_ctx_queue || !state->encoder_p) {
      BOOST_LOG(error) << "[Display] capture_async missing async state"
                       << " state=" << (state ? 1 : 0)
                       << " queue=" << ((state && state->capture_ctx_queue) ? 1 : 0)
                       << " encoder=" << ((state && state->encoder_p) ? 1 : 0);
      return;
    }

    BOOST_LOG(info) << "[Display] capture_async queueing session"
                    << " requested=" << config.width << 'x' << config.height
                    << '@' << config.framerate
                    << " startupFps=" << config.startupFramerate
                    << " preferCursorPlane=" << (config.preferCursorPlane ? 1 : 0)
                    << " displayName=" << (config.display_name.empty() ? "<default>" : config.display_name)
                    << " queueRunning=" << (state->capture_ctx_queue->running() ? 1 : 0);
    state->capture_ctx_queue->raise(capture_ctx_t { images, config });

    if (!state->capture_ctx_queue->running()) {
      BOOST_LOG(warning) << "[Display] capture_async queue stopped immediately after raise";
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
    auto display_wait_started = std::chrono::steady_clock::now();
    auto last_display_wait_log = display_wait_started;
    constexpr auto capture_display_startup_timeout = 3500ms;

    // Track initial scale ratio (encoding resolution / display resolution)
    // Used to maintain consistent scaling when display resolution changes
    float initial_scale_x = 1.0f;
    float initial_scale_y = 1.0f;

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (state->reinit_event.peek()) {
        BOOST_LOG(debug) << "[Display] Reinit event detected, waiting for display ready...";
        std::this_thread::sleep_for(20ms);
        continue;
      }

      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      bool display_not_ready = false;
      {
        auto lg = state->display_wp.lock();
        if (state->display_wp->expired()) {
          display_not_ready = true;
        }
        else {
          display = state->display_wp->lock();
        }
      }
      if (display_not_ready) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - display_wait_started;
        if (now - last_display_wait_log >= 500ms) {
          BOOST_LOG(info) << "[Display] Waiting for capture display before encoder start elapsedMs="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                          << " queueRunning=" << (state->capture_ctx_queue->running() ? 1 : 0)
                          << " shutdown=" << (shutdown_event->peek() ? 1 : 0);
          last_display_wait_log = now;
        }

        if (elapsed >= capture_display_startup_timeout) {
          BOOST_LOG(error) << "[Display] Capture display startup timeout before encoder start elapsedMs="
                           << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                           << " queueRunning=" << (state->capture_ctx_queue->running() ? 1 : 0)
                           << " shutdown=" << (shutdown_event->peek() ? 1 : 0)
                           << " preferCursorPlane=" << (config.preferCursorPlane ? 1 : 0)
                           << " capture=" << config::video.capture
                           << " displayName=" << (config.display_name.empty() ? "<default>" : config.display_name);
          images->stop();
          shutdown_event->raise(true);
          state->capture_ctx_queue->stop();
          return;
        }

        std::this_thread::sleep_for(2ms);
        continue;
      }
      display_wait_started = std::chrono::steady_clock::now();
      last_display_wait_log = display_wait_started;

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
          static_cast<std::uint32_t>(config.width),
          static_cast<std::uint32_t>(config.height)));

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

        if (is_rotation) {
          std::swap(initial_scale_x, initial_scale_y);
        }

        config.width = compute_aligned_resolution(current_width, initial_scale_x);
        config.height = compute_aligned_resolution(current_height, initial_scale_y);

        BOOST_LOG(info) << "New encoding resolution: " << config.width << "x" << config.height
                        << " (scale: " << initial_scale_x << "x" << initial_scale_y << ")";

        resolution_change_event->raise(std::make_pair(
          static_cast<std::uint32_t>(config.width),
          static_cast<std::uint32_t>(config.height)));

        idr_events->raise(true);
        std::this_thread::sleep_for(100ms);
      }

      auto &encoder = *chosen_encoder;

      auto encode_device = make_encode_device(*display, encoder, config);
      if (!encode_device) {
        return;
      }

      // Absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      // Update client with our current HDR display state
      hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
      if (colorspace_is_hdr(encode_device->colorspace)) {
        if (display->get_hdr_metadata(hdr_info->metadata)) {
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
        state->reinit_event, *state->encoder_p,
        channel_data, dynamic_param_events, frame_interest_feedback, input_activity, startup_pacing);
    }
  }

  encoder_t *
  ensure_encoder_selected_for_capture();

  void
  capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data,
    std::optional<dynamic_param_change_event_t> dynamic_param_events,
    frame_interest_feedback_fn_t frame_interest_feedback,
    input_activity_fn_t input_activity,
    startup_pacing_fn_t startup_pacing) {
    auto idr_events = mail->event<bool>(mail::idr);
    auto *encoder = ensure_encoder_selected_for_capture();
    if (!encoder) {
      BOOST_LOG(error) << "Video capture cannot start because no usable encoder is selected";
      mail->event<bool>(mail::shutdown)->raise(true);
      return;
    }

    idr_events->raise(true);
    if (encoder->flags & PARALLEL_ENCODING) {
      capture_async(std::move(mail), config, channel_data, dynamic_param_events, frame_interest_feedback, input_activity, startup_pacing);
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
        frame_interest_feedback,
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
      if (encode(1, *session, packets, nullptr, {})) {
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
  validate_encoder(encoder_t &encoder, bool expect_failure) {
    std::shared_ptr<platf::display_t> disp;
    const auto configured_capture_backend = config::video.capture;
    auto probe_capture_override = capture_override_for_encoder_probe();

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
    const auto output_display_name { display_device::get_display_name(config::video.output_name) };
    reset_display(disp, encoder.platform_formats->dev_type, output_display_name, config_autoselect);
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
        reset_display(disp, encoder.platform_formats->dev_type, output_display_name, generic_hdr_config);
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
  probe_encoders_locked() {
    if (!allow_encoder_probing()) {
      // Error already logged
      return -1;
    }
    auto encoder_list = encoders;

    // If we already have a good encoder, check to see if another probe is required
    if (chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      BOOST_LOG(info) << "Using cached encoder validation results";
      return 0;
    }

    // Restart encoder selection
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;
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
          if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
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
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
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
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
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
      const auto output_display_name { display_device::get_display_name(config::video.output_name) };
      BOOST_LOG(error) << "Unable to find display or encoder during startup."sv;
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

  int
  probe_encoders() {
    std::lock_guard lock(encoder_probe_mutex);
    return probe_encoders_locked();
  }

  encoder_t *
  ensure_encoder_selected_for_capture() {
    std::lock_guard lock(encoder_probe_mutex);
    if (!chosen_encoder) {
      BOOST_LOG(warning) << "No encoder selected when video capture started; probing encoders on video thread";
      if (probe_encoders_locked()) {
        return nullptr;
      }
    }

    return chosen_encoder;
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
    capture_thread_ctx.state = std::make_shared<capture_thread_async_state_t>();
    auto state = capture_thread_ctx.state;

    {
      std::lock_guard lock(encoder_probe_mutex);
      state->encoder_p = chosen_encoder;
    }
    if (!state->encoder_p) {
      BOOST_LOG(error) << "[Display] Cannot launch async capture thread without a selected encoder";
      return -1;
    }
    state->reinit_event.reset();

    state->capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::thread {
      captureThread,
      state->capture_ctx_queue,
      std::ref(state->display_wp),
      std::ref(state->reinit_event),
      std::ref(*state->encoder_p)
    };

    BOOST_LOG(info) << "[Display] Async capture thread launched"
                    << " queueRunning=" << (state->capture_ctx_queue->running() ? 1 : 0)
                    << " encoderReady=" << (state->encoder_p ? 1 : 0);

    return 0;
  }
  void
  end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    auto state = std::move(capture_thread_ctx.state);
    BOOST_LOG(info) << "[Display] Async capture thread shutdown requested"
                    << " state=" << (state ? 1 : 0)
                    << " queue=" << ((state && state->capture_ctx_queue) ? 1 : 0)
                    << " joinable=" << (capture_thread_ctx.capture_thread.joinable() ? 1 : 0);
    if (state && state->capture_ctx_queue) {
      state->capture_ctx_queue->stop();
    }

    if (!capture_thread_ctx.capture_thread.joinable()) {
      BOOST_LOG(info) << "[Display] Async capture thread already not joinable";
      return;
    }

    std::promise<void> joined;
    auto joined_future = joined.get_future();
    std::thread joiner {
      [capture_thread = std::move(capture_thread_ctx.capture_thread),
       state = std::move(state),
       joined = std::move(joined)]() mutable {
        if (capture_thread.joinable()) {
          capture_thread.join();
        }
        joined.set_value();
      }
    };

    if (joined_future.wait_for(3s) == std::future_status::ready) {
      joiner.join();
      BOOST_LOG(info) << "[Display] Async capture thread joined cleanly";
    }
    else {
      BOOST_LOG(error) << "[Display] Capture thread did not stop within 3000ms; detaching join guard to avoid session teardown hang";
      joiner.detach();
    }
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
