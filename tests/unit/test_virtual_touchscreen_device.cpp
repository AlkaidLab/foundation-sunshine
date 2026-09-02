/**
 * @file tests/unit/test_virtual_touchscreen_device.cpp
 * @brief Protocol tests for the virtual USB single-touch touchscreen device.
 */

#include <src/remote_usb/virtual_touchscreen_device.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

  constexpr std::size_t kOpCommon = 8;
  constexpr std::size_t kDeviceBlock = 312;
  constexpr std::size_t kInterfaceBlock = 4;
  constexpr std::size_t kPduHeader = 48;
  constexpr std::int32_t kEpipe = -32;

  void
  put_u16be(std::vector<std::uint8_t> &out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 1] = static_cast<std::uint8_t>(value);
  }

  void
  put_u32be(std::vector<std::uint8_t> &out, std::size_t offset, std::uint32_t value) {
    out[offset] = static_cast<std::uint8_t>(value >> 24);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    out[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    out[offset + 3] = static_cast<std::uint8_t>(value);
  }

  std::uint16_t
  get_u16be_at(const std::vector<std::uint8_t> &in, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[offset]) << 8) | in[offset + 1]);
  }

  std::uint16_t
  get_u16le_at(const std::vector<std::uint8_t> &in, std::size_t offset) {
    return static_cast<std::uint16_t>(in[offset] | (in[offset + 1] << 8));
  }

  std::uint32_t
  get_u32be_at(const std::vector<std::uint8_t> &in, std::size_t offset) {
    return (static_cast<std::uint32_t>(in[offset]) << 24) |
           (static_cast<std::uint32_t>(in[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(in[offset + 2]) << 8) |
           static_cast<std::uint32_t>(in[offset + 3]);
  }

  std::int32_t
  get_i32be_at(const std::vector<std::uint8_t> &in, std::size_t offset) {
    return static_cast<std::int32_t>(get_u32be_at(in, offset));
  }

  std::vector<std::uint8_t>
  make_op_request(std::uint16_t code, const std::vector<std::uint8_t> &tail = {}) {
    std::vector<std::uint8_t> wire(8 + tail.size(), 0);
    put_u16be(wire, 0, 0x0111);
    put_u16be(wire, 2, code);
    put_u32be(wire, 4, 0);
    std::copy(tail.begin(), tail.end(), wire.begin() + 8);
    return wire;
  }

  std::vector<std::uint8_t>
  make_submit(std::uint32_t seqnum, std::uint32_t devid, std::uint32_t direction,
              std::uint32_t endpoint, const std::vector<std::uint8_t> &setup = {},
              const std::vector<std::uint8_t> &payload = {}) {
    std::vector<std::uint8_t> wire(48 + payload.size(), 0);
    put_u32be(wire, 0, 1);  // USBIP_CMD_SUBMIT
    put_u32be(wire, 4, seqnum);
    put_u32be(wire, 8, devid);
    put_u32be(wire, 12, direction);
    put_u32be(wire, 16, endpoint);
    put_u32be(wire, 24, static_cast<std::uint32_t>(payload.size()));
    std::copy(setup.begin(), setup.end(), wire.begin() + 40);
    std::copy(payload.begin(), payload.end(), wire.begin() + 48);
    return wire;
  }

  /** 32-byte NUL-padded busid tail for OP_REQ_IMPORT. */
  std::vector<std::uint8_t>
  make_busid(std::string_view busid) {
    std::vector<std::uint8_t> out(32, 0);
    std::copy(busid.begin(), busid.end(), out.begin());
    return out;
  }

  /** Collects device replies so tests can assert on them. */
  class reply_collector {
  public:
    std::function<void(std::vector<std::uint8_t>)>
    hook() {
      return [this](std::vector<std::uint8_t> wire) {
        std::lock_guard<std::mutex> lock(mutex);
        replies.push_back(std::move(wire));
      };
    }

    std::vector<std::vector<std::uint8_t>>
    take_all() {
      std::lock_guard<std::mutex> lock(mutex);
      return std::exchange(replies, {});
    }

    std::size_t
    count() {
      std::lock_guard<std::mutex> lock(mutex);
      return replies.size();
    }

  private:
    std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> replies;
  };

  class virtual_touchscreen_test : public ::testing::Test {
  protected:
    void
    SetUp() override {
      remote_usb::virtual_touchscreen_device::config cfg;
      cfg.width_px = 1920;
      cfg.height_px = 1080;
      cfg.finger_slots = 5;
      device = std::make_unique<remote_usb::virtual_touchscreen_device>(cfg);
      device->set_send_reply(collector.hook());
    }

    /** Issue one control transfer and return its single RET_SUBMIT (empty on failure). */
    std::vector<std::uint8_t>
    control(std::uint32_t seqnum, const std::vector<std::uint8_t> &setup) {
      EXPECT_TRUE(device->handle_request(make_submit(seqnum, 0, 0, 0, setup)));
      auto replies = collector.take_all();
      EXPECT_EQ(replies.size(), 1u);
      return replies.empty() ? std::vector<std::uint8_t> {} : replies[0];
    }

    reply_collector collector;
    std::unique_ptr<remote_usb::virtual_touchscreen_device> device;
  };

  class virtual_mouse_test : public ::testing::Test {
  protected:
    void
    SetUp() override {
      remote_usb::virtual_touchscreen_device::config cfg;
      cfg.mouse_mode = true;
      cfg.product_id = 0x5402;
      device = std::make_unique<remote_usb::virtual_touchscreen_device>(cfg);
      device->set_send_reply(collector.hook());
    }

    reply_collector collector;
    std::unique_ptr<remote_usb::virtual_touchscreen_device> device;
  };

}  // namespace

TEST_F(virtual_touchscreen_test, info_reports_hid_interface) {
  const auto info = device->info();
  EXPECT_EQ(info.vendor_id, 0x5355);
  EXPECT_EQ(info.product_id, 0x5401);
  EXPECT_EQ(info.busid, "1-2");
  EXPECT_EQ(info.speed, 3u);
  ASSERT_EQ(info.interfaces.size(), 1u);
  EXPECT_EQ(info.interfaces[0].interface_class, 0x03);
}

TEST_F(virtual_touchscreen_test, devlist_reply_contains_device_block) {
  ASSERT_TRUE(device->handle_request(make_op_request(0x8005)));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);

  const auto &wire = replies[0];
  ASSERT_EQ(wire.size(), kOpCommon + 4 + kDeviceBlock + kInterfaceBlock);
  // OP_REP header is big-endian: version 0x0111, code 0x0005, status ok.
  EXPECT_EQ(wire[0], 0x01);
  EXPECT_EQ(wire[1], 0x11);
  EXPECT_EQ(wire[2], 0x00);
  EXPECT_EQ(wire[3], 0x05);
  EXPECT_EQ(get_u32be_at(wire, 4), 0u);
  EXPECT_EQ(get_u32be_at(wire, kOpCommon), 1u);  // ndev

  // Device block: path(256) busid(32) then numeric fields.
  const std::size_t dev = kOpCommon + 4;
  const std::size_t numeric = dev + 256 + 32;
  EXPECT_EQ(get_u32be_at(wire, numeric + 0), 1u);      // busnum
  EXPECT_EQ(get_u32be_at(wire, numeric + 4), 1u);      // devnum
  EXPECT_EQ(get_u32be_at(wire, numeric + 8), 3u);      // speed high
  EXPECT_EQ(get_u16be_at(wire, numeric + 12), 0x5355);  // VID
  EXPECT_EQ(get_u16be_at(wire, numeric + 14), 0x5401);  // PID
  EXPECT_EQ(wire[dev + 311], 1u);                       // bNumInterfaces
  EXPECT_EQ(wire[dev + kDeviceBlock], 0x03);            // interface class HID
}

