#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace {
  constexpr uint16_t VMOUSE_VID = 0x1ACE;
  constexpr uint16_t VMOUSE_PID = 0x0002;

  std::string
  wide_to_utf8(const wchar_t *wstr) {
    if (!wstr) {
      return {};
    }

    const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
      return {};
    }

    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
  }

  std::array<uint8_t, 8>
  build_report(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t hwheel) {
    return {
      0x02,
      buttons,
      static_cast<uint8_t>(dx & 0xFF),
      static_cast<uint8_t>((dx >> 8) & 0xFF),
      static_cast<uint8_t>(dy & 0xFF),
      static_cast<uint8_t>((dy >> 8) & 0xFF),
      static_cast<uint8_t>(wheel),
      static_cast<uint8_t>(hwheel),
    };
  }
}  // namespace

int
main() {
  GUID hid_guid;
  HidD_GetHidGuid(&hid_guid);

  HDEVINFO dev_info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev_info == INVALID_HANDLE_VALUE) {
    std::cerr << "SetupDiGetClassDevs failed err=" << GetLastError() << '\n';
    return 1;
  }

  SP_DEVICE_INTERFACE_DATA iface {};
  iface.cbSize = sizeof(iface);

  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, nullptr, &hid_guid, i, &iface); ++i) {
    DWORD required_size = 0;
    SetupDiGetDeviceInterfaceDetailW(dev_info, &iface, nullptr, 0, &required_size, nullptr);
    if (required_size == 0) {
      continue;
    }

    auto detail_buf = std::make_unique<BYTE[]>(required_size);
    auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail_buf.get());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &iface, detail, required_size, nullptr, nullptr)) {
      continue;
    }

    HANDLE attr_handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (attr_handle == INVALID_HANDLE_VALUE) {
      continue;
    }

    HIDD_ATTRIBUTES attrs {};
    attrs.Size = sizeof(attrs);
    const bool ok_attrs = HidD_GetAttributes(attr_handle, &attrs) == TRUE;
    CloseHandle(attr_handle);
    if (!ok_attrs || attrs.VendorID != VMOUSE_VID || attrs.ProductID != VMOUSE_PID) {
      continue;
    }

    std::cout << "TARGET path=" << wide_to_utf8(detail->DevicePath) << '\n';

    HANDLE caps_handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (caps_handle != INVALID_HANDLE_VALUE) {
      PHIDP_PREPARSED_DATA preparsed_data = nullptr;
      if (HidD_GetPreparsedData(caps_handle, &preparsed_data)) {
        HIDP_CAPS caps {};
        if (HidP_GetCaps(preparsed_data, &caps) == HIDP_STATUS_SUCCESS) {
          std::cout << "CAPS usage_page=" << caps.UsagePage
                    << " usage=" << caps.Usage
                    << " input_len=" << caps.InputReportByteLength
                    << " output_len=" << caps.OutputReportByteLength
                    << " feature_len=" << caps.FeatureReportByteLength << '\n';
        }
        HidD_FreePreparsedData(preparsed_data);
      }
      CloseHandle(caps_handle);
    }

    HANDLE write_handle = CreateFileW(detail->DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (write_handle == INVALID_HANDLE_VALUE) {
      std::cout << "OPEN_WRITE_FAIL err=" << GetLastError() << '\n';
      SetupDiDestroyDeviceInfoList(dev_info);
      return 2;
    }

    // Test HidD_SetFeature (what Sunshine actually uses)
    const auto move_report = build_report(0, 48, 24, 0, 0);
    SetLastError(0);
    const BOOL feat_move_ok = HidD_SetFeature(write_handle, const_cast<uint8_t *>(move_report.data()), static_cast<ULONG>(move_report.size()));
    std::cout << "MOVE_SETFEATURE ok=" << (feat_move_ok ? 1 : 0) << " err=" << GetLastError() << '\n';

    Sleep(100);

    const auto click_report = build_report(1, 0, 0, 0, 0);
    SetLastError(0);
    const BOOL feat_click_ok = HidD_SetFeature(write_handle, const_cast<uint8_t *>(click_report.data()), static_cast<ULONG>(click_report.size()));
    std::cout << "CLICK_SETFEATURE ok=" << (feat_click_ok ? 1 : 0) << " err=" << GetLastError() << '\n';

    Sleep(100);

    const auto release_report = build_report(0, 0, 0, 0, 0);
    SetLastError(0);
    const BOOL feat_release_ok = HidD_SetFeature(write_handle, const_cast<uint8_t *>(release_report.data()), static_cast<ULONG>(release_report.size()));
    std::cout << "RELEASE_SETFEATURE ok=" << (feat_release_ok ? 1 : 0) << " err=" << GetLastError() << '\n';

    // Also test HidD_SetOutputReport for comparison (expected to fail since driver has no Output Report)
    SetLastError(0);
    const BOOL out_ok = HidD_SetOutputReport(write_handle, const_cast<uint8_t *>(move_report.data()), static_cast<ULONG>(move_report.size()));
    std::cout << "MOVE_SETOUTPUTREPORT ok=" << (out_ok ? 1 : 0) << " err=" << GetLastError() << " (expected fail, no output report)\n";

    CloseHandle(write_handle);
    SetupDiDestroyDeviceInfoList(dev_info);
    return 0;
  }

  SetupDiDestroyDeviceInfoList(dev_info);
  std::cout << "TARGET_NOT_FOUND\n";
  return 3;
}
