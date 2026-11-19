/**
 * @file tests/tools/ds5_matrix/win_rig.h
 * @brief Windows rig: the real native virtual DualSense, written to as a game does.
 *
 * Output reports go through WriteFile on the virtual HID, input reports are read with ReadFile, the
 * feedback messages are read from the queue the native client raises them on, and haptics PCM is
 * rendered to the pad's four-channel audio endpoint with WASAPI.  Settings are saved through
 * ds5::tuning::store_t, the object behind the settings page.
 */
#pragma once

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32

  #include <audioclient.h>
  #include <functiondiscoverykeys_devpkey.h>
  #include <hidsdi.h>
  #include <mmdeviceapi.h>
  #include <setupapi.h>
  #include <windows.h>

  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstring>
  #include <deque>
  #include <filesystem>
  #include <mutex>
  #include <thread>

  #include "src/ds5/tuning_profile.h"
  #include "src/ds5/tuning_runtime.h"
  #include "src/platform/windows/ds5/ds5_native_client.h"
  #include "src/thread_safe.h"
  #include "tests/tools/ds5_matrix/matrix_rig.h"

namespace ds5::matrix {
  using namespace std::chrono_literals;

  inline std::int64_t &
  synthetic_clock_us() {
    static std::int64_t value = 1000000;
    return value;
  }

  class win_rig_t: public rig_t {
  public:
    explicit win_rig_t(const std::filesystem::path &profile):
        path_(profile),
        mail_(std::make_shared<safe::mail_raw_t>()) {
      platf::ds5::native_client_t::set_motion_clock([]() -> std::int64_t { return synthetic_clock_us(); });
      clear_store();
      allocate();
    }

    ~win_rig_t() override { release(); }

    const char *
    name() const override { return "device"; }

    bool
    attached() const { return attached_; }

    void
    clear_store() override {
      std::filesystem::remove(path_);
      store_ = std::make_unique<tuning::store_t>(path_);
      store_->load(nullptr);
      const auto before = mark();
      store_->publish();
      absorb_pushed(before);
      if (std::getenv("DS5_MATRIX_TRACE") != nullptr) {
        std::fprintf(stderr, "TRACE clear_store -> %zu pushed:", pushed_.size());
        for (const auto &m : pushed_) std::fprintf(stderr, " [kind=%d mask=0x%x route=%u player=%u]", static_cast<int>(m.kind), m.mask, m.route, m.player);
        std::fprintf(stderr, "\n");
      }
    }

    post_result_t
    post(const json &values) override {
      const auto before = mark();
      auto result = store_->apply_json({ { "schema", "ALK-DS5T/1" }, { "values", values } });
      pushed_.clear();
      if (result.ok) absorb_pushed(before);
      if (std::getenv("DS5_MATRIX_TRACE") != nullptr) {
        std::fprintf(stderr, "TRACE post %s -> %zu pushed:", values.dump().c_str(), pushed_.size());
        for (const auto &m : pushed_) std::fprintf(stderr, " [kind=%d mask=0x%x route=%u player=%u]", static_cast<int>(m.kind), m.mask, m.route, m.player);
        std::fprintf(stderr, "\n");
      }
      return { result.ok, result.http_status, result.body.is_object() ? result.body : json { { "error", result.error } } };
    }

    /** @brief The host pushes the saved state to the pad after a save; the client holds whatever arrives. */
    void
    absorb_pushed(std::size_t before) {
      if (!attached_) return;
      settle(50ms, 45ms, 600ms);
      pushed_ = take(before);
      for (const auto &message : pushed_) absorb(message);
    }

    int
    attach_count() const override { return attach_count_; }

    json
    get() override { return store_->snapshot_json(); }

    std::vector<fb_t>
    output(const report48 &report) override {
      const auto before = mark();
      DWORD written = 0;
      if (writer_ == INVALID_HANDLE_VALUE || !WriteFile(writer_, report.data(), static_cast<DWORD>(report.size()), &written, nullptr)) {
        write_failures_++;
        return {};
      }
      settle(50ms, 45ms, 600ms);
      auto messages = take(before);
      for (const auto &message : messages) absorb(message);
      return messages;
    }

    report64
    input(const pad_state_t &state) override {
      platf::gamepad_state_t native {};
      native.buttonFlags = state.buttons;
      native.lt = state.lt;
      native.rt = state.rt;
      native.lsX = state.lsx;
      native.lsY = state.lsy;
      native.rsX = state.rsx;
      native.rsY = state.rsy;
      const auto previous = latest()[7];
      client_.submit_input(0, native);
      for (int wait = 0; wait < 60; ++wait) {
        std::this_thread::sleep_for(5ms);
        if (latest()[7] != previous) break;
      }
      std::this_thread::sleep_for(15ms);
      return latest();
    }

