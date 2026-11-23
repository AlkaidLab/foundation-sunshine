/**
 * @file tests/unit/platform/windows/test_unicode_input.cpp
 * @brief Order of the key events that type text through KEYEVENTF_UNICODE.
 *
 * The regression: all key-downs first and the key-ups afterwards kept one VK_PACKET key
 * held for the whole run, so apps that track key state by virtual key typed only the
 * first character ("hello" became "hhhhh").
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "src/platform/windows/unicode_input.h"

namespace {
  using platf::win_unicode::interleaved_key_events;
  using platf::win_unicode::key_event_t;

  std::vector<key_event_t>
  events_for(const std::u16string &text) {
    return interleaved_key_events(std::span<const char16_t>(text));
  }

  /// The highest number of units held down at once.
  int
  max_held(const std::vector<key_event_t> &events) {
    int held = 0;
    int peak = 0;
    for (const auto &event : events) {
      held += event.up ? -1 : 1;
      peak = std::max(peak, held);
    }
    return peak;
  }
}  // namespace

TEST(UnicodeInput, EveryUnitIsReleasedBeforeTheNextOneIsPressed) {
  auto events = events_for(u"hello");
  ASSERT_EQ(events.size(), 10u);
  for (std::size_t i = 0; i < events.size(); i += 2) {
    EXPECT_FALSE(events[i].up) << "event " << i;
    EXPECT_TRUE(events[i + 1].up) << "event " << i + 1;
    EXPECT_EQ(events[i].unit, events[i + 1].unit) << "pair at " << i;
  }
  EXPECT_EQ(max_held(events), 1);
}

TEST(UnicodeInput, TextOrderIsKept) {
  auto events = events_for(u"abc");
  std::vector<key_event_t> expected = {
    { 'a', false }, { 'a', true }, { 'b', false }, { 'b', true }, { 'c', false }, { 'c', true }
  };
  EXPECT_EQ(events, expected);
}

TEST(UnicodeInput, RepeatedCharactersGetTheirOwnDownAndUp) {
  auto events = events_for(u"ll");
  std::vector<key_event_t> expected = { { 'l', false }, { 'l', true }, { 'l', false }, { 'l', true } };
  EXPECT_EQ(events, expected);
}

TEST(UnicodeInput, SurrogatePairStaysTogetherInOrder) {
  // U+1F600: D83D DE00, followed by an ASCII character.
  auto events = events_for(u"\U0001F600x");
  std::vector<key_event_t> expected = {
    { 0xD83D, false }, { 0xD83D, true }, { 0xDE00, false }, { 0xDE00, true }, { 'x', false }, { 'x', true }
  };
  EXPECT_EQ(events, expected);
  EXPECT_EQ(max_held(events), 1);
}

TEST(UnicodeInput, NoUnitsMeansNoEvents) {
  EXPECT_TRUE(interleaved_key_events(std::span<const char16_t>()).empty());
  EXPECT_TRUE(events_for(u"").empty());
}

TEST(UnicodeInput, CjkCommitTextHasOneDownUpPerCharacter) {
  auto events = events_for(u"你好");
  ASSERT_EQ(events.size(), 4u);
  EXPECT_EQ(max_held(events), 1);
  EXPECT_EQ(events[0].unit, 0x4F60);
  EXPECT_EQ(events[2].unit, 0x597D);
}

namespace {
  /// The order the host used before the fix: every key-down, then every key-up.
  std::vector<key_event_t>
  legacy_order(const std::u16string &text) {
    std::vector<key_event_t> events;
    for (auto unit : text) {
      events.push_back({ static_cast<std::uint16_t>(unit), false });
    }
    for (auto unit : text) {
      events.push_back({ static_cast<std::uint16_t>(unit), true });
    }
    return events;
  }

  /**
   * A receiver that follows key state by virtual key, the way WeChat does. Every event of a
   * text run has the same virtual key (VK_PACKET), so there is exactly one key state: a
   * key-down while that key is still held is an auto-repeat of the held key and produces the
   * character the key had when it went down; only a key-down of a released key produces the
   * character of its own event.
   */
  std::u16string
  typed_by_key_state_receiver(const std::vector<key_event_t> &events) {
    std::u16string typed;
    bool held = false;
    std::uint16_t held_unit = 0;
    for (const auto &event : events) {
      if (event.up) {
        held = false;
        continue;
      }
      if (held) {
        typed.push_back(static_cast<char16_t>(held_unit));
      }
      else {
        held = true;
        held_unit = event.unit;
        typed.push_back(static_cast<char16_t>(event.unit));
      }
    }
    return typed;
  }
}  // namespace

