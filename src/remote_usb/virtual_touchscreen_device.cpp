/**
 * @file src/remote_usb/virtual_touchscreen_device.cpp
 * @brief Implementation of the virtual USB single-touch touchscreen.
 *
 * USB/IP PDU headers are big-endian (network order); USB descriptor/report
 * payloads are little-endian per the USB specification.
 */

#include "virtual_touchscreen_device.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace remote_usb {

namespace {

  constexpr std::uint16_t kUsbipVersion = 0x0111;

  constexpr std::uint16_t kOpReqDevlist = 0x8005;
  constexpr std::uint16_t kRepDevlist = 0x0005;
  constexpr std::uint16_t kOpReqImport = 0x8003;
  constexpr std::uint16_t kRepImport = 0x0003;

  constexpr std::uint32_t kCmdSubmit = 0x0001;
  constexpr std::uint32_t kCmdUnlink = 0x0002;
  constexpr std::uint32_t kRetSubmit = 0x0003;
  constexpr std::uint32_t kRetUnlink = 0x0004;

  constexpr std::size_t kPduHeaderSize = 48;
  constexpr std::size_t kOpCommonSize = 8;
  constexpr std::size_t kDeviceBlockSize = 312;
  constexpr std::size_t kInterfaceBlockSize = 4;

  constexpr std::int32_t kStallEpipe = -32;  // -EPIPE, usbip status convention

  // Frames buffered while the guest is not polling; oldest frames are dropped
  // beyond this so an unattached device cannot grow without bound.
  constexpr std::size_t kMaxQueuedReports = 64;

  void
  put_u16le(std::vector<std::uint8_t> &out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  }

  void
  put_u32be(std::uint8_t *out, std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>(value >> 24);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    out[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 3] = static_cast<std::uint8_t>(value);
  }

  void
  put_i32be(std::uint8_t *out, std::size_t offset, std::int32_t value) {
    put_u32be(out, offset, static_cast<std::uint32_t>(value));
  }

  void
  append_u16le(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
  }

  void
  append_u16be(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
  }

  void
  append_u32be(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
  }

