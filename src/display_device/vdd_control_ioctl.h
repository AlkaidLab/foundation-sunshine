/**
 * @file vdd_control_ioctl.h
 * @brief Authoritative IOCTL contract between Sunshine and the ZakoVDD driver.
 *
 * This header MUST stay byte-for-byte identical with the copy that ships in
 * the Virtual-Display-Driver repository at
 *   `Common/Include/vdd_control_ioctl.h`
 * Both copies are intentionally duplicated to keep each repo self-contained.
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
 */

#pragma once

#include <Windows.h>
#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

// {DA9F8C2B-7E4F-49A1-9D4E-6F2B0E1A0C4D}
DEFINE_GUID(GUID_DEVINTERFACE_ZAKO_VDD_CONTROL,
  0xDA9F8C2B, 0x7E4F, 0x49A1, 0x9D, 0x4E, 0x6F, 0x2B, 0x0E, 0x1A, 0x0C, 0x4D);

// IOCTL function codes carved out of the driver-defined range (0x800+).
#define IOCTL_VDD_COMMAND \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)

#define IOCTL_VDD_PING \
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)

#ifdef __cplusplus
}
#endif
