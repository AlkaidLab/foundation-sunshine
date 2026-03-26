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

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "virtual_mouse.h"
#include "src/logging.h"

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

    // Flush interval for accumulated mouse movement (4ms ≈ 250Hz).
    // HidD_SetFeature takes ~3ms, so this is the practical minimum.
    static constexpr auto FLUSH_INTERVAL = std::chrono::milliseconds(4);

    struct device_t::impl_t {
      HANDLE hDevice = INVALID_HANDLE_VALUE;
      uint8_t buttonState = 0;  // Current button state (accumulated)

      // Accumulated mouse deltas (written by callers, read by flush thread)
      std::mutex accum_mutex;
      int32_t accum_dx = 0;
      int32_t accum_dy = 0;
      bool accum_dirty = false;

      // Flush thread
      std::thread flush_thread;
      std::atomic<bool> flush_running { false };

      ~impl_t() {
        stop_flush_thread();
        close();
      }

      void
      stop_flush_thread() {
        flush_running.store(false, std::memory_order_release);
        if (flush_thread.joinable()) {
          flush_thread.join();
        }
      }

      void
      start_flush_thread() {
        flush_running.store(true, std::memory_order_release);
        flush_thread = std::thread([this]() {
          while (flush_running.load(std::memory_order_acquire)) {
            flush_accumulated();
            std::this_thread::sleep_for(FLUSH_INTERVAL);
          }
        });
      }

      void
      flush_accumulated() {
        int16_t dx, dy;
        uint8_t buttons;
        {
          std::lock_guard<std::mutex> lk(accum_mutex);
          if (!accum_dirty) return;
          dx = static_cast<int16_t>(std::clamp(accum_dx, -32767, 32767));
          dy = static_cast<int16_t>(std::clamp(accum_dy, -32767, 32767));
          buttons = buttonState;
          accum_dx = 0;
          accum_dy = 0;
          accum_dirty = false;
        }
        sendReportDirect(buttons, dx, dy, 0, 0);
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
        if (hDevice == INVALID_HANDLE_VALUE) return false;

        auto report = detail::build_output_report(buttons, dx, dy, sv, sh);

        BOOL result = HidD_SetFeature(hDevice, (PVOID) report.data(), static_cast<ULONG>(report.size()));

        if (!result) {
          DWORD err = GetLastError();
          if (detail::should_close_on_write_error(err)) {
            VMOUSE_LOG(warning) << "vmouse: Device disconnected, closing handle"sv;
            flush_running.store(false, std::memory_order_release);
            close();
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
      // Accumulate deltas; the flush thread will send them periodically
      std::lock_guard<std::mutex> lk(impl->accum_mutex);
      impl->accum_dx += delta_x;
      impl->accum_dy += delta_y;
      impl->accum_dirty = true;
      return true;
    }

    bool
    device_t::button(uint8_t button_mask, bool release) {
      impl->buttonState = detail::apply_button_transition(impl->buttonState, button_mask, release);
      // Flush any pending movement with the new button state
      impl->flush_accumulated();
      return impl->sendReportDirect(impl->buttonState, 0, 0, 0, 0);
    }

    bool
    device_t::scroll(int8_t distance) {
      return impl->sendReportDirect(impl->buttonState, 0, 0, distance, 0);
    }

    bool
    device_t::hscroll(int8_t distance) {
      return impl->sendReportDirect(impl->buttonState, 0, 0, 0, distance);
    }

    bool
    device_t::send_report(uint8_t buttons, int16_t delta_x, int16_t delta_y,
                          int8_t scroll_v, int8_t scroll_h) {
      impl->buttonState = buttons;
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