  std::uint16_t
  get_u16be(const std::uint8_t *in) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[0]) << 8) | in[1]);
  }

  std::uint16_t
  get_u16le(const std::uint8_t *in) {
    return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
  }

  std::uint32_t
  get_u32be(const std::uint8_t *in) {
    return (static_cast<std::uint32_t>(in[0]) << 24) |
           (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) |
           static_cast<std::uint32_t>(in[3]);
  }

  /** Fixed-size, NUL-padded USB/IP wire string. */
  std::vector<std::uint8_t>
  padded_string(const std::string &value, std::size_t size) {
    std::vector<std::uint8_t> out(size, 0);
    std::memcpy(out.data(), value.data(), std::min(value.size(), size));
    return out;
  }

  std::vector<std::uint8_t>
  hid_utf16_string(const std::string &value) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>((value.size() * 2 + 2) & 0xFF));
    out.push_back(0x03);  // bDescriptorType = STRING
    for (char c : value) {
      append_u16le(out, static_cast<std::uint16_t>(static_cast<unsigned char>(c)));
    }
    return out;
  }

  /**
   * Minimal 3-byte boot-style mouse report descriptor: buttons(1B) + dx + dy.
   */
  std::vector<std::uint8_t>
  build_mouse_report_descriptor() {
    std::vector<std::uint8_t> d;
    d.insert(d.end(), { 0x05, 0x01 });  // Usage Page (Generic Desktop)
    d.insert(d.end(), { 0x09, 0x02 });  // Usage (Mouse)
    d.insert(d.end(), { 0xA1, 0x01 });  // Collection (Application)
    d.insert(d.end(), { 0x09, 0x01 });  //   Usage (Pointer)
    d.insert(d.end(), { 0xA1, 0x00 });  //   Collection (Physical)
    d.insert(d.end(), { 0x05, 0x09 });  //     Usage Page (Buttons)
    d.insert(d.end(), { 0x19, 0x01 });  //     Usage Min (1)
    d.insert(d.end(), { 0x29, 0x03 });  //     Usage Max (3)
    d.insert(d.end(), { 0x15, 0x00 });  //     Logical Min (0)
    d.insert(d.end(), { 0x25, 0x01 });  //     Logical Max (1)
    d.insert(d.end(), { 0x75, 0x01 });  //     Report Size (1)
    d.insert(d.end(), { 0x95, 0x03 });  //     Report Count (3)
    d.insert(d.end(), { 0x81, 0x02 });  //     Input (Data,Var,Abs)
    d.insert(d.end(), { 0x75, 0x01 });  //     Report Size (1)
    d.insert(d.end(), { 0x95, 0x05 });  //     Report Count (5)
    d.insert(d.end(), { 0x81, 0x03 });  //     Input (Const) padding
    d.insert(d.end(), { 0x05, 0x01 });  //     Usage Page (Generic Desktop)
    d.insert(d.end(), { 0x09, 0x30 });  //     Usage (X)
    d.insert(d.end(), { 0x09, 0x31 });  //     Usage (Y)
    d.insert(d.end(), { 0x15, 0x81 });  //     Logical Min (-127)
    d.insert(d.end(), { 0x25, 0x7F });  //     Logical Max (127)
    d.insert(d.end(), { 0x75, 0x08 });  //     Report Size (8)
    d.insert(d.end(), { 0x95, 0x02 });  //     Report Count (2)
    d.insert(d.end(), { 0x81, 0x06 });  //     Input (Data,Var,Rel)
    d.insert(d.end(), { 0xC0 });        //   End Collection
    d.insert(d.end(), { 0xC0 });        // End Collection
    return d;
  }

  /** 3-byte mouse input report: buttons, dx, dy. */
  std::vector<std::uint8_t>
  build_mouse_report(std::int8_t dx, std::int8_t dy, std::uint8_t buttons) {
    return { buttons, static_cast<std::uint8_t>(dx), static_cast<std::uint8_t>(dy) };
  }

  /**
   * Minimal single-touch touchscreen descriptor in the style of decades-old
   * USB resistive touch controllers (eGalax et al): Tip/In-Range + 16-bit
   * absolute X/Y, no report id, no contact id.  5-byte input reports.
   */
  std::vector<std::uint8_t>
  build_report_descriptor(std::uint16_t width_px, std::uint16_t height_px, std::uint8_t slots) {
    (void) slots;
    const std::uint16_t x_max = static_cast<std::uint16_t>(width_px - 1);
    const std::uint16_t y_max = static_cast<std::uint16_t>(height_px - 1);

    std::vector<std::uint8_t> d;
    d.reserve(96);

    d.insert(d.end(), { 0x05, 0x0D });  // Usage Page (Digitizers)
    d.insert(d.end(), { 0x09, 0x04 });  // Usage (Touch Screen)
    d.insert(d.end(), { 0xA1, 0x01 });  // Collection (Application)
    d.insert(d.end(), { 0x09, 0x22 });  //   Usage (Finger)
    d.insert(d.end(), { 0xA1, 0x02 });  //   Collection (Logical)
    d.insert(d.end(), { 0x09, 0x42 });  //     Usage (Tip Switch)
    d.insert(d.end(), { 0x15, 0x00 });  //     Logical Min (0)
    d.insert(d.end(), { 0x25, 0x01 });  //     Logical Max (1)
    d.insert(d.end(), { 0x75, 0x01 });  //     Report Size (1)
    d.insert(d.end(), { 0x95, 0x01 });  //     Report Count (1)
    d.insert(d.end(), { 0x81, 0x02 });  //     Input (Data,Var,Abs)
    d.insert(d.end(), { 0x09, 0x32 });  //     Usage (In Range)
    d.insert(d.end(), { 0x81, 0x02 });  //     Input (Data,Var,Abs)
    d.insert(d.end(), { 0x09, 0x47 });  //     Usage (Confidence)
    d.insert(d.end(), { 0x81, 0x02 });  //     Input (Data,Var,Abs)
    d.insert(d.end(), { 0x95, 0x05 });  //     Report Count (5)
    d.insert(d.end(), { 0x81, 0x03 });  //     Input (Const) padding
    d.insert(d.end(), { 0x05, 0x01 });  //     Usage Page (Generic Desktop)
    d.insert(d.end(), { 0x09, 0x30 });  //     Usage (X)
    d.insert(d.end(), { 0x15, 0x00 });  //     Logical Min (0)
    d.insert(d.end(), { 0x26, static_cast<std::uint8_t>(x_max & 0xFF),
                        static_cast<std::uint8_t>(x_max >> 8) });  // Logical Max
    d.insert(d.end(), { 0x75, 0x10 });  //     Report Size (16)
    d.insert(d.end(), { 0x95, 0x01 });  //     Report Count (1)
    d.insert(d.end(), { 0x81, 0x02 });  //     Input (Data,Var,Abs)
    d.insert(d.end(), { 0x09, 0x31 });  //     Usage (Y)
    d.insert(d.end(), { 0x15, 0x00 });  //     Logical Min (0)
    d.insert(d.end(), { 0x26, static_cast<std::uint8_t>(y_max & 0xFF),
                        static_cast<std::uint8_t>(y_max >> 8) });  // Logical Max
    d.insert(d.end(), { 0x81, 0x02 });  //     Input (Data,Var,Abs)
    d.insert(d.end(), { 0xC0 });        //   End Collection
    d.insert(d.end(), { 0xC0 });        // End Collection

    return d;
  }

  /** One live finger slot as it appears in the input report. */
  struct report_finger {
    std::uint8_t contact_id { 0 };
    std::uint16_t x { 0 };
    std::uint16_t y { 0 };
    std::uint8_t pressure { 0 };
    bool tip { false };
    bool in_range { false };
  };

  bool
  finger_is_live(const report_finger &finger) {
    return finger.tip || finger.in_range;
  }

  std::vector<std::uint8_t>
  build_input_report(const std::vector<report_finger> &fingers, std::uint8_t slots) {
    (void) slots;
    // 5-byte report: status byte, X (LE16), Y (LE16).  Report the first
    // live finger, or the lifted finger during a release frame.
    std::size_t picked = fingers.size();
    for (std::size_t i = 0; i < fingers.size(); ++i) {
      if (finger_is_live(fingers[i])) {
        picked = i;
        break;
      }
    }
    if (picked == fingers.size() && !fingers.empty()) {
      picked = 0;  // release frame: first finger carries the lift
    }

    std::uint8_t bits = 0;
    std::uint16_t x = 0, y = 0;
    if (picked < fingers.size()) {
      const auto &finger = fingers[picked];
      if (finger.tip) {
        bits |= 0x01;
      }
      if (finger.in_range) {
        bits |= 0x02;
        bits |= 0x04;  // confidence: valid touch
      }
      x = finger.x;
      y = finger.y;
    }
    std::vector<std::uint8_t> report;
    report.reserve(5);
    report.push_back(bits);
    append_u16le(report, x);
    append_u16le(report, y);
    return report;
  }


}  // namespace

