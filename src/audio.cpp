/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
// standard includes
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

// local includes
#include "audio.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  namespace {
    void free_avcodec_ctx(AVCodecContext *ctx) {
      avcodec_free_context(&ctx);
    }
    void free_avframe(AVFrame *f) {
      av_frame_free(&f);
    }
    void free_avpacket(AVPacket *p) {
      av_packet_free(&p);
    }
    using avcodec_ctx_t = util::safe_ptr<AVCodecContext, free_avcodec_ctx>;
    using avframe_t = util::safe_ptr<AVFrame, free_avframe>;
    using avpacket_t = util::safe_ptr<AVPacket, free_avpacket>;

    // Format an FFmpeg error code into a Boost.Log error line. Centralised so
    // callers don't sprinkle 4-line ebuf/av_strerror snippets everywhere.
    void log_av_error(const char *codec_name, const char *what, int err) {
      char ebuf[128] {};
      av_strerror(err, ebuf, sizeof(ebuf));
      BOOST_LOG(error) << codec_name << ' ' << what << ": "sv << ebuf;
    }

    // Build an AVChannelLayout for the supported speaker counts. Returns
    // true on success; on failure the layout is left zero-initialised and
    // false is returned so the caller can bail with a sensible log.
    bool make_channel_layout(int channels, AVChannelLayout &out) {
      switch (channels) {
        case 2:
          out = AV_CHANNEL_LAYOUT_STEREO;
          return true;
        case 6:
          out = AV_CHANNEL_LAYOUT_5POINT1;
          return true;
        default:
          // av_channel_layout_default mirrors FFmpeg's own fallback.
          av_channel_layout_default(&out, channels);
          return out.nb_channels == channels;
      }
    }

    // Drain all packets currently available from the encoder and forward
    // them to the audio packet queue. Used both inside the encode loop and
    // during flush. Returns false on a fatal libav error (caller should
    // packets->stop() and exit); true otherwise (including on EAGAIN/EOF).
    template <typename Packets>
    bool drain_packets(AVCodecContext *ctx, AVPacket *pkt, Packets &packets,
                       void *channel_data, const char *codec_name) {
      while (true) {
        int err = avcodec_receive_packet(ctx, pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
          return true;
        }
        if (err < 0) {
          log_av_error(codec_name, "avcodec_receive_packet", err);
          return false;
        }
        buffer_t out { (std::size_t) pkt->size };
        std::memcpy(out.begin(), pkt->data, pkt->size);
        packets->raise(channel_data, std::move(out));
        av_packet_unref(pkt);
      }
    }

    // Pack a signed 16-bit sample as two little-endian bytes. Used by the
    // PCM_S16 path so we don't reinterpret_cast a uint8_t buffer to int16_t*
    // (alignment + endian UB) and to keep the inner loop one line.
    inline void write_s16le(uint8_t *dst, int16_t v) {
      const uint16_t u = static_cast<uint16_t>(v);
      dst[0] = static_cast<uint8_t>(u & 0xFF);
      dst[1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    }
  }  // namespace

  // Encodes one Sunshine audio capture stream as AC3 / E-AC3 (passthrough to a
  // bit-perfect decoder on the client). Returns true if encoding ran to
  // completion (capture queue drained); false if encoder init failed. The
  // codec is already negotiated via RTSP, so the caller MUST stop the audio
  // stream on failure -- silently downgrading to Opus would feed the client's
  // AC3 decoder Opus bytes and produce noise.
  static bool encodeThreadFFmpeg(sample_queue_t samples,
                                 const opus_stream_config_t &stream,
                                 const config_t &config,
                                 void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);

    enum AVCodecID codec_id = (config.codec == CODEC_EAC3) ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
    const char *codec_name = (codec_id == AV_CODEC_ID_EAC3) ? "E-AC3" : "AC3";

    // AC3 / E-AC3 max 6 channels (5.1). Refuse 7.1+ for now.
    if (stream.channelCount > 6) {
      BOOST_LOG(error) << codec_name << " encoder: capture has "sv << stream.channelCount
                       << " channels but AC3 only supports up to 5.1; audio stream will be stopped."sv;
      return false;
    }

    const AVCodec *codec = avcodec_find_encoder(codec_id);
    if (!codec) {
      BOOST_LOG(error) << codec_name << " encoder not available in linked FFmpeg; audio stream will be stopped."sv;
      return false;
    }

    avcodec_ctx_t ctx { avcodec_alloc_context3(codec) };
    if (!ctx) {
      BOOST_LOG(error) << "Failed to allocate "sv << codec_name << " encoder context"sv;
      return false;
    }

    ctx->sample_rate = stream.sampleRate;
    ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (!make_channel_layout(stream.channelCount, ctx->ch_layout)) {
      BOOST_LOG(error) << codec_name << " unsupported channel count: "sv << stream.channelCount;
      return false;
    }

    // Pick a sane bitrate: client value if provided, else codec defaults.
    int bit_rate = config.bitrate;
    if (bit_rate <= 0) {
      bit_rate = (stream.channelCount >= 6) ? 448000 :
                 (stream.channelCount == 2)  ? 192000 :
                                                96000;
    }
    ctx->bit_rate = bit_rate;

    int err = avcodec_open2(ctx.get(), codec, nullptr);
    if (err < 0) {
      log_av_error(codec_name, "avcodec_open2", err);
      return false;
    }

    // AC3 is fixed at 1536 samples per frame; we already forced
    // packetDuration=32ms in rtsp::cmd_announce so capture frame_size matches.
    const int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 1536;
    const int capture_frame_size = config.packetDuration * stream.sampleRate / 1000;
    if (capture_frame_size != frame_size) {
      BOOST_LOG(error) << codec_name << " encoder expects "sv << frame_size
                       << " samples/frame but capture is "sv << capture_frame_size
                       << "; audio stream will be stopped."sv;
      return false;
    }

    avframe_t frame { av_frame_alloc() };
    if (!frame) {
      BOOST_LOG(error) << "Failed to allocate AVFrame for "sv << codec_name;
      return false;
    }
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->nb_samples = frame_size;
    if (av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout) < 0 ||
        av_frame_get_buffer(frame.get(), 0) < 0) {
      BOOST_LOG(error) << "Failed to allocate AVFrame buffers for "sv << codec_name;
      return false;
    }

    avpacket_t pkt { av_packet_alloc() };
    if (!pkt) {
      BOOST_LOG(error) << "Failed to allocate AVPacket for "sv << codec_name;
      return false;
    }

    BOOST_LOG(info) << codec_name << " initialized: "sv << stream.sampleRate / 1000
                    << " kHz, "sv << stream.channelCount << " channels, "sv
                    << bit_rate / 1000 << " kbps"sv;

    const int channels = stream.channelCount;
    int64_t pts = 0;
    while (auto sample = samples->pop()) {
      // Sunshine capture is interleaved float in speaker_e order
      // (FL,FR,FC,LFE,BL,BR for 5.1) which already matches
      // AV_CHANNEL_LAYOUT_5POINT1 channel order, so no remap needed —
      // just deinterleave into planar.
      if (av_frame_make_writable(frame.get()) < 0) {
        BOOST_LOG(error) << codec_name << " av_frame_make_writable failed"sv;
        packets->stop();
        return true;
      }

      const float *src = sample->data();
      for (int s = 0; s < frame_size; ++s) {
        for (int c = 0; c < channels; ++c) {
          ((float *) frame->data[c])[s] = src[s * channels + c];
        }
      }
      frame->pts = pts;
      pts += frame_size;

      err = avcodec_send_frame(ctx.get(), frame.get());
      if (err < 0) {
        log_av_error(codec_name, "avcodec_send_frame", err);
        packets->stop();
        return true;
      }

      if (!drain_packets(ctx.get(), pkt.get(), packets, channel_data, codec_name)) {
        packets->stop();
        return true;
      }
    }

    // Flush: drain any remaining packets and forward them to the client so
    // the tail of the capture isn't lost.
    err = avcodec_send_frame(ctx.get(), nullptr);
    if (err < 0 && err != AVERROR_EOF) {
      log_av_error(codec_name, "flush avcodec_send_frame", err);
      packets->stop();
      return true;
    }
    if (!drain_packets(ctx.get(), pkt.get(), packets, channel_data, codec_name)) {
      packets->stop();
      return true;
    }
    return true;
  }

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
    {
      SAMPLE_RATE,
      12,
      8,
      4,
      platf::speaker::map_surround714,
      600000,
    },
    {
      SAMPLE_RATE,
      12,
      12,
      0,
      platf::speaker::map_surround714,
      3072000,
    },
  };

  // Raw LPCM passthrough thread: float [-1,1] -> s16 LE, no encoder
  // involved. packetDuration is forced to 5 ms by the client SDP path.
  static void encodePcmThread(sample_queue_t samples,
                              const opus_stream_config_t &stream,
                              const config_t &config,
                              void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    const int frame_samples = config.packetDuration * stream.sampleRate / 1000;
    const int channels = stream.channelCount;
    const int pcm_bytes = frame_samples * channels * static_cast<int>(sizeof(int16_t));

    BOOST_LOG(info) << "PCM_S16 passthrough: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << channels << " channels, "sv << config.packetDuration << " ms frames ("sv
                    << pcm_bytes << " B/frame, "sv
                    << (frame_samples * 1000 / config.packetDuration * channels * 2 * 8 / 1000) << " kbps)"sv;

    while (auto sample = samples->pop()) {
      buffer_t packet {static_cast<std::size_t>(pcm_bytes)};
      const float *src = sample->data();
      auto *out = packet.begin();
      const int total = frame_samples * channels;
      for (int i = 0; i < total; ++i) {
        float v = src[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        write_s16le(out + i * 2, static_cast<int16_t>(v * 32767.0f));
      }
      packets->raise(channel_data, std::move(packet));
    }
  }

  // Opus encoder thread (default audio codec). Bit-exact behaviour as the
  // pre-split implementation; only moved out of the dispatcher.
  static void encodeOpusThread(sample_queue_t samples,
                               const opus_stream_config_t &stream,
                               const config_t &config,
                               void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_COMPLEXITY(10));

    // Note: In-band FEC (OPUS_SET_INBAND_FEC) is a SILK-only feature and has no effect
    // in RESTRICTED_LOWDELAY mode (CELT-only). DRED is the CELT equivalent.

#ifdef OPUS_SET_DRED_DURATION_REQUEST  // Opus >= 1.5.0
    // DRED (Deep REDundancy): ML-based redundancy for graceful packet loss recovery
    // Works with CELT mode (RESTRICTED_LOWDELAY). Embeds redundancy in each packet
    // allowing the decoder to recover up to 100ms of lost audio from subsequent packets.
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_DRED_DURATION(100));
    BOOST_LOG(info) << "Opus DRED enabled: 100ms redundancy"sv;
#endif

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    const int frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();
        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  // Dispatcher: pick the right encoder thread based on the negotiated codec.
  // Each per-codec function owns its own packet-queue handle, init logging
  // and error path, so adding a new codec is just a new case below + a new
  // worst-case entry in stream.cpp's audio_payload table.
  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    platf::adjust_thread_priority(platf::thread_priority_e::high);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    switch (config.codec) {
      case CODEC_AC3:
      case CODEC_EAC3: {
        // Codec was already negotiated via RTSP cmd_announce, so the client
        // is committed to decoding an AC3/E-AC3 bitstream. If FFmpeg encoder
        // init fails we MUST stop the stream rather than fall back to Opus
        // — feeding Opus bytes to the client's AC3 decoder produces noise.
        if (encodeThreadFFmpeg(std::move(samples), stream, config, channel_data)) {
          return;
        }
        BOOST_LOG(error) << (config.codec == CODEC_AC3 ? "AC3"sv : "E-AC3"sv)
                         << " encoder init failed; stopping audio stream to avoid "
                            "feeding Opus bytes to the client's AC3 decoder."sv;
        mail::man->queue<packet_t>(mail::audio_packets)->stop();
        return;
      }
      case CODEC_PCM_S16:
        encodePcmThread(std::move(samples), stream, config, channel_data);
        return;
      default:
        encodeOpusThread(std::move(samples), stream, config, channel_data);
        return;
    }
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream) {
      BOOST_LOG(info) << "Audio streaming is disabled in configuration";
      shutdown_event->view();
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    BOOST_LOG(debug) << "Audio capture: acquiring context reference";
    auto ref = get_audio_ctx_ref();
    if (!ref) {
      BOOST_LOG(error) << "Audio capture: failed to get context reference";
      return;
    }
    BOOST_LOG(debug) << "Audio capture: context reference acquired successfully";

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
    });

    auto &control = ref->control;
    if (!control) {
      BOOST_LOG(error) << "Audio capture: control is null";
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    std::string *sink = &ref->sink.host;
    if (!config::audio.sink.empty()) {
      sink = &config::audio.sink;
    }

    // Prefer the virtual sink if host playback is disabled or there's no other sink
    if (ref->sink.null && (!config.flags[config_t::HOST_AUDIO] || sink->empty())) {
      auto &null = *ref->sink.null;
      switch (stream.channelCount) {
        case 2:
          sink = &null.stereo;
          break;
        case 6:
          sink = &null.surround51;
          break;
        case 8:
          sink = &null.surround71;
          break;
        case 12:
          if (!null.surround714.empty()) {
            sink = &null.surround714;
          }
          break;
      }
    }

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != *sink;
      if (ref->restore_sink) {
        if (control->set_sink(*sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
    if (!mic) {
      BOOST_LOG(error) << "Audio capture: failed to initialize microphone";
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();
    BOOST_LOG(info) << "Audio capture initialized successfully, entering sampling loop";

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    auto samples = std::make_shared<sample_queue_t::element_type>(30);
    std::thread thread {encodeThread, samples, config, channel_data};

    auto fg = util::fail_guard([&]() {
      samples->stop();
      thread.join();

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      switch (status) {
        case platf::capture_e::ok:
          break;
        case platf::capture_e::timeout:
          continue;
        case platf::capture_e::reinit:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        default:
          return;
      }

      samples->raise(std::move(sample_buffer));
    }
    
    BOOST_LOG(info) << "Audio capture sampling loop ended (shutdown requested)";
  }

  // 确保唯一实例
  namespace {
    auto control_shared = safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control);
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    return control_shared.ref();
  }

  // 检查音频上下文是否有活动的引用，不触发构造
  bool has_audio_ctx_ref() {
    return control_shared.has_ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    int shift = quality ? 1 : 0;
    switch (channels) {
      case 2:
        return STEREO + shift;
      case 6:
        return SURROUND51 + shift;
      case 8:
        return SURROUND71 + shift;
      case 12:
        return SURROUND714 + shift;
    }
    if (channels >= 12) {
      return SURROUND714 + shift;
    }
    if (channels >= 8) {
      return SURROUND71 + shift;
    }
    return STEREO;
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // 检查 control 是否存在，如果不存在则无法恢复 sink
    if (!ctx.control) {
      BOOST_LOG(debug) << "Audio control not available, skipping sink restoration";
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail
      ctx.control->set_sink(sink);
    }
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    stream.channelCount = params.channelCount;
    stream.streams = params.streams;
    stream.coupledStreams = params.coupledStreams;
    stream.mapping = params.mapping;
  }

  int init_mic_redirect_device() {
    // 关键修复：先检查是否有活动的引用，避免触发 start_audio_control
    // 如果没有活动的引用，说明音频上下文没有启动，不应该初始化麦克风设备
    if (!has_audio_ctx_ref()) {
      BOOST_LOG(debug) << "Audio context not active, skipping microphone device initialization";
      return -1;
    }
    
    auto ref = get_audio_ctx_ref();
    if (!ref || !ref->control) {
      BOOST_LOG(error) << "Audio context not available for microphone data writing";
      return -1;
    }
    return ref->control->init_mic_redirect_device();
  }

  void release_mic_redirect_device() {
    // 关键修复：先检查是否有活动的引用，避免触发 start_audio_control
    // 如果没有活动的引用，说明音频上下文没有启动，不需要释放
    if (!has_audio_ctx_ref()) {
      BOOST_LOG(debug) << "Audio context not active, skipping microphone device release";
      return;
    }
    
    auto ref = get_audio_ctx_ref();
    if (!ref || !ref->control) {
      BOOST_LOG(warning) << "Audio context not available for microphone device release";
      return;
    }
    ref->control->release_mic_redirect_device();
  }

  int write_mic_data(const std::uint8_t *data, size_t size, uint16_t seq) {
    // 先检查是否有活动引用，避免不必要地触发 start_audio_control
    // 如果音频捕获线程正在运行，它会持有引用，这里会返回 true
    if (!has_audio_ctx_ref()) {
      BOOST_LOG(debug) << "Audio context not active, skipping microphone data write";
      // 注意：这不是错误，而是正常情况
      // 可能音频捕获还没有启动，或者已经停止
      return -1;
    }
    
    auto ref = get_audio_ctx_ref();
    if (!ref || !ref->control) {
      BOOST_LOG(warning) << "Audio context reference invalid for microphone data writing";
      return -1;
    }

    return ref->control->write_mic_data(reinterpret_cast<const char*>(data), size, seq);
  }
}  // namespace audio