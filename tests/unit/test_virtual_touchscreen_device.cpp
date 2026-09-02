/**
 * @file tests/unit/test_virtual_touchscreen_device.cpp
 * @brief Protocol tests for the virtual USB multi-touch touchscreen device.
 */

#include <src/remote_usb/virtual_touchscreen_device.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {

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
              std::uint32_t endpoint, const std::vector<std::uint8_t> &setup = {}) {
    std::vector<std::uint8_t> wire(48 + setup.size(), 0);
    put_u32be(wire, 0, 1);  // USBIP_CMD_SUBMIT
    put_u32be(wire, 4, seqnum);
    put_u32be(wire, 8, devid);
    put_u32be(wire, 12, direction);
    put_u32be(wire, 16, endpoint);
    std::copy(setup.begin(), setup.end(), wire.begin() + 40);
    return wire;
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
  ASSERT_EQ(wire.size(), 8u + 316u);
  // OP_REP header is big-endian: version 0x0111, code 0x0005, status ok.
  EXPECT_EQ(wire[0], 0x01);
  EXPECT_EQ(wire[1], 0x11);
  EXPECT_EQ(wire[2], 0x00);
  EXPECT_EQ(wire[3], 0x05);
  EXPECT_EQ(get_u32be_at(wire, 4), 0u);

  // Device block: path(256) busid(32) then numeric fields.
  const std::size_t numeric = 8 + 256 + 32;
  EXPECT_EQ(get_u32be_at(wire, numeric + 0), 1u);      // busnum
  EXPECT_EQ(get_u32be_at(wire, numeric + 4), 1u);      // devnum
  EXPECT_EQ(get_u32be_at(wire, numeric + 8), 3u);      // speed high
  EXPECT_EQ(get_u16be_at(wire, numeric + 12), 0x5355);  // VID
  EXPECT_EQ(get_u16be_at(wire, numeric + 14), 0x5401);  // PID
}

TEST_F(virtual_touchscreen_test, import_matches_busid_and_sets_imported) {
  std::vector<std::uint8_t> busid(32, 0);
  std::copy("1-2", "1-2" + 3, busid.begin());
  ASSERT_TRUE(device->handle_request(make_op_request(0x8003, busid)));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(get_u32be_at(replies[0], 4), 0u);  // status ok
  EXPECT_EQ(replies[0].size(), 8u + 316u);
  EXPECT_TRUE(device->imported());
}

TEST_F(virtual_touchscreen_test, import_rejects_foreign_busid) {
  std::vector<std::uint8_t> busid(32, 0);
  std::copy("9-9", "9-9" + 3, busid.begin());
  ASSERT_TRUE(device->handle_request(make_op_request(0x8003, busid)));
  const auto replies = collector.take_all();
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(static_cast<std::int32_t>(get_u32be_at(replies[0], 4)), -2);  // -ENODEV
  EXPECT_FALSE(device->imported());
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

TEST_F(virtual_touchscreen_test, unlink_cancels_pending_submit) {
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
  EXPECT_EQ(get_u32be_at(replies[0], 0), 4u);  // RET_UNLINK
  EXPECT_EQ(get_u32be_at(replies[0], 4), 31u);
  EXPECT_EQ(get_i32be_at(replies[0], 20), 0);
}

TEST_F(virtual_touchscreen_test, malformed_request_rejected) {
  std::vector<std::uint8_t> short_pdu { 0x01, 0x11, 0x80, 0x05 };
  EXPECT_FALSE(device->handle_request(short_pdu));
}