struct virtual_touchscreen_device::impl {
  config cfg;
  std::function<void(std::vector<std::uint8_t>)> send;
  std::string busid;
  std::string path;

  // State shared between the bridge I/O thread and the touch producer.
  std::mutex mutex;
  bool imported_flag { false };
  unsigned in_submit_count { 0 };

  std::vector<std::uint8_t> report_descriptor;
  std::vector<std::uint8_t> config_descriptor;
  std::size_t interrupt_packet_size { 64 };

  /** Addressing of one interrupt-IN submit, echoed back in RET_SUBMIT. */
  struct pending_submit {
    std::uint32_t seqnum;
    std::uint32_t devid;
    std::uint32_t endpoint;
  };

  // Frames queued for delivery (lift transitions, then the live snapshot).
  std::vector<std::vector<std::uint8_t>> queued_reports;
  // Live contact snapshot; re-sent for every poll while contacts stay down.
  std::vector<report_finger> active_fingers;
  std::vector<std::uint8_t> last_mouse_report;

  explicit impl(config config_in) :
      cfg(std::move(config_in)) {
    report_descriptor = cfg.mouse_mode
                          ? build_mouse_report_descriptor()
                          : build_report_descriptor(cfg.width_px, cfg.height_px, cfg.finger_slots);
    build_config_descriptor();
  }

