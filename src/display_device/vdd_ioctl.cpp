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
      HDEVINFO h_dev_info = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
        nullptr,
        nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

      if (h_dev_info == INVALID_HANDLE_VALUE) {
        BOOST_LOG(debug) << "vdd_ioctl: SetupDiGetClassDevsW failed (err=" << GetLastError() << ")";
        return {};
      }

      SP_DEVICE_INTERFACE_DATA iface_data {};
      iface_data.cbSize = sizeof(iface_data);

      // Only ever use the first instance: the VDD always exposes exactly
      // one control interface per device.
      if (!SetupDiEnumDeviceInterfaces(
            h_dev_info,
            nullptr,
            &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
            0,
            &iface_data)) {
        const DWORD err = GetLastError();
        if (err != ERROR_NO_MORE_ITEMS) {
          BOOST_LOG(debug) << "vdd_ioctl: SetupDiEnumDeviceInterfaces failed (err=" << err << ")";
        }
        SetupDiDestroyDeviceInfoList(h_dev_info);
        return {};
      }

      // First call retrieves the required buffer size.
      DWORD required_size = 0;
      SetupDiGetDeviceInterfaceDetailW(h_dev_info, &iface_data, nullptr, 0, &required_size, nullptr);
      if (required_size == 0) {
        BOOST_LOG(debug) << "vdd_ioctl: SetupDiGetDeviceInterfaceDetailW size probe failed (err=" << GetLastError() << ")";
        SetupDiDestroyDeviceInfoList(h_dev_info);
        return {};
      }

      std::vector<BYTE> buffer(required_size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

      if (!SetupDiGetDeviceInterfaceDetailW(
            h_dev_info,
            &iface_data,
            detail,
            required_size,
            nullptr,
            nullptr)) {
        BOOST_LOG(debug) << "vdd_ioctl: SetupDiGetDeviceInterfaceDetailW failed (err=" << GetLastError() << ")";
        SetupDiDestroyDeviceInfoList(h_dev_info);
        return {};
      }

      std::wstring path { detail->DevicePath };
      SetupDiDestroyDeviceInfoList(h_dev_info);
      return path;
    }

    /**
     * @brief RAII wrapper around `CreateFileW` for the resolved device path.
     */
    class device_handle {
    public:
      device_handle() = default;

      bool open() {
        const std::wstring path = resolve_interface_path();
        if (path.empty()) {
          return false;
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
          BOOST_LOG(debug) << "vdd_ioctl: CreateFileW failed (err=" << GetLastError() << ")";
          return false;
        }
        return true;
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

  bool
  send_command(const std::wstring &command) {
    if (command.empty()) {
      return false;
    }

    device_handle dev;
    if (!dev.open()) {
      return false;
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
      BOOST_LOG(warning) << "vdd_ioctl: DeviceIoControl(IOCTL_VDD_COMMAND) failed (err=" << GetLastError() << ")";
      return false;
    }

    return true;
  }

  bool
  ping() {
    device_handle dev;
    if (!dev.open()) {
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
