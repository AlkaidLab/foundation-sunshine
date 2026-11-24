/**
 * @file src/remote_usb/virtual_touchscreen_device.h
 * @brief A local virtual single-touch HID touchscreen served over the loopback
 *        USB/IP bridge, so usbip-win2 attaches it as a real USB touchscreen.
 *
 * The device answers control transfers (descriptors) and queued interrupt-IN
 * submits (touch frames) directly from Sunshine.  Because the touch input
 * enters Windows through the standard USB HID stack (hidusb.sys), the shell
 * sees a genuine touchscreen: the native touch-keyboard auto-invoke chain,
 * taskbar button and pointer integration all behave as with real hardware.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "loopback_usbip_bridge.h"

namespace remote_usb {

/** One finger sample in digitizer coordinates. */
struct touchscreen_contact {
  /** Stable per-finger id while the contact lives (0..0xFF). */
  std::uint8_t contact_id { 0 };
  std::uint16_t x { 0 };
  std::uint16_t y { 0 };
  /** Accepted for API stability; the current single-touch report layout
   *  does not carry pressure. */
  std::uint8_t pressure { 0 };
  /** Finger is touching the surface. */
  bool tip { false };
  /** Finger is tracked (hover or contact). */
  bool in_range { false };
};

/**
 * A single-client virtual USB single-touch touchscreen.  The class owns the
 * USB/IP device-side protocol state machine; the owner wires it to a
 * loopback_usbip_bridge with callbacks::on_request and send_reply(), and
 * calls reset() from callbacks::on_closed.
 *
 * Thread-safety: all public methods may be called from different threads;
 * internal state is mutex-protected.
 */
class virtual_touchscreen_device final {
public:
  struct config {
    /** false = HID touchscreen digitizer, true = plain boot-style mouse
     *  (control experiment to separate driver-stack issues from the
     *  touchscreen descriptor). */
    bool mouse_mode { false };
    std::uint16_t vendor_id { 0x5355 };  // "SS"
    std::uint16_t product_id { 0x5401 }; // touchscreen rev 1
    /** Digitizer active-area resolution (logical max of X/Y axes). */
    std::uint16_t width_px { 1920 };
    std::uint16_t height_px { 1080 };
    /** Reserved for a future parallel-slot descriptor.  The current
     *  implementation emits single-touch reports and ignores this value. */
    std::uint8_t finger_slots { 5 };
  };

  explicit virtual_touchscreen_device(config cfg);
  ~virtual_touchscreen_device();

  virtual_touchscreen_device(const virtual_touchscreen_device &) = delete;
  virtual_touchscreen_device &operator=(const virtual_touchscreen_device &) = delete;

  /** Descriptor metadata for loopback_usbip_bridge::start(). */
  device_info info() const;

  /**
   * Handle one complete USB/IP request PDU (OP_REQ_* or USBIP_CMD_*).
   * Replies are emitted through the send hook installed by set_send_reply().
   * Returns false when the PDU is malformed for the current state.
   */
  bool handle_request(const std::vector<std::uint8_t> &pdu);

  /** Install the reply hook (typically bridge.send_reply). */
  void set_send_reply(std::function<void(std::vector<std::uint8_t>)> send);

  /** True between a successful OP_REQ_IMPORT and reset(). */
  bool imported() const;

  /**
   * Drop all per-connection state (imported flag, queued frames, live
   * contacts, last mouse report, submit counter).  Call when the USB/IP
   * client disconnects so the next session does not inherit stale frames.
   */
  void reset();

  /**
   * Replace the current contact set.  Contacts present in `contacts` are
   * reported; previously active contacts missing from the set emit one lift
   * report (tip=0, in_range=0) and then go idle.
   */
  void update_contacts(const std::vector<touchscreen_contact> &contacts);

  /** Mouse mode: send a relative movement + button state frame. */
  void update_mouse(std::int8_t dx, std::int8_t dy, std::uint8_t buttons);

  /** Number of interrupt-IN submits answered so far (used by tests). */
  unsigned submitted_in_urbs() const;

private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace remote_usb