TEST_F(virtual_touchscreen_test, import_matches_busid_and_sets_imported) {
  ASSERT_TRUE(device->handle_request(make_op_request(0x8003, make_busid("1-2"))));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(get_u32be_at(replies[0], 4), 0u);  // status ok
  EXPECT_EQ(replies[0].size(), kOpCommon + kDeviceBlock);  // no interface block
  EXPECT_TRUE(device->imported());
}

TEST_F(virtual_touchscreen_test, import_rejects_foreign_busid) {
  ASSERT_TRUE(device->handle_request(make_op_request(0x8003, make_busid("9-9"))));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(static_cast<std::int32_t>(get_u32be_at(replies[0], 4)), -2);  // -ENODEV
  EXPECT_EQ(replies[0].size(), kOpCommon);
  EXPECT_FALSE(device->imported());
}

TEST_F(virtual_touchscreen_test, import_rejects_short_busid_tail) {
  EXPECT_FALSE(device->handle_request(make_op_request(0x8003, std::vector<std::uint8_t>(31, 0))));
  EXPECT_EQ(collector.count(), 0u);
}

TEST_F(virtual_touchscreen_test, get_device_descriptor) {
  // GET_DESCRIPTOR(DEVICE), wLength=18
  std::vector<std::uint8_t> setup = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
  ASSERT_TRUE(device->handle_request(make_submit(1, 0, 0, 0, setup)));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].size(), 48u + 18u);
  EXPECT_EQ(get_u32be_at(replies[0], 0), 3u);  // RET_SUBMIT
  EXPECT_EQ(get_u32be_at(replies[0], 4), 1u);  // seqnum echo
  EXPECT_EQ(get_i32be_at(replies[0], 20), 0);  // status
  EXPECT_EQ(get_i32be_at(replies[0], 24), 18);  // actual length
  const std::size_t data = 48;
  EXPECT_EQ(replies[0][data + 0], 0x12);
  EXPECT_EQ(replies[0][data + 1], 0x01);
  EXPECT_EQ(get_u16le_at(replies[0], data + 8), 0x5355);  // VID LE
  EXPECT_EQ(get_u16le_at(replies[0], data + 10), 0x5401);  // PID LE
}

