/**
 * @file src/platform/windows/virtual_microphone.cpp
 * @brief Session of the host's virtual microphone (see the header).
 */
#include "virtual_microphone.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <windows.h>

#include <boost/log/trivial.hpp>

#include "src/logging_severity.h"
#include "src/platform/windows/audio_default_guard.h"
#include "src/platform/windows/usbip_device_transport.h"
#include "src/virtual_usb/uac1_microphone.h"

namespace platf::audio {

  namespace {
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    /* The endpoint's adapter name is the product string of the device. */
    constexpr std::wstring_view kProductName = L"Sunshine Virtual Microphone";

    std::atomic<std::uint32_t> next_generation { 1 };

    /* One session per process, found by the status endpoint without a lock on the audio path. */
    std::mutex registry_mutex;
    virtual_microphone_t *registered_session = nullptr;
    virtual_microphone_status last_failure;
    bool have_failure = false;

    bool
    usbip_helper_present() {
      if (SearchPathW(nullptr, L"usbip.exe", nullptr, 0, nullptr, nullptr) != 0) return true;
      /* The usbip-win2 installer does not add its directory to the PATH of the service account;
       * the host controller that runs the attach also probes the machine-wide install directory. */
      for (const wchar_t *variable : { L"ProgramW6432", L"ProgramFiles" }) {
        wchar_t base[MAX_PATH];
        const auto length = GetEnvironmentVariableW(variable, base, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) continue;
        const auto attributes = GetFileAttributesW((std::wstring(base) + L"\\USBip\\usbip.exe").c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return true;
      }
      return false;
    }
  }  // namespace

  struct virtual_microphone_t::impl {
    std::unique_ptr<virtual_usb::uac1_microphone> model;
    std::unique_ptr<usbip_device_transport> transport;
    std::thread guard;
    std::mutex guard_mutex;
    std::condition_variable guard_wake;
    bool guard_cancel { false };
    HANDLE host_lock { nullptr };
    std::atomic<bool> up { false };
    std::atomic<int> phase { 0 };  // 0 absent, 1 creating, 2 live, 3 destroying
    std::uint32_t generation { 0 };
    std::string error_code;
    std::int32_t last_error { 0 };

    void
    release_lock() {
      if (host_lock) {
        ReleaseMutex(host_lock);
        CloseHandle(host_lock);
        host_lock = nullptr;
      }
    }

    /** One virtual microphone per host: a named mutex shared by every process of the machine. */
    bool
    take_host_lock() {
      host_lock = CreateMutexW(nullptr, FALSE, L"Global\\SunshineVirtualMicrophone");
      if (!host_lock) host_lock = CreateMutexW(nullptr, FALSE, L"Local\\SunshineVirtualMicrophone");
      if (!host_lock) return false;
      /* WAIT_ABANDONED means a previous holder died; its stale import is swept by the attach. */
      const auto wait = WaitForSingleObject(host_lock, 0);
      if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return true;
      CloseHandle(host_lock);
      host_lock = nullptr;
      return false;
    }
  };

  virtual_microphone_t::virtual_microphone_t():
      impl_(std::make_unique<impl>()) {}

  virtual_microphone_t::~virtual_microphone_t() { stop(); }

  bool
  virtual_microphone_t::running() const noexcept { return impl_->up.load(std::memory_order_acquire); }

  std::string
  virtual_microphone_t::start() {
    auto &s = *impl_;
    if (s.up.load()) return {};
    s.error_code.clear();
    s.last_error = 0;
    s.generation = next_generation.fetch_add(1);
    s.phase = 1;

    auto fail = [this, &s](std::string code) {
      s.error_code = code;
      s.phase = 0;
      stop();
      virtual_microphone_status failure;
      failure.component_available = usbip_helper_present();
      failure.generation = s.generation;
      failure.state = "absent";
      failure.error_code = code;
      failure.last_error = s.last_error;
      std::lock_guard lock(registry_mutex);
      last_failure = failure;
      have_failure = true;
      return code;
    };

    if (!s.take_host_lock()) {
      BOOST_LOG(warning) << "The virtual microphone is already in use on this host"sv;
      return fail("MIC_USBIP_DEVICE_BUSY");
    }
    if (!usbip_helper_present()) {
      BOOST_LOG(warning) << "usbip.exe was not found; the virtual microphone needs the usbip-win2 host component"sv;
      return fail("MIC_USBIP_COMPONENT_UNAVAILABLE");
    }

    s.model = std::make_unique<virtual_usb::uac1_microphone>();
    s.transport = std::make_unique<usbip_device_transport>();
    if (!s.transport->available()) return fail("MIC_USBIP_COMPONENT_UNAVAILABLE");

    std::vector<default_endpoint> defaults;
    if (auto captured = capture_default_endpoints()) defaults = std::move(*captured);

    if (!s.transport->attach(*s.model, 15s)) {
      const auto status = s.transport->status();
      s.last_error = status.error;
      BOOST_LOG(error) << "Virtual microphone attach failed: "sv << status.detail << " (code "sv << status.error << ')';
      switch (static_cast<usbip_transport_error>(status.error)) {
        case usbip_transport_error::unsupported:
          return fail("MIC_USBIP_COMPONENT_UNAVAILABLE");
        case usbip_transport_error::attach_timed_out:
          return fail("MIC_USBIP_START_TIMEOUT");
        default:
          return fail("MIC_USBIP_CREATE_FAILED");
      }
    }

    /* The class driver publishes the endpoints after the attach; put back any default that moved. */
    s.guard_cancel = false;
    s.guard = std::thread([&s, defaults = std::move(defaults)] {
      std::unique_lock lock(s.guard_mutex);
      for (const auto delay : { 0ms, 250ms, 1000ms, 2500ms, 5000ms }) {
        if (s.guard_wake.wait_for(lock, delay, [&s] { return s.guard_cancel; })) return;
        lock.unlock();
        (void) restore_default_endpoints(defaults, { kProductName });
        lock.lock();
      }
    });

    s.phase = 2;
    s.up.store(true, std::memory_order_release);
    {
      std::lock_guard lock(registry_mutex);
      registered_session = this;
      have_failure = false;
    }
    BOOST_LOG(info) << "Virtual microphone created (generation "sv << s.generation << ')';
    return {};
  }

