/**
 * @file src/virtual_touchscreen_session.h
 * @brief Session-scoped virtual USB touchscreen lifecycle for Windows.
 *
 * When enabled for a stream session, this module attaches a virtual USB HID
 * touchscreen through usbip-win2 so the OS has a touch digitizer for the
 * duration of the session (enabling the touch keyboard and native touch
 * routing).  Touch reports are pushed with push_contact()/lift_all().
 *
 * Non-Windows platforms are no-ops.
 */
#pragma once

#include <cstdint>
#include <string>

namespace vts {

  struct session_config {
    /** Path to usbip.exe (usbip-win2).  Empty resolves "usbip.exe" via PATH. */
    std::string usbip_executable;
    /** Digitizer active-area resolution; matches the streamed resolution. */
    std::uint16_t width_px { 1920 };
    std::uint16_t height_px { 1080 };
  };

  /**
   * Attach the virtual touchscreen.  Blocking for up to ~10 s while the
   * VHCI imports the device.  Must be paired with stop().
   * @return true when the device is attached and visible to the OS.
   */
  bool
  start(const session_config &config);

  /** Lift all contacts and send a report (one per active contact). */
  void
  update_contact(std::uint8_t contact_id, std::uint16_t x, std::uint16_t y, bool tip);

  /** Release a contact (explicit lift frame). */
  void
  lift_contact(std::uint8_t contact_id);

  /** Detach the device and tear everything down. */
  void
  stop();

}  // namespace vts