TEST_F(virtual_touchscreen_test, get_config_descriptor_and_report_descriptor) {
  // GET_DESCRIPTOR(CONFIGURATION), wLength=255
  std::vector<std::uint8_t> setup = { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x00 };
  ASSERT_TRUE(device->handle_request(make_submit(2, 0, 0, 0, setup)));
  auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].size(), 48u + 34u);
  const std::size_t cfg = 48;
  EXPECT_EQ(replies[0][cfg + 0], 0x09);
  EXPECT_EQ(replies[0][cfg + 1], 0x02);
  EXPECT_EQ(get_u16le_at(replies[0], cfg + 2), 34u);  // wTotalLength
  EXPECT_EQ(replies[0][cfg + 9 + 5], 0x03);           // interface class: HID

  // GET_DESCRIPTOR(REPORT) via class request to interface
  std::vector<std::uint8_t> setup_report = { 0x81, 0x06, 0x00, 0x22, 0x00, 0x00, 0xFF, 0x00 };
  ASSERT_TRUE(device->handle_request(make_submit(3, 0, 0, 0, setup_report)));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_GT(replies[0].size(), 48u);
  const std::size_t len = replies[0].size() - 48u;
  EXPECT_EQ(len, 60u);
  EXPECT_EQ(replies[0][48 + 0], 0x05);  // Usage Page (Digitizers)
  EXPECT_EQ(replies[0][48 + 1], 0x0D);
}