  void
  virtual_microphone_t::stop() {
    auto &s = *impl_;
    {
      std::lock_guard lock(registry_mutex);
      if (registered_session == this) registered_session = nullptr;
    }
    if (!s.up.exchange(false) && !s.transport && !s.host_lock && !s.guard.joinable()) return;
    s.phase = 3;
    {
      std::lock_guard lock(s.guard_mutex);
      s.guard_cancel = true;
    }
    s.guard_wake.notify_all();
    if (s.guard.joinable()) s.guard.join();
    if (s.transport) s.transport->detach();
    s.transport.reset();
    s.model.reset();
    s.release_lock();
    s.phase = 0;
    BOOST_LOG(info) << "Virtual microphone removed"sv;
  }

  int
  virtual_microphone_t::write_pcm(const std::int16_t *samples, std::size_t frame_count) noexcept {
    auto &s = *impl_;
    if (!s.up.load(std::memory_order_acquire) || !s.model) return -1;
    return static_cast<int>(s.model->write_pcm(samples, frame_count) * sizeof(std::int16_t));
  }

  void
  virtual_microphone_t::flush() noexcept {
    if (impl_->up.load(std::memory_order_acquire) && impl_->model) impl_->model->flush();
  }

  bool
  virtual_microphone_t::host_capturing() const noexcept {
    return impl_->up.load(std::memory_order_acquire) && impl_->model && impl_->model->stats().capturing;
  }

  virtual_microphone_status
  virtual_microphone_t::status() const {
    const auto &s = *impl_;
    virtual_microphone_status out;
    out.component_available = usbip_helper_present();
    out.generation = s.generation;
    out.error_code = s.error_code;
    out.last_error = s.last_error;
    switch (s.phase.load()) {
      case 1:
        out.state = "creating";
        out.device_created = false;
        return out;
      case 3:
        out.state = "destroying";
        out.device_created = true;
        return out;
      case 2:
        break;
      default:
        return out;
    }
    if (!s.model || !s.transport) return out;
    const auto bus = s.transport->status();
    const auto stats = s.model->stats();
    const auto counters = s.transport->counters();
    out.device_created = true;
    out.online = bus.state == virtual_usb::transport_state::attached;
    out.host_streaming = stats.capturing;
    out.buffered_bytes = stats.buffered_frames * 2;
    out.underruns = stats.underruns;
    out.dropped_frames = static_cast<std::uint32_t>(stats.frames_dropped + stats.frames_trimmed);
    out.submit_errors = static_cast<std::uint32_t>(counters.protocol_errors);
    if (bus.state == virtual_usb::transport_state::failed) {
      out.state = "device_faulted";
      out.last_error = bus.error;
      out.error_code = "MIC_USBIP_TRANSPORT_LOST";
    }
    else if (!stats.configured) {
      out.state = "enumerating";
    }
    else if (stats.capturing && stats.remote_active) {
      out.state = "remote_active";
    }
    else if (stats.capturing) {
      out.state = "host_capturing";
    }
    else {
      out.state = "idle";
    }
    return out;
  }

  virtual_usb::microphone_stats
  virtual_microphone_t::model_stats() const {
    return impl_->model ? impl_->model->stats() : virtual_usb::microphone_stats {};
  }

  virtual_microphone_status
  virtual_microphone_t::current_status() {
    std::lock_guard lock(registry_mutex);
    if (registered_session != nullptr) return registered_session->status();
    if (have_failure) return last_failure;
    virtual_microphone_status absent;
    absent.component_available = usbip_helper_present();
    return absent;
  }

}  // namespace platf::audio