  void
  build_config_descriptor() {
    const std::size_t report_size = 5;  // minimal single-touch report
    interrupt_packet_size = report_size;
    const std::uint16_t packet_size = static_cast<std::uint16_t>(
      std::max<std::size_t>(report_size, 64));

    config_descriptor.clear();
    config_descriptor.reserve(34);

    // CONFIGURATION
    config_descriptor.insert(config_descriptor.end(), { 0x09, 0x02 });
    append_u16le(config_descriptor, 34);  // wTotalLength
    config_descriptor.push_back(0x01);    // bNumInterfaces
    config_descriptor.push_back(0x01);    // bConfigurationValue
    config_descriptor.push_back(0x00);    // iConfiguration
    config_descriptor.push_back(0x80);    // bmAttributes: bus-powered
    config_descriptor.push_back(0x32);    // bMaxPower: 100 mA
    // INTERFACE
    config_descriptor.insert(config_descriptor.end(), { 0x09, 0x04 });
    config_descriptor.push_back(0x00);    // bInterfaceNumber
    config_descriptor.push_back(0x00);    // bAlternateSetting
    config_descriptor.push_back(0x01);    // bNumEndpoints
    config_descriptor.push_back(0x03);    // bInterfaceClass: HID
    config_descriptor.push_back(cfg.mouse_mode ? 0x01 : 0x00);  // subclass: boot
    config_descriptor.push_back(cfg.mouse_mode ? 0x02 : 0x00);  // protocol: mouse
    config_descriptor.push_back(0x00);    // iInterface
    // HID class descriptor
    config_descriptor.insert(config_descriptor.end(), { 0x09, 0x21 });
    append_u16le(config_descriptor, 0x0111);  // bcdHID
    config_descriptor.push_back(0x00);        // bCountryCode
    config_descriptor.push_back(0x01);        // bNumDescriptors
    config_descriptor.push_back(0x22);        // bDescriptorType: REPORT
    append_u16le(config_descriptor, static_cast<std::uint16_t>(report_descriptor.size()));
    // ENDPOINT (interrupt IN)
    config_descriptor.insert(config_descriptor.end(), { 0x07, 0x05 });
    config_descriptor.push_back(0x81);    // bEndpointAddress: IN, EP1
    config_descriptor.push_back(0x03);    // bmAttributes: interrupt
    append_u16le(config_descriptor, packet_size);
    config_descriptor.push_back(0x04);    // bInterval: 1 ms @ high speed
  }

  /** Bounded FIFO push; drops the oldest frame once the cap is reached. */
  void
  push_report_locked(std::vector<std::uint8_t> report) {
    if (queued_reports.size() >= kMaxQueuedReports) {
      queued_reports.erase(queued_reports.begin());
    }
    queued_reports.push_back(std::move(report));
  }

  /** Clears all per-connection state so a new client starts clean. */
  void
  reset_locked() {
    imported_flag = false;
    in_submit_count = 0;
    queued_reports.clear();
    active_fingers.clear();
    last_mouse_report.clear();
  }

