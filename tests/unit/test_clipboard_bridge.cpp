/**
 * @file tests/unit/test_clipboard_bridge.cpp
 * @brief The clipboard bridge's decision to offer the capability, and the per-session counters behind the stream statistics.
 */
#include "src/clipboard_bridge.h"

#include <gtest/gtest.h>
#include <string>

namespace {
  using clipboard_bridge::bridge_t;
  using clipboard_bridge::offer_e;

  // the bridge is a process-wide singleton: every test leaves it as it found it (no sink)
  struct bridge_reset_t {
    ~bridge_reset_t() {
      bridge_t::instance().set_inbound_sink({});
    }
  };
}  // namespace

TEST(ClipboardBridgeOffer, WithNoSubscriberTheAgentIsMissingWhateverItsLastSignOfLife) {
  bridge_reset_t reset;
  auto &bridge = bridge_t::instance();
  bridge.set_inbound_sink({});
  std::int64_t age = 0;
  EXPECT_EQ(bridge.agent_state(&age), offer_e::no_agent);
  bridge.notify_gui_alive();  // a heartbeat without a subscriber does not make the agent usable
  EXPECT_EQ(bridge.agent_state(), offer_e::no_agent);
  EXPECT_FALSE(bridge.gui_alive());
}

TEST(ClipboardBridgeOffer, ASubscriberWithAFreshHeartbeatIsOffered) {
  bridge_reset_t reset;
  auto &bridge = bridge_t::instance();
  bridge.set_inbound_sink([](clipboard_bridge::session_id, const clipboard_bridge::payload_t &) {});
  bridge.notify_gui_alive();
  std::int64_t age = -2;
  EXPECT_EQ(bridge.agent_state(&age), offer_e::offered);
  EXPECT_GE(age, 0);
  EXPECT_LT(age, 5000);
  EXPECT_TRUE(bridge.gui_alive());
}

TEST(ClipboardBridgeOffer, TheWordsThatLeaveTheComputerAreFixed) {
  EXPECT_STREQ(clipboard_bridge::offer_name(offer_e::offered), "offered");
  EXPECT_STREQ(clipboard_bridge::offer_name(offer_e::off_config), "off_config");
  EXPECT_STREQ(clipboard_bridge::offer_name(offer_e::no_agent), "no_agent");
  EXPECT_STREQ(clipboard_bridge::offer_name(offer_e::agent_stale), "agent_stale");
  EXPECT_STREQ(clipboard_bridge::offer_name(offer_e::unknown), "unknown");
}

TEST(ClipboardBridgeNotes, ASessionCountsWhatWasForwardedDroppedAndSentAndForgetsItWhenTaken) {
  auto &bridge = bridge_t::instance();
  constexpr clipboard_bridge::session_id sid = 424242;
  bridge.note_offer(sid, offer_e::no_agent);
  bridge.note_inbound(sid, false);
  bridge.note_inbound(sid, false);
  bridge.note_inbound(sid, true);
  bridge.note_outbound(sid);
  const auto note = bridge.take_note(sid);
  EXPECT_EQ(note.offer, offer_e::no_agent);
  EXPECT_EQ(note.inbound_dropped, 2u);
  EXPECT_EQ(note.inbound_forwarded, 1u);
  EXPECT_EQ(note.outbound_sent, 1u);
  const auto again = bridge.take_note(sid);
  EXPECT_EQ(again.offer, offer_e::unknown);
  EXPECT_EQ(again.inbound_dropped, 0u);
}

TEST(ClipboardBridgeNotes, ACountForASessionThatWasNeverNotedIsIgnored) {
  auto &bridge = bridge_t::instance();
  bridge.note_inbound(999001, true);
  bridge.note_outbound(999001);
  EXPECT_EQ(bridge.take_note(999001).inbound_forwarded, 0u);
}

TEST(ClipboardBridgeNotes, TheTableOfNotesIsBounded) {
  auto &bridge = bridge_t::instance();
  for (clipboard_bridge::session_id sid = 1; sid <= 500; ++sid) bridge.note_offer(1'000'000 + sid, offer_e::offered);
  std::size_t kept = 0;
  for (clipboard_bridge::session_id sid = 1; sid <= 500; ++sid)
    if (bridge.take_note(1'000'000 + sid).offer == offer_e::offered) ++kept;
  EXPECT_LE(kept, 66u);
  EXPECT_GE(kept, 1u);
}

TEST(ClipboardBridgeNotes, ATableThatIsFullGivesUpARecordOfALaunchAndKeepsTheOnesOfLiveStreams) {
  auto &bridge = bridge_t::instance();
  constexpr clipboard_bridge::session_id live = 2'000'000;
  bridge.note_offer(live, offer_e::offered);  // the oldest record, and its stream is running
  bridge.session_started(live);
  for (clipboard_bridge::session_id sid = 1; sid <= 200; ++sid) bridge.note_offer(2'000'000 + sid, offer_e::off_config);
  bridge.note_inbound(live, true);
  bridge.note_outbound(live);
  const auto note = bridge.take_note(live);
  EXPECT_EQ(note.offer, offer_e::offered);  // the counts of the stream that is running were not thrown out
  EXPECT_EQ(note.inbound_forwarded, 1u);
  EXPECT_EQ(note.outbound_sent, 1u);
  bridge.session_stopped(live);
  for (clipboard_bridge::session_id sid = 1; sid <= 200; ++sid) bridge.take_note(2'000'000 + sid);
}

TEST(ClipboardBridgeInbound, ReportsWhetherASinkTookThePayload) {
  bridge_reset_t reset;
  auto &bridge = bridge_t::instance();
  bridge.set_inbound_sink({});
  EXPECT_FALSE(bridge.on_inbound(7, { 1, 2, 3 }));
  int calls = 0;
  bridge.set_inbound_sink([&calls](clipboard_bridge::session_id, const clipboard_bridge::payload_t &) { ++calls; });
  EXPECT_TRUE(bridge.on_inbound(7, { 1, 2, 3 }));
  EXPECT_EQ(calls, 1);
}