TEST_F(virtual_touchscreen_test, touch_down_delivers_queued_report) {
  remote_usb::touchscreen_contact finger;
  finger.contact_id = 1;
  finger.x = 960;
  finger.y = 540;
  finger.pressure = 128;
  finger.tip = true;
  finger.in_range = true;
  device->update_contacts({ finger });
  // The frame is queued until the guest polls the endpoint.
  EXPECT_EQ(collector.take_all().size(), 0u);

  ASSERT_TRUE(device->handle_request(make_submit(10, 7, 1, 1)));
  EXPECT_EQ(device->submitted_in_urbs(), 1u);

  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  const auto &wire = replies[0];
  EXPECT_EQ(get_u32be_at(wire, 0), 3u);        // RET_SUBMIT
  EXPECT_EQ(get_u32be_at(wire, 4), 10u);       // seqnum echo
  EXPECT_EQ(get_u32be_at(wire, 8), 7u);        // devid echo
  EXPECT_EQ(get_u32be_at(wire, 12), 1u);       // direction IN
  EXPECT_EQ(get_u32be_at(wire, 16), 1u);       // endpoint
  EXPECT_EQ(get_i32be_at(wire, 20), 0);        // status
  EXPECT_EQ(get_i32be_at(wire, 24), 5);        // status + X16 + Y16

  const std::size_t r = 48;
  EXPECT_EQ(wire[r], 0x07);                    // tip | in range | confidence
  EXPECT_EQ(get_u16le_at(wire, r + 1), 960u);  // x
  EXPECT_EQ(get_u16le_at(wire, r + 3), 540u);  // y
}

TEST_F(virtual_touchscreen_test, held_contact_repeats_snapshot_per_poll) {
  remote_usb::touchscreen_contact finger;
  finger.contact_id = 1;
  finger.x = 100;
  finger.y = 200;
  finger.tip = true;
  finger.in_range = true;
  device->update_contacts({ finger });
  // No pending submit: nothing is sent until the guest polls.
  EXPECT_EQ(collector.take_all().size(), 0u);

  ASSERT_TRUE(device->handle_request(make_submit(11, 0, 1, 1)));
  auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0][48], 0x07);      // still down

  // Second poll while held: snapshot repeats.
  ASSERT_TRUE(device->handle_request(make_submit(12, 0, 1, 1)));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0][48], 0x07);
}

TEST_F(virtual_touchscreen_test, lift_emits_explicit_release_frame) {
  remote_usb::touchscreen_contact finger;
  finger.contact_id = 3;
  finger.x = 10;
  finger.y = 20;
  finger.tip = true;
  finger.in_range = true;
  device->update_contacts({ finger });
  // First poll consumes the down frame.
  ASSERT_TRUE(device->handle_request(make_submit(19, 0, 1, 1)));
  collector.take_all();

  // Lift: the next poll receives the explicit release frame.
  device->update_contacts({});

  ASSERT_TRUE(device->handle_request(make_submit(20, 0, 1, 1)));
  auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0][48], 0x00);      // released status byte

  // With no contacts left, further polls answer with zero-length transfers.
  ASSERT_TRUE(device->handle_request(make_submit(21, 0, 1, 1)));
  const auto idle = collector.take_all();
  ASSERT_EQ(idle.size(), 1u);
  EXPECT_EQ(idle[0].size(), 48u);
  EXPECT_EQ(get_i32be_at(idle[0], 24), 0);  // actual_length 0
}

TEST_F(virtual_touchscreen_test, unlink_unknown_seqnum_replies_ok) {
  // Idle polls answer immediately, so there is nothing parked to unlink; the
  // unlink still completes with status 0.
  ASSERT_TRUE(device->handle_request(make_submit(30, 0, 1, 1)));
  collector.take_all();

  std::vector<std::uint8_t> unlink(48, 0);
  put_u32be(unlink, 0, 2);   // USBIP_CMD_UNLINK
  put_u32be(unlink, 4, 31);  // this request's seqnum
  put_u32be(unlink, 20, 30); // unlink seqnum target
  ASSERT_TRUE(device->handle_request(unlink));

  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].size(), kPduHeader);
  EXPECT_EQ(get_u32be_at(replies[0], 0), 4u);  // RET_UNLINK
  EXPECT_EQ(get_u32be_at(replies[0], 4), 31u);
  EXPECT_EQ(get_i32be_at(replies[0], 20), 0);
}