  /** 312-byte `usbip_usb_device` block (shared by DEVLIST and IMPORT). */
  void
  append_device_block(std::vector<std::uint8_t> &out) const {
    auto path_str = padded_string(path, 256);
    out.insert(out.end(), path_str.begin(), path_str.end());
    auto busid_str = padded_string(busid, 32);
    out.insert(out.end(), busid_str.begin(), busid_str.end());

    append_u32be(out, 1);                                  // busnum
    append_u32be(out, 1);                                  // devnum
    append_u32be(out, 3);                                  // speed: high

    append_u16be(out, cfg.vendor_id);
    append_u16be(out, cfg.product_id);
    append_u16be(out, 0x0100);                             // bcdDevice

    out.push_back(0x00);                                   // bDeviceClass: per-interface
    out.push_back(0x00);                                   // bDeviceSubClass
    out.push_back(0x00);                                   // bDeviceProtocol
    out.push_back(0x01);                                   // bConfigurationValue
    out.push_back(0x01);                                   // bNumConfigurations
    out.push_back(0x01);                                   // bNumInterfaces
  }

  /** 4-byte `usbip_usb_interface` block (DEVLIST only). */
  void
  append_interface_block(std::vector<std::uint8_t> &out) const {
    out.push_back(0x03);                                   // bInterfaceClass: HID
    out.push_back(0x00);                                   // bInterfaceSubClass
    out.push_back(0x00);                                   // bInterfaceProtocol
    out.push_back(0x00);                                   // padding
  }

  void
  reply_submit_locked(std::uint32_t seqnum, std::uint32_t devid, std::uint32_t endpoint,
                      std::int32_t status, const std::uint8_t *data, std::size_t size) {
    std::vector<std::uint8_t> wire(kPduHeaderSize + size, 0);
    put_u32be(wire.data(), 0, kRetSubmit);
    put_u32be(wire.data(), 4, seqnum);
    put_u32be(wire.data(), 8, devid);
    put_u32be(wire.data(), 12, 1);  // direction: IN
    put_u32be(wire.data(), 16, endpoint);
    put_i32be(wire.data(), 20, status);
    put_i32be(wire.data(), 24, static_cast<std::int32_t>(size));
    put_i32be(wire.data(), 32, -1);  // number_of_packets
    put_i32be(wire.data(), 36, 0);   // error_count
    if (size != 0) {
      std::memcpy(wire.data() + kPduHeaderSize, data, size);
    }
    if (send) {
      send(std::move(wire));
    }
  }

  void
  reply_unlink_locked(std::uint32_t seqnum, std::int32_t status) {
    std::vector<std::uint8_t> wire(kPduHeaderSize, 0);
    put_u32be(wire.data(), 0, kRetUnlink);
    put_u32be(wire.data(), 4, seqnum);
    put_i32be(wire.data(), 20, status);
    if (send) {
      send(std::move(wire));
    }
  }

  void
  reply_control_locked(std::uint32_t seqnum, const std::uint8_t *data, std::size_t size,
                       std::int32_t status) {
    std::vector<std::uint8_t> wire(kPduHeaderSize + size, 0);
    put_u32be(wire.data(), 0, kRetSubmit);
    put_u32be(wire.data(), 4, seqnum);
    put_u32be(wire.data(), 8, 0);    // devid: canonical control reply
    put_u32be(wire.data(), 12, 0);   // direction: OUT
    put_u32be(wire.data(), 16, 0);   // endpoint: control
    put_i32be(wire.data(), 20, status);
    put_i32be(wire.data(), 24, static_cast<std::int32_t>(size));
    put_i32be(wire.data(), 32, -1);
    put_i32be(wire.data(), 36, 0);
    if (size != 0) {
      std::memcpy(wire.data() + kPduHeaderSize, data, size);
    }
    if (send) {
      send(std::move(wire));
    }
  }