    report64
    gyro(float x, float y, float z, bool read) override {
      /* Samples come as fast as the loop runs: stamp them on a 250 Hz clock so the filter sees real sample spacing. */
      synthetic_clock_us() += 4000;
      client_.submit_motion({ { 0, 0 }, LI_MOTION_TYPE_GYRO, x, y, z });
      if (!read) return {};
      std::this_thread::sleep_for(70ms);
      return latest();
    }

    report64
    touch(int event, std::uint32_t pointer, float x, float y) override {
      const std::uint8_t type = event == 0 ? LI_TOUCH_EVENT_DOWN : (event == 1 ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_UP);
      client_.submit_touch({ { 0, 0 }, type, pointer, x, y, 1.0f });
      std::this_thread::sleep_for(70ms);
      return latest();
    }

    std::vector<fb_t>
    play(std::span<const std::int16_t> lanes) override {
      const auto before = mark();
      render(lanes);
      settle(300ms, 100ms, 1500ms);
      auto messages = take(before);
      std::vector<fb_t> pcm;
      for (auto &message : messages) {
        if (message.kind != fb_kind::pcm) continue;
        bool silent = true;
        for (const auto sample : message.pcm) silent = silent && sample == 0;
        if (!silent) pcm.push_back(std::move(message));
      }
      return pcm;
    }

    std::vector<fb_t>
    end_session() override {
      const auto before = mark();
      release();
      auto messages = take(before);
      reset_views();
      allocate();
      return messages;
    }

    std::uint32_t
    identity_mode() override { return tuning::runtime::identity_gamepad_mode(); }

    unsigned
    write_failures() const { return write_failures_; }

  private:
    static HANDLE
    open_pad() {
      GUID guid {};
      HidD_GetHidGuid(&guid);
      auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      if (devices == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
      HANDLE result = INVALID_HANDLE_VALUE;
      for (DWORD index = 0; result == INVALID_HANDLE_VALUE; ++index) {
        SP_DEVICE_INTERFACE_DATA entry { sizeof(entry) };
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &entry)) break;
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<std::uint8_t> storage(size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
        if (std::wstring(detail->DevicePath).find(L"vid_054c&pid_0ce6") == std::wstring::npos) continue;
        result = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      }
      SetupDiDestroyDeviceInfoList(devices);
      return result;
    }

    void
    allocate() {
      auto queue = mail_->queue<platf::gamepad_feedback_msg_t>("ds5-matrix");
      queue_ = queue;
      stop_reader_ = false;
      stop_feedback_ = false;
      attached_ = client_.alloc({ 0, 0 }, std::move(queue), true) == 0;
      if (!attached_) return;
      ++attach_count_;
      client_.submit_input(0, platf::gamepad_state_t {});
      for (int attempt = 0; attempt < 60 && reader_ == INVALID_HANDLE_VALUE; ++attempt) {
        reader_ = open_pad();
        if (reader_ == INVALID_HANDLE_VALUE) std::this_thread::sleep_for(250ms);
      }
      writer_ = open_pad();
      feedback_thread_ = std::thread([this] { feedback_loop(); });
      report_thread_ = std::thread([this] { report_loop(); });
      std::this_thread::sleep_for(500ms);
      /* The first output report of a new pad always produces a haptics-state message; wait until one arrives so the
       * first probe report is not lost while the HID path is still coming up. */
      for (int attempt = 0; attempt < 20; ++attempt) {
        const auto before = mark();
        DWORD written = 0;
        report48 idle {};
        idle[0] = 2;
        if (writer_ != INVALID_HANDLE_VALUE) WriteFile(writer_, idle.data(), static_cast<DWORD>(idle.size()), &written, nullptr);
        settle(100ms, 60ms, 800ms);
        bool ready = false;
        for (const auto &message : take(before)) ready = ready || message.kind == fb_kind::state;
        if (ready) break;
      }
    }

    void
    release() {
      stop_reader_ = true;
      if (reader_ != INVALID_HANDLE_VALUE) CancelIoEx(reader_, nullptr);
      if (report_thread_.joinable()) report_thread_.join();
      if (reader_ != INVALID_HANDLE_VALUE) CloseHandle(reader_);
      if (writer_ != INVALID_HANDLE_VALUE) CloseHandle(writer_);
      reader_ = writer_ = INVALID_HANDLE_VALUE;
      if (attached_) client_.free(0);
      attached_ = false;
      /* The fade-out messages are raised while the pad is freed; keep reading until they have arrived. */
      std::this_thread::sleep_for(300ms);
      stop_feedback_ = true;
      if (feedback_thread_.joinable()) feedback_thread_.join();
    }

