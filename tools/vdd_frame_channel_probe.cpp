#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "src/display_device/vdd_control_ioctl.h"
#include "src/platform/windows/vdd_frame_channel.h"

extern "C" const GUID GUID_DEVINTERFACE_ZAKO_VDD_CONTROL = ZAKO_VDD_CONTROL_GUID_INIT;

#include <SetupAPI.h>
#include <windows.h>

#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

  using shared_frame_metadata_t = platf::dxgi::vdd_frame_channel::shared_frame_metadata_t;

  struct devinfo_guard {
    HDEVINFO handle = INVALID_HANDLE_VALUE;

    ~devinfo_guard() {
      if (handle != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(handle);
      }
    }
  };

  struct handle_guard {
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~handle_guard() {
      if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
    }
  };

  void
  close_remote_handle_value(std::uint64_t value) {
    if (value != 0) {
      CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
    }
  }

  std::wstring
  resolve_vdd_control_path() {
    devinfo_guard devs {
      SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
        nullptr,
        nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT)
    };
    if (devs.handle == INVALID_HANDLE_VALUE) {
      return {};
    }

    SP_DEVICE_INTERFACE_DATA iface {};
    iface.cbSize = sizeof(iface);
    if (!SetupDiEnumDeviceInterfaces(devs.handle, nullptr, &GUID_DEVINTERFACE_ZAKO_VDD_CONTROL, 0, &iface)) {
      return {};
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(devs.handle, &iface, nullptr, 0, &required, nullptr);
    if (required == 0) {
      return {};
    }

    std::vector<BYTE> buffer(required);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(devs.handle, &iface, detail, required, nullptr, nullptr)) {
      return {};
    }

    return std::wstring {detail->DevicePath};
  }

  handle_guard
  open_vdd_control() {
    const auto path = resolve_vdd_control_path();
    if (path.empty()) {
      return {};
    }

    return handle_guard {
      CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IMPERSONATION,
        nullptr)
    };
  }

  int
  usage() {
    std::cout << "Usage: vdd-frame-channel-probe [--open] [--monitor N] [--slots N]\n"
              << "  --slots 0 uses the driver default; nonzero values must match caps max_slots.\n";
    return 2;
  }

  bool
  parse_uint32_arg(const char *text, UINT32 &value) {
    try {
      const std::string input { text ? text : "" };
      std::size_t parsed_chars = 0;
      const auto parsed = std::stoul(input, &parsed_chars, 0);
      if (input.empty() || parsed_chars != input.size() || parsed > std::numeric_limits<UINT32>::max()) {
        return false;
      }
      value = static_cast<UINT32>(parsed);
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

}  // namespace

int
main(int argc, char **argv) {
  bool do_open = false;
  UINT32 monitor_index = 0;
  UINT32 desired_slots = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--open") {
      do_open = true;
    }
    else if (arg == "--monitor" && i + 1 < argc) {
      if (!parse_uint32_arg(argv[++i], monitor_index)) {
        return usage();
      }
    }
    else if (arg == "--slots" && i + 1 < argc) {
      if (!parse_uint32_arg(argv[++i], desired_slots)) {
        return usage();
      }
    }
    else if (arg == "--help" || arg == "-h") {
      return usage();
    }
    else {
      return usage();
    }
  }

  auto dev = open_vdd_control();
  if (!dev.handle || dev.handle == INVALID_HANDLE_VALUE) {
    std::cerr << "VDD control interface not found\n";
    return 1;
  }

  VDD_FRAME_CHANNEL_CAPS caps {};
  caps.Size = sizeof(caps);
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
        dev.handle,
        IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS,
        nullptr, 0,
        &caps, sizeof(caps),
        &bytes_returned,
        nullptr)) {
    const DWORD err = GetLastError();
    std::cerr << "QUERY_FRAME_CHANNEL_CAPS failed: " << err << "\n";
    return 1;
  }

  std::cout << "caps:"
            << " version=" << caps.Version
            << " flags=0x" << std::hex << caps.Flags << std::dec
            << " max_slots=" << caps.MaxSharedSlots
            << " metadata_size=" << caps.MetadataSize
            << "\n";

  const UINT32 required_flags =
    VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW |
    VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES |
    VDD_FRAME_CHANNEL_FLAG_STRICT_DACL;
  if ((caps.Flags & required_flags) != required_flags) {
    std::cerr << "caps missing required sealed-channel flags\n";
    return 1;
  }

  if (!do_open) {
    return 0;
  }

  VDD_FRAME_CHANNEL_OPEN_REQUEST request {};
  request.Size = sizeof(request);
  request.Version = VDD_FRAME_CHANNEL_OPEN_VERSION;
  request.MonitorIndex = monitor_index;
  request.RequiredFlags = required_flags;
  request.TargetProcessId = GetCurrentProcessId();
  request.DesiredSlots = desired_slots;

  VDD_FRAME_CHANNEL_OPEN_RESPONSE response {};
  response.Size = sizeof(response);
  bytes_returned = 0;
  if (!DeviceIoControl(
        dev.handle,
        IOCTL_VDD_OPEN_FRAME_CHANNEL,
        &request, sizeof(request),
        &response, sizeof(response),
        &bytes_returned,
        nullptr)) {
    const DWORD err = GetLastError();
    std::cerr << "OPEN_FRAME_CHANNEL failed: " << err << "\n";
    return 1;
  }

  std::cout << "open:"
            << " version=" << response.Version
            << " flags=0x" << std::hex << response.Flags << std::dec
            << " slots=" << response.SlotCount
            << " metadata_size=" << response.MetadataSize
            << " metadata_handle=0x" << std::hex << response.MetadataHandle
            << " event_handle=0x" << response.FrameReadyEventHandle
            << std::dec << "\n";

  if (response.MetadataHandle != 0) {
    auto *meta = static_cast<const shared_frame_metadata_t *>(
      MapViewOfFile(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(response.MetadataHandle)),
        FILE_MAP_READ,
        0, 0,
        sizeof(shared_frame_metadata_t)));
    if (!meta) {
      std::cerr << "metadata MapViewOfFile failed: " << GetLastError() << "\n";
    }
    else {
      shared_frame_metadata_t snapshot {};
      if (!platf::dxgi::vdd_frame_channel::read_stable_metadata(meta, snapshot, 8)) {
        std::cerr << "metadata stable snapshot failed\n";
      }
      else {
        const auto generation = platf::dxgi::vdd_frame_channel::metadata_channel_generation(snapshot.MetadataSequence);
        const auto sequence = platf::dxgi::vdd_frame_channel::metadata_sequence_counter(snapshot.MetadataSequence);
        std::cout << "metadata:"
                  << " magic=0x" << std::hex << snapshot.Magic << std::dec
                  << " version=" << snapshot.Version
                  << " size=" << snapshot.MetadataSize
                  << " " << snapshot.Width << "x" << snapshot.Height
                  << " fmt=" << snapshot.DxgiFormat
                  << " hdr=" << snapshot.IsHdr
                  << " frame=" << snapshot.FrameCounter
                  << " slot=" << snapshot.SlotIndex << "/" << snapshot.SlotCount
                  << " meta_seq=0x" << std::hex << snapshot.MetadataSequence << std::dec
                  << " seq=" << sequence
                  << " generation=" << generation
                  << " luid=" << snapshot.AdapterLuidLowPart << ":" << snapshot.AdapterLuidHighPart
                  << " publish_qpc=" << snapshot.LastPublishQpc
                  << " present_qpc=" << snapshot.LastPresentQpc
                  << " replaced_unread=" << snapshot.ReplacedUnreadFrames
                  << " dropped_held=" << snapshot.DroppedConsumerHeldFrames
                  << " dropped_acquire=" << snapshot.DroppedAcquireFailures
                  << "\n";
      }
      UnmapViewOfFile(meta);
    }
  }

  close_remote_handle_value(response.MetadataHandle);
  close_remote_handle_value(response.FrameReadyEventHandle);
  const UINT32 slot_count = response.SlotCount <= VDD_FRAME_CHANNEL_MAX_SLOTS ? response.SlotCount : VDD_FRAME_CHANNEL_MAX_SLOTS;
  for (UINT32 slot = 0; slot < slot_count; ++slot) {
    std::cout << "slot " << slot << " texture_handle=0x"
              << std::hex << response.Slots[slot].TextureHandle << std::dec << "\n";
    close_remote_handle_value(response.Slots[slot].TextureHandle);
  }

  return 0;
}
