/**
 * @file src/platform/windows/virtual_mouse.cpp
 * @brief Zako Virtual Mouse client implementation.
 *
 * Finds and opens the Zako Virtual Mouse HID device by matching
 * VID/PID, then sends mouse data via HID output reports.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// HID headers
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "virtual_mouse.h"
#include "src/logging.h"
#include "src/platform/common.h"

// From vmouse_shared.h - duplicated here to avoid driver header dependency
static constexpr uint16_t VMOUSE_VID = 0x1ACE;
static constexpr uint16_t VMOUSE_PID = 0x0002;
static constexpr uint8_t VMOUSE_OUTPUT_REPORT_ID = 0x02;
static constexpr uint16_t VMOUSE_OUTPUT_REPORT_SIZE = 8;

namespace {
#ifdef SUNSHINE_VIRTUAL_MOUSE_STANDALONE_TEST
  struct null_log_t {
    template<typename T>
    null_log_t &
    operator<<(T &&) {
      return *this;
    }
  };

  null_log_t &
  null_log() {
    static null_log_t logger;
    return logger;
  }

  #define VMOUSE_LOG(severity) null_log()
#else
  #define VMOUSE_LOG(severity) BOOST_LOG(severity)
#endif

  /**
   * @brief Convert a wide string to a narrow (UTF-8) string for logging.
   */
  std::string
  wide_to_utf8(const wchar_t *wstr) {
    if (!wstr) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
  }

  bool
  query_caps(HANDLE device, HIDP_CAPS &caps) {
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(device, &preparsed)) {
      return false;
    }

    const auto status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    return status == HIDP_STATUS_SUCCESS;
  }

  bool
  is_vmouse_device(const HIDD_ATTRIBUTES &attrs, const HIDP_CAPS &caps) {
    return attrs.VendorID == VMOUSE_VID &&
           attrs.ProductID == VMOUSE_PID &&
           caps.FeatureReportByteLength >= VMOUSE_OUTPUT_REPORT_SIZE;
  }
}  // namespace

using namespace std::literals;

namespace platf {
  namespace vmouse {
    namespace detail {
      output_report_t
      build_output_report(uint8_t buttons, int16_t delta_x, int16_t delta_y,
                          int8_t scroll_v, int8_t scroll_h) {
        return output_report_t {
          VMOUSE_OUTPUT_REPORT_ID,
          buttons,
          static_cast<uint8_t>(delta_x & 0xFF),
          static_cast<uint8_t>((delta_x >> 8) & 0xFF),
          static_cast<uint8_t>(delta_y & 0xFF),
          static_cast<uint8_t>((delta_y >> 8) & 0xFF),
          static_cast<uint8_t>(scroll_v),
          static_cast<uint8_t>(scroll_h),
        };
      }

      uint8_t
      apply_button_transition(uint8_t current_buttons, uint8_t button_mask,
                              bool release) {
        return release ? static_cast<uint8_t>(current_buttons & ~button_mask) :
                         static_cast<uint8_t>(current_buttons | button_mask);
      }

      bool
      should_close_on_write_error(unsigned long err) {
        return err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_GEN_FAILURE;
      }
    }  // namespace detail

    // ========================================================================
    // Implementation Detail
    // ========================================================================

    // Flush interval for accumulated mouse movement.
    // Combined with deadline-based scheduling, 2ms is enough to push toward
    // the current UMDF/HID path limit without adding an extra sleep after each
    // HidD_SetFeature() call.
    static constexpr auto FLUSH_INTERVAL = std::chrono::milliseconds(2);

    namespace {
      int16_t
      clamp_relative_delta(int32_t delta) {
        return static_cast<int16_t>(std::clamp(delta, -32767, 32767));
      }
    }  // namespace

    struct device_t::impl_t {
      HANDLE hDevice = INVALID_HANDLE_VALUE;

      // Shared state for accumulated mouse deltas and current button state.
      std::mutex state_mutex;
      std::condition_variable wake_cv;
      uint8_t buttonState = 0;
      int32_t accum_dx = 0;
      int32_t accum_dy = 0;
      bool accum_dirty = false;
      std::atomic<bool> stop_requested { false };

