/**
 * @file tests/unit/test_text_context_bridge.cpp
 * @brief Correlation and wire-format tests for remote text context.
 */

#include <cstdint>
#include <deque>

#include "../tests_common.h"
#include "src/text_context/bridge.h"

namespace {
  std::uint16_t read_u16(const text_context::payload_t &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
  }

  std::uint32_t read_u32(const text_context::payload_t &bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
  }


  std::uint64_t read_u64(const text_context::payload_t &bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
  }
}

TEST(TextContextBridge, UiaRequiresEditableHitAndEncodesCaptureCoordinates) {
  constexpr text_context::session_id sid = 0x54455854554941ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  bridge.record_mouse_button(sid, false, 250, 180, 100, 50, 1920, 1080);

  text_context::observation_t miss;
  miss.source = text_context::source_e::uia;
  miss.active = true;
  miss.editable = true;
  miss.element_rect = text_context::screen_rect_t {300, 300, 500, 400};
  EXPECT_FALSE(bridge.observe(miss));

  auto hit = miss;
  hit.password = true;
  hit.element_rect = text_context::screen_rect_t {200, 150, 500, 220};
  hit.caret_rect = text_context::screen_rect_t {240, 170, 241, 190};
  ASSERT_TRUE(bridge.observe(hit));

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  const auto &message = messages.front();
  ASSERT_EQ(message.target, sid);
  ASSERT_EQ(message.bytes.size(), text_context::kWireSize);
  EXPECT_EQ(message.bytes[0], text_context::kWireVersion);
  EXPECT_EQ(message.bytes[1], text_context::kWireSize);
  EXPECT_EQ(message.bytes[24], static_cast<std::uint8_t>(text_context::source_e::uia));
  EXPECT_EQ(message.bytes[25], static_cast<std::uint8_t>(text_context::cause_e::remote_mouse));
  EXPECT_EQ(read_u32(message.bytes, 28), 150u);
  EXPECT_EQ(read_u32(message.bytes, 32), 130u);
  EXPECT_EQ(read_u32(message.bytes, 36), 100u);
  EXPECT_EQ(read_u32(message.bytes, 40), 100u);
  EXPECT_NE(read_u16(message.bytes, 2) & 0x0040, 0);
  EXPECT_EQ(read_u32(message.bytes, 52), 140u);
  EXPECT_EQ(read_u32(message.bytes, 56), 120u);
  EXPECT_EQ(read_u32(message.bytes, 68), 1920u);
  EXPECT_EQ(read_u32(message.bytes, 72), 1080u);

  bridge.record_mouse_button(sid, true, 250, 180, 100, 50, 1920, 1080);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, InputPaneRequiresRemoteTouchAutoShowTransition) {
  constexpr text_context::session_id sid = 0x5445585450414e45ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  bridge.record_touch(sid, 1, 7, 300, 400, 0, 0, 1280, 720);

  text_context::observation_t pane;
  pane.source = text_context::source_e::input_pane;
  pane.active = true;
  pane.editable = true;
  pane.pane_visible = true;
  EXPECT_FALSE(bridge.observe(pane));

  pane.auto_show = true;
  EXPECT_TRUE(bridge.observe(pane));
  bridge.record_touch(sid, 2, 7, 300, 400, 0, 0, 1280, 720);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, UiaRoutesToTheSessionWhoseInputHitTheElement) {
  constexpr text_context::session_id sid_a = 0x544558544d554c41ULL;
  constexpr text_context::session_id sid_b = 0x544558544d554c42ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid_a);
  bridge.session_started(sid_b);
  bridge.record_touch(sid_a, 1, 1, 110, 120, 0, 0, 1920, 1080);
  bridge.record_touch(sid_a, 2, 1, 110, 120, 0, 0, 1920, 1080);
  bridge.record_touch(sid_b, 1, 2, 710, 720, 0, 0, 1280, 720);
  bridge.record_touch(sid_b, 2, 2, 710, 720, 0, 0, 1280, 720);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {100, 100, 300, 200};
  ASSERT_TRUE(bridge.observe(observation));
  EXPECT_FALSE(bridge.observe(observation));

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages.front().target, sid_a);
  EXPECT_EQ(messages.front().bytes[25], static_cast<std::uint8_t>(text_context::cause_e::remote_touch));

  bridge.session_stopped(sid_a);
  bridge.session_stopped(sid_b);
}