    void
    reset_views() {
      std::lock_guard lock(mutex_);
      messages_.clear();
      consumed_ = 0;
    }

    void
    report_loop() {
      report64 buffer {};
      while (!stop_reader_.load()) {
        DWORD bytes = 0;
        if (reader_ == INVALID_HANDLE_VALUE || !ReadFile(reader_, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) break;
        std::lock_guard lock(report_mutex_);
        last_ = buffer;
      }
    }

    report64
    latest() {
      std::lock_guard lock(report_mutex_);
      return last_;
    }

    static fb_t
    convert(const platf::gamepad_feedback_msg_t &message, bool &keep) {
      using platf::gamepad_feedback_e;
      keep = true;
      fb_t out;
      switch (message.type) {
        case gamepad_feedback_e::rumble:
          out.kind = fb_kind::rumble;
          out.low = message.data.rumble.lowfreq;
          out.high = message.data.rumble.highfreq;
          break;
        case gamepad_feedback_e::set_adaptive_triggers:
          out.kind = fb_kind::trigger;
          out.flags = message.data.adaptive_triggers.event_flags;
          out.left_type = message.data.adaptive_triggers.type_left;
          out.right_type = message.data.adaptive_triggers.type_right;
          out.left = message.data.adaptive_triggers.left;
          out.right = message.data.adaptive_triggers.right;
          break;
        case gamepad_feedback_e::set_rgb_led:
          out.kind = fb_kind::rgb;
          out.r = message.data.rgb_led.r;
          out.g = message.data.rgb_led.g;
          out.b = message.data.rgb_led.b;
          break;
        case gamepad_feedback_e::ds5_output_extension:
          out.kind = fb_kind::ext;
          out.mask = message.data.ds5_output_extension.valid_mask;
          out.player = message.data.ds5_output_extension.player_led_mask;
          out.mute = message.data.ds5_output_extension.mute_led;
          out.speaker = message.data.ds5_output_extension.speaker_volume;
          out.headset = message.data.ds5_output_extension.headphone_volume;
          out.mic = message.data.ds5_output_extension.mic_volume;
          out.mic_mute = message.data.ds5_output_extension.mic_mute;
          out.route = message.data.ds5_output_extension.audio_routing;
          out.brightness = message.data.ds5_output_extension.lightbar_brightness;
          break;
        case gamepad_feedback_e::ds5_haptics_state:
          out.kind = fb_kind::state;
          out.state = message.data.ds5_haptics_state;
          break;
        case gamepad_feedback_e::ds5_haptics_pcm: {
          out.kind = fb_kind::pcm;
          out.flags = message.data.ds5_haptics.flags;
          const auto frames = message.data.ds5_haptics.frame_count;
          out.pcm.resize(frames * 2);
          std::memcpy(out.pcm.data(), message.data.ds5_haptics.pcm.data(), frames * 4);
          break;
        }
        default:
          keep = false;
          break;
      }
      return out;
    }

    void
    feedback_loop() {
      auto queue = queue_;
      while (!stop_feedback_.load()) {
        if (auto message = queue->pop(100ms)) {
          bool keep = false;
          auto converted = convert(*message, keep);
          if (!keep) continue;
          std::lock_guard lock(mutex_);
          messages_.push_back(std::move(converted));
          last_message_ = std::chrono::steady_clock::now();
        }
      }
      while (auto message = queue->pop(50ms)) {
        bool keep = false;
        auto converted = convert(*message, keep);
        if (!keep) continue;
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(converted));
        last_message_ = std::chrono::steady_clock::now();
      }
    }

    std::size_t
    mark() {
      std::lock_guard lock(mutex_);
      /* A message that reached the queue after the last take (the host raised it a moment late) still belongs to
       * the client's state: apply it now, in arrival order, before the next step starts. */
      for (std::size_t i = seen_; i < messages_.size(); ++i) absorb(messages_[i]);
      seen_ = messages_.size();
      return messages_.size();
    }

    /** @brief Wait at least @p minimum, then until no message arrived for @p quiet, at most @p cap. */
    void
    settle(std::chrono::milliseconds minimum, std::chrono::milliseconds quiet, std::chrono::milliseconds cap) {
      const auto start = std::chrono::steady_clock::now();
      std::this_thread::sleep_for(minimum);
      while (std::chrono::steady_clock::now() - start < cap) {
        {
          std::lock_guard lock(mutex_);
          if (std::chrono::steady_clock::now() - last_message_ >= quiet) return;
        }
        std::this_thread::sleep_for(10ms);
      }
    }

