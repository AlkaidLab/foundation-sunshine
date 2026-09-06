// Read-only hardware smoke test. No Android debugging authorization is required.
// Build with MSVC: cl /EHsc /std:c++17 /MT usb_control_probe.cpp setupapi.lib winusb.lib
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <usb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool transfer(WINUSB_INTERFACE_HANDLE usb, UCHAR request, USHORT value,
                     USHORT index, UCHAR *buffer, USHORT length) {
  WINUSB_SETUP_PACKET setup {0x80, request, value, index, length};
  ULONG actual = 0;
  if (!WinUsb_ControlTransfer(usb, setup, buffer, length, &actual, nullptr) || actual != length) {
    std::fprintf(stderr, "control request=%u failed error=%lu length=%lu/%u\n",
                 request, GetLastError(), actual, length);
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: usb_control_probe VID-hex PID-hex serial\n");
    return 2;
  }
  const auto vid = std::strtoul(argv[1], nullptr, 16);
  const auto pid = std::strtoul(argv[2], nullptr, 16);
  // Android's installed WinUSB device-interface GUID (android_winusb.inf).
  const GUID guid {0xf72fe0d4, 0xcbcb, 0x407d, {0x88,0x14,0x9e,0xd6,0x73,0xd0,0xdd,0x6b}};
  auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (devices == INVALID_HANDLE_VALUE) return 3;
  int result = 4;
  for (DWORD n = 0;; ++n) {
    SP_DEVICE_INTERFACE_DATA entry {}; entry.cbSize = sizeof(entry);
    if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, n, &entry)) break;
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
    if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<UCHAR> storage(size);
    auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(*detail);
    if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) continue;
    // Restrict access to the requested VID/PID before opening a device.
    wchar_t match[40]; swprintf_s(match, L"vid_%04x&pid_%04x", unsigned(vid), unsigned(pid));
    if (!wcsstr(detail->DevicePath, match)) continue;
    auto file = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      std::fprintf(stderr, "open failed: %lu\n", GetLastError()); continue;
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) { CloseHandle(file); continue; }
    ULONG timeout = 3000;
    WinUsb_SetPipePolicy(usb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);
    USB_DEVICE_DESCRIPTOR descriptor {};
    bool ok = transfer(usb, 6, 0x0100, 0, reinterpret_cast<UCHAR *>(&descriptor), sizeof(descriptor));
    ok = ok && descriptor.idVendor == vid && descriptor.idProduct == pid && descriptor.iSerialNumber;
    UCHAR languages[4] {}, header[2] {};
    ok = ok && transfer(usb, 6, 0x0300, 0, languages, sizeof(languages));
    USHORT language = languages[2] | (languages[3] << 8);
    ok = ok && transfer(usb, 6, 0x0300 | descriptor.iSerialNumber, language, header, sizeof(header));
    std::vector<UCHAR> serial(header[0]);
    ok = ok && serial.size() >= 2 && transfer(usb, 6, 0x0300 | descriptor.iSerialNumber,
                                            language, serial.data(), static_cast<USHORT>(serial.size()));
    std::vector<char> ascii;
    if (ok) {
      for (size_t i = 2; i + 1 < serial.size(); i += 2) ascii.push_back(serial[i + 1] ? '?' : serial[i]);
      ascii.push_back(0);
      ok = std::strcmp(ascii.data(), argv[3]) == 0;
    }
    // GET_STATUS uses a control transfer, rather than SetupAPI's cached identity.
    for (int i = 0; ok && i < 20; ++i) {
      UCHAR status[2] {};
      ok = transfer(usb, 0, 0, 0, status, sizeof(status));
      if (ok) std::printf("GET_STATUS %d: %02x%02x\n", i + 1, status[1], status[0]);
    }
    if (ok) {
      std::printf("PASS USB_CONTROL vid=%04lx pid=%04lx serial=%s status_reads=20\n", vid, pid, ascii.data());
      result = 0;
    }
    WinUsb_Free(usb); CloseHandle(file);
    if (result == 0) break;
  }
  SetupDiDestroyDeviceInfoList(devices);
  if (result) std::fprintf(stderr, "USB control probe failed (%d)\n", result);
  return result;
}