      // Serialize writes to the HID handle so the flush thread and synchronous
      // button/scroll paths don't race each other.
      std::mutex send_mutex;

      // Throttle reopen attempts after the device disconnects (e.g. driver
      // re-enumeration via `pnputil /remove-device` + `devcon install`).
      // Without this, streaming would be permanently degraded to SendInput
      // until the Sunshine service is restarted manually.
      std::chrono::steady_clock::time_point last_reopen_attempt {};
      static constexpr auto REOPEN_RETRY_INTERVAL = std::chrono::seconds(2);

      // Flush thread and timer.
      std::thread flush_thread;
      std::unique_ptr<high_precision_timer> flush_timer = create_high_precision_timer();

      ~impl_t() {
        stop_flush_thread();
        close();
      }

      void
      stop_flush_thread() {
        stop_requested.store(true, std::memory_order_release);
        wake_cv.notify_one();
        if (flush_thread.joinable()) {
          flush_thread.join();
        }
      }

      void
      start_flush_thread() {
        // Guard: if a flush thread is already running (typical for the
        // reopen-from-flush-thread path), do nothing. Replacing the
        // std::thread object while the old one is joinable would call
        // std::terminate() and crash the process.
        if (flush_thread.joinable()) {
          return;
        }
        stop_requested.store(false, std::memory_order_release);
        flush_thread = std::thread([this]() {
          adjust_thread_priority(thread_priority_e::high);

          bool active = false;
          auto next_deadline = std::chrono::steady_clock::time_point {};

          while (!stop_requested.load(std::memory_order_acquire)) {
            if (!active) {
              std::unique_lock<std::mutex> lk(state_mutex);
              // Wait with timeout so we can periodically poll for device
              // re-arrival even when there is no pending input. Without this,
              // an unplug/reinstall while the client has no input activity
              // would leave the handle closed forever.
              wake_cv.wait_for(lk, REOPEN_RETRY_INTERVAL, [this]() {
                return stop_requested.load(std::memory_order_acquire) || accum_dirty;
              });

              if (stop_requested.load(std::memory_order_acquire)) {
                break;
              }

              // Periodic re-acquire / liveness path: if device handle is closed,
              // try to reopen even without input. If handle is open, query
              // preparsed data as a read-only liveness ping so we can detect
              // a silent driver removal and close the stale handle.
              // Throttled by REOPEN_RETRY_INTERVAL.
              //
              // Drop state_mutex before taking send_mutex: every public API
              // (move/button/scroll) takes state_mutex first and only then
              // touches send_mutex via sendReportDirect, so reversing the
              // order here would risk a future deadlock if any caller starts
              // holding both at the same time.
              if (!accum_dirty) {
                lk.unlock();
                std::lock_guard<std::mutex> send_lk(send_mutex);
                const auto now = std::chrono::steady_clock::now();
                if (now - last_reopen_attempt >= REOPEN_RETRY_INTERVAL) {
                  last_reopen_attempt = now;
                  if (hDevice == INVALID_HANDLE_VALUE) {
                    if (open()) {
                      VMOUSE_LOG(info) << "vmouse: Re-acquired virtual mouse device after disconnect (proactive)"sv;
                    }
                  }
                  else {
                    PHIDP_PREPARSED_DATA pp = nullptr;
                    if (!HidD_GetPreparsedData(hDevice, &pp)) {
                      // Treat any failure as device gone (the only documented
                      // way this fails on a still-attached device is OOM).
                      VMOUSE_LOG(warning) << "vmouse: Liveness ping failed, closing stale handle"sv;
                      close();
                      last_reopen_attempt = {};
                    }
                    else {
                      HidD_FreePreparsedData(pp);
                    }
                  }
                }
                continue;
              }

              active = true;
              next_deadline = std::chrono::steady_clock::now() + FLUSH_INTERVAL;
            }

            const auto now = std::chrono::steady_clock::now();
            if (next_deadline > now) {
              const auto sleep_period = next_deadline - now;
              if (flush_timer && static_cast<bool>(*flush_timer)) {
                flush_timer->sleep_for(sleep_period);
              }
              else {
                std::this_thread::sleep_for(sleep_period);
              }
            }

            if (stop_requested.load(std::memory_order_acquire)) {
              break;
            }

            flush_accumulated();
            next_deadline += FLUSH_INTERVAL;

            // Catch-up clamp: if HidD_SetFeature stalled (it can take >2ms,
            // and the send_mutex may also block on a synchronous button() /
            // scroll() call), next_deadline can fall arbitrarily far behind
            // wall clock, which would otherwise cause an unbounded burst of
            // back-to-back reports once the stall clears. Clamp the deadline
            // to one interval ahead of now so we resync with the wall clock
            // and pace at FLUSH_INTERVAL again.
            const auto after_send = std::chrono::steady_clock::now();
            if (next_deadline + FLUSH_INTERVAL < after_send) {
              next_deadline = after_send + FLUSH_INTERVAL;
            }

            std::lock_guard<std::mutex> lk(state_mutex);
            active = accum_dirty;
          }
        });
      }