  void
  handle_control_submit_locked(std::uint32_t seqnum, const std::uint8_t *setup,
                               const std::vector<std::uint8_t> &out_data) {
    (void) out_data;
    const std::uint8_t bmRequestType = setup[0];
    const std::uint8_t bRequest = setup[1];
    const std::uint16_t wValue = get_u16le(setup + 2);
    const std::uint16_t wIndex = get_u16le(setup + 4);
    const std::uint16_t wLength = get_u16le(setup + 6);
    (void) wIndex;

    const bool dir_in = (bmRequestType & 0x80) != 0;
    const bool class_request = (bmRequestType & 0x60) == 0x20;

    if (class_request) {
      if (dir_in) {
        // GET_REPORT.  Neither descriptor declares Feature/Output reports, so
        // only INPUT is answerable; anything else stalls.
        const std::uint8_t report_type = static_cast<std::uint8_t>(wValue >> 8);
        if (report_type != 0x01) {
          reply_control_locked(seqnum, nullptr, 0, kStallEpipe);
          return;
        }
        std::vector<std::uint8_t> report;
        if (cfg.mouse_mode) {  // last mouse report
          report = last_mouse_report.empty() ? build_mouse_report(0, 0, 0) : last_mouse_report;
        }
        else {                 // idle-touch snapshot
          report = build_input_report(active_fingers, cfg.finger_slots);
        }
        const std::size_t size = std::min<std::size_t>(report.size(), wLength);
        reply_control_locked(seqnum, report.data(), size, 0);
      }
      else {
        // SET_IDLE / SET_PROTOCOL / SET_REPORT: acknowledge silently.
        reply_control_locked(seqnum, nullptr, 0, 0);
      }
      return;
    }

    if (bRequest == 0x09 || bRequest == 0x01 || bRequest == 0x03) {
      // SET_CONFIGURATION / CLEAR_FEATURE / SET_FEATURE
      reply_control_locked(seqnum, nullptr, 0, 0);
      return;
    }
    if (bRequest == 0x00) {
      // GET_STATUS
      const std::uint8_t status[2] = { 0x00, 0x00 };
      reply_control_locked(seqnum, status, std::min<std::size_t>(2, wLength), 0);
      return;
    }
    if (bRequest != 0x06) {
      reply_control_locked(seqnum, nullptr, 0, kStallEpipe);
      return;
    }

    // GET_DESCRIPTOR
    const std::uint8_t type = static_cast<std::uint8_t>(wValue >> 8);
    const std::uint8_t index = static_cast<std::uint8_t>(wValue & 0xFF);
    std::vector<std::uint8_t> descriptor;

    switch (type) {
      case 0x01: {  // DEVICE
        descriptor.resize(18);
        descriptor[0] = 0x12;
        descriptor[1] = 0x01;
        put_u16le(descriptor, 2, 0x0200);  // bcdUSB
        descriptor[4] = 0x00;              // class per-interface
        descriptor[5] = 0x00;
        descriptor[6] = 0x00;
        descriptor[7] = 0x40;              // bMaxPacketSize0
        put_u16le(descriptor, 8, cfg.vendor_id);
        put_u16le(descriptor, 10, cfg.product_id);
        put_u16le(descriptor, 12, 0x0100);  // bcdDevice
        descriptor[14] = 0x01;              // iManufacturer
        descriptor[15] = 0x02;              // iProduct
        descriptor[16] = 0x03;              // iSerialNumber
        descriptor[17] = 0x01;              // bNumConfigurations
        break;
      }
      case 0x02:  // CONFIGURATION
        descriptor = config_descriptor;
        break;
      case 0x03: {  // STRING
        static const char *kStrings[] = { nullptr, "Sunshine", "Sunshine Virtual Touchscreen", "SSVTS0001" };
        if (index == 0) {
          descriptor = { 0x04, 0x03, 0x09, 0x04 };  // LANGID 0x0409
        }
        else if (index < std::size(kStrings) && kStrings[index] != nullptr) {
          descriptor = hid_utf16_string(kStrings[index]);
        }
        else {
          reply_control_locked(seqnum, nullptr, 0, kStallEpipe);
          return;
        }
        break;
      }
      case 0x22:  // HID REPORT
        descriptor = report_descriptor;
        break;
      default:
        reply_control_locked(seqnum, nullptr, 0, kStallEpipe);
        return;
    }

    const std::size_t size = std::min<std::size_t>(descriptor.size(), wLength);
    reply_control_locked(seqnum, descriptor.data(), size, 0);
  }
};