TEST(UnicodeInput, KeyStateReceiverReproducesTheReportedRepeatWithTheOldOrder) {
  // Owner report: sending 你好啊我是谁 arrived in WeChat as 你你你你你你.
  EXPECT_EQ(typed_by_key_state_receiver(legacy_order(u"你好啊我是谁")), u"你你你你你你");
  EXPECT_EQ(typed_by_key_state_receiver(legacy_order(u"hello")), u"hhhhh");
}

TEST(UnicodeInput, KeyStateReceiverTypesEveryCharacterWithTheNewOrder) {
  EXPECT_EQ(typed_by_key_state_receiver(events_for(u"你好啊我是谁")), u"你好啊我是谁");
  EXPECT_EQ(typed_by_key_state_receiver(events_for(u"hello")), u"hello");
  EXPECT_EQ(typed_by_key_state_receiver(events_for(u"ll")), u"ll");
}

TEST(UnicodeInput, KeyStateReceiverKeepsSurrogatePairsIntact) {
  // U+1F600 (D83D DE00) and U+20BB7 (D842 DFB7) between BMP characters.
  const std::u16string text = u"好\U0001F600是\U00020BB7a";
  EXPECT_EQ(typed_by_key_state_receiver(events_for(text)), text);
  // With the old order the whole run collapses to the first unit.
  EXPECT_EQ(typed_by_key_state_receiver(legacy_order(text)), std::u16string(text.size(), text.front()));
}

TEST(UnicodeInput, ReceiverSeesNoOverlapForMixedCjkAndSurrogateText) {
  const std::u16string text = u"你好 \U0001F600 world";
  auto events = events_for(text);
  EXPECT_EQ(events.size(), text.size() * 2);
  EXPECT_EQ(max_held(events), 1);
  EXPECT_GT(max_held(legacy_order(text)), 1);
}

TEST(UnicodeInput, GeneratorAcceptsPlainUint16Units) {
  const std::vector<std::uint16_t> raw = { 0x4F60, 0xD83D, 0xDE00 };
  EXPECT_EQ(interleaved_key_events(std::span<const std::uint16_t>(raw)), events_for(u"\u4F60\U0001F600"));
}

#ifdef _WIN32
// WCHAR is wchar_t, 16 bits wide on Windows: the production path hands it over as it is.
TEST(UnicodeInput, GeneratorAcceptsNativeWideCharacters) {
  const std::vector<wchar_t> wide = { L'\x4F60', L'\xD83D', L'\xDE00' };
  EXPECT_EQ(interleaved_key_events(std::span<const wchar_t>(wide.data(), wide.size())), events_for(u"\u4F60\U0001F600"));
}
#endif

namespace {
  using platf::win_unicode::pending_key_ups;
}

TEST(UnicodeInput, NothingIsOwedWhenTheWholeRunWasInjected) {
  auto events = events_for(u"hello");
  EXPECT_TRUE(pending_key_ups(events, events.size()).empty());
  EXPECT_TRUE(pending_key_ups(events, events.size() + 7).empty());
}

TEST(UnicodeInput, NothingIsOwedWhenNothingOrWholeCharactersWereInjected) {
  auto events = events_for(u"abc");
  EXPECT_TRUE(pending_key_ups(events, 0).empty());
  EXPECT_TRUE(pending_key_ups(events, 2).empty());  // a down and its up
  EXPECT_TRUE(pending_key_ups(events, 4).empty());
}

TEST(UnicodeInput, KeyDownInjectedWithoutItsUpIsReleased) {
  auto events = events_for(u"abc");
  // Partial injection ended right after the down of 'b'.
  std::vector<key_event_t> expected = { { 'b', true } };
  EXPECT_EQ(pending_key_ups(events, 3), expected);
  // ... and after the down of the first unit of a surrogate pair.
  auto pair = events_for(u"\U0001F600");
  std::vector<key_event_t> expected_pair = { { 0xD83D, true } };
  EXPECT_EQ(pending_key_ups(pair, 1), expected_pair);
}

TEST(UnicodeInput, RetryOfTheRemainderPlusReleaseNeverLeavesAKeyDown) {
  const std::u16string text = u"你好\U0001F600a";
  auto events = events_for(text);
  for (std::size_t cut = 0; cut <= events.size(); cut++) {
    // Prefix that went out, then the owed key-ups: the final state has no key held.
    std::vector<key_event_t> sent(events.begin(), events.begin() + cut);
    for (const auto &up : pending_key_ups(events, cut)) {
      sent.push_back(up);
    }
    int held = 0;
    for (const auto &event : sent) {
      held += event.up ? -1 : 1;
    }
    EXPECT_EQ(held, 0) << "cut at " << cut;
    EXPECT_LE(max_held(sent), 1) << "cut at " << cut;
  }
}