      bool
      take_accumulated_report(
          uint8_t &buttons,
          int16_t &dx,
          int16_t &dy) {
        std::lock_guard<std::mutex> lk(state_mutex);
        if (!accum_dirty) {
          return false;
        }

        buttons = buttonState;
        dx = clamp_relative_delta(accum_dx);
        dy = clamp_relative_delta(accum_dy);
        accum_dx = 0;
        accum_dy = 0;
        accum_dirty = false;
        return true;
      }

      bool
      flush_accumulated() {
        uint8_t buttons;
        int16_t dx;
        int16_t dy;
        if (!take_accumulated_report(buttons, dx, dy)) {
          return false;
        }

        return sendReportDirect(buttons, dx, dy, 0, 0);
      }

      void
      close() {
        if (hDevice != INVALID_HANDLE_VALUE) {
          CloseHandle(hDevice);
          hDevice = INVALID_HANDLE_VALUE;
        }
      }

      /**
       * @brief Find and open the virtual mouse HID device.
       *
       * Enumerates all HID devices and finds the one matching our VID/PID.
       * Opens it for writing (to send output reports).
       */
      bool
      open() {
        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO devInfoSet = SetupDiGetClassDevsW(
            &hidGuid, NULL, NULL,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

        if (devInfoSet == INVALID_HANDLE_VALUE) {
          VMOUSE_LOG(debug) << "vmouse: SetupDiGetClassDevs failed"sv;
          return false;
        }

        bool found = false;
        SP_DEVICE_INTERFACE_DATA devInterfaceData;
        devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

        for (DWORD i = 0;
             SetupDiEnumDeviceInterfaces(devInfoSet, NULL, &hidGuid, i, &devInterfaceData);
             i++) {
          // Get required buffer size
          DWORD requiredSize = 0;
          SetupDiGetDeviceInterfaceDetailW(devInfoSet, &devInterfaceData, NULL, 0, &requiredSize, NULL);
          if (requiredSize == 0) continue;

          // Allocate and get detail
          auto detailBuf = std::make_unique<BYTE[]>(requiredSize);
          auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.get());
          detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

          if (!SetupDiGetDeviceInterfaceDetailW(
                  devInfoSet, &devInterfaceData, detail, requiredSize, NULL, NULL)) {
            continue;
          }

          // Open with no access first so we can inspect attributes even if
          // the device only allows write access for output reports.
          HANDLE h = CreateFileW(
              detail->DevicePath,
              0,
              FILE_SHARE_READ | FILE_SHARE_WRITE,
              NULL,
              OPEN_EXISTING,
              0,
              NULL);

          if (h == INVALID_HANDLE_VALUE) continue;

          // Check if this is our virtual mouse device.
          HIDD_ATTRIBUTES attrs;
          attrs.Size = sizeof(HIDD_ATTRIBUTES);
          if (HidD_GetAttributes(h, &attrs)) {
            HIDP_CAPS caps;
            if (query_caps(h, caps) && is_vmouse_device(attrs, caps)) {
              hDevice = h;
              found = true;

              VMOUSE_LOG(info) << "vmouse: Found virtual mouse device at "sv
                               << wide_to_utf8(detail->DevicePath);
              break;
            }
          }

          CloseHandle(h);
        }

        SetupDiDestroyDeviceInfoList(devInfoSet);

        if (!found) {
          VMOUSE_LOG(debug) << "vmouse: Virtual mouse device not found (VID="sv
                            << std::hex << VMOUSE_VID
                            << " PID="sv << VMOUSE_PID << ")"sv;
        }

        if (found) {
          start_flush_thread();
        }

        return found;
      }

