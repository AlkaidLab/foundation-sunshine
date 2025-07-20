/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// platform includes
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mutex>
#include <newdev.h>
#include <roapi.h>
#include <synchapi.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"

// lib includes
#include <opus/opus.h>

// Must be the last included file
// clang-format off
#include "PolicyConfig.h"
// clang-format on

DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);

#if defined(__x86_64) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#elif defined(__i386) || defined(_M_IX86)
  #define STEAM_DRIVER_SUBDIR L"x86"
#else
  #warning No known Steam audio driver for this architecture
#endif

namespace {

  enum class match_field_e {
    device_id,  ///< Match device_id
    device_friendly_name,  ///< Match endpoint friendly name
    adapter_friendly_name,  ///< Match adapter friendly name
    device_description,  ///< Match endpoint description
  };

  using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
  using matched_field_t = std::pair<match_field_e, std::wstring>;

  constexpr auto SAMPLE_RATE = 48000;
  constexpr auto STEAM_AUDIO_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";

  constexpr auto waveformat_mask_stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  constexpr auto waveformat_mask_surround51_with_backspeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

  constexpr auto waveformat_mask_surround51_with_sidespeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  constexpr auto waveformat_mask_surround71 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                              SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                                              SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  enum class sample_format_e {
    f32,
    s32,
    s24in32,
    s24,
    s16,
    _size,
  };

  constexpr WAVEFORMATEXTENSIBLE
  create_waveformat(sample_format_e sample_format, WORD channel_count, DWORD channel_mask) {
    WAVEFORMATEXTENSIBLE waveformat = {};

    switch (sample_format) {
      default:
      case sample_format_e::f32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s24in32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s24:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 24;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s16:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 16;
        waveformat.Samples.wValidBitsPerSample = 16;
        break;
    }

    static_assert((int) sample_format_e::_size == 5, "Unrecognized sample_format_e");

    waveformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveformat.Format.nChannels = channel_count;
    waveformat.Format.nSamplesPerSec = SAMPLE_RATE;

    waveformat.Format.nBlockAlign = waveformat.Format.nChannels * waveformat.Format.wBitsPerSample / 8;
    waveformat.Format.nAvgBytesPerSec = waveformat.Format.nSamplesPerSec * waveformat.Format.nBlockAlign;
    waveformat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    waveformat.dwChannelMask = channel_mask;

    return waveformat;
  }

  using virtual_sink_waveformats_t = std::vector<WAVEFORMATEXTENSIBLE>;

  /**
   * @brief List of supported waveformats for an N-channel virtual audio device
   * @tparam channel_count Number of virtual audio channels
   * @returns std::vector<WAVEFORMATEXTENSIBLE>
   * @note The list of virtual formats returned are sorted in preference order and the first valid
   *       format will be used. All bits-per-sample options are listed because we try to match
   *       this to the default audio device. See also: set_format() below.
   */
  template <WORD channel_count>
  virtual_sink_waveformats_t
  create_virtual_sink_waveformats() {
    if constexpr (channel_count == 2) {
      auto channel_mask = waveformat_mask_stereo;
      // The 32-bit formats are a lower priority for stereo because using one will disable Dolby/DTS
      // spatial audio mode if the user enabled it on the Steam speaker.
      return {
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
      };
    }
    else if (channel_count == 6) {
      auto channel_mask1 = waveformat_mask_surround51_with_backspeakers;
      auto channel_mask2 = waveformat_mask_surround51_with_sidespeakers;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask2),
      };
    }
    else if (channel_count == 8) {
      auto channel_mask = waveformat_mask_surround71;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
      };
    }
  }

  std::string
  waveformat_to_pretty_string(const WAVEFORMATEXTENSIBLE &waveformat) {
    std::string result = waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? "F" :
                         waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_PCM        ? "S" :
                                                                                   "UNKNOWN";

    result += std::to_string(waveformat.Samples.wValidBitsPerSample) + " " +
              std::to_string(waveformat.Format.nSamplesPerSec) + " ";

    switch (waveformat.dwChannelMask) {
      case (waveformat_mask_stereo):
        result += "2.0";
        break;

      case (waveformat_mask_surround51_with_backspeakers):
        result += "5.1";
        break;

      case (waveformat_mask_surround51_with_sidespeakers):
        result += "5.1 (sidespeakers)";
        break;

      case (waveformat_mask_surround71):
        result += "7.1";
        break;

      default:
        result += std::to_string(waveformat.Format.nChannels) + " channels (unrecognized)";
        break;
    }

    return result;
  }

}  // namespace

using namespace std::literals;

namespace platf::audio {
  template <class T>
  void
  Release(T *p) {
    p->Release();
  }

  template <class T>
  void
  co_task_free(T *p) {
    CoTaskMemFree((LPVOID) p);
  }

  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>>;
  using device_t = util::safe_ptr<IMMDevice, Release<IMMDevice>>;
  using collection_t = util::safe_ptr<IMMDeviceCollection, Release<IMMDeviceCollection>>;
  using audio_client_t = util::safe_ptr<IAudioClient, Release<IAudioClient>>;
  using audio_capture_t = util::safe_ptr<IAudioCaptureClient, Release<IAudioCaptureClient>>;
  using wave_format_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;
  using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
  using handle_t = util::safe_ptr_v2<void, BOOL, CloseHandle>;
  using policy_t = util::safe_ptr<IPolicyConfig, Release<IPolicyConfig>>;
  using prop_t = util::safe_ptr<IPropertyStore, Release<IPropertyStore>>;

  class co_init_t: public deinit_t {
  public:
    co_init_t() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    }