TEST(TextContextBridge, UiaRejectsOverlappingCandidatesFromDifferentSessions) {
  constexpr text_context::session_id sid_a = 0x54455854414d4241ULL;
  constexpr text_context::session_id sid_b = 0x54455854414d4242ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid_a);
  bridge.session_started(sid_b);
  bridge.record_touch(sid_a, 1, 1, 200, 150, 0, 0, 1920, 1080);
  bridge.record_touch(sid_b, 1, 2, 200, 150, 0, 0, 1920, 1080);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.password = true;
  observation.element_rect = text_context::screen_rect_t {100, 100, 300, 200};
  EXPECT_FALSE(bridge.observe(observation));

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  EXPECT_TRUE(messages.empty());

  bridge.record_touch(sid_a, 2, 1, 200, 150, 0, 0, 1920, 1080);
  bridge.record_touch(sid_b, 2, 2, 200, 150, 0, 0, 1920, 1080);
  bridge.session_stopped(sid_a);
  bridge.session_stopped(sid_b);
}

TEST(TextContextBridge, SequenceCountersAreIndependentPerSession) {
  constexpr text_context::session_id sid_a = 0x5445585453455141ULL;
  constexpr text_context::session_id sid_b = 0x5445585453455142ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid_a);
  bridge.session_started(sid_b);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {50, 50, 150, 150};

  bridge.record_mouse_button(sid_a, false, 100, 100, 0, 0, 1920, 1080);
  ASSERT_TRUE(bridge.observe(observation));
  bridge.record_mouse_button(sid_a, true, 100, 100, 0, 0, 1920, 1080);

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(read_u32(messages.front().bytes, 4), 1u);
  EXPECT_EQ(read_u64(messages.front().bytes, 8), 1u);
  EXPECT_EQ(read_u64(messages.front().bytes, 16), 1u);
  bridge.session_stopped(sid_a);

  bridge.record_mouse_button(sid_b, false, 100, 100, 0, 0, 1920, 1080);
  ASSERT_TRUE(bridge.observe(observation));
  bridge.record_mouse_button(sid_b, true, 100, 100, 0, 0, 1920, 1080);

  messages.clear();
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(read_u32(messages.front().bytes, 4), 1u);
  EXPECT_EQ(read_u64(messages.front().bytes, 8), 1u);
  EXPECT_EQ(read_u64(messages.front().bytes, 16), 1u);

  bridge.session_stopped(sid_b);
}

TEST(TextContextBridge, FocusTransferDeactivatesThePreviousSession) {
  constexpr text_context::session_id sid_a = 0x5445585453574954ULL;
  constexpr text_context::session_id sid_b = 0x5445585453574955ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid_a);
  bridge.session_started(sid_b);

  text_context::observation_t first;
  first.source = text_context::source_e::uia;
  first.active = true;
  first.editable = true;
  first.element_rect = text_context::screen_rect_t {50, 50, 150, 150};
  bridge.record_mouse_button(sid_a, false, 100, 100, 0, 0, 1920, 1080);
  ASSERT_TRUE(bridge.observe(first));
  bridge.record_mouse_button(sid_a, true, 100, 100, 0, 0, 1920, 1080);

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  const auto first_activation = read_u64(messages.front().bytes, 8);
  messages.clear();

  auto second = first;
  second.element_rect = text_context::screen_rect_t {450, 450, 550, 550};
  bridge.record_mouse_button(sid_b, false, 500, 500, 0, 0, 1920, 1080);
  ASSERT_TRUE(bridge.observe(second));
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(messages[0].target, sid_a);
  EXPECT_EQ(read_u64(messages[0].bytes, 8), first_activation);
  EXPECT_EQ(read_u16(messages[0].bytes, 2) & 0x0003, 0);
  EXPECT_EQ(messages[1].target, sid_b);
  EXPECT_NE(read_u16(messages[1].bytes, 2) & 0x0003, 0);

  bridge.record_mouse_button(sid_b, true, 500, 500, 0, 0, 1920, 1080);
  bridge.session_stopped(sid_a);
  bridge.session_stopped(sid_b);
}

