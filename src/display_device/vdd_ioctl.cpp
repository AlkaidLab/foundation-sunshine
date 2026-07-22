/**
 * @file vdd_ioctl.cpp
 * @brief Self-contained IOCTL transport for the ZakoVDD control channel.
 *
 * See `vdd_ioctl.h` for the rationale; this TU keeps the SetupAPI and
 * `<Windows.h>` blast radius out of `vdd_utils.cpp`.
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "vdd_ioctl.h"
#include "vdd_control_ioctl.h"

extern "C" const GUID GUID_DEVINTERFACE_ZAKO_VDD_CONTROL = ZAKO_VDD_CONTROL_GUID_INIT;

#include <SetupAPI.h>
#include <devguid.h>

#include <iomanip>
#include <algorithm>
#include <cwchar>
#include <vector>

#include "src/logging.h"

namespace display_device::vdd_ioctl {

  namespace {

    constexpr DEVPROPKEY kDeviceProblemCodeProperty {
      { 0x4340a6c5, 0x93fa, 0x4706, { 0x97, 0x2c, 0x7b, 0x64, 0x80, 0x08, 0xa5, 0xa7 } },
      3
    };

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
          SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION,
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

    void
    close_raw_frame_channel_handles(VDD_FRAME_CHANNEL_OPEN_RESPONSE &response) {
      auto close_if_valid = [](UINT64 handle_value) {
        if (handle_value != 0) {
          CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_value)));
        }
      };

      close_if_valid(response.MetadataHandle);
      close_if_valid(response.FrameReadyEventHandle);
      const UINT32 slot_count = std::min<UINT32>(response.SlotCount, VDD_FRAME_CHANNEL_MAX_SLOTS);
      for (UINT32 slot = 0; slot < slot_count; ++slot) {
        close_if_valid(response.Slots[slot].TextureHandle);
      }
      response = {};
    }

  }  // namespace

  std::uint32_t
  required_sealed_frame_channel_flags() {
    return VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW |
           VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES |
           VDD_FRAME_CHANNEL_FLAG_STRICT_DACL;
  }

  adapter_status_t
  query_adapter_status() {
    constexpr wchar_t kVddHardwareId[] = L"ROOT\\ZAKOVDD";

    HDEVINFO h_raw = SetupDiGetClassDevsW(
      &GUID_DEVCLASS_DISPLAY,
      nullptr,
      nullptr,
      DIGCF_PRESENT);
    if (h_raw == INVALID_HANDLE_VALUE) {
      BOOST_LOG(debug) << "vdd_ioctl: display adapter enumeration failed (err=" << GetLastError() << ")";
      return {};
    }

    devinfo_guard h_dev_info { h_raw };
    for (DWORD index = 0;; ++index) {
      SP_DEVINFO_DATA device_info {};
      device_info.cbSize = sizeof(device_info);
      if (!SetupDiEnumDeviceInfo(h_dev_info.get(), index, &device_info)) {
        const DWORD err = GetLastError();
        if (err != ERROR_NO_MORE_ITEMS) {
          BOOST_LOG(debug) << "vdd_ioctl: display adapter enumeration stopped (err=" << err << ")";
        }
        return {};
      }

      DWORD required_size = 0;
      DWORD property_type = 0;
      SetupDiGetDeviceRegistryPropertyW(
        h_dev_info.get(),
        &device_info,
        SPDRP_HARDWAREID,
        &property_type,
        nullptr,
        0,
        &required_size);
      if (required_size == 0) {
        continue;
      }

      std::vector<BYTE> buffer(required_size + sizeof(wchar_t), 0);
      if (!SetupDiGetDeviceRegistryPropertyW(
            h_dev_info.get(),
            &device_info,
            SPDRP_HARDWAREID,
            &property_type,
            buffer.data(),
            required_size,
            nullptr)) {
        continue;
      }

      const auto *hardware_id = reinterpret_cast<const wchar_t *>(buffer.data());
      const auto wchar_count = buffer.size() / sizeof(wchar_t);
      for (std::size_t offset = 0; offset < wchar_count && hardware_id[offset] != L'\0';) {
        const wchar_t *candidate = hardware_id + offset;
        if (_wcsicmp(candidate, kVddHardwareId) == 0) {
          DWORD problem_code = 0;
          DEVPROPTYPE problem_type = 0;
          DWORD problem_size = 0;
          const bool problem_query_succeeded = SetupDiGetDevicePropertyW(
            h_dev_info.get(),
            &device_info,
            &kDeviceProblemCodeProperty,
            &problem_type,
            reinterpret_cast<PBYTE>(&problem_code),
            sizeof(problem_code),
            &problem_size,
            0) != FALSE;
          if (!problem_query_succeeded || problem_type != DEVPROP_TYPE_UINT32 || problem_size != sizeof(problem_code)) {
            const auto error = problem_query_succeeded ? ERROR_INVALID_DATA : GetLastError();
            BOOST_LOG(debug) << "vdd_ioctl: adapter problem-code query failed or returned an unexpected value (err=" << error << ")";
            return { true, false, 0 };
          }
          return { true, true, problem_code };
        }
        offset += std::wcslen(candidate) + 1;
      }
    }
  }

  bool
  adapter_present() {
    return query_adapter_status().present;
  }

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

  frame_channel_status
  query_frame_channel_caps(frame_channel_caps &caps) {
    caps = {};

    device_handle dev;
    switch (dev.open()) {
      case device_handle::open_result::success:
        break;
      case device_handle::open_result::interface_missing:
        return frame_channel_status::interface_missing;
      case device_handle::open_result::failed:
        return frame_channel_status::failed;
    }

    VDD_FRAME_CHANNEL_CAPS raw_caps {};
    raw_caps.Size = sizeof(raw_caps);
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
      dev.get(),
      IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS,
      nullptr, 0,
      &raw_caps, sizeof(raw_caps),
      &bytes_returned,
      nullptr);

    if (!ok) {
      const DWORD err = GetLastError();
      if (err == ERROR_INVALID_FUNCTION ||
          err == ERROR_NOT_SUPPORTED ||
          err == ERROR_INVALID_PARAMETER) {
        BOOST_LOG(debug) << "vdd_ioctl: frame-channel caps unsupported by driver (err=" << err << ")";
        return frame_channel_status::unsupported;
      }
      BOOST_LOG(warning) << "vdd_ioctl: DeviceIoControl(IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS) failed (err=" << err << ")";
      return frame_channel_status::failed;
    }

    if (bytes_returned < sizeof(VDD_FRAME_CHANNEL_CAPS) ||
        raw_caps.Size < sizeof(VDD_FRAME_CHANNEL_CAPS) ||
        raw_caps.Version != VDD_FRAME_CHANNEL_CAPS_VERSION) {
      BOOST_LOG(warning) << "vdd_ioctl: invalid frame-channel caps response"
                         << " bytes=" << bytes_returned
                         << " size=" << raw_caps.Size
                         << " version=" << raw_caps.Version;
      return frame_channel_status::failed;
    }

    if ((raw_caps.Flags & VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW) == 0 ||
        (raw_caps.Flags & VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES) == 0 ||
        (raw_caps.Flags & VDD_FRAME_CHANNEL_FLAG_STRICT_DACL) == 0) {
      BOOST_LOG(warning) << "vdd_ioctl: frame-channel caps missing required security flags: 0x"
                         << std::hex << raw_caps.Flags << std::dec;
      return frame_channel_status::failed;
    }

    caps.version = raw_caps.Version;
    caps.flags = raw_caps.Flags;
    caps.max_shared_slots = raw_caps.MaxSharedSlots;
    caps.metadata_size = raw_caps.MetadataSize;
    return frame_channel_status::supported;
  }

  frame_channel_open_status
  open_frame_channel(const frame_channel_open_request &request,
                     frame_channel_open_response &response,
                     bool log_failures) {
    response = {};

    device_handle dev;
    switch (dev.open()) {
      case device_handle::open_result::success:
        break;
      case device_handle::open_result::interface_missing:
        return frame_channel_open_status::interface_missing;
      case device_handle::open_result::failed:
        return frame_channel_open_status::failed;
    }

    VDD_FRAME_CHANNEL_OPEN_REQUEST raw_request {};
    raw_request.Size = sizeof(raw_request);
    raw_request.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
    raw_request.MonitorIndex = request.monitor_index;
    raw_request.RequiredFlags = request.required_flags;
    raw_request.TargetProcessId = GetCurrentProcessId();
    raw_request.DesiredSlots = request.desired_slots;
    raw_request.AdapterLuidLowPart = request.adapter_luid_low_part;
    raw_request.AdapterLuidHighPart = request.adapter_luid_high_part;

    VDD_FRAME_CHANNEL_OPEN_RESPONSE raw_response {};
    raw_response.Size = sizeof(raw_response);
    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(
      dev.get(),
      IOCTL_VDD_OPEN_FRAME_CHANNEL,
      &raw_request, sizeof(raw_request),
      &raw_response, sizeof(raw_response),
      &bytes_returned,
      nullptr);

    if (!ok) {
      const DWORD err = GetLastError();
      if (err == ERROR_NOT_READY) {
        if (log_failures) {
          BOOST_LOG(debug) << "vdd_ioctl: frame-channel producer is not ready yet"
                           << " (status=" << frame_channel_open_status_name(frame_channel_open_status::not_ready)
                           << ", win32=" << err << ")";
        }
        return frame_channel_open_status::not_ready;
      }
      if (err == ERROR_INVALID_FUNCTION ||
          err == ERROR_NOT_SUPPORTED ||
          err == ERROR_INVALID_PARAMETER) {
        BOOST_LOG(debug) << "vdd_ioctl: open frame-channel unsupported by driver (err=" << err << ")";
        return frame_channel_open_status::unsupported;
      }
      if (log_failures) {
        BOOST_LOG(warning) << "vdd_ioctl: DeviceIoControl(IOCTL_VDD_OPEN_FRAME_CHANNEL) failed"
                           << " (status=" << frame_channel_open_status_name(frame_channel_open_status::failed)
                           << ", win32=" << err << ")";
      }
      return frame_channel_open_status::failed;
    }

    if (bytes_returned < sizeof(VDD_FRAME_CHANNEL_OPEN_RESPONSE) ||
        raw_response.Size < sizeof(VDD_FRAME_CHANNEL_OPEN_RESPONSE) ||
        raw_response.Version != VDD_FRAME_CHANNEL_OPEN_VERSION ||
        raw_response.SlotCount == 0 ||
        raw_response.SlotCount > VDD_FRAME_CHANNEL_MAX_SLOTS ||
        raw_response.MetadataHandle == 0 ||
        raw_response.FrameReadyEventHandle == 0) {
      BOOST_LOG(warning) << "vdd_ioctl: invalid frame-channel open response"
                         << " bytes=" << bytes_returned
                         << " size=" << raw_response.Size
                         << " version=" << raw_response.Version
                         << " slots=" << raw_response.SlotCount
                         << " meta_handle=" << raw_response.MetadataHandle
                         << " event_handle=" << raw_response.FrameReadyEventHandle;
      close_raw_frame_channel_handles(raw_response);
      return frame_channel_open_status::failed;
    }

    if ((raw_response.Flags & request.required_flags) != request.required_flags) {
      BOOST_LOG(warning) << "vdd_ioctl: frame-channel open response missing required flags"
                         << " required=0x" << std::hex << request.required_flags
                         << " got=0x" << raw_response.Flags << std::dec;
      close_raw_frame_channel_handles(raw_response);
      return frame_channel_open_status::failed;
    }

    response.version = raw_response.Version;
    response.flags = raw_response.Flags;
    response.slot_count = raw_response.SlotCount;
    response.metadata_size = raw_response.MetadataSize;
    response.metadata_handle = raw_response.MetadataHandle;
    response.frame_ready_event_handle = raw_response.FrameReadyEventHandle;
    response.slots.reserve(raw_response.SlotCount);
    for (UINT32 slot = 0; slot < raw_response.SlotCount; ++slot) {
      if (raw_response.Slots[slot].TextureHandle == 0) {
        BOOST_LOG(warning) << "vdd_ioctl: frame-channel open response has null texture handle for slot " << slot;
        close_raw_frame_channel_handles(raw_response);
        response = {};
        return frame_channel_open_status::failed;
      }
      response.slots.push_back(frame_channel_slot_handle { raw_response.Slots[slot].TextureHandle });
    }
    return frame_channel_open_status::opened;
  }

}  // namespace display_device::vdd_ioctl