    ~co_init_t() override {
      CoUninitialize();
    }
  };

  class prop_var_t {
  public:
    prop_var_t() {
      PropVariantInit(&prop);
    }

    ~prop_var_t() {
      PropVariantClear(&prop);
    }

    PROPVARIANT prop;
  };

  struct format_t {
    WORD channel_count;
    std::string name;
    int capture_waveformat_channel_mask;
    virtual_sink_waveformats_t virtual_sink_waveformats;
  };

  const std::array<const format_t, 3> formats = {
    format_t {
      2,
      "Stereo",
      waveformat_mask_stereo,
      create_virtual_sink_waveformats<2>(),
    },
    format_t {
      6,
      "Surround 5.1",
      waveformat_mask_surround51_with_backspeakers,
      create_virtual_sink_waveformats<6>(),
    },
    format_t {
      8,
      "Surround 7.1",
      waveformat_mask_surround71,
      create_virtual_sink_waveformats<8>(),
    },
  };

  audio_client_t
  make_audio_client(device_t &device, const format_t &format) {
    audio_client_t audio_client;
    auto status = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      (void **) &audio_client);

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't activate Device: [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    WAVEFORMATEXTENSIBLE capture_waveformat =
      create_waveformat(sample_format_e::f32, format.channel_count, format.capture_waveformat_channel_mask);

    {
      wave_format_t mixer_waveformat;
      status = audio_client->GetMixFormat(&mixer_waveformat);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't get mix format for audio device: [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      // Prefer the native channel layout of captured audio device when channel counts match
      if (mixer_waveformat->nChannels == format.channel_count &&
          mixer_waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          mixer_waveformat->cbSize >= 22) {
        auto waveformatext_pointer = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mixer_waveformat.get());
        capture_waveformat.dwChannelMask = waveformatext_pointer->dwChannelMask;
      }

      BOOST_LOG(info) << "Audio mixer format is "sv << mixer_waveformat->wBitsPerSample << "-bit, "sv
                      << mixer_waveformat->nSamplesPerSec << " Hz, "sv
                      << ((mixer_waveformat->nSamplesPerSec != 48000) ? "will be resampled to 48000 by Windows"sv : "no resampling needed"sv);
    }

    status = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,  // Enable automatic resampling to 48 KHz
      0,
      0,
      (LPWAVEFORMATEX) &capture_waveformat,
      nullptr);

    if (status) {
      BOOST_LOG(error) << "Couldn't initialize audio client for ["sv << format.name << "]: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    BOOST_LOG(info) << "Audio capture format is "sv << logging::bracket(waveformat_to_pretty_string(capture_waveformat));

    return audio_client;
  }

  device_t
  default_device(device_enum_t &device_enum) {
    device_t device;
    HRESULT status;
    status = device_enum->GetDefaultAudioEndpoint(
      eRender,
      eConsole,
      &device);

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get default audio endpoint [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return device;
  }

  class audio_notification_t: public ::IMMNotificationClient {
  public:
    audio_notification_t() {
    }

    // IUnknown implementation (unused by IMMDeviceEnumerator)
    ULONG STDMETHODCALLTYPE
    AddRef() {
      return 1;
    }

    ULONG STDMETHODCALLTYPE
    Release() {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE
    QueryInterface(REFIID riid, VOID **ppvInterface) {
      if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *) this;
        return S_OK;
      }
      else if (__uuidof(IMMNotificationClient) == riid) {
        AddRef();
        *ppvInterface = (IMMNotificationClient *) this;
        return S_OK;
      }
      else {
        *ppvInterface = NULL;
        return E_NOINTERFACE;
      }
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE
    OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) {
      if (flow == eRender) {
        default_render_device_changed_flag.store(true);
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    OnDeviceAdded(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    OnDeviceStateChanged(
      LPCWSTR pwstrDeviceId,
      DWORD dwNewState) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    OnPropertyValueChanged(
      LPCWSTR pwstrDeviceId,
      const PROPERTYKEY key) {
      return S_OK;
    }

    /**
     * @brief Checks if the default rendering device changed and resets the change flag
     * @return `true` if the device changed since last call
     */
    bool
    check_default_render_device_changed() {
      return default_render_device_changed_flag.exchange(false);
    }

  private:
    std::atomic_bool default_render_device_changed_flag;
  };

  class mic_wasapi_t: public mic_t {
  public:
    capture_e
    sample(std::vector<float> &sample_out) override {
      auto sample_size = sample_out.size();

      // Refill the sample buffer if needed
      while (sample_buf_pos - std::begin(sample_buf) < sample_size) {
        auto capture_result = _fill_buffer();
        if (capture_result != capture_e::ok) {
          return capture_result;
        }
      }

      // Fill the output buffer with samples
      std::copy_n(std::begin(sample_buf), sample_size, std::begin(sample_out));

      // Move any excess samples to the front of the buffer
      std::move(&sample_buf[sample_size], sample_buf_pos, std::begin(sample_buf));
      sample_buf_pos -= sample_size;

      return capture_e::ok;
    }

    int
    init(std::uint32_t sample_rate, std::uint32_t frame_size, std::uint32_t channels_out) {
      audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      if (!audio_event) {
        BOOST_LOG(error) << "Couldn't create Event handle"sv;

        return -1;
      }

      HRESULT status;

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = device_enum->RegisterEndpointNotificationCallback(&endpt_notification);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't register endpoint notification [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      auto device = default_device(device_enum);
      if (!device) {
        return -1;
      }

      for (const auto &format : formats) {
        if (format.channel_count != channels_out) {
          BOOST_LOG(debug) << "Skipping audio format ["sv << format.name << "] with channel count ["sv
                           << format.channel_count << " != "sv << channels_out << ']';
          continue;
        }

        BOOST_LOG(debug) << "Trying audio format ["sv << format.name << ']';
        audio_client = make_audio_client(device, format);

        if (audio_client) {
          BOOST_LOG(debug) << "Found audio format ["sv << format.name << ']';
          channels = channels_out;
          break;
        }
      }

      if (!audio_client) {
        BOOST_LOG(error) << "Couldn't find supported format for audio"sv;
        return -1;
      }

      REFERENCE_TIME default_latency;
      audio_client->GetDevicePeriod(&default_latency, nullptr);
      default_latency_ms = default_latency / 1000;

      std::uint32_t frames;
      status = audio_client->GetBufferSize(&frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't acquire the number of audio frames [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      // *2 --> needs to fit double
      sample_buf = util::buffer_t<float> { std::max(frames, frame_size) * 2 * channels_out };
      sample_buf_pos = std::begin(sample_buf);

      status = audio_client->GetService(IID_IAudioCaptureClient, (void **) &audio_capture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't initialize audio capture client [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = audio_client->SetEventHandle(audio_event.get());
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't set event handle [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      {
        DWORD task_index = 0;
        mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
        if (!mmcss_task_handle) {
          BOOST_LOG(error) << "Couldn't associate audio capture thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
        }
      }

      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't start recording [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      return 0;
    }

    ~mic_wasapi_t() override {
      if (device_enum) {
        device_enum->UnregisterEndpointNotificationCallback(&endpt_notification);
      }

      if (audio_client) {
        audio_client->Stop();
      }

      if (mmcss_task_handle) {
        AvRevertMmThreadCharacteristics(mmcss_task_handle);
      }
    }

  private:
    capture_e
    _fill_buffer() {
      HRESULT status;

      // Total number of samples
      struct sample_aligned_t {
        std::uint32_t uninitialized;
        float *samples;
      } sample_aligned;

      // number of samples / number of channels
      struct block_aligned_t {
        std::uint32_t audio_sample_size;
      } block_aligned;

      // Check if the default audio device has changed
      if (endpt_notification.check_default_render_device_changed()) {
        // Invoke the audio_control_t's callback if it wants one
        if (default_endpt_changed_cb) {
          (*default_endpt_changed_cb)();
        }

        // Reinitialize to pick up the new default device
        return capture_e::reinit;
      }

      status = WaitForSingleObjectEx(audio_event.get(), default_latency_ms, FALSE);
      switch (status) {
        case WAIT_OBJECT_0:
          break;
        case WAIT_TIMEOUT:
          return capture_e::timeout;
        default:
          BOOST_LOG(error) << "Couldn't wait for audio event: [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::error;
      }

      std::uint32_t packet_size {};
      for (
        status = audio_capture->GetNextPacketSize(&packet_size);
        SUCCEEDED(status) && packet_size > 0;
        status = audio_capture->GetNextPacketSize(&packet_size)) {
        DWORD buffer_flags;
        status = audio_capture->GetBuffer(
          (BYTE **) &sample_aligned.samples,
          &block_aligned.audio_sample_size,
          &buffer_flags,
          nullptr,
          nullptr);

        switch (status) {
          case S_OK:
            break;
          case AUDCLNT_E_DEVICE_INVALIDATED:
            return capture_e::reinit;
          default:
            BOOST_LOG(error) << "Couldn't capture audio [0x"sv << util::hex(status).to_string_view() << ']';
            return capture_e::error;
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          BOOST_LOG(debug) << "Audio capture signaled buffer discontinuity";
        }

        sample_aligned.uninitialized = std::end(sample_buf) - sample_buf_pos;
        auto n = std::min(sample_aligned.uninitialized, block_aligned.audio_sample_size * channels);

        if (n < block_aligned.audio_sample_size * channels) {
          BOOST_LOG(warning) << "Audio capture buffer overflow";
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill_n(sample_buf_pos, n, 0);
        }
        else {
          std::copy_n(sample_aligned.samples, n, sample_buf_pos);
        }

        sample_buf_pos += n;

        audio_capture->ReleaseBuffer(block_aligned.audio_sample_size);
      }

      if (status == AUDCLNT_E_DEVICE_INVALIDATED) {
        return capture_e::reinit;
      }

      if (FAILED(status)) {
        return capture_e::error;
      }

      return capture_e::ok;
    }

  public:
    handle_t audio_event;

    device_enum_t device_enum;
    device_t device;
    audio_client_t audio_client;
    audio_capture_t audio_capture;

    audio_notification_t endpt_notification;
    std::optional<std::function<void()>> default_endpt_changed_cb;

    REFERENCE_TIME default_latency_ms;

    util::buffer_t<float> sample_buf;
    float *sample_buf_pos;
    int channels;

    HANDLE mmcss_task_handle = NULL;
  };

  // 添加一个新的麦克风写入类，继承自mic_t
  class mic_write_wasapi_t: public mic_t {
  public:
    mic_write_wasapi_t() = default;

    ~mic_write_wasapi_t() override {
      if (opus_decoder) {
        opus_decoder_destroy(opus_decoder);
      }
      if (audio_client) {
        audio_client->Stop();
      }
      if (mmcss_task_handle) {
        AvRevertMmThreadCharacteristics(mmcss_task_handle);
      }
    }

    // 这个类不用于采样，只用于写入
    capture_e
    sample(std::vector<float> &sample_out) override {
      BOOST_LOG(error) << "mic_write_wasapi_t::sample() should not be called";
      return capture_e::error;
    }

    int
    init() {
      // 初始化OPUS解码器
      int opus_error;
      opus_decoder = opus_decoder_create(48000, 1, &opus_error);  // 48kHz, 单声道
      if (opus_error != OPUS_OK) {
        BOOST_LOG(error) << "Failed to create OPUS decoder: " << opus_strerror(opus_error);
        return -1;
      }

      // 初始化设备枚举器
      HRESULT hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum);

      if (FAILED(hr)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator for mic write: [0x" << util::hex(hr).to_string_view() << "]";
        return -1;
      }

      // 尝试创建或使用虚拟音频设备
      if (create_virtual_audio_device() != 0) {
        BOOST_LOG(warning) << "Virtual audio device not available, will try to use existing devices";
      }

      // 设置loopback
      if (setup_virtual_mic_loopback() != 0) {
        BOOST_LOG(warning) << "Failed to setup virtual microphone loopback";
      }

      // 对于麦克风重定向，我们需要使用虚拟音频输出设备
      device_t device;

      auto vb_matched = find_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
      if (vb_matched) {
        hr = device_enum->GetDevice(vb_matched->second.c_str(), &device);
        if (SUCCEEDED(hr) && device) {
          BOOST_LOG(info) << "Using VB-Audio Virtual Cable for client mic redirection";
        }
      }

      // 最后尝试使用默认的扬声器设备
      if (FAILED(hr) || !device) {
        hr = device_enum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr) && device) {
          BOOST_LOG(info) << "Using default console audio output device for client mic redirection";
        }
      }

      if (FAILED(hr) || !device) {
        BOOST_LOG(error) << "No suitable audio output device available for client mic redirection";
        return -1;
      }

      // 激活 IAudioClient
      auto status = device->Activate(
        IID_IAudioClient,
        CLSCTX_ALL,
        nullptr,
        (void **) &audio_client);
      if (FAILED(status) || !audio_client) {
        BOOST_LOG(error) << "Failed to activate IAudioClient for mic write: [0x" << util::hex(status).to_string_view() << "]";

        // 获取设备信息以便调试
        wstring_t device_id;
        device->GetId(&device_id);
        BOOST_LOG(error) << "Device ID: " << to_utf8(device_id.get());

        return -1;
      }

      // 尝试多种音频格式，从最兼容的开始
      std::vector<WAVEFORMATEX> formats_to_try = {
        // 16位单声道，48kHz
        { WAVE_FORMAT_PCM, 1, 48000, 96000, 2, 16, 0 },
        // 16位单声道，44.1kHz
        { WAVE_FORMAT_PCM, 1, 44100, 88200, 2, 16, 0 },
        // 16位立体声，48kHz
        { WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 },
        // 16位立体声，44.1kHz
        { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0 },
      };

      HRESULT init_status = E_FAIL;
      WAVEFORMATEX *used_format = nullptr;

      for (const auto &format : formats_to_try) {
        BOOST_LOG(debug) << "Trying audio format: " << format.nChannels << " channels, "
                         << format.nSamplesPerSec << " Hz, " << format.wBitsPerSample << " bits";

        init_status = audio_client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          0,  // 不使用特殊标志
          1000000,  // 100ms buffer (10000000 was 10 seconds)
          0,
          &format,
          nullptr);

        if (SUCCEEDED(init_status)) {
          used_format = const_cast<WAVEFORMATEX *>(&format);
          BOOST_LOG(info) << "Successfully initialized with format: " << format.nChannels << " channels, "
                          << format.nSamplesPerSec << " Hz, " << format.wBitsPerSample << " bits";
          break;
        }
        else {
          BOOST_LOG(debug) << "Format failed: [0x" << util::hex(init_status).to_string_view() << "]";
        }
      }

      if (FAILED(init_status)) {
        BOOST_LOG(error) << "Failed to initialize IAudioClient with any supported format: [0x" << util::hex(init_status).to_string_view() << "]";
        return -1;
      }

      // 保存使用的格式信息
      current_format = *used_format;

      // 启动音频客户端
      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to start IAudioClient for mic write: [0x" << util::hex(status).to_string_view() << "]";
        return -1;
      }

      // 获取 IAudioRenderClient - 用于写入音频数据
      status = audio_client->GetService(IID_IAudioRenderClient, (void **) &audio_render);
      if (FAILED(status) || !audio_render) {
        BOOST_LOG(error) << "Failed to get IAudioRenderClient for mic write: [0x" << util::hex(status).to_string_view() << "]";
        audio_client->Stop();
        return -1;
      }

      // 设置MMCSS优先级
      {
        DWORD task_index = 0;
        mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
        if (!mmcss_task_handle) {
          BOOST_LOG(warning) << "Couldn't associate mic write thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
        }
      }

      BOOST_LOG(info) << "Successfully initialized mic write device with OPUS decoder";
      return 0;
    }

    int
    write_data(const char *data, size_t len) {
      if (!audio_client || !audio_render) {
        BOOST_LOG(error) << "Mic write device not initialized";
        return -1;
      }

      // 解码OPUS数据
      int frame_size = opus_decoder_get_nb_samples(opus_decoder, (const unsigned char *) data, len);
      if (frame_size < 0) {
        BOOST_LOG(error) << "Failed to get OPUS frame size: " << opus_strerror(frame_size);
        return -1;
      }

      std::vector<int16_t> pcm_mono_buffer;
      pcm_mono_buffer.resize(frame_size);  // Resize to exactly fit the decoded mono samples

      int samples_decoded = opus_decode(
        opus_decoder,
        (const unsigned char *) data,
        len,
        pcm_mono_buffer.data(),
        frame_size,
        0  // No FEC
      );

      if (samples_decoded < 0) {
        BOOST_LOG(error) << "Failed to decode OPUS data: " << opus_strerror(samples_decoded);
        return -1;
      }

      // Handle channel conversion if necessary
      std::vector<int16_t> pcm_output_buffer;
      UINT32 framesToWrite;

      if (current_format.nChannels == 1) {
        // Mono output, direct copy
        pcm_output_buffer = std::move(pcm_mono_buffer);
        framesToWrite = samples_decoded;
      }
      else if (current_format.nChannels == 2) {
        // Stereo output, duplicate mono samples
        pcm_output_buffer.resize(samples_decoded * 2);
        for (int i = 0; i < samples_decoded; ++i) {
          pcm_output_buffer[i * 2] = pcm_mono_buffer[i];  // Left channel
          pcm_output_buffer[i * 2 + 1] = pcm_mono_buffer[i];  // Right channel
        }
        framesToWrite = samples_decoded;  // Each original mono sample becomes one stereo frame
      }
      else {
        BOOST_LOG(error) << "Unsupported channel count for mic write: " << current_format.nChannels;
        return -1;
      }

      // 获取缓冲区大小和当前填充的帧数
      UINT32 bufferFrameCount = 0;
      UINT32 padding = 0;
      auto status = audio_client->GetBufferSize(&bufferFrameCount);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get buffer size for mic write: [0x" << util::hex(status).to_string_view() << "]";
        return -1;
      }
      status = audio_client->GetCurrentPadding(&padding);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get current padding for mic write: [0x" << util::hex(status).to_string_view() << "]";
        return -1;
      }

      UINT32 availableFrames = bufferFrameCount - padding;

      // 确保我们不会写入超过可用空间的数据
      if (framesToWrite > availableFrames) {
        BOOST_LOG(warning) << "Mic write buffer overflow: " << framesToWrite << " frames to write, but only " << availableFrames << " available.";
        framesToWrite = availableFrames;
      }

      if (framesToWrite == 0) {
        return 0;
      }

      // 获取渲染缓冲区
      BYTE *pData = nullptr;
      status = audio_render->GetBuffer(framesToWrite, &pData);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to get render buffer for mic write: [0x" << util::hex(status).to_string_view() << "]";
        return -1;
      }

      // 拷贝解码后的PCM数据到缓冲区
      memcpy(pData, pcm_output_buffer.data(), framesToWrite * current_format.nBlockAlign);

      // 释放缓冲区
      status = audio_render->ReleaseBuffer(framesToWrite, 0);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to release render buffer for mic write: [0x" << util::hex(status).to_string_view() << "]";
        return -1;
      }

      return framesToWrite * current_format.nBlockAlign;  // 返回实际写入的字节数
    }

    // 添加一个简单的测试方法
    int
    test_write() {
      if (!audio_client || !audio_render) {
        BOOST_LOG(error) << "Mic write device not initialized for test";
        return -1;
      }

      // 创建一个简单的测试音频数据（静音）
      const int test_frames = 480;  // 10ms at 48kHz
      const int test_bytes = test_frames * current_format.nBlockAlign;
      std::vector<BYTE> test_data(test_bytes, 0);  // 全零数据（静音）

      BOOST_LOG(info) << "Testing client mic redirection with " << test_frames << " frames, " << test_bytes << " bytes";

      return write_data(reinterpret_cast<const char *>(test_data.data()), test_bytes);
    }

    int
    redirect_client_mic(const char *data, size_t len) {
      if (!audio_client || !audio_render) {
        BOOST_LOG(error) << "Client mic redirection device not initialized";
        return -1;
      }

      BOOST_LOG(debug) << "Redirecting " << len << " bytes of client microphone data to host";

      return write_data(data, len);
    }

    int
    create_virtual_audio_device() {
      BOOST_LOG(info) << "Attempting to create/use virtual audio device for client mic redirection";

      // 检查VB-Cable虚拟设备
      auto vb_matched = find_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
      if (vb_matched) {
        BOOST_LOG(info) << "Found existing VB-Audio Virtual Cable device";
        virtual_device_type = VirtualDeviceType::VB_CABLE;
        return 0;  // 设备已存在
      }

      BOOST_LOG(info) << "Attempting to install VB-Cable automatically";

      // 检查是否已安装VB-Cable驱动程序
      HKEY hKey;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VB\\VBAudioVAC", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        BOOST_LOG(info) << "VB-Cable driver is already installed";
        return -1;  // 已安装但未找到设备，可能是未启用
      }

      // 自动为用户安装VB-Cable
      BOOST_LOG(info) << "Downloading VB-Cable installer...";

      // 下载VB-Cable安装程序
      std::wstring download_url = L"https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip";
      std::wstring temp_path = std::filesystem::temp_directory_path().wstring() + L"\\VBCABLE_Driver_Pack43.zip";

      HMODULE urlmon = LoadLibraryW(L"urlmon.dll");
      if (!urlmon) {
        BOOST_LOG(error) << "Failed to load urlmon.dll";
        return -1;
      }

      auto URLDownloadToFileW_ptr = (decltype(URLDownloadToFileW) *) GetProcAddress(urlmon, "URLDownloadToFileW");
      if (!URLDownloadToFileW_ptr || URLDownloadToFileW_ptr(nullptr, download_url.c_str(), temp_path.c_str(), 0, nullptr) != S_OK) {
        BOOST_LOG(error) << "Failed to download VB-Cable installer";
        FreeLibrary(urlmon);
        return -1;
      }
      FreeLibrary(urlmon);

      BOOST_LOG(info) << "Extracting VB-Cable installer...";

      // 解压安装包
      std::wstring extract_path = std::filesystem::temp_directory_path().wstring() + L"\\VBCABLE_Install";
      if (!std::filesystem::create_directory(extract_path)) {
        BOOST_LOG(error) << "Failed to create extraction directory";
        return -1;
      }

      // 执行安装程序
      BOOST_LOG(info) << "Installing VB-Cable...";
      std::wstring install_cmd = L"powershell -command \"Expand-Archive -Path '" + temp_path + L"' -DestinationPath '" + extract_path + L"'; Start-Process -FilePath '" + extract_path + L"\\VBCABLE_Setup_x64.exe' -ArgumentList '/S' -Wait\"";

      if (_wsystem(install_cmd.c_str()) != 0) {
        BOOST_LOG(error) << "Failed to install VB-Cable";
        return -1;
      }

      BOOST_LOG(info) << "VB-Cable installed successfully";

      return 0;
    }

  private:
    // 虚拟设备类型枚举
    enum class VirtualDeviceType {
      NONE,
      STEAM,
      VB_CABLE,
    };

    device_enum_t device_enum;
    audio_client_t audio_client;
    IAudioRenderClient *audio_render = nullptr;
    OpusDecoder *opus_decoder = nullptr;
    HANDLE mmcss_task_handle = NULL;
    WAVEFORMATEX current_format = {};
    VirtualDeviceType virtual_device_type = VirtualDeviceType::NONE;

    std::optional<matched_field_t>
    find_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate render devices: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      return find_device_in_collection(collection, match_list);
    }

    std::optional<matched_field_t>
    find_capture_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate capture devices: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      return find_device_in_collection(collection, match_list);
    }

    std::optional<matched_field_t>
    find_device_in_collection(collection_t &collection, const match_fields_list_t &match_list) {
      UINT count = 0;
      collection->GetCount(&count);

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        device_t device;
        collection->Item(x, &device);

        wstring_t wstring_id;
        device->GetId(&wstring_id);
        std::wstring device_id = wstring_id.get();

        prop_t prop;
        device->OpenPropertyStore(STGM_READ, &prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
            switch (match_list[i].first) {
              case match_field_e::device_id:
                match_value = device_id.c_str();
                break;

              case match_field_e::device_friendly_name:
                match_value = device_friendly_name.prop.pwszVal;
                break;

              case match_field_e::adapter_friendly_name:
                match_value = adapter_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_description:
                match_value = device_desc.prop.pwszVal;
                break;
            }
            if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
              matched[i] = device_id;
            }
          }
        }
      }

      for (size_t i = 0; i < match_list.size(); i++) {
        if (!matched[i].empty()) {
          return matched_field_t(match_list[i].first, matched[i]);
        }
      }

      return std::nullopt;
    }

    int
    setup_virtual_mic_loopback() {
      if (virtual_device_type == VirtualDeviceType::NONE) {
        BOOST_LOG(warning) << "No virtual device available for loopback setup";
        return -1;
      }

      BOOST_LOG(info) << "Setting up virtual microphone loopback for client mic redirection";

      // 根据虚拟设备类型设置循环
      switch (virtual_device_type) {
        case VirtualDeviceType::STEAM:
          return setup_steam_mic_loopback();
        case VirtualDeviceType::VB_CABLE:
          return setup_vb_cable_mic_loopback();
        default:
          BOOST_LOG(warning) << "Unknown virtual device type for loopback setup";
          return -1;
      }
    }

    // Steam虚拟麦克风循环设置
    int
    setup_steam_mic_loopback() {
      BOOST_LOG(info) << "Setting up Steam virtual microphone loopback";

      // Steam Streaming Speakers 会自动循环到 Steam Streaming Microphone
      // 我们需要确保Steam Streaming Microphone被设置为默认录音设备

      // 查找Steam Streaming Microphone设备
      auto mic_matched = find_capture_device_id({ { match_field_e::adapter_friendly_name, L"Steam Streaming Microphone" } });
      if (mic_matched) {
        BOOST_LOG(info) << "Found Steam Streaming Microphone, attempting to set as default";

        // 这里可以添加设置默认录音设备的逻辑
        // 使用IPolicyConfig::SetDefaultEndpoint
        // 但需要先获取policy接口
        IPolicyConfig *policy_raw = nullptr;
        HRESULT hr = CoCreateInstance(
          CLSID_CPolicyConfigClient,
          nullptr,
          CLSCTX_ALL,
          IID_IPolicyConfig,
          (void **) &policy_raw);

        if (SUCCEEDED(hr) && policy_raw) {
          policy_t policy(policy_raw);

          hr = policy->SetDefaultEndpoint(mic_matched->second.c_str(), eCommunications);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "Failed to set Steam Streaming Microphone as default communications device: [0x" << util::hex(hr).to_string_view() << "]";
          }

          hr = policy->SetDefaultEndpoint(mic_matched->second.c_str(), eConsole);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "Failed to set Steam Streaming Microphone as default console device: [0x" << util::hex(hr).to_string_view() << "]";
          }
        }
        else {
          BOOST_LOG(error) << "Couldn't create PolicyConfig instance: [0x" << util::hex(hr).to_string_view() << "]";
        }

        BOOST_LOG(info) << "Steam virtual microphone loopback setup complete";
        return 0;
      }
      else {
        BOOST_LOG(warning) << "Steam Streaming Microphone not found, loopback may not work";
        return -1;
      }
    }

    // VB-Cable虚拟麦克风循环设置
    int
    setup_vb_cable_mic_loopback() {
      BOOST_LOG(info) << "Setting up VB-Cable virtual microphone loopback";

      // VB-Cable Output 会自动循环到 VB-Cable Input
      // 我们需要确保VB-Cable Input被设置为默认录音设备， VB-Cable Output 不被设置为默认播放设备

      // 查找VB-Cable Input设备
      auto vb_input_matched = find_capture_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
      if (vb_input_matched) {
        // 设置VB-Cable Input为默认录音设备
        IPolicyConfig *policy_raw = nullptr;
        HRESULT hr = CoCreateInstance(
          CLSID_CPolicyConfigClient,
          nullptr,
          CLSCTX_ALL,
          IID_IPolicyConfig,
          (void **) &policy_raw);

        if (SUCCEEDED(hr) && policy_raw) {
          policy_t policy(policy_raw);

          // 设置为默认通信设备
          hr = policy->SetDefaultEndpoint(vb_input_matched->second.c_str(), eCommunications);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "Failed to set VB-Cable Input as default communications device: [0x" << util::hex(hr).to_string_view() << "]";
          }

          // 设置为默认控制台设备
          hr = policy->SetDefaultEndpoint(vb_input_matched->second.c_str(), eConsole);
          if (FAILED(hr)) {
            BOOST_LOG(error) << "Failed to set VB-Cable Input as default console device: [0x" << util::hex(hr).to_string_view() << "]";
          }
        }
        else {
          BOOST_LOG(error) << "Couldn't create PolicyConfig instance: [0x" << util::hex(hr).to_string_view() << "]";
        }
      }

      // 确保VB-Cable Output不被设置为默认播放设备
      auto vb_output_matched = find_device_id({ { match_field_e::adapter_friendly_name, L"VB-Audio Virtual Cable" } });
      if (vb_output_matched) {
        // 获取第一个播放设备作为替代
        device_t first_device;
        HRESULT hr = device_enum->GetDefaultAudioEndpoint(eRender, eConsole, &first_device);
        if (SUCCEEDED(hr) && first_device) {
          wstring_t first_device_id;
          first_device->GetId(&first_device_id);

          // 如果当前默认设备是VB-Cable Output，则改为第一个设备
          if (first_device_id.get() == vb_output_matched->second) {
            IPolicyConfig *policy_raw = nullptr;
            hr = CoCreateInstance(
              CLSID_CPolicyConfigClient,
              nullptr,
              CLSCTX_ALL,
              IID_IPolicyConfig,
              (void **) &policy_raw);

            if (SUCCEEDED(hr) && policy_raw) {
              policy_t policy(policy_raw);

              // 枚举所有播放设备
              collection_t collection;
              hr = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
              if (SUCCEEDED(hr)) {
                UINT count;
                collection->GetCount(&count);

                // 找到第一个非VB-Cable设备
                for (UINT i = 0; i < count; i++) {
                  device_t device;
                  collection->Item(i, &device);
                  wstring_t device_id;
                  device->GetId(&device_id);

                  if (device_id.get() != vb_output_matched->second) {
                    // 设置为默认设备
                    hr = policy->SetDefaultEndpoint(device_id.get(), eConsole);
                    if (SUCCEEDED(hr)) {
                      BOOST_LOG(info) << "Set first available device as default playback device";
                    }
                    break;
                  }
                }
              }
            }
          }
        }
      }

      BOOST_LOG(info) << "VB-Cable virtual microphone loopback should be automatic";
      return 0;
    }
  };

  class audio_control_t: public ::platf::audio_control_t {
  public:
    std::optional<sink_t>
    sink_info() override {
      sink_t sink;

      // Fill host sink name with the device_id of the current default audio device.
      {
        auto device = default_device(device_enum);
        if (!device) {
          return std::nullopt;
        }

        audio::wstring_t id;
        device->GetId(&id);

        sink.host = to_utf8(id.get());
      }

      // Prepare to search for the device_id of the virtual audio sink device,
      // this device can be either user-configured or
      // the Steam Streaming Speakers we use by default.
      match_fields_list_t match_list;
      if (config::audio.virtual_sink.empty()) {
        match_list = match_steam_speakers();
      }
      else {
        match_list = match_all_fields(from_utf8(config::audio.virtual_sink));
      }

      // Search for the virtual audio sink device currently present in the system.
      auto matched = find_device_id(match_list);
      if (matched) {
        // Prepare to fill virtual audio sink names with device_id.
        auto device_id = to_utf8(matched->second);
        // Also prepend format name (basically channel layout at the moment)
        // because we don't want to extend the platform interface.
        sink.null = std::make_optional(sink_t::null_t {
          "virtual-"s + formats[0].name + device_id,
          "virtual-"s + formats[1].name + device_id,
          "virtual-"s + formats[2].name + device_id,
        });
      }
      else if (!config::audio.virtual_sink.empty()) {
        BOOST_LOG(warning) << "Couldn't find the specified virtual audio sink " << config::audio.virtual_sink;
      }

      return sink;
    }

    bool
    is_sink_available(const std::string &sink) override {
      const auto match_list = match_all_fields(from_utf8(sink));
      const auto matched = find_device_id(match_list);
      return static_cast<bool>(matched);
    }

    /**
     * @brief Extract virtual audio sink information possibly encoded in the sink name.
     * @param sink The sink name
     * @return A pair of device_id and format reference if the sink name matches
     *         our naming scheme for virtual audio sinks, `std::nullopt` otherwise.
     */
    std::optional<std::pair<std::wstring, std::reference_wrapper<const format_t>>>
    extract_virtual_sink_info(const std::string &sink) {
      // Encoding format:
      // [virtual-(format name)]device_id
      std::string current = sink;
      auto prefix = "virtual-"sv;
      if (current.find(prefix) == 0) {
        current = current.substr(prefix.size(), current.size() - prefix.size());

        for (const auto &format : formats) {
          auto &name = format.name;
          if (current.find(name) == 0) {
            auto device_id = from_utf8(current.substr(name.size(), current.size() - name.size()));
            return std::make_pair(device_id, std::reference_wrapper(format));
          }
        }
      }

      return std::nullopt;
    }

    std::unique_ptr<mic_t>
    microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) override {
      auto mic = std::make_unique<mic_wasapi_t>();

      if (mic->init(sample_rate, frame_size, channels)) {
        return nullptr;
      }

      // If this is a virtual sink, set a callback that will change the sink back if it's changed
      auto virtual_sink_info = extract_virtual_sink_info(assigned_sink);
      if (virtual_sink_info) {
        mic->default_endpt_changed_cb = [this] {
          BOOST_LOG(info) << "Resetting sink to ["sv << assigned_sink << "] after default changed";
          set_sink(assigned_sink);
        };
      }

      return mic;
    }

    /**
     * If the requested sink is a virtual sink, meaning no speakers attached to
     * the host, then we can seamlessly set the format to stereo and surround sound.
     *
     * Any virtual sink detected will be prefixed by:
     *    virtual-(format name)
     * If it doesn't contain that prefix, then the format will not be changed
     */
    std::optional<std::wstring>
    set_format(const std::string &sink) {
      if (sink.empty()) {
        return std::nullopt;
      }

      auto virtual_sink_info = extract_virtual_sink_info(sink);

      if (!virtual_sink_info) {
        // Sink name does not begin with virtual-(format name), hence it's not a virtual sink
        // and we don't want to change playback format of the corresponding device.
        // Also need to perform matching, sink name is not necessarily device_id in this case.
        auto matched = find_device_id(match_all_fields(from_utf8(sink)));
        if (matched) {
          return matched->second;
        }
        else {
          BOOST_LOG(error) << "Couldn't find audio sink " << sink;
          return std::nullopt;
        }
      }

      // When switching to a Steam virtual speaker device, try to retain the bit depth of the
      // default audio device. Switching from a 16-bit device to a 24-bit one has been known to
      // cause glitches for some users.
      int wanted_bits_per_sample = 32;
      auto current_default_dev = default_device(device_enum);
      if (current_default_dev) {
        audio::prop_t prop;
        prop_var_t current_device_format;

        if (SUCCEEDED(current_default_dev->OpenPropertyStore(STGM_READ, &prop)) && SUCCEEDED(prop->GetValue(PKEY_AudioEngine_DeviceFormat, &current_device_format.prop))) {
          auto *format = (WAVEFORMATEXTENSIBLE *) current_device_format.prop.blob.pBlobData;
          wanted_bits_per_sample = format->Samples.wValidBitsPerSample;
          BOOST_LOG(info) << "Virtual audio device will use "sv << wanted_bits_per_sample << "-bit to match default device"sv;
        }
      }

      auto &device_id = virtual_sink_info->first;
      auto &waveformats = virtual_sink_info->second.get().virtual_sink_waveformats;
      for (const auto &waveformat : waveformats) {
        // We're using completely undocumented and unlisted API,
        // better not pass objects without copying them first.
        auto device_id_copy = device_id;
        auto waveformat_copy = waveformat;
        auto waveformat_copy_pointer = reinterpret_cast<WAVEFORMATEX *>(&waveformat_copy);

        if (wanted_bits_per_sample != waveformat.Samples.wValidBitsPerSample) {
          continue;
        }

        WAVEFORMATEXTENSIBLE p {};
        if (SUCCEEDED(policy->SetDeviceFormat(device_id_copy.c_str(), waveformat_copy_pointer, (WAVEFORMATEX *) &p))) {
          BOOST_LOG(info) << "Changed virtual audio sink format to " << logging::bracket(waveformat_to_pretty_string(waveformat));
          return device_id;
        }
      }

      BOOST_LOG(error) << "Couldn't set virtual audio sink waveformat";
      return std::nullopt;
    }

    int
    set_sink(const std::string &sink) override {
      auto device_id = set_format(sink);
      if (!device_id) {
        return -1;
      }

      int failure {};
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        auto status = policy->SetDefaultEndpoint(device_id->c_str(), (ERole) x);
        if (status) {
          // Depending on the format of the string, we could get either of these errors
          if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == E_INVALIDARG) {
            BOOST_LOG(warning) << "Audio sink not found: "sv << sink;
          }
          else {
            BOOST_LOG(warning) << "Couldn't set ["sv << sink << "] to role ["sv << x << "]: 0x"sv << util::hex(status).to_string_view();
          }

          ++failure;
        }
      }

      // Remember the assigned sink name, so we have it for later if we need to set it
      // back after another application changes it
      if (!failure) {
        assigned_sink = sink;
      }

      return failure;
    }

    enum class match_field_e {
      device_id,  ///< Match device_id
      device_friendly_name,  ///< Match endpoint friendly name
      adapter_friendly_name,  ///< Match adapter friendly name
      device_description,  ///< Match endpoint description
    };

    using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
    using matched_field_t = std::pair<match_field_e, std::wstring>;

    audio_control_t::match_fields_list_t
    match_steam_speakers() {
      return {
        { match_field_e::adapter_friendly_name, L"Steam Streaming Speakers" }
      };
    }

    audio_control_t::match_fields_list_t
    match_all_fields(const std::wstring &name) {
      return {
        { match_field_e::device_id, name },  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        { match_field_e::device_friendly_name, name },  // Digital Audio (S/PDIF) (High Definition Audio Device)
        { match_field_e::device_description, name },  // Digital Audio (S/PDIF)
        { match_field_e::adapter_friendly_name, name },  // High Definition Audio Device
      };
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
     * @param match_list Pairs of match fields and values
     * @return Optional pair of matched field and device_id
     */
    std::optional<matched_field_t>
    find_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      UINT count = 0;
      collection->GetCount(&count);

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        audio::device_t device;
        collection->Item(x, &device);

        audio::wstring_t wstring_id;
        device->GetId(&wstring_id);
        std::wstring device_id = wstring_id.get();

        audio::prop_t prop;
        device->OpenPropertyStore(STGM_READ, &prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
            switch (match_list[i].first) {
              case match_field_e::device_id:
                match_value = device_id.c_str();
                break;

              case match_field_e::device_friendly_name:
                match_value = device_friendly_name.prop.pwszVal;
                break;

              case match_field_e::adapter_friendly_name:
                match_value = adapter_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_description:
                match_value = device_desc.prop.pwszVal;
                break;
            }
            if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
              matched[i] = device_id;
            }
          }
        }
      }

      for (size_t i = 0; i < match_list.size(); i++) {
        if (!matched[i].empty()) {
          return matched_field_t(match_list[i].first, matched[i]);
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Resets the default audio device from Steam Streaming Speakers.
     */
    void
    reset_default_device() {
      auto matched_steam = find_device_id(match_steam_speakers());
      if (!matched_steam) {
        return;
      }
      auto steam_device_id = matched_steam->second;

      {
        // Get the current default audio device (if present)
        auto current_default_dev = default_device(device_enum);
        if (!current_default_dev) {
          return;
        }

        audio::wstring_t current_default_id;
        current_default_dev->GetId(&current_default_id);

        // If Steam Streaming Speakers are already not default, we're done.
        if (steam_device_id != current_default_id.get()) {
          return;
        }
      }

      // Disable the Steam Streaming Speakers temporarily to allow the OS to pick a new default.
      auto hr = policy->SetEndpointVisibility(steam_device_id.c_str(), FALSE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to disable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // Get the newly selected default audio device
      auto new_default_dev = default_device(device_enum);

      // Enable the Steam Streaming Speakers again
      hr = policy->SetEndpointVisibility(steam_device_id.c_str(), TRUE);
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "Failed to enable Steam audio device: "sv << util::hex(hr).to_string_view();
        return;
      }

      // If there's now no audio device, the Steam Streaming Speakers were the only device available.
      // There's no other device to set as the default, so just return.
      if (!new_default_dev) {
        return;
      }

      audio::wstring_t new_default_id;
      new_default_dev->GetId(&new_default_id);

      // Set the new default audio device
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        policy->SetDefaultEndpoint(new_default_id.get(), (ERole) x);
      }

      BOOST_LOG(info) << "Successfully reset default audio device"sv;
    }

    /**
     * @brief Installs the Steam Streaming Speakers driver, if present.
     * @return `true` if installation was successful.
     */
    bool
    install_steam_audio_drivers() {
#ifdef STEAM_DRIVER_SUBDIR
      // MinGW's libnewdev.a is missing DiInstallDriverW() even though the headers have it,
      // so we have to load it at runtime. It's Vista or later, so it will always be available.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(error) << "newdev.dll failed to load"sv;
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        BOOST_LOG(error) << "DiInstallDriverW() is missing"sv;
        return false;
      }

      // Get the current default audio device (if present)
      auto old_default_dev = default_device(device_enum);

      // Install the Steam Streaming Speakers driver
      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_AUDIO_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        BOOST_LOG(info) << "Successfully installed Steam Streaming Speakers"sv;

        // Wait for 5 seconds to allow the audio subsystem to reconfigure things before
        // modifying the default audio device or enumerating devices again.
        Sleep(5000);

        // If there was a previous default device, restore that original device as the
        // default output device just in case installing the new one changed it.
        if (old_default_dev) {
          audio::wstring_t old_default_id;
          old_default_dev->GetId(&old_default_id);

          for (int x = 0; x < (int) ERole_enum_count; ++x) {
            policy->SetDefaultEndpoint(old_default_id.get(), (ERole) x);
          }
        }

        return true;
      }
      else {
        auto err = GetLastError();
        switch (err) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Administrator privileges are required to install Steam Streaming Speakers"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Steam audio drivers not found. This is expected if you don't have Steam installed."sv;
            break;
          default:
            BOOST_LOG(warning) << "Failed to install Steam audio drivers: "sv << err;
            break;
        }

        return false;
      }