virtual_touchscreen_device::virtual_touchscreen_device(config cfg) :
    impl_(new impl(std::move(cfg))) {
  impl_->busid = "1-2";
  impl_->path = "/sys/devices/pci0000:00/1-2";
}

virtual_touchscreen_device::~virtual_touchscreen_device() = default;

device_info
virtual_touchscreen_device::info() const {
  device_info dev;
  dev.busid = impl_->busid;
  dev.path = impl_->path;
  dev.busnum = 1;
  dev.devnum = 1;
  dev.speed = 3;
  dev.vendor_id = impl_->cfg.vendor_id;
  dev.product_id = impl_->cfg.product_id;
  dev.device_bcd = 0x0100;
  dev.device_class = 0x00;
  dev.device_subclass = 0x00;
  dev.device_protocol = 0x00;
  dev.configuration_value = 1;
  dev.num_configurations = 1;
  dev.interfaces.push_back(interface_info { 0x03, 0x00, 0x00 });
  return dev;
}

void
virtual_touchscreen_device::set_send_reply(std::function<void(std::vector<std::uint8_t>)> send) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->send = std::move(send);
}

void
virtual_touchscreen_device::reset() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->reset_locked();
}

bool
virtual_touchscreen_device::imported() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->imported_flag;
}

unsigned
virtual_touchscreen_device::submitted_in_urbs() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->in_submit_count;
}

bool
virtual_touchscreen_device::handle_request(const std::vector<std::uint8_t> &pdu) {
  if (pdu.size() < 8) {
    return false;
  }
  const std::uint16_t code = get_u16be(pdu.data() + 2);

  std::lock_guard<std::mutex> lock(impl_->mutex);

  if (code == kOpReqDevlist) {
    if (pdu.size() != kOpCommonSize) {
      return false;
    }
    // op_common + ndev + usbip_usb_device + usbip_usb_interface[bNumInterfaces]
    std::vector<std::uint8_t> wire;
    wire.reserve(kOpCommonSize + 4 + kDeviceBlockSize + kInterfaceBlockSize);
    append_u16be(wire, kUsbipVersion);
    append_u16be(wire, kRepDevlist);
    append_u32be(wire, 0);  // status: ok
    append_u32be(wire, 1);  // ndev
    impl_->append_device_block(wire);
    impl_->append_interface_block(wire);
    if (impl_->send) {
      impl_->send(std::move(wire));
    }
    return true;
  }

  if (code == kOpReqImport) {
    if (pdu.size() < kOpCommonSize + 32) {
      return false;
    }
    const auto requested = padded_string(impl_->busid, 32);
    const bool match = std::equal(requested.begin(), requested.end(), pdu.begin() + kOpCommonSize);
    // op_common + usbip_usb_device (no interface block on IMPORT)
    std::vector<std::uint8_t> wire;
    wire.reserve(kOpCommonSize + kDeviceBlockSize);
    append_u16be(wire, kUsbipVersion);
    append_u16be(wire, kRepImport);
    append_u32be(wire, match ? 0 : static_cast<std::uint32_t>(-2));  // -ENODEV on mismatch
    if (match) {
      impl_->append_device_block(wire);
      impl_->imported_flag = true;
    }
    if (impl_->send) {
      impl_->send(std::move(wire));
    }
    return true;
  }

  if (code == kCmdSubmit) {
    if (pdu.size() < kPduHeaderSize) {
      return false;
    }
    const std::uint32_t seqnum = get_u32be(pdu.data() + 4);
    const std::uint32_t devid = get_u32be(pdu.data() + 8);
    const std::uint32_t direction = get_u32be(pdu.data() + 12);
    const std::uint32_t endpoint = get_u32be(pdu.data() + 16);

    if (endpoint == 0) {
      const std::uint8_t *setup = pdu.data() + 40;
      std::vector<std::uint8_t> out_data;
      if (direction == 0 && pdu.size() > kPduHeaderSize) {
        out_data.assign(pdu.begin() + kPduHeaderSize, pdu.end());
      }
      impl_->handle_control_submit_locked(seqnum, setup, out_data);
      return true;
    }

    if (direction == 1) {
      ++impl_->in_submit_count;
      // Real interrupt endpoints are polled: answer queued frames first,
      // repeat the live snapshot while contacts are held.  When idle, answer
      // with a zero-length transfer -- parking the URB makes usbip-win2
      // consider the device unresponsive and reset the connection.
      if (!impl_->queued_reports.empty()) {
        auto report = std::move(impl_->queued_reports.front());
        impl_->queued_reports.erase(impl_->queued_reports.begin());
        impl_->reply_submit_locked(seqnum, devid, endpoint, 0, report.data(), report.size());
      }
      else if (!impl_->cfg.mouse_mode && !impl_->active_fingers.empty()) {
        auto report = build_input_report(impl_->active_fingers, impl_->cfg.finger_slots);
        impl_->reply_submit_locked(seqnum, devid, endpoint, 0, report.data(), report.size());
      }
      else {
        impl_->reply_submit_locked(seqnum, devid, endpoint, 0, nullptr, 0);
      }
      return true;
    }

    // Unexpected OUT data to an interrupt endpoint: accept and drop.
    std::vector<std::uint8_t> wire(kPduHeaderSize, 0);
    put_u32be(wire.data(), 0, kRetSubmit);
    put_u32be(wire.data(), 4, seqnum);
    put_u32be(wire.data(), 8, devid);
    put_u32be(wire.data(), 12, direction);
    put_u32be(wire.data(), 16, endpoint);
    put_i32be(wire.data(), 20, 0);
    put_i32be(wire.data(), 24, static_cast<std::int32_t>(pdu.size() - kPduHeaderSize));
    put_i32be(wire.data(), 32, -1);
    put_i32be(wire.data(), 36, 0);
    if (impl_->send) {
      impl_->send(std::move(wire));
    }
    return true;
  }

  if (code == kCmdUnlink) {
    if (pdu.size() < kPduHeaderSize) {
      return false;
    }
    // Every submit is answered synchronously, so nothing is ever parked and
    // there is nothing to cancel; acknowledge the unlink as a no-op.
    const std::uint32_t seqnum = get_u32be(pdu.data() + 4);
    impl_->reply_unlink_locked(seqnum, 0);
    return true;
  }

  return false;
}

