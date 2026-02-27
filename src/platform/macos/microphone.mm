/**
 * @file src/platform/macos/microphone.mm
 * @brief Definitions for microphone capture on macOS.
 */
#include "src/platform/common.h"
#include "src/platform/macos/av_audio.h"

#include "src/config.h"
#include "src/logging.h"

namespace platf {
  using namespace std::literals;

  struct av_mic_t: public mic_t {
    AVAudio *av_audio_capture {};

    ~av_mic_t() override {
      [av_audio_capture release];
    }

    capture_e
    sample(std::vector<float> &sample_in) override {
      auto sample_size = sample_in.size();

      uint32_t length = 0;
      void *byteSampleBuffer = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &length);

      while (length < sample_size * sizeof(float)) {
        [av_audio_capture.samplesArrivedSignal wait];
        byteSampleBuffer = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &length);
      }

      const float *sampleBuffer = (float *) byteSampleBuffer;
      std::vector<float> vectorBuffer(sampleBuffer, sampleBuffer + sample_size);

      std::copy_n(std::begin(vectorBuffer), sample_size, std::begin(sample_in));

      TPCircularBufferConsume(&av_audio_capture->audioSampleBuffer, sample_size * sizeof(float));

      return capture_e::ok;
    }
  };

  struct macos_audio_control_t: public audio_control_t {
    AVCaptureDevice *audio_capture_device {};

    // 新增：音频播放相关
    AVAudioEngine *audio_engine {};
    AVAudioPlayerNode *player_node {};
    AVAudioFormat *audio_format {};

  public:
    int
    set_sink(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::set_sink() unimplemented: "sv << sink;
      return 0;
    }

    std::unique_ptr<mic_t>
    microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) override {
      auto mic = std::make_unique<av_mic_t>();
      const char *audio_sink = "";

      if (!config::audio.sink.empty()) {
        audio_sink = config::audio.sink.c_str();
      }

      if ((audio_capture_device = [AVAudio findMicrophone:[NSString stringWithUTF8String:audio_sink]]) == nullptr) {
        BOOST_LOG(error) << "opening microphone '"sv << audio_sink << "' failed. Please set a valid input source in the Sunshine config."sv;
        BOOST_LOG(error) << "Available inputs:"sv;

        for (NSString *name in [AVAudio microphoneNames]) {
          BOOST_LOG(error) << "\t"sv << [name UTF8String];
        }

        return nullptr;
      }

      mic->av_audio_capture = [[AVAudio alloc] init];

      if ([mic->av_audio_capture setupMicrophone:audio_capture_device sampleRate:sample_rate frameSize:frame_size channels:channels]) {
        BOOST_LOG(error) << "Failed to setup microphone."sv;
        return nullptr;
      }

      return mic;
    }

    std::optional<sink_t>
    sink_info() override {
      sink_t sink;

      return sink;
    }

    bool
    is_sink_available(const std::string &sink) override {
      return true;
    }

    int
    write_mic_data(const char *data, size_t size, uint16_t seq = 0) override {
      return -1;
    }

    int
    init_mic_redirect_device() override {
      // 1. 确定输出设备名称
      NSString *deviceName = @"BlackHole 2ch";
      if (!config::audio.virtual_sink.empty()) {
        deviceName = [NSString stringWithUTF8String:config::audio.virtual_sink.c_str()];
      }

      BOOST_LOG(info) << "Initializing virtual microphone with device: " << [deviceName UTF8String];

      // 2. 创建 AVAudioEngine
      audio_engine = [[AVAudioEngine alloc] init];
      if (!audio_engine) {
        BOOST_LOG(error) << "Failed to create AVAudioEngine";
        return -1;
      }

      // 3. 创建 AVAudioPlayerNode
      player_node = [[AVAudioPlayerNode alloc] init];
      if (!player_node) {
        BOOST_LOG(error) << "Failed to create AVAudioPlayerNode";
        [audio_engine release];
        audio_engine = nullptr;
        return -1;
      }

      // 4. 配置音频格式（48kHz, 2ch, float32）
      audio_format = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                      sampleRate:48000
                                                        channels:2
                                                     interleaved:NO];
      if (!audio_format) {
        BOOST_LOG(error) << "Failed to create audio format";
        [player_node release];
        [audio_engine release];
        player_node = nullptr;
        audio_engine = nullptr;
        return -1;
      }

      // 5. 连接节点
      [audio_engine attachNode:player_node];
      [audio_engine connect:player_node
                         to:audio_engine.mainMixerNode
                     format:audio_format];

      // 6. 启动音频引擎
      NSError *nsError = nil;
      if (![audio_engine startAndReturnError:&nsError]) {
        BOOST_LOG(error) << "Failed to start audio engine: " << [[nsError localizedDescription] UTF8String];
        [audio_format release];
        [player_node release];
        [audio_engine release];
        audio_format = nullptr;
        player_node = nullptr;
        audio_engine = nullptr;
        return -1;
      }

      // 7. 启动播放节点
      [player_node play];

      BOOST_LOG(info) << "Virtual microphone initialized successfully";
      BOOST_LOG(info) << "Please select '" << [deviceName UTF8String] << "' as input device in System Settings → Sound → Input";

      return 0;
    }

    void
    release_mic_redirect_device() override {
    }
  };

  std::unique_ptr<audio_control_t>
  audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }
}  // namespace platf