#else
      BOOST_LOG(warning) << "Unable to install Steam Streaming Speakers on unknown architecture"sv;
      return false;
#endif
    }

    int
    init() {
      auto status = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        (void **) &policy);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create audio policy config: [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      return 0;
    }

    ~audio_control_t() override {
    }

    policy_t policy;
    audio::device_enum_t device_enum;
    std::string assigned_sink;

    int
    write_mic_data(const char *data, size_t len) {
      // 使用单例模式的客户端麦克风重定向设备
      static std::unique_ptr<mic_write_wasapi_t> mic_redirect_device = nullptr;
      static std::mutex mic_redirect_mutex;

      std::lock_guard<std::mutex> lock(mic_redirect_mutex);

      if (!mic_redirect_device) {
        mic_redirect_device = std::make_unique<mic_write_wasapi_t>();
        if (mic_redirect_device->init() != 0) {
          BOOST_LOG(error) << "Failed to initialize client mic redirection device";
          mic_redirect_device.reset();
          return -1;
        }
        BOOST_LOG(info) << "Successfully initialized client mic redirection device";

        // 执行测试写入
        if (mic_redirect_device->test_write() < 0) {
          BOOST_LOG(warning) << "Client mic redirection test failed, but continuing anyway";
        }
      }

      // 重定向客户端麦克风数据到主机
      return mic_redirect_device->redirect_client_mic(data, len);
    }
  };
}  // namespace platf::audio

namespace platf {

  // It's not big enough to justify it's own source file :/
  namespace dxgi {
    int
    init();
  }

  std::unique_ptr<audio_control_t>
  audio_control() {
    auto control = std::make_unique<audio::audio_control_t>();

    if (control->init()) {
      return nullptr;
    }

    // Install Steam Streaming Speakers if needed. We do this during audio_control() to ensure
    // the sink information returned includes the new Steam Streaming Speakers device.
    if (config::audio.install_steam_drivers && !control->find_device_id(control->match_steam_speakers())) {
      // This is best effort. Don't fail if it doesn't work.
      control->install_steam_audio_drivers();
    }

    return control;
  }

  std::unique_ptr<deinit_t>
  init() {
    if (dxgi::init()) {
      return nullptr;
    }

    // Initialize COM
    auto co_init = std::make_unique<platf::audio::co_init_t>();

    // If Steam Streaming Speakers are currently the default audio device,
    // change the default to something else (if another device is available).
    audio::audio_control_t audio_ctrl;
    if (audio_ctrl.init() == 0) {
      audio_ctrl.reset_default_device();
    }

    return co_init;
  }
}  // namespace platf