void
virtual_touchscreen_device::update_mouse(std::int8_t dx, std::int8_t dy, std::uint8_t buttons) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->last_mouse_report = build_mouse_report(dx, dy, buttons);
  impl_->push_report_locked(impl_->last_mouse_report);
}

void
virtual_touchscreen_device::update_contacts(const std::vector<touchscreen_contact> &contacts) {
  std::lock_guard<std::mutex> lock(impl_->mutex);

  // Index the requested contact set by id.
  std::map<std::uint8_t, report_finger> next;
  for (const auto &contact : contacts) {
    next[contact.contact_id] = report_finger {
      contact.contact_id,
      contact.x,
      contact.y,
      contact.pressure,
      contact.tip,
      contact.in_range,
    };
  }

  // Contacts that disappeared since the last update emit one explicit lift
  // frame (tip=0, in_range=0) so the touch stack releases them cleanly.  The
  // lift frame carries only the released contacts: build_input_report()
  // prefers a live finger, so mixing would swallow the lift.
  std::vector<report_finger> lifted_only;
  for (const auto &finger : impl_->active_fingers) {
    if (finger_is_live(finger) && !next.contains(finger.contact_id)) {
      lifted_only.push_back(report_finger {
        finger.contact_id, finger.x, finger.y, 0, false, false,
      });
    }
  }
  if (!lifted_only.empty()) {
    impl_->push_report_locked(build_input_report(lifted_only, impl_->cfg.finger_slots));
  }

  std::vector<report_finger> snapshot;
  snapshot.reserve(next.size());
  for (const auto &[id, finger] : next) {
    (void) id;
    snapshot.push_back(finger);
  }
  impl_->active_fingers = std::move(snapshot);
}

}  // namespace remote_usb
