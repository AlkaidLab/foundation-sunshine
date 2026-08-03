/**
 * @file tests/unit/test_input_activity.cpp
 * @brief Tests for VRR input activity classification.
 */

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <moonlight-common-c/src/Input.h>

#include <src/config.h>
#include <src/cursor_channel.h>
#include <src/input_activity.h>
#include <src/utility.h>

#ifdef INPUT_ACTIVITY_STANDALONE_TEST
namespace config {
  input_t input;
}
#endif

namespace {
  constexpr std::uint32_t session_id = 0xFFFFFFFEu;

  template <typename Packet>
  Packet
  make_packet(std::uint32_t magic) {
    Packet packet {};
    packet.header.size = util::endian::big<std::uint32_t>(sizeof(Packet) - sizeof(packet.header.size));
    packet.header.magic = util::endian::little(magic);
    return packet;
  }

  template <typename Packet>
  std::vector<std::uint8_t>
  packet_bytes(const Packet &packet) {
    std::vector<std::uint8_t> bytes(sizeof(packet));
    std::memcpy(bytes.data(), &packet, sizeof(packet));
    return bytes;
  }

  struct activity_case_t {
    std::string name;
    std::vector<std::uint8_t> packet;
    bool local_cursor;
    bool mouse_enabled;
    bool keyboard_enabled;
    bool expected;
  };

  class InputActivityTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      original_mouse = config::input.mouse;
      original_keyboard = config::input.keyboard;
      cursor_channel::set_session_enabled(session_id, false);
    }

    void
    TearDown() override {
      cursor_channel::set_session_enabled(session_id, false);
      config::input.mouse = original_mouse;
      config::input.keyboard = original_keyboard;
    }

    bool original_mouse {};
    bool original_keyboard {};
  };
}  // namespace

TEST_F(InputActivityTest, ClassifiesPointerMouseAndKeyboardPackets) {
  auto relative_zero = make_packet<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  auto relative_move = make_packet<NV_REL_MOUSE_MOVE_PACKET>(MOUSE_MOVE_REL_MAGIC_GEN5);
  relative_move.deltaX = util::endian::big<std::int16_t>(0x0102);
  relative_move.deltaY = util::endian::big<std::int16_t>(-3);

  const auto absolute_move = make_packet<NV_ABS_MOUSE_MOVE_PACKET>(MOUSE_MOVE_ABS_MAGIC);
  const auto button_down = make_packet<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
  const auto button_up = make_packet<NV_MOUSE_BUTTON_PACKET>(MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5);

  auto vertical_zero = make_packet<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  auto vertical_scroll = make_packet<NV_SCROLL_PACKET>(SCROLL_MAGIC_GEN5);
  vertical_scroll.scrollAmt1 = util::endian::big<std::int16_t>(0x0102);

  auto horizontal_zero = make_packet<SS_HSCROLL_PACKET>(SS_HSCROLL_MAGIC);
  auto horizontal_scroll = make_packet<SS_HSCROLL_PACKET>(SS_HSCROLL_MAGIC);
  horizontal_scroll.scrollAmount = util::endian::big<std::int16_t>(-0x0102);

  const auto key_down = make_packet<NV_KEYBOARD_PACKET>(KEY_DOWN_EVENT_MAGIC);
  const auto key_up = make_packet<NV_KEYBOARD_PACKET>(KEY_UP_EVENT_MAGIC);
  const auto text = make_packet<NV_UNICODE_PACKET>(UTF8_TEXT_EVENT_MAGIC);

  const std::vector<activity_case_t> cases {
    {"relative zero in local mode", packet_bytes(relative_zero), true, true, true, false},
    {"relative move outside local mode", packet_bytes(relative_move), false, true, true, false},
    {"relative move in local mode", packet_bytes(relative_move), true, true, true, true},
    {"absolute move outside local mode", packet_bytes(absolute_move), false, true, true, false},
    {"absolute move in local mode", packet_bytes(absolute_move), true, true, true, true},
    {"button down", packet_bytes(button_down), false, true, true, true},
    {"button up", packet_bytes(button_up), false, true, true, true},
    {"vertical scroll zero", packet_bytes(vertical_zero), false, true, true, false},
    {"vertical scroll non-zero", packet_bytes(vertical_scroll), false, true, true, true},
    {"horizontal scroll zero", packet_bytes(horizontal_zero), false, true, true, false},
    {"horizontal scroll non-zero", packet_bytes(horizontal_scroll), false, true, true, true},
    {"mouse disabled", packet_bytes(button_down), false, false, true, false},
    {"key down", packet_bytes(key_down), false, true, true, true},
    {"key up", packet_bytes(key_up), false, true, true, true},
    {"text", packet_bytes(text), false, true, true, true},
    {"keyboard disabled", packet_bytes(key_down), false, true, false, false},
  };

  input::activity::tracker_t tracker;
  for (const auto &test : cases) {
    SCOPED_TRACE(test.name);
    cursor_channel::set_session_enabled(session_id, test.local_cursor);
    config::input.mouse = test.mouse_enabled;
    config::input.keyboard = test.keyboard_enabled;
    const auto *payload = reinterpret_cast<const NV_INPUT_HEADER *>(test.packet.data());
    EXPECT_EQ(tracker.evaluate(payload, test.packet.size()), test.expected);
  }
}

TEST_F(InputActivityTest, RejectsTruncatedPacketsBeforeReadingEventFields) {
  const std::vector<std::pair<std::uint32_t, std::size_t>> cases {
    {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
  };

  input::activity::tracker_t tracker;
  config::input.mouse = true;
  cursor_channel::set_session_enabled(session_id, true);

  for (const auto &[magic, full_size] : cases) {
    SCOPED_TRACE(magic);
    std::vector<std::uint8_t> bytes(full_size - 1);
    auto *payload = reinterpret_cast<PNV_INPUT_HEADER>(bytes.data());
    payload->size = util::endian::big<std::uint32_t>(static_cast<std::uint32_t>(full_size - sizeof(payload->size)));
    payload->magic = util::endian::little(magic);
    EXPECT_FALSE(tracker.evaluate(payload, bytes.size()));
  }

  NV_INPUT_HEADER header {};
  auto bytes = packet_bytes(header);
  bytes.resize(sizeof(header) - 1);
  EXPECT_FALSE(tracker.evaluate(reinterpret_cast<PNV_INPUT_HEADER>(bytes.data()), bytes.size()));
}
