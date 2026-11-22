/**
 * @file src/platform/macos/microphone.mm
 * @brief Definitions for microphone capture on macOS.
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

  /**
   * @brief Plays the client's microphone into a virtual audio device, so that host
   *        applications can record it from that device's input side.
   *
   * write_pcm() is the only producer and the device's IO thread the only consumer,
   * which is the one-reader-one-writer case TPCircularBuffer is safe for without a lock.
   */
  struct mic_redirect_t {
    AudioComponentInstance unit {};
    TPCircularBuffer ring {};
    UInt32 channels { 2 };

    ~mic_redirect_t() {
      if (unit) {
        AudioOutputUnitStop(unit);
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        TPCircularBufferCleanup(&ring);
      }
    }

    static std::string
    string_property(AudioDeviceID device, AudioObjectPropertySelector selector) {
      AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
      CFStringRef value = nullptr;
      UInt32 size = sizeof(value);
      char text[256] {};
      if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) == noErr && value) {
        CFStringGetCString(value, text, sizeof(text), kCFStringEncodingUTF8);
        CFRelease(value);
      }
      return text;
    }

    /// The output device named by `virtual_sink` (name or UID); without that setting, the first virtual one.
    static AudioDeviceID
    find_device(const std::string &wanted) {
      AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
      UInt32 size = 0;
      if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) {
        return kAudioObjectUnknown;
      }
      std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
      if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, devices.data()) != noErr) {
        return kAudioObjectUnknown;
      }

      for (auto device : devices) {
        AudioObjectPropertyAddress streams { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        UInt32 streams_size = 0;
        if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &streams_size) != noErr || streams_size == 0) {
          continue;  // no output side
        }

        if (!wanted.empty()) {
          if (string_property(device, kAudioObjectPropertyName) == wanted || string_property(device, kAudioDevicePropertyDeviceUID) == wanted) {
            return device;
          }
          continue;
        }

        AudioObjectPropertyAddress transport_address { kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 transport = 0;
        UInt32 transport_size = sizeof(transport);
        if (AudioObjectGetPropertyData(device, &transport_address, 0, nullptr, &transport_size, &transport) == noErr &&
            transport == kAudioDeviceTransportTypeVirtual) {
          return device;
        }
      }
      return kAudioObjectUnknown;
    }

    static OSStatus
    render(void *self_ptr, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *data) {
      auto *self = static_cast<mic_redirect_t *>(self_ptr);
      auto &out = data->mBuffers[0];

      uint32_t available = 0;
      void *tail = TPCircularBufferTail(&self->ring, &available);
      const auto copied = std::min<uint32_t>(available, out.mDataByteSize);
      if (copied) {
        std::memcpy(out.mData, tail, copied);
        TPCircularBufferConsume(&self->ring, copied);
      }
      std::memset(static_cast<char *>(out.mData) + copied, 0, out.mDataByteSize - copied);  // silence when the client sends nothing
      return noErr;
    }

    static std::unique_ptr<mic_redirect_t>
    open(const std::string &wanted) {
      auto device = find_device(wanted);
      if (device == kAudioObjectUnknown) {
        BOOST_LOG(warning) << "Microphone redirect: no "sv << (wanted.empty() ? "virtual audio output device"s : "output device named ["s + wanted + ']')
                           << " found. Install a virtual audio driver, or set 'virtual_sink'."sv;
        return nullptr;
      }

      AudioComponentDescription description { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple };
      auto self = std::make_unique<mic_redirect_t>();
      if (AudioComponentInstanceNew(AudioComponentFindNext(nullptr, &description), &self->unit) != noErr) {
        self->unit = nullptr;
        return nullptr;
      }
      TPCircularBufferInit(&self->ring, 128 * 1024);  // about a third of a second of 48 kHz stereo float

      AudioStreamBasicDescription format {};
      UInt32 size = sizeof(format);
      AURenderCallbackStruct callback { render, self.get() };
      bool ok = AudioUnitSetProperty(self->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device)) == noErr;
      if (ok && AudioUnitGetProperty(self->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size) == noErr && format.mChannelsPerFrame) {
        self->channels = format.mChannelsPerFrame;
      }
      format = {};
      format.mSampleRate = 48000;
      format.mFormatID = kAudioFormatLinearPCM;
      format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
      format.mChannelsPerFrame = self->channels;
      format.mBitsPerChannel = 32;
      format.mFramesPerPacket = 1;
      format.mBytesPerFrame = format.mBytesPerPacket = self->channels * sizeof(float);
      ok = ok && AudioUnitSetProperty(self->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)) == noErr &&
           AudioUnitSetProperty(self->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) == noErr &&
           AudioUnitInitialize(self->unit) == noErr &&
           AudioOutputUnitStart(self->unit) == noErr;
      if (!ok) {
        BOOST_LOG(error) << "Microphone redirect: couldn't open ["sv << string_property(device, kAudioObjectPropertyName) << ']';
        return nullptr;
      }

      BOOST_LOG(info) << "Microphone redirect: client microphone plays into ["sv << string_property(device, kAudioObjectPropertyName) << ']';
      return self;
    }

    /// @param samples Mono 48 kHz samples, copied to every channel of the device.
    int
    write_pcm(const std::int16_t *samples, std::size_t frame_count) {
      std::vector<float> out(frame_count * channels);
      for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = samples[i / channels] / 32768.0f;
      }
      TPCircularBufferProduceBytes(&ring, out.data(), out.size() * sizeof(float));  // a full ring drops the block: late audio is worse than lost audio
      return static_cast<int>(frame_count * sizeof(std::int16_t));
    }
  };

  struct macos_audio_control_t: public audio_control_t {
    AVCaptureDevice *audio_capture_device {};
    std::unique_ptr<mic_redirect_t> mic_redirect;

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
      BOOST_LOG(warning) << "audio_control_t::is_sink_available() unimplemented: "sv << sink;
      return true;
    }

    int
    write_mic_pcm(const std::int16_t *samples, std::size_t frame_count) override {
      return mic_redirect ? mic_redirect->write_pcm(samples, frame_count) : -1;
    }

    int
    init_mic_redirect_device() override {
      if (!mic_redirect) {
        mic_redirect = mic_redirect_t::open(config::audio.virtual_sink);
      }
      return mic_redirect ? 0 : -1;
    }

    void
    release_mic_redirect_device() override {
      mic_redirect.reset();
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
