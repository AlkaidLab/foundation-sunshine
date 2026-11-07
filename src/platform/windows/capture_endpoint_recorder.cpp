/**
 * @file src/platform/windows/capture_endpoint_recorder.cpp
 * @brief WASAPI shared-mode capture of one endpoint (see the header).
 *
 * Capture ramp-up sources: Microsoft Learn, "IAudioClient::Start
 * (audioclient.h)" and "IAudioCaptureClient::GetNextPacketSize
 * (audioclient.h)", read 2026-11-09.  Start only begins the streaming between
 * the endpoint buffer and the engine, and GetNextPacketSize reports the next
 * shared-mode packet (zero while none is ready); neither promises when the
 * first packet of a freshly arrived device becomes ready, so the open retry
 * and the first-packet wait below are bounded instead of assumed.
 */
#include "capture_endpoint_recorder.h"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <cstring>
#include <string>
#include <thread>

namespace platf::audio {

  namespace {
    template <class T>
    struct com_ptr {
      T *p { nullptr };
      com_ptr() = default;
      com_ptr(const com_ptr &) = delete;
      com_ptr &
      operator=(const com_ptr &) = delete;
      ~com_ptr() { reset(); }
      void
      reset() {
        if (p) p->Release();
        p = nullptr;
      }
      T **
      out() {
        reset();
        return &p;
      }
      T *
      operator->() const { return p; }
      explicit
      operator bool() const { return p != nullptr; }
    };
  }  // namespace

  struct capture_endpoint_recorder::impl {
    HRESULT com_status { E_FAIL };
    com_ptr<IMMDevice> device;
    com_ptr<IAudioClient> client;
    com_ptr<IAudioCaptureClient> capture;
    WAVEFORMATEX *format { nullptr };
    bool floating { false };
    bool started { false };

    bool
    find(std::wstring_view name_part) {
      com_ptr<IMMDeviceEnumerator> enumerator;
      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void **>(enumerator.out())))) {
        return false;
      }
      com_ptr<IMMDeviceCollection> collection;
      if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.out()))) return false;
      UINT count = 0;
      collection->GetCount(&count);
      for (UINT i = 0; i < count; ++i) {
        com_ptr<IMMDevice> candidate;
        if (FAILED(collection->Item(i, candidate.out()))) continue;
        com_ptr<IPropertyStore> store;
        if (FAILED(candidate->OpenPropertyStore(STGM_READ, store.out()))) continue;
        PROPVARIANT name;
        PropVariantInit(&name);
        const bool match = SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
                           std::wstring_view(name.pwszVal).find(name_part) != std::wstring_view::npos;
        PropVariantClear(&name);
        if (match) {
          device.p = candidate.p;
          candidate.p = nullptr;
          return true;
        }
      }
      return false;
    }

    bool
    initialize() {
      if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(client.out())))) {
        return false;
      }
      if (FAILED(client->GetMixFormat(&format)) || !format) return false;
      floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                 (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   reinterpret_cast<WAVEFORMATEXTENSIBLE *>(format)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
      if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, format, nullptr))) return false;
      if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(capture.out())))) return false;
      return true;
    }

    float
    first_channel(const BYTE *frame) const {
      const int bytes = format->wBitsPerSample / 8;
      if (floating && bytes == 4) {
        float v;
        std::memcpy(&v, frame, 4);
        return v;
      }
      if (bytes == 2) {
        std::int16_t v;
        std::memcpy(&v, frame, 2);
        return v / 32768.0f;
      }
      if (bytes == 3) {
        const std::int32_t v = (frame[0] << 8 | frame[1] << 16 | frame[2] << 24) >> 8;
        return v / 8388608.0f;
      }
      std::int32_t v;
      std::memcpy(&v, frame, 4);
      return v / 2147483648.0f;
    }
  };

  capture_endpoint_recorder::capture_endpoint_recorder():
      impl_(std::make_unique<impl>()) {
    impl_->com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  }

  capture_endpoint_recorder::~capture_endpoint_recorder() {
    close();
    if (SUCCEEDED(impl_->com_status)) CoUninitialize();
  }

  bool
  capture_endpoint_recorder::open(std::wstring_view name_part, std::chrono::milliseconds wait) {
    auto &s = *impl_;
    if (FAILED(s.com_status) && s.com_status != RPC_E_CHANGED_MODE) return false;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    /* The endpoint name can be published before the audio interface accepts a
     * stream on the freshly arrived device, so activation is retried inside the
     * same wait instead of failing the whole open on the first attempt. */
    while (true) {
      if (s.find(name_part) && s.initialize()) return true;
      close();
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }

  void
  capture_endpoint_recorder::close() {
    auto &s = *impl_;
    if (s.started && s.client) s.client->Stop();
    s.started = false;
    s.capture.reset();
    s.client.reset();
    s.device.reset();
    if (s.format) CoTaskMemFree(s.format);
    s.format = nullptr;
  }

  std::uint32_t
  capture_endpoint_recorder::sample_rate() const {
    return impl_->format ? impl_->format->nSamplesPerSec : 0;
  }

  bool
  capture_endpoint_recorder::record(std::chrono::milliseconds duration, std::vector<block> &blocks,
    std::chrono::milliseconds first_packet_wait) {
    auto &s = *impl_;
    if (!s.capture) return false;
    if (!s.started) {
      if (FAILED(s.client->Start())) return false;
      s.started = true;
    }
    /* The engine of a freshly arrived device can need seconds before its first
     * capture packet; that ramp-up must not eat into the measured window, or a
     * healthy device fails for "too little audio".  The measurement therefore
     * starts at the first packet (or when the bounded wait runs out). */
    const auto started_at = std::chrono::steady_clock::now();
    auto wait_end = started_at + first_packet_wait;
    bool measuring = first_packet_wait <= std::chrono::milliseconds::zero();
    auto end = started_at + (measuring ? duration : std::chrono::milliseconds::zero());
    while (true) {
      UINT32 packet = 0;
      if (FAILED(s.capture->GetNextPacketSize(&packet))) return false;
      if (packet == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (measuring && now >= end) break;
        if (!measuring && now >= wait_end) {
          measuring = true;
          end = now + duration;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (!measuring) {
        measuring = true;
        end = std::chrono::steady_clock::now() + duration;
      }
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 position = 0, qpc = 0;
      if (FAILED(s.capture->GetBuffer(&data, &frames, &flags, &position, &qpc))) return false;
      LARGE_INTEGER now, frequency;
      QueryPerformanceCounter(&now);
      QueryPerformanceFrequency(&frequency);
      block b;
      b.read_qpc_100ns = static_cast<std::uint64_t>(static_cast<double>(now.QuadPart) * 1e7 / static_cast<double>(frequency.QuadPart));
      b.qpc_100ns = qpc;
      b.position = position;
      b.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
      b.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
      b.samples.resize(frames);
      for (UINT32 i = 0; i < frames; ++i) {
        b.samples[i] = b.silent ? 0.0f : s.first_channel(data + static_cast<std::size_t>(i) * s.format->nBlockAlign);
      }
      s.capture->ReleaseBuffer(frames);
      blocks.push_back(std::move(b));
    }
    return true;
  }

}  // namespace platf::audio
