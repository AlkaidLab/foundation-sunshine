/**
 * @file vdd_ioctl.cpp
 * @brief Self-contained IOCTL transport for the ZakoVDD control channel.
 *
 * See `vdd_ioctl.h` for the rationale; this TU keeps the SetupAPI and
 * `<Windows.h>` blast radius out of `vdd_utils.cpp`.
 */

#define WIN32_LEAN_AND_MEAN
#include "vdd_ioctl.h"

// Emit the storage for GUID_DEVINTERFACE_ZAKO_VDD_CONTROL exactly once
// (DEFINE_GUID expands to extern unless INITGUID is set first).
#define INITGUID
#include "vdd_control_ioctl.h"

#include <Windows.h>
#include <SetupAPI.h>

#include <vector>

#include "src/logging.h"

namespace display_device::vdd_ioctl {

  namespace {

    /**
     * @brief RAII guard for `HDEVINFO` returned by `SetupDiGetClassDevs*`.
     *
     * Ensures the kernel-side device-info list is released on every exit
     * path (including exceptions thrown by `std::vector` allocation or
     * `std::wstring` construction below).
     */
    class devinfo_guard {
    public:
      explicit devinfo_guard(HDEVINFO h):
          h_ { h } {}

      ~devinfo_guard() {
        if (h_ != INVALID_HANDLE_VALUE) {
          SetupDiDestroyDeviceInfoList(h_);
        }
      }

      devinfo_guard(const devinfo_guard &) = delete;
      devinfo_guard &operator=(const devinfo_guard &) = delete;
      devinfo_guard(devinfo_guard &&) = delete;
      devinfo_guard &operator=(devinfo_guard &&) = delete;

      HDEVINFO get() const {
        return h_;
      }

    private:
      HDEVINFO h_ { INVALID_HANDLE_VALUE };
    };

    /**
     * @brief Resolve the first registered VDD control device interface path.
     *
     * The driver registers its custom interface in `VirtualDisplayDriverDeviceAdd`
     * via `WdfDeviceCreateDeviceInterface(GUID_DEVINTERFACE_ZAKO_VDD_CONTROL)`.
     * If the driver isn't installed, or hasn't reached D0 yet, the enumeration
     * returns no interfaces.
     *
     * @return Empty wstring on failure (driver not installed / no interface).
     */
    std::wstring
    resolve_interface_path() {
      HDEVINFO h_raw = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
        nullptr,
        nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

      if (h_raw == INVALID_HANDLE_VALUE) {
        BOOST_LOG(debug) << "vdd_ioctl: SetupDiGetClassDevsW failed (err=" << GetLastError() << ")";
        return {};
      }

      // RAII: from here on every exit path (including std::bad_alloc from
      // the std::vector / std::wstring constructions below) destroys the
      // device-info list.
      devinfo_guard h_dev_info { h_raw };

      SP_DEVICE_INTERFACE_DATA iface_data {};
      iface_data.cbSize = sizeof(iface_data);

      // Only ever use the first instance: the VDD always exposes exactly
      // one control interface per device.
      if (!SetupDiEnumDeviceInterfaces(
            h_dev_info.get(),
            nullptr,
            &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
            0,
            &iface_data)) {
        const DWORD err = GetLastError();
        if (err != ERROR_NO_MORE_ITEMS) {
          BOOST_LOG(debug) << "vdd_ioctl: SetupDiEnumDeviceInterfaces failed (err=" << err << ")";
        }
        return {};
      }

      // First call retrieves the required buffer size.
      DWORD required_size = 0;
      SetupDiGetDeviceInterfaceDetailW(h_dev_info.get(), &iface_data, nullptr, 0, &required_size, nullptr);
      if (required_size == 0) {
        BOOST_LOG(debug) << "vdd_ioctl: SetupDiGetDeviceInterfaceDetailW size probe failed (err=" << GetLastError() << ")";
        return {};
      }

      std::vector<BYTE> buffer(required_size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      if (!SetupDiGetDeviceInterfaceDetailW(
            h_dev_info.get(),
            &iface_data,
            detail,
            required_size,
            nullptr,
            nullptr)) {
        BOOST_LOG(debug) << "vdd_ioctl: SetupDiGetDeviceInterfaceDetailW failed (err=" << GetLastError() << ")";
        return {};
      }

      return std::wstring { detail->DevicePath };
    }

    /**
     * @brief RAII wrapper around `CreateFileW` for the resolved device path.
     */
    class device_handle {
    public:
      device_handle() = default;

      /**
       * @brief Outcome of `open()`. Mirrors the public `result` enum so the
       *        per-IOCTL helpers can forward the distinction without an
       *        extra translation step.
       */
      enum class open_result {
        success,
        interface_missing,  ///< No registered interface yet -- driver too old / not installed.
        failed,             ///< Interface enumerated but `CreateFileW` failed -- driver is there but unhappy.
      };

      open_result open() {
        const std::wstring path = resolve_interface_path();
        if (path.empty()) {
          // Empty path = SetupDi enumeration found no instance of our
          // GUID. This is the "driver isn't installed / hasn't registered
          // yet" case and the only one where falling back to the legacy
          // pipe is correct.
          return open_result::interface_missing;
        }

        // GENERIC_READ|WRITE matches the FILE_READ_ACCESS|FILE_WRITE_DATA
        // bits encoded in the IOCTL function codes. SHARED open so multiple
        // Sunshine processes / external tools can coexist.
        m_handle = CreateFileW(
          path.c_str(),
          GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr,
          OPEN_EXISTING,
          0,
          nullptr);

        if (m_handle == INVALID_HANDLE_VALUE) {
          // Interface was enumerated (path resolved) but the kernel still
          // refused to give us a handle. The driver is present, so the
          // pipe fallback would either deadlock on the same WUDFHost or
          // duplicate-execute the command -- propagate failure instead.
          BOOST_LOG(warning) << "vdd_ioctl: CreateFileW failed (err=" << GetLastError() << ")";
          return open_result::failed;
        }
        return open_result::success;
      }

      ~device_handle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
          CloseHandle(m_handle);
        }
      }

