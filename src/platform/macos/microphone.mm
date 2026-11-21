/**
 * @file src/platform/macos/microphone.mm
 * @brief Definitions for microphone capture and virtual mic redirect on macOS.
 */
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cstring>
#include <vector>

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
        [av_audio_capture.samplesArrivedSignal lock];
        BOOL arrived = [av_audio_capture.samplesArrivedSignal waitUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        [av_audio_capture.samplesArrivedSignal unlock];
        if (!arrived) {
          return capture_e::timeout;
        }
        byteSampleBuffer = TPCircularBufferTail(&av_audio_capture->audioSampleBuffer, &length);
      }

      const float *sampleBuffer = (float *) byteSampleBuffer;
      std::vector<float> vectorBuffer(sampleBuffer, sampleBuffer + sample_size);

      std::copy_n(std::begin(vectorBuffer), sample_size, std::begin(sample_in));

      TPCircularBufferConsume(&av_audio_capture->audioSampleBuffer, sample_size * sizeof(float));

      return capture_e::ok;
    }
  };

  /**
   * @brief Manages routing decoded client microphone PCM into a macOS virtual audio loopback device.
   */
  class macos_mic_redirect_sink_t {
  public:
    AudioComponentInstance audio_unit { nullptr };
    TPCircularBuffer ring_buffer {};
    bool is_initialized { false };
    bool is_started { false };
    UInt32 device_channels { 2 };
    std::string device_name;

    static OSStatus
    render_callback(void *inRefCon,
                    AudioUnitRenderActionFlags *ioActionFlags,
                    const AudioTimeStamp *inTimeStamp,
                    UInt32 inBusNumber,
                    UInt32 inNumberFrames,
                    AudioBufferList *ioData) {
      auto *self = static_cast<macos_mic_redirect_sink_t *>(inRefCon);
      if (!self || !ioData || ioData->mNumberBuffers == 0) {
        return noErr;
      }

      auto &buffer = ioData->mBuffers[0];
      const UInt32 requested_bytes = inNumberFrames * self->device_channels * sizeof(float);
      const UInt32 bytes_to_copy = std::min(requested_bytes, buffer.mDataByteSize);

      uint32_t available_bytes = 0;
      void *tail = TPCircularBufferTail(&self->ring_buffer, &available_bytes);

      if (tail && available_bytes >= bytes_to_copy) {
        std::memcpy(buffer.mData, tail, bytes_to_copy);
        TPCircularBufferConsume(&self->ring_buffer, bytes_to_copy);
      }
      else if (tail && available_bytes > 0) {
        std::memcpy(buffer.mData, tail, available_bytes);
        std::memset(static_cast<char *>(buffer.mData) + available_bytes, 0, bytes_to_copy - available_bytes);
        TPCircularBufferConsume(&self->ring_buffer, available_bytes);
      }
      else {
        std::memset(buffer.mData, 0, bytes_to_copy);
      }

      return noErr;
    }

    static AudioDeviceID
    find_target_output_device(const std::string &preferred_name) {
      AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      UInt32 size = 0;
      if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return kAudioObjectUnknown;
      }

      int count = size / sizeof(AudioDeviceID);
      std::vector<AudioDeviceID> devices(count);
      if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data()) != noErr) {
        return kAudioObjectUnknown;
      }

      auto check_device = [](AudioDeviceID dev, std::string &name_out, std::string &uid_out) -> bool {
        CFStringRef name = nullptr;
        UInt32 nameSize = sizeof(name);
        AudioObjectPropertyAddress nameAddr = {
          kAudioObjectPropertyName,
          kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(dev, &nameAddr, 0, nullptr, &nameSize, &name) == noErr && name) {
          char buf[256] = {0};
          CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
          name_out = buf;
          CFRelease(name);
        }

        CFStringRef uid = nullptr;
        UInt32 uidSize = sizeof(uid);
        AudioObjectPropertyAddress uidAddr = {
          kAudioDevicePropertyDeviceUID,
          kAudioObjectPropertyScopeGlobal,
          kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(dev, &uidAddr, 0, nullptr, &uidSize, &uid) == noErr && uid) {
          char buf[256] = {0};
          CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
          uid_out = buf;
          CFRelease(uid);
        }

        AudioObjectPropertyAddress outAddr = {
          kAudioDevicePropertyStreamConfiguration,
          kAudioObjectPropertyScopeOutput,
          kAudioObjectPropertyElementMain
        };
        UInt32 outSize = 0;
        AudioObjectGetPropertyDataSize(dev, &outAddr, 0, nullptr, &outSize);
        return outSize > 0;
      };

      AudioDeviceID fallback_dev = kAudioObjectUnknown;

      for (auto dev : devices) {
        std::string name, uid;
        if (!check_device(dev, name, uid)) continue;

        if (!preferred_name.empty()) {
          if (name.find(preferred_name) != std::string::npos || uid == preferred_name) {
            return dev;
          }
        }

        // Auto-detect virtual loopback audio sinks in priority order:
        if (name.find("BlackHole") != std::string::npos || uid.find("BlackHole") != std::string::npos) {
          return dev;
        }
        if (name.find("Microsoft Teams Audio") != std::string::npos || uid == "MSLoopbackDriverDevice_UID") {
          fallback_dev = dev;
        }
        if (fallback_dev == kAudioObjectUnknown && (name.find("LoomAudioDevice") != std::string::npos || name.find("MJAudioRecorder") != std::string::npos)) {
          fallback_dev = dev;
        }
      }

      return fallback_dev;
    }

    int
    init(const std::string &preferred_device) {
      if (is_initialized) return 0;

      AudioDeviceID target_dev = find_target_output_device(preferred_device);
      if (target_dev == kAudioObjectUnknown) {
        BOOST_LOG(warning) << "[mic_redirect] No compatible virtual audio loopback device found on macOS (looked for BlackHole, Teams Audio, Loom, etc.)"sv;
        return -1;
      }

      CFStringRef devName = nullptr;
      UInt32 devNameSize = sizeof(devName);
      AudioObjectPropertyAddress nameAddr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
      };
      if (AudioObjectGetPropertyData(target_dev, &nameAddr, 0, nullptr, &devNameSize, &devName) == noErr && devName) {
        char buf[256] = {0};
        CFStringGetCString(devName, buf, sizeof(buf), kCFStringEncodingUTF8);
        device_name = buf;
        CFRelease(devName);
      }

      BOOST_LOG(info) << "[mic_redirect] Directing client microphone audio into virtual device: ["sv << device_name << "] (DeviceID="sv << target_dev << ")"sv;

      // 128 KB ring buffer ~ 340ms of 48kHz stereo float32 PCM
      if (!TPCircularBufferInit(&ring_buffer, 128 * 1024)) {
        BOOST_LOG(error) << "[mic_redirect] Failed to allocate TPCircularBuffer for microphone"sv;
        return -1;
      }

      AudioComponentDescription desc {};
      desc.componentType = kAudioUnitType_Output;
      desc.componentSubType = kAudioUnitSubType_HALOutput;
      desc.componentManufacturer = kAudioUnitManufacturer_Apple;

      AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
      if (!comp) {
        BOOST_LOG(error) << "[mic_redirect] HALOutput AudioComponent not found"sv;
        TPCircularBufferCleanup(&ring_buffer);
        return -1;
      }

      if (AudioComponentInstanceNew(comp, &audio_unit) != noErr || !audio_unit) {
        BOOST_LOG(error) << "[mic_redirect] AudioComponentInstanceNew failed"sv;
        TPCircularBufferCleanup(&ring_buffer);
        return -1;
      }

      UInt32 enableIO = 1;
      AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
      UInt32 disableIO = 0;
      AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disableIO, sizeof(disableIO));

      if (AudioUnitSetProperty(audio_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &target_dev, sizeof(target_dev)) != noErr) {
        BOOST_LOG(error) << "[mic_redirect] Failed to assign target audio device to HALOutput"sv;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        TPCircularBufferCleanup(&ring_buffer);
        return -1;
      }

      AudioStreamBasicDescription devFormat {};
      UInt32 formatSize = sizeof(devFormat);
      if (AudioUnitGetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &devFormat, &formatSize) == noErr) {
        device_channels = devFormat.mChannelsPerFrame > 0 ? devFormat.mChannelsPerFrame : 2;
      }
      else {
        device_channels = 2;
      }

      AudioStreamBasicDescription streamFormat {};
      streamFormat.mSampleRate = 48000.0;
      streamFormat.mFormatID = kAudioFormatLinearPCM;
      streamFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
      streamFormat.mChannelsPerFrame = device_channels;
      streamFormat.mBitsPerChannel = 32;
      streamFormat.mBytesPerFrame = device_channels * sizeof(float);
      streamFormat.mFramesPerPacket = 1;
      streamFormat.mBytesPerPacket = streamFormat.mBytesPerFrame;

      AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &streamFormat, sizeof(streamFormat));

      AURenderCallbackStruct callback {};
      callback.inputProc = render_callback;
      callback.inputProcRefCon = this;
      AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));

      if (AudioUnitInitialize(audio_unit) != noErr) {
        BOOST_LOG(error) << "[mic_redirect] AudioUnitInitialize failed"sv;
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        TPCircularBufferCleanup(&ring_buffer);
        return -1;
      }

      if (AudioOutputUnitStart(audio_unit) != noErr) {
        BOOST_LOG(error) << "[mic_redirect] AudioOutputUnitStart failed"sv;
        AudioUnitUninitialize(audio_unit);
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
        TPCircularBufferCleanup(&ring_buffer);
        return -1;
      }

      is_initialized = true;
      is_started = true;
      BOOST_LOG(info) << "[mic_redirect] Active microphone pipeline initialized: 48kHz, "sv << device_channels << "ch into ["sv << device_name << "]"sv;
      return 0;
    }

    int
    write_pcm(const std::int16_t *samples, std::size_t frame_count) {
      if (!is_started || !samples || frame_count == 0) return -1;

      constexpr float scale = 1.0f / 32768.0f;
      if (device_channels == 1) {
        std::vector<float> mono(frame_count);
        for (std::size_t i = 0; i < frame_count; ++i) {
          mono[i] = static_cast<float>(samples[i]) * scale;
        }
        TPCircularBufferProduceBytes(&ring_buffer, mono.data(), mono.size() * sizeof(float));
      }
      else {
        std::vector<float> stereo(frame_count * 2);
        for (std::size_t i = 0; i < frame_count; ++i) {
          float val = static_cast<float>(samples[i]) * scale;
          stereo[i * 2] = val;
          stereo[i * 2 + 1] = val;
        }
        TPCircularBufferProduceBytes(&ring_buffer, stereo.data(), stereo.size() * sizeof(float));
      }
      return static_cast<int>(frame_count * sizeof(std::int16_t));
    }

    void
    release() {
      if (!is_initialized) return;

      if (audio_unit) {
        if (is_started) {
          AudioOutputUnitStop(audio_unit);
          is_started = false;
        }
        AudioUnitUninitialize(audio_unit);
        AudioComponentInstanceDispose(audio_unit);
        audio_unit = nullptr;
      }

      TPCircularBufferCleanup(&ring_buffer);
      is_initialized = false;
      BOOST_LOG(info) << "[mic_redirect] Released microphone audio pipeline"sv;
    }
  };

  struct macos_audio_control_t: public audio_control_t {
    AVCaptureDevice *audio_capture_device {};
    std::unique_ptr<macos_mic_redirect_sink_t> mic_sink_device;

  public:
    int
    set_sink(const std::string &sink) override {
      BOOST_LOG(warning) << "audio_control_t::set_sink() unimplemented: "sv << sink;
      return 0;
    }

    std::unique_ptr<mic_t>
    microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio) override {
      auto mic = std::make_unique<av_mic_t>();
      const char *audio_sink = "";

      if (!config::audio.sink.empty()) {
        audio_sink = config::audio.sink.c_str();
        audio_capture_device = [AVAudio findMicrophone:[NSString stringWithUTF8String:audio_sink]];
      }
      else {
        NSArray<AVCaptureDevice *> *mics = [AVAudio microphones];
        if (mics && mics.count > 0) {
          audio_capture_device = mics[0];
          BOOST_LOG(info) << "Defaulting host audio capture to: ["sv << [audio_capture_device.localizedName UTF8String] << "]"sv;
        }
      }

      if (audio_capture_device == nullptr) {
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
      BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
      return true;
    }

    int
    write_mic_pcm(const std::int16_t *samples, std::size_t frame_count) override {
      if (mic_sink_device) {
        return mic_sink_device->write_pcm(samples, frame_count);
      }
      return -1;
    }

    int
    init_mic_redirect_device() override {
      if (!mic_sink_device) {
        mic_sink_device = std::make_unique<macos_mic_redirect_sink_t>();
      }
      return mic_sink_device->init("");
    }

    void
    release_mic_redirect_device() override {
      if (mic_sink_device) {
        mic_sink_device->release();
        mic_sink_device.reset();
      }
    }
  };

  std::unique_ptr<audio_control_t>
  audio_control() {
    return std::make_unique<macos_audio_control_t>();
  }

  std::unique_ptr<deinit_t>
  init_audio_thread() {
    return std::make_unique<deinit_t>();
  }
}  // namespace platf
