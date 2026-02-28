/**
 * @file src/platform/macos/microphone.mm
 * @brief Definitions for microphone capture on macOS.
 */
#include "src/platform/common.h"
#include "src/platform/macos/av_audio.h"

#include "src/config.h"
#include "src/logging.h"

#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>
#import <AudioToolbox/AudioToolbox.h>

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

    // AudioQueue remote microphone fields
    AudioQueueRef audio_queue {nullptr};
    AudioStreamBasicDescription audio_format {};
    AudioQueueBufferRef buffers[3] {nullptr, nullptr, nullptr};
    AudioDeviceID blackhole_device_id {kAudioDeviceUnknown};
    bool mic_initialized {false};

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

    // Helper: Find BlackHole audio device ID
    AudioDeviceID
    find_blackhole_device_id(NSString *deviceName) {
      AudioDeviceID deviceID = kAudioDeviceUnknown;

      // Get all audio devices
      AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };

      UInt32 dataSize = 0;
      OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                       &propertyAddress,
                                                       0,
                                                       NULL,
                                                       &dataSize);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to get audio devices size: " << status;
        return kAudioDeviceUnknown;
      }

      int deviceCount = dataSize / sizeof(AudioDeviceID);
      AudioDeviceID *audioDevices = (AudioDeviceID *)malloc(dataSize);

      status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                         &propertyAddress,
                                         0,
                                         NULL,
                                         &dataSize,
                                         audioDevices);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to get audio devices: " << status;
        free(audioDevices);
        return kAudioDeviceUnknown;
      }

      // Search for BlackHole device
      for (int i = 0; i < deviceCount; i++) {
        CFStringRef deviceNameRef = NULL;
        UInt32 propertySize = sizeof(deviceNameRef);

        AudioObjectPropertyAddress nameAddress = {
          kAudioDevicePropertyDeviceNameCFString,
          kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain
        };

        status = AudioObjectGetPropertyData(audioDevices[i],
                                           &nameAddress,
                                           0,
                                           NULL,
                                           &propertySize,
                                           &deviceNameRef);

        if (status == noErr && deviceNameRef) {
          if (CFStringCompare(deviceNameRef, (__bridge CFStringRef)deviceName, 0) == kCFCompareEqualTo) {
            deviceID = audioDevices[i];
            BOOST_LOG(info) << "Found BlackHole device ID: " << deviceID;
            CFRelease(deviceNameRef);
            break;
          }
          CFRelease(deviceNameRef);
        }
      }

      free(audioDevices);
      return deviceID;
    }

    // AudioQueue callback: called when buffer is done playing
    static void
    audio_queue_output_callback(void *inUserData,
                                AudioQueueRef inAQ,
                                AudioQueueBufferRef inBuffer) {
      // Buffer is now free to reuse
      // No action needed - we'll manage buffers manually
    }

    int
    write_mic_data(const char *data, size_t size, uint16_t seq = 0) override {
      if (!mic_initialized || !audio_queue) {
        return -1;
      }

      // Input: int16 PCM, stereo interleaved
      const int16_t *samples = reinterpret_cast<const int16_t *>(data);
      size_t frameCount = size / (2 * sizeof(int16_t));  // 2 channels

      if (frameCount == 0) {
        return 0;
      }

      // Find available buffer
      AudioQueueBufferRef buffer = nullptr;
      for (int i = 0; i < 3; i++) {
        if (buffers[i] && buffers[i]->mAudioDataByteSize == 0) {
          buffer = buffers[i];
          break;
        }
      }

      if (!buffer) {
        // All buffers in use, allocate temporary
        UInt32 bufferSize = frameCount * 2 * sizeof(float);
        OSStatus status = AudioQueueAllocateBuffer(audio_queue, bufferSize, &buffer);
        if (status != noErr) {
          BOOST_LOG(warning) << "Failed to allocate temp buffer, skipping frame";
          return -1;
        }
      }

      // Convert int16 → float32, non-interleaved
      float *audioData = (float *)buffer->mAudioData;
      float *leftChannel = audioData;
      float *rightChannel = audioData + frameCount;

      for (size_t i = 0; i < frameCount; i++) {
        leftChannel[i] = samples[i * 2] / 32768.0f;
        rightChannel[i] = samples[i * 2 + 1] / 32768.0f;
      }

      buffer->mAudioDataByteSize = frameCount * 2 * sizeof(float);

      // Enqueue buffer
      OSStatus status = AudioQueueEnqueueBuffer(audio_queue, buffer, 0, NULL);
      if (status != noErr) {
        BOOST_LOG(warning) << "Failed to enqueue buffer: " << status;
        buffer->mAudioDataByteSize = 0;  // Mark as free
        return -1;
      }

      return 0;
    }

    /*
    int
    write_mic_data(const char *data, size_t size, uint16_t seq = 0) override {
      if (!audio_engine || !player_node || !audio_format) {
        BOOST_LOG(warning) << "Audio engine not initialized, cannot write mic data";
        return -1;
      }

      // 假设客户端发送的是 48kHz, 2ch, int16 PCM
      const int16_t *samples = reinterpret_cast<const int16_t *>(data);
      size_t frameCount = size / (2 * sizeof(int16_t));  // 2 channels

      if (frameCount == 0) {
        return 0;  // 空数据，直接返回
      }

      // 创建 AVAudioPCMBuffer
      AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:audio_format
           frameCapacity:frameCount];
      if (!buffer) {
        BOOST_LOG(error) << "Failed to create AVAudioPCMBuffer";
        return -1;
      }

      buffer.frameLength = frameCount;

      // 转换 int16 → float32
      float *leftChannel = buffer.floatChannelData[0];
      float *rightChannel = buffer.floatChannelData[1];

      for (size_t i = 0; i < frameCount; i++) {
        leftChannel[i] = samples[i * 2] / 32768.0f;
        rightChannel[i] = samples[i * 2 + 1] / 32768.0f;
      }

      // 调度播放
      [player_node scheduleBuffer:buffer
                completionHandler:^{
                  [buffer release];
                }];

      return 0;
    }
    */

    int
    init_mic_redirect_device() override {
      if (mic_initialized) {
        BOOST_LOG(warning) << "Remote microphone already initialized";
        return 0;
      }

      // 1. Determine device name
      NSString *deviceName = @"BlackHole 2ch";
      if (!config::audio.virtual_sink.empty()) {
        deviceName = [NSString stringWithUTF8String:config::audio.virtual_sink.c_str()];
      }

      BOOST_LOG(info) << "Initializing remote microphone with device: " << [deviceName UTF8String];

      // 2. Find BlackHole device ID
      blackhole_device_id = find_blackhole_device_id(deviceName);
      if (blackhole_device_id == kAudioDeviceUnknown) {
        BOOST_LOG(error) << "Failed to find BlackHole device: " << [deviceName UTF8String];
        BOOST_LOG(error) << "Please install BlackHole: brew install blackhole-2ch";
        return -1;
      }

      // 3. Configure audio format (48kHz, 2ch, float32, non-interleaved)
      audio_format.mSampleRate = 48000.0;
      audio_format.mFormatID = kAudioFormatLinearPCM;
      audio_format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
      audio_format.mBytesPerPacket = sizeof(float);
      audio_format.mFramesPerPacket = 1;
      audio_format.mBytesPerFrame = sizeof(float);
      audio_format.mChannelsPerFrame = 2;
      audio_format.mBitsPerChannel = 32;
      audio_format.mReserved = 0;

      // 4. Create AudioQueue
      OSStatus status = AudioQueueNewOutput(&audio_format,
                                           audio_queue_output_callback,
                                           this,
                                           NULL,
                                           kCFRunLoopCommonModes,
                                           0,
                                           &audio_queue);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to create AudioQueue: " << status;
        return -1;
      }

      // 5. Set output device to BlackHole
      status = AudioQueueSetProperty(audio_queue,
                                     kAudioQueueProperty_CurrentDevice,
                                     &blackhole_device_id,
                                     sizeof(blackhole_device_id));
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to set AudioQueue output device: " << status;
        AudioQueueDispose(audio_queue, true);
        audio_queue = nullptr;
        return -1;
      }

      BOOST_LOG(info) << "Successfully set output device to: " << [deviceName UTF8String];

      // 6. Allocate buffers (3 buffers, 100ms each)
      UInt32 bufferSize = 48000 * 2 * sizeof(float) / 10;  // 100ms
      for (int i = 0; i < 3; i++) {
        status = AudioQueueAllocateBuffer(audio_queue, bufferSize, &buffers[i]);
        if (status != noErr) {
          BOOST_LOG(error) << "Failed to allocate buffer " << i << ": " << status;
          // Clean up
          for (int j = 0; j < i; j++) {
            AudioQueueFreeBuffer(audio_queue, buffers[j]);
            buffers[j] = nullptr;
          }
          AudioQueueDispose(audio_queue, true);
          audio_queue = nullptr;
          return -1;
        }
      }

      // 7. Start the queue
      status = AudioQueueStart(audio_queue, NULL);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to start AudioQueue: " << status;
        for (int i = 0; i < 3; i++) {
          AudioQueueFreeBuffer(audio_queue, buffers[i]);
          buffers[i] = nullptr;
        }
        AudioQueueDispose(audio_queue, true);
        audio_queue = nullptr;
        return -1;
      }

      mic_initialized = true;
      BOOST_LOG(info) << "Remote microphone initialized successfully";
      BOOST_LOG(info) << "Set system input to '" << [deviceName UTF8String] << "' in System Settings → Sound → Input";

      return 0;
    }

    /*
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

      // 5. 查找 BlackHole 设备 ID 并设置为输出设备
      AVAudioOutputNode *outputNode = audio_engine.outputNode;
      AudioUnit outputUnit = outputNode.audioUnit;

      AudioDeviceID blackHoleDeviceID = kAudioDeviceUnknown;
      UInt32 propertySize;
      AudioObjectPropertyAddress propertyAddress = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };

      AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &propertySize);
      int deviceCount = propertySize / sizeof(AudioDeviceID);
      AudioDeviceID *audioDevices = (AudioDeviceID *)malloc(propertySize);
      AudioObjectGetPropertyData(kAudioObjectSystemObject, &propertyAddress, 0, NULL, &propertySize, audioDevices);

      for (int i = 0; i < deviceCount; i++) {
        CFStringRef deviceNameRef = NULL;
        propertySize = sizeof(deviceNameRef);
        propertyAddress.mSelector = kAudioDevicePropertyDeviceNameCFString;
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;

        AudioObjectGetPropertyData(audioDevices[i], &propertyAddress, 0, NULL, &propertySize, &deviceNameRef);

        if (deviceNameRef) {
          if (CFStringCompare(deviceNameRef, (__bridge CFStringRef)deviceName, 0) == kCFCompareEqualTo) {
            blackHoleDeviceID = audioDevices[i];
            CFRelease(deviceNameRef);
            break;
          }
          CFRelease(deviceNameRef);
        }
      }
      free(audioDevices);

      if (blackHoleDeviceID == kAudioDeviceUnknown) {
        BOOST_LOG(error) << "Failed to find BlackHole audio device: " << [deviceName UTF8String];
        [audio_format release];
        [player_node release];
        [audio_engine release];
        audio_format = nullptr;
        player_node = nullptr;
        audio_engine = nullptr;
        return -1;
      }

      // 设置输出设备为 BlackHole
      OSStatus status = AudioUnitSetProperty(outputUnit,
                                            kAudioOutputUnitProperty_CurrentDevice,
                                            kAudioUnitScope_Global,
                                            0,
                                            &blackHoleDeviceID,
                                            sizeof(blackHoleDeviceID));

      if (status != noErr) {
        BOOST_LOG(error) << "Failed to set output device to BlackHole: " << status;
        [audio_format release];
        [player_node release];
        [audio_engine release];
        audio_format = nullptr;
        player_node = nullptr;
        audio_engine = nullptr;
        return -1;
      }

      BOOST_LOG(info) << "Successfully set output device to: " << [deviceName UTF8String];

      // 6. 连接节点
      [audio_engine attachNode:player_node];
      [audio_engine connect:player_node
                         to:audio_engine.mainMixerNode
                     format:audio_format];

      // 7. 启动音频引擎
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

      // 8. 启动播放节点
      [player_node play];

      BOOST_LOG(info) << "Virtual microphone initialized successfully";
      BOOST_LOG(info) << "Please select '" << [deviceName UTF8String] << "' as input device in System Settings → Sound → Input";

      return 0;
    }
    */

    void
    release_mic_redirect_device() override {
      if (!mic_initialized) {
        return;
      }

      BOOST_LOG(info) << "Releasing remote microphone";

      if (audio_queue) {
        // Stop the queue immediately
        AudioQueueStop(audio_queue, true);

        // Free all buffers
        for (int i = 0; i < 3; i++) {
          if (buffers[i]) {
            AudioQueueFreeBuffer(audio_queue, buffers[i]);
            buffers[i] = nullptr;
          }
        }

        // Dispose queue
        AudioQueueDispose(audio_queue, true);
        audio_queue = nullptr;
      }

      blackhole_device_id = kAudioDeviceUnknown;
      mic_initialized = false;

      BOOST_LOG(info) << "Remote microphone released";
    }

    /*
    void
    release_mic_redirect_device() override {
      BOOST_LOG(info) << "Releasing virtual microphone resources";

      if (player_node) {
        [player_node stop];
        [player_node release];
        player_node = nullptr;
      }

      if (audio_engine) {
        [audio_engine stop];
        [audio_engine release];
        audio_engine = nullptr;
      }

      if (audio_format) {
        [audio_format release];
        audio_format = nullptr;
      }

      BOOST_LOG(info) << "Virtual microphone resources released";
    }
    */
  };

  std::unique_ptr<audio_control_t>
  audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }
}  // namespace platf