TEST(TextContextBridge, DragAndCancelledTouchDoNotActivateTextInput) {
  constexpr text_context::session_id sid = 0x5445585444524147ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);

  bridge.record_touch(sid, 1, 10, 100, 100, 0, 0, 1920, 1080);
  bridge.record_touch(sid, 3, 10, 160, 100, 0, 0, 1920, 1080);
  bridge.record_touch(sid, 2, 10, 160, 100, 0, 0, 1920, 1080);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {50, 50, 250, 150};
  EXPECT_FALSE(bridge.observe(observation));

  bridge.record_touch(sid, 1, 11, 100, 100, 0, 0, 1920, 1080);
  bridge.record_touch(sid, 4, 11, 100, 100, 0, 0, 1920, 1080);
  EXPECT_FALSE(bridge.observe(observation));

  bridge.record_touch(sid, 1, 12, 100, 100, 0, 0, 1920, 1080);
  bridge.record_touch(sid, 7, 0, 0, 0, 0, 0, 1920, 1080);
  EXPECT_FALSE(bridge.observe(observation));
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, RepeatedTouchDownKeepsTheOriginalCandidate) {
  constexpr text_context::session_id sid = 0x5445585444555044ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  bridge.record_touch(sid, 1, 7, 100, 100, 0, 0, 1920, 1080);
  bridge.record_touch(sid, 1, 7, 500, 500, 0, 0, 1920, 1080);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {50, 50, 150, 150};
  EXPECT_TRUE(bridge.observe(observation));

  bridge.record_touch(sid, 7, 0, 0, 0, 0, 0, 1920, 1080);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, ActiveTouchCountIsBoundedPerSession) {
  constexpr text_context::session_id sid = 0x544558544c494d54ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  for (std::uint32_t pointer_id = 0; pointer_id < 16; ++pointer_id) {
    bridge.record_touch(sid, 1, pointer_id, 100, 100, 0, 0, 1920, 1080);
  }
  bridge.record_touch(sid, 1, 99, 500, 500, 0, 0, 1920, 1080);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {450, 450, 550, 550};
  EXPECT_FALSE(bridge.observe(observation));

  bridge.record_touch(sid, 7, 0, 0, 0, 0, 0, 1920, 1080);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, MouseDragDoesNotActivateTextInput) {
  constexpr text_context::session_id sid = 0x544558544d445241ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  bridge.record_mouse_button(sid, false, 100, 100, 0, 0, 1920, 1080);
  bridge.record_mouse_button(sid, true, 180, 100, 0, 0, 1920, 1080);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {50, 50, 250, 150};
  EXPECT_FALSE(bridge.observe(observation));
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, CachedUiaFocusMatchesClickIntoAlreadyFocusedEditor) {
  constexpr text_context::session_id sid = 0x5445585443414348ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  bridge.notify_gui_alive(false, true);

  text_context::observation_t observation;
  observation.source = text_context::source_e::uia;
  observation.active = true;
  observation.editable = true;
  observation.element_rect = text_context::screen_rect_t {100, 100, 500, 300};
  EXPECT_FALSE(bridge.observe(observation));

  bridge.record_mouse_button(sid, false, 250, 180, 0, 0, 1920, 1080);
  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages.front().target, sid);
  EXPECT_EQ(messages.front().bytes[25], static_cast<std::uint8_t>(text_context::cause_e::remote_mouse));

  bridge.record_mouse_button(sid, true, 250, 180, 0, 0, 1920, 1080);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, NonEditableTransitionInvalidatesCachedUiaFocus) {
  constexpr text_context::session_id sid = 0x544558544e454741ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);
  bridge.notify_gui_alive(false, true);

  text_context::observation_t editable;
  editable.source = text_context::source_e::uia;
  editable.active = true;
  editable.editable = true;
  editable.element_rect = text_context::screen_rect_t {100, 100, 500, 300};
  EXPECT_FALSE(bridge.observe(editable));

  auto non_editable = editable;
  non_editable.editable = false;
  EXPECT_FALSE(bridge.observe(non_editable));
  bridge.record_touch(sid, 1, 42, 250, 180, 0, 0, 1920, 1080);

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  EXPECT_TRUE(messages.empty());
  bridge.record_touch(sid, 2, 42, 250, 180, 0, 0, 1920, 1080);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, RemoteClickOnNonEditableElementDeactivatesTheActiveContext) {
  constexpr text_context::session_id sid = 0x5445585444454143ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);

  text_context::observation_t editable;
  editable.source = text_context::source_e::uia;
  editable.active = true;
  editable.editable = true;
  editable.element_rect = text_context::screen_rect_t {100, 100, 500, 300};
  bridge.record_touch(sid, 1, 1, 200, 180, 0, 0, 1920, 1080);
  ASSERT_TRUE(bridge.observe(editable));
  bridge.record_touch(sid, 2, 1, 200, 180, 0, 0, 1920, 1080);

  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  const auto activation_id = read_u64(messages.front().bytes, 8);
  messages.clear();

  bridge.record_touch(sid, 1, 2, 800, 500, 0, 0, 1920, 1080);
  auto non_editable = editable;
  non_editable.editable = false;
  non_editable.element_rect = text_context::screen_rect_t {700, 450, 900, 550};
  ASSERT_TRUE(bridge.observe(non_editable));
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(read_u64(messages.front().bytes, 8), activation_id);
  const auto flags = read_u16(messages.front().bytes, 2);
  EXPECT_NE(flags & 0x0080, 0);
  EXPECT_EQ(flags & 0x0002, 0);

  bridge.record_touch(sid, 2, 2, 800, 500, 0, 0, 1920, 1080);
  bridge.session_stopped(sid);
}