      /**
       * @brief Directly send an output report to the virtual mouse driver.
       * Called from the flush thread or for non-movement reports.
       */
      bool
      sendReportDirect(uint8_t buttons, int16_t dx, int16_t dy, int8_t sv, int8_t sh) {
        std::lock_guard<std::mutex> send_lk(send_mutex);

        // If the handle was closed (device disconnected, driver reinstalled),
        // try to re-open at most once every REOPEN_RETRY_INTERVAL so a
        // back-to-back stream of input events doesn't enumerate HID devices on
        // every call.
        if (hDevice == INVALID_HANDLE_VALUE) {
          const auto now = std::chrono::steady_clock::now();
          if (now - last_reopen_attempt >= REOPEN_RETRY_INTERVAL) {
            last_reopen_attempt = now;
            if (open()) {
              VMOUSE_LOG(info) << "vmouse: Re-acquired virtual mouse device after disconnect"sv;
            }
          }
          if (hDevice == INVALID_HANDLE_VALUE) return false;
        }

        auto report = detail::build_output_report(buttons, dx, dy, sv, sh);

        BOOL result = HidD_SetFeature(hDevice, (PVOID) report.data(), static_cast<ULONG>(report.size()));

        if (!result) {
          DWORD err = GetLastError();
          if (detail::should_close_on_write_error(err)) {
            VMOUSE_LOG(warning) << "vmouse: Device disconnected, closing handle (will retry)"sv;
            close();
            // Do NOT stop the flush thread here: keeping it alive lets queued
            // mouse movement be flushed automatically once the device is back.
            //
            // Zero-latency recovery: try to reopen and resend immediately so
            // the current input event isn't dropped. The new PDO is usually
            // already enumerable by the time HidD_SetFeature returns the
            // error. Bookkeep last_reopen_attempt so the flush-thread
            // heartbeat doesn't re-attempt within REOPEN_RETRY_INTERVAL.
            last_reopen_attempt = std::chrono::steady_clock::now();
            if (open()) {
              VMOUSE_LOG(info) << "vmouse: Re-acquired virtual mouse device after disconnect (immediate)"sv;
              if (HidD_SetFeature(hDevice, (PVOID) report.data(), static_cast<ULONG>(report.size()))) {
                return true;
              }
              // Reopen succeeded but write still failed — fall through and let
              // future calls retry. Don't close again; the handle may still be
              // valid, just temporarily refusing.
            }
          }
          return false;
        }

        return true;
      }
    };

    // ========================================================================
    // device_t Methods
    // ========================================================================

    device_t::device_t(): impl(std::make_unique<impl_t>()) {}
    device_t::~device_t() = default;
    device_t::device_t(device_t &&other) noexcept = default;
    device_t &device_t::operator=(device_t &&other) noexcept = default;

    bool
    device_t::is_available() const {
      return impl && impl->hDevice != INVALID_HANDLE_VALUE;
    }

    bool
    device_t::move(int16_t delta_x, int16_t delta_y) {
      if (!impl) {
        return false;
      }

      // Even if hDevice is currently INVALID (driver disconnect / re-install),
      // we keep accumulating so that when the flush thread or the next
      // button/scroll call re-opens the device, the latest deltas are flushed
      // out instead of getting silently dropped.
      bool should_notify = false;
      {
        std::lock_guard<std::mutex> lk(impl->state_mutex);
        should_notify = !impl->accum_dirty;
        impl->accum_dx += delta_x;
        impl->accum_dy += delta_y;
        impl->accum_dirty = true;
      }

      if (should_notify) {
        impl->wake_cv.notify_one();
      }

      return true;
    }