TEST_F(virtual_touchscreen_test, malformed_request_rejected) {
  std::vector<std::uint8_t> short_pdu { 0x01, 0x11, 0x80, 0x05 };
  EXPECT_FALSE(device->handle_request(short_pdu));
  // DEVLIST must be exactly op_common; SUBMIT/UNLINK need the full header.
  EXPECT_FALSE(device->handle_request(make_op_request(0x8005, { 0x00 })));
  std::vector<std::uint8_t> short_submit(47, 0);
  put_u32be(short_submit, 0, 1);
  EXPECT_FALSE(device->handle_request(short_submit));
  std::vector<std::uint8_t> short_unlink(47, 0);
  put_u32be(short_unlink, 0, 2);
  EXPECT_FALSE(device->handle_request(short_unlink));
  // Unknown op code is not handled.
  EXPECT_FALSE(device->handle_request(make_op_request(0x8009)));
  EXPECT_EQ(collector.count(), 0u);
}

// ---- control-request branches ----

TEST_F(virtual_touchscreen_test, get_report_feature_stalls_without_feature_report) {
  // GET_REPORT(FEATURE, id 0) -- the descriptor declares no Feature report.
  const auto wire = control(40, { 0xA1, 0x01, 0x00, 0x03, 0x00, 0x00, 0x02, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(wire, 20), kEpipe);
}

TEST_F(virtual_touchscreen_test, get_report_input_returns_idle_snapshot) {
  // GET_REPORT(INPUT) with no contacts: 5-byte all-zero frame.
  const auto wire = control(41, { 0xA1, 0x01, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00 });
  ASSERT_EQ(wire.size(), kPduHeader + 5);
  EXPECT_EQ(get_i32be_at(wire, 20), 0);
  EXPECT_EQ(wire[kPduHeader], 0x00);
  EXPECT_EQ(get_u16le_at(wire, kPduHeader + 1), 0u);
  EXPECT_EQ(get_u16le_at(wire, kPduHeader + 3), 0u);
}

TEST_F(virtual_touchscreen_test, class_out_requests_are_acked) {
  // SET_IDLE
  auto wire = control(42, { 0x21, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(wire, 20), 0);
  // SET_REPORT(OUTPUT) carrying one data byte
  ASSERT_TRUE(device->handle_request(
    make_submit(43, 0, 0, 0, { 0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00 }, { 0xAA })));
  auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(replies[0], 20), 0);
}

TEST_F(virtual_touchscreen_test, standard_requests_status_and_unknown) {
  // GET_STATUS -> 2 zero bytes
  auto wire = control(44, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00 });
  ASSERT_EQ(wire.size(), kPduHeader + 2);
  EXPECT_EQ(wire[kPduHeader], 0x00);
  EXPECT_EQ(wire[kPduHeader + 1], 0x00);
  // SET_CONFIGURATION(1) -> zero-length ACK
  wire = control(45, { 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(wire, 20), 0);
  // Unknown bRequest (SET_ADDRESS is never seen by a device behind VHCI) -> STALL
  wire = control(46, { 0x00, 0x05, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(wire, 20), kEpipe);
}

TEST_F(virtual_touchscreen_test, string_descriptors) {
  // index 0: LANGID table
  auto wire = control(50, { 0x80, 0x06, 0x00, 0x03, 0x00, 0x00, 0xFF, 0x00 });
  ASSERT_EQ(wire.size(), kPduHeader + 4);
  EXPECT_EQ(wire[kPduHeader + 0], 0x04);
  EXPECT_EQ(wire[kPduHeader + 1], 0x03);
  EXPECT_EQ(get_u16le_at(wire, kPduHeader + 2), 0x0409);
  // index 1: "Sunshine" as UTF-16LE -> 2 + 8*2 bytes
  wire = control(51, { 0x80, 0x06, 0x01, 0x03, 0x09, 0x04, 0xFF, 0x00 });
  ASSERT_EQ(wire.size(), kPduHeader + 18);
  EXPECT_EQ(wire[kPduHeader + 0], 18);
  EXPECT_EQ(wire[kPduHeader + 1], 0x03);
  EXPECT_EQ(wire[kPduHeader + 2], 'S');
  EXPECT_EQ(wire[kPduHeader + 3], 0x00);
  // wLength truncation honoured
  wire = control(52, { 0x80, 0x06, 0x02, 0x03, 0x09, 0x04, 0x04, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader + 4);
  EXPECT_EQ(get_i32be_at(wire, 24), 4);
  // out-of-range index -> STALL
  wire = control(53, { 0x80, 0x06, 0x09, 0x03, 0x09, 0x04, 0xFF, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(wire, 20), kEpipe);
}

TEST_F(virtual_touchscreen_test, unknown_descriptor_type_stalls) {
  // GET_DESCRIPTOR(DEVICE_QUALIFIER = 0x06): not a high-speed-capable device.
  const auto wire = control(54, { 0x80, 0x06, 0x00, 0x06, 0x00, 0x00, 0x0A, 0x00 });
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(wire, 20), kEpipe);
}

TEST_F(virtual_touchscreen_test, interrupt_out_is_accepted_and_dropped) {
  ASSERT_TRUE(device->handle_request(make_submit(60, 5, 0, 1, {}, { 0x01, 0x02, 0x03 })));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  const auto &wire = replies[0];
  EXPECT_EQ(wire.size(), kPduHeader);
  EXPECT_EQ(get_u32be_at(wire, 0), 3u);    // RET_SUBMIT
  EXPECT_EQ(get_u32be_at(wire, 4), 60u);
  EXPECT_EQ(get_u32be_at(wire, 8), 5u);
  EXPECT_EQ(get_u32be_at(wire, 12), 0u);   // direction OUT echoed
  EXPECT_EQ(get_u32be_at(wire, 16), 1u);
  EXPECT_EQ(get_i32be_at(wire, 20), 0);
  EXPECT_EQ(get_i32be_at(wire, 24), 3);    // bytes consumed
  EXPECT_EQ(device->submitted_in_urbs(), 0u);
}

// ---- lifecycle ----

TEST_F(virtual_touchscreen_test, reset_clears_connection_state) {
  ASSERT_TRUE(device->handle_request(make_op_request(0x8003, make_busid("1-2"))));
  remote_usb::touchscreen_contact finger;
  finger.contact_id = 1;
  finger.x = 5;
  finger.y = 6;
  finger.tip = true;
  finger.in_range = true;
  device->update_contacts({ finger });
  ASSERT_TRUE(device->handle_request(make_submit(70, 0, 1, 1)));
  collector.take_all();
  ASSERT_TRUE(device->imported());
  ASSERT_EQ(device->submitted_in_urbs(), 1u);

  device->reset();
  EXPECT_FALSE(device->imported());
  EXPECT_EQ(device->submitted_in_urbs(), 0u);

  // Neither queued frames nor the live snapshot survive: the next poll of a
  // fresh session is idle.
  ASSERT_TRUE(device->handle_request(make_submit(71, 0, 1, 1)));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].size(), kPduHeader);
  EXPECT_EQ(get_i32be_at(replies[0], 24), 0);
}

TEST_F(virtual_touchscreen_test, queued_reports_are_bounded_drop_oldest) {
  // Each down->lift pair queues exactly one lift frame; overflow the cap.
  remote_usb::touchscreen_contact finger;
  finger.contact_id = 1;
  finger.tip = true;
  finger.in_range = true;
  const unsigned total = 100;
  for (unsigned i = 0; i < total; ++i) {
    finger.x = static_cast<std::uint16_t>(i);
    device->update_contacts({ finger });
    device->update_contacts({});
  }
  EXPECT_EQ(collector.count(), 0u);

  // Drain: at most 64 frames come back, and the first one is the oldest
  // survivor (x == total - 64), i.e. the earliest frames were dropped.
  unsigned frames = 0;
  std::uint16_t first_x = 0xFFFF;
  for (unsigned seq = 100; seq < 100 + total + 1; ++seq) {
    ASSERT_TRUE(device->handle_request(make_submit(seq, 0, 1, 1)));
    auto replies = collector.take_all();
    ASSERT_EQ(replies.size(), 1u);
    if (replies[0].size() == kPduHeader) {
      break;  // idle: queue drained
    }
    if (frames == 0) {
      first_x = get_u16le_at(replies[0], kPduHeader + 1);
    }
    ++frames;
  }
  EXPECT_EQ(frames, 64u);
  EXPECT_EQ(first_x, total - 64);
}

// ---- mouse control mode ----

TEST_F(virtual_mouse_test, mouse_descriptors_and_reports) {
  // Config descriptor advertises boot-mouse subclass/protocol.
  ASSERT_TRUE(device->handle_request(
    make_submit(80, 0, 0, 0, { 0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 0xFF, 0x00 })));
  auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].size(), kPduHeader + 34);
  EXPECT_EQ(replies[0][kPduHeader + 9 + 5], 0x03);  // HID
  EXPECT_EQ(replies[0][kPduHeader + 9 + 6], 0x01);  // boot subclass
  EXPECT_EQ(replies[0][kPduHeader + 9 + 7], 0x02);  // mouse protocol

  // Report descriptor starts with Generic Desktop / Mouse.
  ASSERT_TRUE(device->handle_request(
    make_submit(81, 0, 0, 0, { 0x81, 0x06, 0x00, 0x22, 0x00, 0x00, 0xFF, 0x00 })));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_GT(replies[0].size(), kPduHeader + 4);
  EXPECT_EQ(replies[0][kPduHeader + 0], 0x05);
  EXPECT_EQ(replies[0][kPduHeader + 1], 0x01);
  EXPECT_EQ(replies[0][kPduHeader + 2], 0x09);
  EXPECT_EQ(replies[0][kPduHeader + 3], 0x02);

  // GET_REPORT(INPUT) before any movement: neutral 3-byte report.
  ASSERT_TRUE(device->handle_request(
    make_submit(82, 0, 0, 0, { 0xA1, 0x01, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00 })));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].size(), kPduHeader + 3);
  EXPECT_EQ(replies[0][kPduHeader + 0], 0x00);

  // update_mouse queues one frame; the poll returns buttons, dx, dy.
  device->update_mouse(-3, 7, 0x01);
  ASSERT_TRUE(device->handle_request(make_submit(83, 0, 1, 1)));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].size(), kPduHeader + 3);
  EXPECT_EQ(replies[0][kPduHeader + 0], 0x01);
  EXPECT_EQ(static_cast<std::int8_t>(replies[0][kPduHeader + 1]), -3);
  EXPECT_EQ(static_cast<std::int8_t>(replies[0][kPduHeader + 2]), 7);

  // GET_REPORT(INPUT) now echoes the last mouse report.
  ASSERT_TRUE(device->handle_request(
    make_submit(84, 0, 0, 0, { 0xA1, 0x01, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00 })));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_EQ(replies[0].size(), kPduHeader + 3);
  EXPECT_EQ(replies[0][kPduHeader + 0], 0x01);
  EXPECT_EQ(static_cast<std::int8_t>(replies[0][kPduHeader + 1]), -3);

  // Mouse mode has no held snapshot: the next poll is idle.
  ASSERT_TRUE(device->handle_request(make_submit(85, 0, 1, 1)));
  replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].size(), kPduHeader);
}