      device_handle(const device_handle &) = delete;
      device_handle &operator=(const device_handle &) = delete;

      HANDLE get() const { return m_handle; }

    private:
      HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

  }  // namespace

  result
  send_command(const std::wstring &command) {
    if (command.empty()) {
      return result::failed;
    }

    device_handle dev;
    switch (dev.open()) {
      case device_handle::open_result::success:
        break;
      case device_handle::open_result::interface_missing:
        return result::interface_missing;
      case device_handle::open_result::failed:
        return result::failed;
    }

    // Send the buffer including its terminating L'\0' so the driver-side
    // dispatcher (DispatchVddCommandBuffer) can treat it as a NUL-terminated
    // string without further bookkeeping.
    const DWORD bytes_to_send = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    DWORD bytes_returned = 0;

    const BOOL ok = DeviceIoControl(
      dev.get(),
      IOCTL_VDD_COMMAND,
      const_cast<wchar_t *>(command.c_str()),
      bytes_to_send,
      nullptr,
      0,
      &bytes_returned,
      nullptr);

    if (!ok) {
      // The driver answered the IRP with an error status. Do NOT retry
      // on the legacy pipe: the kernel-side handler may have partially
      // applied the command (e.g. CREATEMONITOR allocated state before
      // failing) and re-running it via pipe could double-create or leave
      // the device in a worse spot than just surfacing the failure.
      BOOST_LOG(warning) << "vdd_ioctl: DeviceIoControl(IOCTL_VDD_COMMAND) failed (err=" << GetLastError() << ")";
      return result::failed;
    }

    return result::success;
  }

  bool
  ping() {
    device_handle dev;
    if (dev.open() != device_handle::open_result::success) {
      return false;
    }

    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
      dev.get(),
      IOCTL_VDD_PING,
      nullptr, 0,
      nullptr, 0,
      &bytes_returned,
      nullptr);

    if (!ok) {
      BOOST_LOG(debug) << "vdd_ioctl: DeviceIoControl(IOCTL_VDD_PING) failed (err=" << GetLastError() << ")";
      return false;
    }
    return true;
  }

}  // namespace display_device::vdd_ioctl