    bool
    device_t::button(uint8_t button_mask, bool release) {
      uint8_t buttons;
      int16_t dx = 0;
      int16_t dy = 0;

      {
        std::lock_guard<std::mutex> lk(impl->state_mutex);
        impl->buttonState = detail::apply_button_transition(impl->buttonState, button_mask, release);
        buttons = impl->buttonState;

        if (impl->accum_dirty) {
          dx = clamp_relative_delta(impl->accum_dx);
          dy = clamp_relative_delta(impl->accum_dy);
          impl->accum_dx = 0;
          impl->accum_dy = 0;
          impl->accum_dirty = false;
        }
      }

      // Single combined report: a HID mouse input report carries buttons +
      // movement together, so we don't need a separate button-only follow-up.
      // If movement was pending, fold it into this same report.
      //
      // Note: on send failure we do NOT re-queue dx/dy. The handle teardown
      // path inside sendReportDirect already attempts an immediate reopen +
      // resend, so a single dropped delta is rare. Re-queuing under the
      // state_mutex here would also race with movement that arrived between
      // the take above and the failure, producing out-of-order accumulation.
      return impl->sendReportDirect(buttons, dx, dy, 0, 0);
    }

    bool
    device_t::scroll(int8_t distance) {
      uint8_t buttons;
      {
        std::lock_guard<std::mutex> lk(impl->state_mutex);
        buttons = impl->buttonState;
      }
      return impl->sendReportDirect(buttons, 0, 0, distance, 0);
    }

    bool
    device_t::hscroll(int8_t distance) {
      uint8_t buttons;
      {
        std::lock_guard<std::mutex> lk(impl->state_mutex);
        buttons = impl->buttonState;
      }
      return impl->sendReportDirect(buttons, 0, 0, 0, distance);
    }

    bool
    device_t::send_report(uint8_t buttons, int16_t delta_x, int16_t delta_y,
                          int8_t scroll_v, int8_t scroll_h) {
      {
        std::lock_guard<std::mutex> lk(impl->state_mutex);
        impl->buttonState = buttons;
      }
      return impl->sendReportDirect(buttons, delta_x, delta_y, scroll_v, scroll_h);
    }

    // ========================================================================
    // Factory Functions
    // ========================================================================

    device_t
    create() {
      device_t dev;
      if (!dev.impl->open()) {
        VMOUSE_LOG(info) << "vmouse: Virtual mouse driver not available, "
                            "falling back to SendInput"sv;
      }
      return dev;
    }

    bool
    is_driver_installed() {
      GUID hidGuid;
      HidD_GetHidGuid(&hidGuid);

      HDEVINFO devInfoSet = SetupDiGetClassDevsW(
          &hidGuid, NULL, NULL,
          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

      if (devInfoSet == INVALID_HANDLE_VALUE) return false;

      bool found = false;
      SP_DEVICE_INTERFACE_DATA devInterfaceData;
      devInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

      for (DWORD i = 0;
           SetupDiEnumDeviceInterfaces(devInfoSet, NULL, &hidGuid, i, &devInterfaceData);
           i++) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfoSet, &devInterfaceData, NULL, 0, &requiredSize, NULL);
        if (requiredSize == 0) continue;

        auto detailBuf = std::make_unique<BYTE[]>(requiredSize);
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.get());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(
                devInfoSet, &devInterfaceData, detail, requiredSize, NULL, NULL)) {
          continue;
        }

        HANDLE h = CreateFileW(
            detail->DevicePath,
            0,  // No access needed, just check attributes
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL);

        if (h == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs;
        attrs.Size = sizeof(HIDD_ATTRIBUTES);
        if (HidD_GetAttributes(h, &attrs)) {
          HIDP_CAPS caps;
          if (query_caps(h, caps) && is_vmouse_device(attrs, caps)) {
            found = true;
            CloseHandle(h);
            break;
          }
        }

        CloseHandle(h);
      }

      SetupDiDestroyDeviceInfoList(devInfoSet);
      return found;
    }

  }  // namespace vmouse
}  // namespace platf

#undef VMOUSE_LOG