TEST(TextContextBridge, TransientDeactivationDoesNotConsumeTheNextEditorActivation) {
  constexpr text_context::session_id sid = 0x5445585452414e53ULL;
  auto &bridge = text_context::bridge_t::instance();
  bridge.session_started(sid);

  text_context::observation_t first;
  first.source = text_context::source_e::uia;
  first.active = true;
  first.editable = true;
  first.element_rect = text_context::screen_rect_t {100, 100, 300, 200};
  bridge.record_mouse_button(sid, false, 150, 150, 0, 0, 1920, 1080);
  ASSERT_TRUE(bridge.observe(first));
  bridge.record_mouse_button(sid, true, 150, 150, 0, 0, 1920, 1080);
  std::deque<text_context::outbound_msg_t> messages;
  bridge.drain_outbound(messages);
  messages.clear();

  bridge.record_mouse_button(sid, false, 450, 150, 0, 0, 1920, 1080);
  auto transient = first;
  transient.active = false;
  transient.editable = false;
  transient.element_rect.reset();
  ASSERT_TRUE(bridge.observe(transient));

  auto second = first;
  second.element_rect = text_context::screen_rect_t {400, 100, 600, 200};
  ASSERT_TRUE(bridge.observe(second));
  bridge.drain_outbound(messages);
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(read_u16(messages[0].bytes, 2) & 0x0002, 0);
  EXPECT_NE(read_u16(messages[1].bytes, 2) & 0x0002, 0);

  bridge.record_mouse_button(sid, true, 450, 150, 0, 0, 1920, 1080);
  bridge.session_stopped(sid);
}
