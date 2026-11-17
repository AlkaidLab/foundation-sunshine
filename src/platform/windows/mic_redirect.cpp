/**
 * @file src/platform/windows/mic_redirect.cpp
 * @brief Backend choice, status and self-test of the client microphone redirection.
 *
 * Two backends carry the client microphone into Windows:
 *   usbip_experimental  the host's own virtual USB microphone (virtual_microphone_t),
 *   vb_cable            writes to the render side of an installed VB-Audio Virtual Cable.
 * `auto` tries the first and falls back to the second; `disabled` creates nothing.
 * The backend names and error codes are read by the Web UI and stay stable.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

#include "mic_write.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/virtual_microphone.h"
#include "src/platform/windows/virtual_microphone_selftest.h"

namespace platf::audio {

  using namespace std::chrono_literals;
  using namespace std::string_view_literals;

  /* Initialization, writing and release run on the microphone receive thread. */
  std::unique_ptr<mic_write_wasapi_t> mic_redirect_device;

  namespace {
    enum class mic_backend_e {
      none,
      vb_cable,
      usbip,
    };
    enum class mic_redirect_owner_e {
      none,
      session,
      test,
    };
    std::atomic<mic_backend_e> active_mic_backend { mic_backend_e::none };
    std::atomic<mic_redirect_owner_e> mic_redirect_owner { mic_redirect_owner_e::none };
    std::mutex mic_backend_status_mutex;
    std::string mic_backend_fallback_reason;
    std::unique_ptr<virtual_microphone_t> usbip_session;

    /* Level meter of the client microphone input, shared by both backends.  The
     * status reader takes the peak since its previous read, so a poll every few
     * seconds still shows that something arrived between the polls. */
    std::atomic<std::uint32_t> mic_input_peak { 0 };  // absolute sample peak, 0..32767
    std::atomic<std::int64_t> mic_input_last_ns { INT64_MIN / 2 };
    std::atomic<std::uint64_t> mic_input_frames { 0 };

    std::int64_t
    steady_now_ns() {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
    }

    bool
    wants_usbip(const std::string &backend) {
      return backend == "usbip_experimental" || backend == "auto";
    }

    /** Run the self-test of the USB microphone and map its result to the Web UI codes. */
    mic_redirect_test_result_t
    test_usbip_backend() {
      mic_redirect_test_result_t result;
      result.backend = "usbip_experimental";
      result.mode = "tone";
      const auto outcome = run_virtual_microphone_selftest();
      result.success = outcome.success;
      result.error_code = outcome.error_code;
      result.heard_seconds = outcome.heard_seconds;
      result.windows_total = outcome.windows_total;
      result.windows_good = outcome.windows_good;
      result.heard_amplitude = outcome.heard_amplitude;
      return result;
    }

    /** Take the metered input level; the peak is consumed by the reader. */
    void
    read_input_level(mic_redirect_status_t &status) {
      status.input_peak = static_cast<double>(mic_input_peak.exchange(0, std::memory_order_relaxed)) / 32768.0;
      const auto last = mic_input_last_ns.load(std::memory_order_relaxed);
      status.input_active_ms = last == INT64_MIN / 2 ? -1 : (steady_now_ns() - last) / 1000000;
      status.input_frames = mic_input_frames.load(std::memory_order_relaxed);
    }
  }  // namespace

  void
  report_mic_redirect_backend(std::string active_backend, std::string fallback_reason) {
    if (active_backend == "usbip_experimental") {
      active_mic_backend = mic_backend_e::usbip;
    }
    else if (active_backend == "vb_cable") {
      active_mic_backend = mic_backend_e::vb_cable;
    }
    else {
      active_mic_backend = mic_backend_e::none;
    }
    std::lock_guard lock(mic_backend_status_mutex);
    mic_backend_fallback_reason = std::move(fallback_reason);
  }

  bool
  try_begin_mic_redirect_test() {
    auto expected = mic_redirect_owner_e::none;
    return mic_redirect_owner.compare_exchange_strong(expected, mic_redirect_owner_e::test);
  }

  void
  end_mic_redirect_test() {
    auto expected = mic_redirect_owner_e::test;
    mic_redirect_owner.compare_exchange_strong(expected, mic_redirect_owner_e::none);
  }

  mic_redirect_status_t
  mic_redirect_status() {
    mic_redirect_status_t result;
    result.configured_backend = config::audio.microphone_redirect_backend;
    switch (active_mic_backend.load()) {
      case mic_backend_e::usbip:
        result.active_backend = "usbip_experimental";
        break;
      case mic_backend_e::vb_cable:
        result.active_backend = "vb_cable";
        break;
      default:
        break;
    }
    {
      std::lock_guard lock(mic_backend_status_mutex);
      result.fallback_reason = mic_backend_fallback_reason;
    }
    read_input_level(result);

    const auto usbip = virtual_microphone_t::current_status();
    result.component_available = usbip.component_available;
    if (result.active_backend == "usbip_experimental" ||
        (result.active_backend.empty() && (usbip.device_created || !usbip.error_code.empty()))) {
      result.online = usbip.online;
      result.device_created = usbip.device_created;
      result.host_streaming = usbip.host_streaming;
      result.generation = usbip.generation;
      result.buffered_bytes = usbip.buffered_bytes;
      result.underruns = usbip.underruns;
      result.dropped_frames = usbip.dropped_frames;
      result.submit_errors = usbip.submit_errors;
      result.last_error = usbip.last_error;
      result.state = usbip.state;
      result.error_code = usbip.error_code;
    }
    else if (result.active_backend == "vb_cable") {
      result.online = true;
      result.device_created = true;
      result.state = "ready";
    }
    return result;
  }

  int
  init_mic_redirect() {
    const auto &backend = config::audio.microphone_redirect_backend;
    std::string usbip_fallback_reason;
    if (backend == "disabled") {
      report_mic_redirect_backend({}, "MIC_BACKEND_DISABLED");
      BOOST_LOG(info) << "Client microphone redirection backend is disabled"sv;
      return -1;
    }
    auto expected_owner = mic_redirect_owner_e::none;
    if (!mic_redirect_owner.compare_exchange_strong(expected_owner, mic_redirect_owner_e::session)) {
      BOOST_LOG(warning) << "Microphone redirect path is already in use"sv;
      return -1;
    }
    auto release_owner = [] {
      auto owner = mic_redirect_owner_e::session;
      mic_redirect_owner.compare_exchange_strong(owner, mic_redirect_owner_e::none);
    };

    if (wants_usbip(backend)) {
      auto session = std::make_unique<virtual_microphone_t>();
      usbip_fallback_reason = session->start();
      if (usbip_fallback_reason.empty()) {
        usbip_session = std::move(session);
        report_mic_redirect_backend("usbip_experimental");
        BOOST_LOG(info) << "Using the USB/IP virtual microphone backend"sv;
        return 0;
      }
      if (backend == "usbip_experimental") {
        report_mic_redirect_backend({}, usbip_fallback_reason);
        BOOST_LOG(error) << "The USB/IP virtual microphone backend failed: "sv << usbip_fallback_reason;
        release_owner();
        return -1;
      }
      BOOST_LOG(warning) << "USB/IP virtual microphone unavailable ("sv << usbip_fallback_reason
                         << "); trying VB-Cable"sv;
    }

    if (!mic_redirect_device) {
      mic_redirect_device = std::make_unique<mic_write_wasapi_t>();
    }
    if (mic_redirect_device->init() != 0) {
      BOOST_LOG(warning) << "Failed to initialize client mic redirection device";
      mic_redirect_device.reset();
      report_mic_redirect_backend({}, backend == "auto" && !usbip_fallback_reason.empty() ? usbip_fallback_reason : "MIC_TEST_DEVICE_UNAVAILABLE");
      release_owner();
      return -1;
    }
    report_mic_redirect_backend("vb_cable", usbip_fallback_reason);
    BOOST_LOG(info) << "Successfully initialized client mic redirection device";
    return 0;
  }

  void
  release_mic_redirect() {
    if (usbip_session) {
      usbip_session->stop();
      usbip_session.reset();
    }
    if (mic_redirect_device) {
      mic_redirect_device->restore_audio_devices();
      mic_redirect_device.reset();
    }
    report_mic_redirect_backend({});
    auto expected_owner = mic_redirect_owner_e::session;
    mic_redirect_owner.compare_exchange_strong(expected_owner, mic_redirect_owner_e::none);
  }

  int
  write_mic_redirect_pcm(const std::int16_t *samples, std::size_t frame_count) {
    if (samples != nullptr && frame_count != 0) {
      std::int16_t peak = 0;
      for (std::size_t i = 0; i < frame_count; ++i) {
        int value = std::abs(static_cast<int>(samples[i]));
        if (value > 32767) value = 32767;  // abs(INT16_MIN) does not fit back into int16_t
        if (value > peak) peak = static_cast<std::int16_t>(value);
      }
      auto known = mic_input_peak.load(std::memory_order_relaxed);
      while (peak > known && !mic_input_peak.compare_exchange_weak(known, static_cast<std::uint32_t>(peak), std::memory_order_relaxed)) {}
      mic_input_last_ns.store(steady_now_ns(), std::memory_order_relaxed);
      mic_input_frames.fetch_add(frame_count, std::memory_order_relaxed);
    }
    if (usbip_session) return usbip_session->write_pcm(samples, frame_count);
    if (!mic_redirect_device || mic_redirect_device->is_cleaning_up.load()) {
      BOOST_LOG(warning) << "Mic redirect device not available or cleaning up";
      return -1;
    }
    return mic_redirect_device->write_pcm(samples, frame_count);
  }

  mic_redirect_test_result_t
  test_mic_redirect() {
    static std::mutex test_mutex;
    const std::lock_guard lock(test_mutex);

    const auto backend = config::audio.microphone_redirect_backend;
    if (mic_redirect_owner.load() == mic_redirect_owner_e::session) {
      /* A stream owns the microphone path: a second device or a test tone would
       * fight the live session, so the test reports the live route and level. */
      mic_redirect_test_result_t result;
      result.mode = "live";
      const auto live = mic_redirect_status();
      result.backend = live.active_backend;
      result.input_peak = live.input_peak;
      result.input_active_ms = live.input_active_ms;
      result.success = !live.active_backend.empty();
      if (!result.success) result.error_code = live.error_code.empty() ? "MIC_TEST_DEVICE_UNAVAILABLE" : live.error_code;
      return result;
    }
    if (!try_begin_mic_redirect_test()) {
      mic_redirect_test_result_t busy;
      busy.error_code = "MIC_TEST_BUSY";
      busy.backend = mic_redirect_status().active_backend;
      return busy;
    }
    struct test_reservation_t {
      ~test_reservation_t() {
        end_mic_redirect_test();
      }
    } test_reservation;
    if (backend == "disabled") {
      report_mic_redirect_backend({}, "MIC_BACKEND_DISABLED");
      return { false, "MIC_BACKEND_DISABLED", "disabled" };
    }

    std::string usbip_error;
    if (wants_usbip(backend)) {
      auto usbip = test_usbip_backend();
      if (usbip.success || backend == "usbip_experimental") {
        report_mic_redirect_backend(usbip.success ? "usbip_experimental" : "", usbip.success ? "" : usbip.error_code);
        return usbip;
      }
      usbip_error = usbip.error_code;
    }

    const auto com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
      report_mic_redirect_backend({}, "MIC_TEST_COM_FAILED");
      return { false, "MIC_TEST_COM_FAILED", "vb_cable" };
    }

    mic_redirect_test_result_t result;
    {
      mic_write_wasapi_t test_device;
      result.mode = "tone";
      if (test_device.init(true) != 0) {
        result.error_code = "MIC_TEST_DEVICE_UNAVAILABLE";
      }
      else if (test_device.test_write() <= 0) {
        result.error_code = "MIC_TEST_WRITE_FAILED";
      }
      else {
        result.success = true;
      }
      result.backend = "vb_cable";
      report_mic_redirect_backend(result.success ? "vb_cable" : "", result.success ? usbip_error : result.error_code);
    }

    if (SUCCEEDED(com_status)) {
      CoUninitialize();
    }
    return result;
  }

}  // namespace platf::audio
