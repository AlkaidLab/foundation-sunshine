/**
 * @file vdd_control_ioctl.h
 * @brief Authoritative IOCTL contract between Sunshine and the ZakoVDD driver.
 *
 * This header MUST stay byte-for-byte identical across the Sunshine and
 * Virtual-Display-Driver repositories. The canonical copy paths are:
 *   Sunshine: `src/display_device/vdd_control_ioctl.h`
 *   VDD: `Common/Include/vdd_control_ioctl.h`
 * The copies are intentionally duplicated to keep each repo self-contained.
 *
 * Transport summary:
 *   The driver exposes a custom WDF device interface
 *   (`GUID_DEVINTERFACE_ZAKO_VDD_CONTROL`). Opening this interface with
 *   `CreateFileW` PnP-wakes the IddCx driver back into D0, eliminating the
 *   race that the old named pipe transport had with WUDFHost recycling.
 *
 *   `IOCTL_VDD_COMMAND` carries the same NUL-terminated UTF-16 command
 *   buffer grammar as the legacy pipe protocol (e.g. `RELOAD_DRIVER`,
 *   `CREATEMONITOR ...`, `DESTROYMONITOR`). The in-driver dispatcher is
 *   shared verbatim between both transports, so callers do not need to
 *   re-encode anything when migrating from pipe to IOCTL.
 *
 *   `IOCTL_VDD_PING` is a cheap liveness probe (no payload).
 *
 *   `IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS` is the v2 frame-channel negotiation
 *   probe. Drivers that do not implement it are treated as legacy named-object
 *   frame producers by Sunshine.
 *
 *   `IOCTL_VDD_OPEN_FRAME_CHANNEL` asks the driver to duplicate an unnamed
 *   producer-owned frame channel into the requesting Sunshine process. The
 *   returned handle values are valid only in `TargetProcessId`.
 *
 *   `DesiredSlots` is a compatibility guard, not a request for the driver to
 *   resize its producer ring. Use 0 for the driver default, or the exact
 *   `MaxSharedSlots` value returned by `IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS`.
 */

#pragma once

#if defined(_WIN32) || defined(_WIN64)

#if defined(INITGUID)
#define VDD_CONTROL_RESTORE_INITGUID
#undef INITGUID
#endif

#include <Windows.h>
#include <winioctl.h>

#if defined(VDD_CONTROL_RESTORE_INITGUID)
#define INITGUID
#undef VDD_CONTROL_RESTORE_INITGUID
#endif

#ifdef __cplusplus
extern "C" {
#endif

// {DA9F8C2B-7E4F-49A1-9D4E-6F2B0E1A0C4D}
#define ZAKO_VDD_CONTROL_GUID_INIT \
  { 0xDA9F8C2B, 0x7E4F, 0x49A1, { 0x9D, 0x4E, 0x6F, 0x2B, 0x0E, 0x1A, 0x0C, 0x4D } }

DEFINE_GUID(GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
  0xDA9F8C2B, 0x7E4F, 0x49A1, 0x9D, 0x4E, 0x6F, 0x2B, 0x0E, 0x1A, 0x0C, 0x4D);

// IOCTL function codes carved out of the driver-defined range (0x800+).
#define IOCTL_VDD_COMMAND \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

#define IOCTL_VDD_PING \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_VDD_QUERY_FRAME_CHANNEL_CAPS \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_VDD_OPEN_FRAME_CHANNEL \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define VDD_FRAME_CHANNEL_CAPS_VERSION 1u
#define VDD_FRAME_CHANNEL_OPEN_VERSION 1u
#define VDD_FRAME_CHANNEL_MAX_SLOTS 8u
#define VDD_FRAME_CHANNEL_FLAG_SEALED_BORROW 0x00000001u
#define VDD_FRAME_CHANNEL_FLAG_UNNAMED_HANDLES 0x00000002u
#define VDD_FRAME_CHANNEL_FLAG_STRICT_DACL 0x00000004u

typedef struct VDD_FRAME_CHANNEL_CAPS {
  UINT32 Size;
  UINT32 Version;
  UINT32 Flags;
  UINT32 MaxSharedSlots;
  UINT32 MetadataSize;
  UINT32 Reserved0;
  UINT64 Reserved1;
} VDD_FRAME_CHANNEL_CAPS;

typedef struct VDD_FRAME_CHANNEL_OPEN_REQUEST {
  UINT32 Size;
  UINT32 Version;
  UINT32 MonitorIndex;
  UINT32 RequiredFlags;
  UINT32 TargetProcessId;
  UINT32 DesiredSlots;
  UINT32 AdapterLuidLowPart;
  INT32 AdapterLuidHighPart;
  UINT64 Reserved0;
} VDD_FRAME_CHANNEL_OPEN_REQUEST;

typedef struct VDD_FRAME_CHANNEL_SLOT_HANDLE {
  UINT64 TextureHandle;
  UINT64 Reserved0;
} VDD_FRAME_CHANNEL_SLOT_HANDLE;

typedef struct VDD_FRAME_CHANNEL_OPEN_RESPONSE {
  UINT32 Size;
  UINT32 Version;
  UINT32 Flags;
  UINT32 SlotCount;
  UINT32 MetadataSize;
  UINT32 Reserved0;
  UINT64 MetadataHandle;
  UINT64 FrameReadyEventHandle;
  VDD_FRAME_CHANNEL_SLOT_HANDLE Slots[VDD_FRAME_CHANNEL_MAX_SLOTS];
} VDD_FRAME_CHANNEL_OPEN_RESPONSE;

#ifdef __cplusplus
}
#endif

#endif  // _WIN32 || _WIN64
