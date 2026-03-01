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

#include <opus/opus.h>
#include <vector>
#include "../../third-party/TPCircularBuffer/TPCircularBuffer.h"

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

    // AudioUnit remote microphone fields
    AudioUnit audio_unit {nullptr};
    AudioStreamBasicDescription audio_format {};
    AudioDeviceID blackhole_device_id {kAudioDeviceUnknown};
    bool mic_initialized {false};

    // OPUS decoder for remote microphone
    OpusDecoder *opus_decoder {nullptr};

    // Ring buffer for audio data (lock-free communication between threads)
    TPCircularBuffer ring_buffer;

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
    // AudioUnit render callback (called on real-time thread)
    static OSStatus
    audio_unit_render_callback(void *inRefCon,
                               AudioUnitRenderActionFlags *ioActionFlags,
                               const AudioTimeStamp *inTimeStamp,
                               UInt32 inBusNumber,
                               UInt32 inNumberFrames,
                               AudioBufferList *ioData) {
      macos_audio_control_t *self = (macos_audio_control_t *)inRefCon;

      static int callback_count = 0;
      bool should_log = (++callback_count % 100 == 1);

      // Read audio data from ring buffer (interleaved stereo float32)
      uint32_t bytesNeeded = inNumberFrames * 2 * sizeof(float);  // stereo interleaved
      uint32_t availableBytes = 0;
      float *readPtr = (float *)TPCircularBufferTail(&self->ring_buffer, &availableBytes);

      if (should_log) {
        BOOST_LOG(info) << "AudioUnit render callback: frames=" << inNumberFrames
                        << " buffers=" << ioData->mNumberBuffers
                        << " bytesNeeded=" << bytesNeeded
                        << " available=" << availableBytes;
      }

      uint32_t bytesToCopy = std::min(bytesNeeded, availableBytes);
      uint32_t framesToCopy = bytesToCopy / (2 * sizeof(float));

      if (bytesToCopy > 0 && ioData->mNumberBuffers >= 2) {
        // Non-interleaved format: separate buffers for left and right channels
        float *leftChannel = (float *)ioData->mBuffers[0].mData;
        float *rightChannel = (float *)ioData->mBuffers[1].mData;

        // De-interleave: readPtr contains [L, R, L, R, ...]
        for (uint32_t frame = 0; frame < framesToCopy; frame++) {
          leftChannel[frame] = readPtr[frame * 2];      // Left
          rightChannel[frame] = readPtr[frame * 2 + 1]; // Right
        }

        TPCircularBufferConsume(&self->ring_buffer, bytesToCopy);

        if (should_log) {
          // Check max sample value
          float maxSample = 0.0f;
          for (uint32_t i = 0; i < framesToCopy; i++) {
            maxSample = std::max(maxSample, fabs(leftChannel[i]));
            maxSample = std::max(maxSample, fabs(rightChannel[i]));
          }
          BOOST_LOG(info) << "Copied " << framesToCopy << " frames, max sample: " << maxSample;
        }

        // Fill remaining with silence if not enough data
        if (framesToCopy < inNumberFrames) {
          memset(leftChannel + framesToCopy, 0, (inNumberFrames - framesToCopy) * sizeof(float));
          memset(rightChannel + framesToCopy, 0, (inNumberFrames - framesToCopy) * sizeof(float));
        }
      } else {
        // No data available, output silence
        for (UInt32 i = 0; i < ioData->mNumberBuffers; i++) {
          memset(ioData->mBuffers[i].mData, 0, inNumberFrames * sizeof(float));
        }
        if (should_log) {
          BOOST_LOG(info) << "No data available, outputting silence";
        }
      }

      return noErr;
    }

    int
    write_mic_data(const char *data, size_t size, uint16_t seq = 0) override {
      static int call_count = 0;
      if (++call_count % 100 == 1) {  // Log every 100 calls
        BOOST_LOG(info) << "write_mic_data called: size=" << size << " seq=" << seq;
      }

      if (!mic_initialized || !audio_unit || !opus_decoder) {
        if (call_count % 100 == 1) {
          BOOST_LOG(warning) << "write_mic_data: not initialized";
        }
        return -1;
      }

      // 1. Decode OPUS data to PCM int16 mono
      int frame_size = opus_decoder_get_nb_samples(opus_decoder, (const unsigned char *)data, size);
      if (frame_size < 0) {
        BOOST_LOG(error) << "Failed to get OPUS frame size: " << opus_strerror(frame_size);
        return -1;
      }

      std::vector<int16_t> pcm_mono(frame_size);
      int samples_decoded = opus_decode(opus_decoder,
                                       (const unsigned char *)data,
                                       size,
                                       pcm_mono.data(),
                                       frame_size,
                                       0);  // Normal decode
      if (samples_decoded < 0) {
        BOOST_LOG(error) << "Failed to decode OPUS: " << opus_strerror(samples_decoded);
        return -1;
      }

      if (call_count % 100 == 1) {
        BOOST_LOG(info) << "Decoded " << samples_decoded << " samples from OPUS";
      }

      // 2. Convert mono int16 → stereo float32 interleaved
      std::vector<float> stereo_float(samples_decoded * 2);
      for (int i = 0; i < samples_decoded; i++) {
        float sample = pcm_mono[i] / 32768.0f;
        stereo_float[i * 2] = sample;      // Left
        stereo_float[i * 2 + 1] = sample;  // Right (duplicate mono)
      }

      // 3. Write to ring buffer
      uint32_t bytesToWrite = samples_decoded * 2 * sizeof(float);
      bool success = TPCircularBufferProduceBytes(&ring_buffer, stereo_float.data(), bytesToWrite);

      if (!success) {
        BOOST_LOG(warning) << "Ring buffer full, dropping audio frame";
        return -1;
      }

      if (call_count % 100 == 1) {
        uint32_t available = 0;
        TPCircularBufferTail(&ring_buffer, &available);
        BOOST_LOG(info) << "Wrote " << bytesToWrite << " bytes to ring buffer, available: " << available;
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

      // 2. Create OPUS decoder (48kHz, mono)
      int opus_error = 0;
      opus_decoder = opus_decoder_create(48000, 1, &opus_error);
      if (opus_error != OPUS_OK || !opus_decoder) {
        BOOST_LOG(error) << "Failed to create OPUS decoder: " << opus_strerror(opus_error);
        return -1;
      }
      BOOST_LOG(info) << "OPUS decoder created successfully";

      // 3. Initialize ring buffer (1 second of audio = 48000 samples * 2 channels * 4 bytes)
      bool bufferInit = TPCircularBufferInit(&ring_buffer, 48000 * 2 * sizeof(float));
      if (!bufferInit) {
        BOOST_LOG(error) << "Failed to initialize ring buffer";
        opus_decoder_destroy(opus_decoder);
        opus_decoder = nullptr;
        return -1;
      }
      BOOST_LOG(info) << "Ring buffer initialized (1 second capacity)";

      // 4. Find BlackHole device ID
      blackhole_device_id = find_blackhole_device_id(deviceName);
      if (blackhole_device_id == kAudioDeviceUnknown) {
        BOOST_LOG(error) << "Failed to find BlackHole device: " << [deviceName UTF8String];
        BOOST_LOG(error) << "Please install BlackHole: brew install blackhole-2ch";
        opus_decoder_destroy(opus_decoder);
        opus_decoder = nullptr;
        return -1;
      }

      // 5. Configure audio format (48kHz, 2ch, float32, NON-interleaved)
      // HAL Output AudioUnit expects non-interleaved format
      audio_format.mSampleRate = 48000.0;
      audio_format.mFormatID = kAudioFormatLinearPCM;
      audio_format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
      audio_format.mChannelsPerFrame = 2;
      audio_format.mBitsPerChannel = 32;
      audio_format.mBytesPerFrame = sizeof(float);  // Per channel, not total
      audio_format.mFramesPerPacket = 1;
      audio_format.mBytesPerPacket = audio_format.mBytesPerFrame * audio_format.mFramesPerPacket;
      audio_format.mReserved = 0;

      BOOST_LOG(info) << "Audio format: " << audio_format.mSampleRate << "Hz, "
                      << audio_format.mChannelsPerFrame << "ch, "
                      << audio_format.mBitsPerChannel << "bit, "
                      << "non-interleaved float";

      // 6. Create HAL Output AudioUnit
      AudioComponentDescription desc;
      desc.componentType = kAudioUnitType_Output;
      desc.componentSubType = kAudioUnitSubType_HALOutput;  // macOS HAL output
      desc.componentManufacturer = kAudioUnitManufacturer_Apple;
      desc.componentFlags = 0;
      desc.componentFlagsMask = 0;

      AudioComponent component = AudioComponentFindNext(NULL, &desc);
      if (!component) {
        BOOST_LOG(error) << "Failed to find HAL Output AudioUnit component";
        return -1;
      }

      OSStatus status = AudioComponentInstanceNew(component, &audio_unit);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to create AudioUnit: " << status;
        return -1;
      }

      BOOST_LOG(info) << "Created HAL Output AudioUnit";

      // 6. Enable output on the AudioUnit
      UInt32 enableIO = 1;
      status = AudioUnitSetProperty(audio_unit,
                                    kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Output,
                                    0,
                                    &enableIO,
                                    sizeof(enableIO));
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to enable AudioUnit output: " << status;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        return -1;
      }

      BOOST_LOG(info) << "Enabled AudioUnit output";

      // 7. Set output device to BlackHole using AudioDeviceID
      status = AudioUnitSetProperty(audio_unit,
                                    kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global,
                                    0,
                                    &blackhole_device_id,
                                    sizeof(blackhole_device_id));
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to set AudioUnit output device: " << status;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        return -1;
      }

      BOOST_LOG(info) << "Successfully set AudioUnit output device to: " << [deviceName UTF8String];

      // 7. Set audio format on AudioUnit input scope (data we provide)
      status = AudioUnitSetProperty(audio_unit,
                                    kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Input,
                                    0,
                                    &audio_format,
                                    sizeof(audio_format));
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to set AudioUnit input format: " << status;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        return -1;
      }

      BOOST_LOG(info) << "Set AudioUnit input format successfully";

      // Also set output format to match
      status = AudioUnitSetProperty(audio_unit,
                                    kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output,
                                    0,
                                    &audio_format,
                                    sizeof(audio_format));
      if (status != noErr) {
        BOOST_LOG(warning) << "Failed to set AudioUnit output format: " << status << " (may be OK)";
      } else {
        BOOST_LOG(info) << "Set AudioUnit output format successfully";
      }

      // 8. Set render callback
      AURenderCallbackStruct callbackStruct;
      callbackStruct.inputProc = audio_unit_render_callback;
      callbackStruct.inputProcRefCon = this;

      status = AudioUnitSetProperty(audio_unit,
                                    kAudioUnitProperty_SetRenderCallback,
                                    kAudioUnitScope_Input,
                                    0,
                                    &callbackStruct,
                                    sizeof(callbackStruct));
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to set AudioUnit render callback: " << status;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        return -1;
      }

      // 9. Initialize AudioUnit
      status = AudioUnitInitialize(audio_unit);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to initialize AudioUnit: " << status;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        return -1;
      }

      // Verify the device is still set after initialization
      AudioDeviceID verifyDeviceID = 0;
      UInt32 verifySize = sizeof(verifyDeviceID);
      status = AudioUnitGetProperty(audio_unit,
                                    kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global,
                                    0,
                                    &verifyDeviceID,
                                    &verifySize);
      if (status == noErr) {
        BOOST_LOG(info) << "Verified AudioUnit device ID after init: " << verifyDeviceID << " (expected: " << blackhole_device_id << ")";
        if (verifyDeviceID != blackhole_device_id) {
          BOOST_LOG(error) << "Device ID mismatch! AudioUnit is using wrong device!";
        }
      }

      // 10. Start AudioUnit
      status = AudioOutputUnitStart(audio_unit);
      if (status != noErr) {
        BOOST_LOG(error) << "Failed to start AudioUnit: " << status;
        AudioUnitUninitialize(audio_unit);
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        return -1;
      }

      mic_initialized = true;
      BOOST_LOG(info) << "Remote microphone initialized successfully with AudioUnit";
      BOOST_LOG(info) << "AudioUnit started, render callback will read from ring buffer";
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

      if (audio_unit) {
        // Stop AudioUnit
        AudioOutputUnitStop(audio_unit);

        // Uninitialize
        AudioUnitUninitialize(audio_unit);

        // Dispose
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
      }

      blackhole_device_id = kAudioDeviceUnknown;
      mic_initialized = false;

      // Destroy OPUS decoder
      if (opus_decoder) {
        opus_decoder_destroy(opus_decoder);
        opus_decoder = nullptr;
      }

      // Cleanup ring buffer
      TPCircularBufferCleanup(&ring_buffer);

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