    std::vector<fb_t>
    take(std::size_t from) {
      std::lock_guard lock(mutex_);
      seen_ = messages_.size();  // the caller absorbs what it takes
      return { messages_.begin() + static_cast<std::ptrdiff_t>(std::min(from, messages_.size())), messages_.end() };
    }

    /** @brief Render @p lanes (two interleaved int16 lanes) to channels 3 and 4 of the pad's audio endpoint. */
    void
    render(std::span<const std::int16_t> lanes) {
      if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
      IMMDeviceEnumerator *enumerator = nullptr;
      IMMDevice *device = nullptr;
      IAudioClient *client = nullptr;
      IAudioRenderClient *renderer = nullptr;
      WAVEFORMATEX *format = nullptr;
      if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
        IMMDeviceCollection *collection = nullptr;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
          UINT count = 0;
          collection->GetCount(&count);
          for (UINT index = 0; index < count && !device; ++index) {
            IMMDevice *candidate = nullptr;
            if (FAILED(collection->Item(index, &candidate))) continue;
            IPropertyStore *store = nullptr;
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &store)) && SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR && name.pwszVal && std::wstring(name.pwszVal).find(L"Wireless Controller") != std::wstring::npos) {
              device = candidate;
              candidate = nullptr;
            }
            PropVariantClear(&name);
            if (store) store->Release();
            if (candidate) candidate->Release();
          }
          collection->Release();
        }
      }
      if (device && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client))) &&
          SUCCEEDED(client->GetMixFormat(&format)) && format->nChannels >= 4 &&
          SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2'000'000, 0, format, nullptr)) &&
          SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&renderer)))) {
        UINT32 buffer_frames = 0;
        client->GetBufferSize(&buffer_frames);
        const bool is_float = format->wBitsPerSample == 32;
        const std::size_t total = lanes.size() / 2;
        std::size_t done = 0;
        client->Start();
        while (done < total) {
          UINT32 padding = 0;
          client->GetCurrentPadding(&padding);
          const UINT32 room = buffer_frames - padding;
          const auto frames = static_cast<UINT32>(std::min<std::size_t>(room, total - done));
          if (frames == 0) {
            std::this_thread::sleep_for(4ms);
            continue;
          }
          BYTE *target = nullptr;
          if (FAILED(renderer->GetBuffer(frames, &target))) break;
          for (UINT32 frame = 0; frame < frames; ++frame) {
            for (WORD channel = 0; channel < format->nChannels; ++channel) {
              const std::int16_t sample = channel == 2 ? lanes[(done + frame) * 2] : channel == 3 ? lanes[(done + frame) * 2 + 1] :
                                                                                                    std::int16_t { 0 };
              if (is_float) {
                reinterpret_cast<float *>(target)[frame * format->nChannels + channel] = static_cast<float>(sample) / 32768.0f;
              }
              else {
                reinterpret_cast<std::int16_t *>(target)[frame * format->nChannels + channel] = sample;
              }
            }
          }
          renderer->ReleaseBuffer(frames, 0);
          done += frames;
          std::this_thread::sleep_for(2ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(total * 1000 / 48000 + 200));
        client->Stop();
      }
      else {
        render_failures_++;
      }
      if (format) CoTaskMemFree(format);
      if (renderer) renderer->Release();
      if (client) client->Release();
      if (device) device->Release();
      if (enumerator) enumerator->Release();
      CoUninitialize();
    }

  public:
    unsigned
    render_failures() const { return render_failures_; }

  private:
    std::filesystem::path path_;
    std::unique_ptr<tuning::store_t> store_;
    std::shared_ptr<safe::mail_raw_t> mail_;
    platf::feedback_queue_t queue_;
    platf::ds5::native_client_t client_;
    bool attached_ { false };
    int attach_count_ { 0 };
    std::size_t seen_ { 0 };
    HANDLE reader_ { INVALID_HANDLE_VALUE };
    HANDLE writer_ { INVALID_HANDLE_VALUE };
    std::atomic_bool stop_reader_ { false };
    std::atomic_bool stop_feedback_ { false };
    std::thread feedback_thread_;
    std::thread report_thread_;
    std::mutex mutex_;
    std::deque<fb_t> messages_;
    std::size_t consumed_ { 0 };
    std::chrono::steady_clock::time_point last_message_ {};
    std::mutex report_mutex_;
    report64 last_ {};
    unsigned write_failures_ { 0 };
    unsigned render_failures_ { 0 };
  };
}  // namespace ds5::matrix

#endif
