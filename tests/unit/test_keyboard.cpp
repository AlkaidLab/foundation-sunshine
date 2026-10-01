/**
 * @file tests/unit/test_keyboard.cpp
 * @brief Regression tests for client keyboard state and key mappings.
 */

#include <gtest/gtest.h>
#include <moonlight-common-c/src/Input.h>

#include <src/config.h>
#include <src/input.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {
  using input::testing::keyboard_event_t;

  constexpr std::uint16_t VKEY_LSHIFT = 0xA0;
  constexpr std::uint16_t VKEY_RSHIFT = 0xA1;
  constexpr std::uint16_t VKEY_LCONTROL = 0xA2;
  constexpr std::uint16_t VKEY_RCONTROL = 0xA3;
  constexpr std::uint16_t VKEY_LMENU = 0xA4;
  constexpr std::uint16_t VKEY_RMENU = 0xA5;
  constexpr std::uint16_t VKEY_SHIFT = 0x10;
  constexpr std::uint16_t VKEY_CONTROL = 0x11;
  constexpr std::uint16_t VKEY_MENU = 0x12;
  constexpr std::uint16_t VKEY_LWIN = 0x5B;
  constexpr std::uint16_t VKEY_RWIN = 0x5C;
  constexpr std::uint16_t VKEY_SPACE = 0x20;
  constexpr std::uint16_t VKEY_A = 0x41;
  constexpr std::uint16_t VKEY_B = 0x42;
  constexpr std::uint16_t VKEY_N = 0x4E;
  constexpr std::uint16_t VKEY_Z = 0x5A;

  std::string
  describe(const keyboard_event_t &event) {
    auto token = (event.release ? "-" : "+") + std::to_string(event.key_code);
    if (event.flags != 0) {
      token += ":" + std::to_string(event.flags);
    }
    return token;
  }

  class KeyboardStateTest: public ::testing::Test {
  protected:
    void
    SetUp() override {
      original_input = config::input;
      config::input.keyboard = true;
      config::input.keybindings.clear();
      config::input.key_repeat_delay = std::chrono::milliseconds { 0 };
      input::testing::set_keyboard_sink([this](const keyboard_event_t &event) {
        events.push_back(describe(event));
      });
    }

    void
    TearDown() override {
      input::testing::set_keyboard_sink({});
      input::testing::reset_keyboard_state();
      config::input = original_input;
    }

    void
    press(const std::shared_ptr<input::input_t> &input, std::uint16_t key,
          std::uint8_t modifiers = 0, std::uint8_t flags = 0) {
      auto retained = input;
      input::testing::send_keyboard_packet(retained, key, modifiers, flags, false);
    }

    void
    release(const std::shared_ptr<input::input_t> &input, std::uint16_t key,
            std::uint8_t modifiers = 0, std::uint8_t flags = 0) {
      auto retained = input;
      input::testing::send_keyboard_packet(retained, key, modifiers, flags, true);
    }

    std::vector<std::string>
    taken() {
      return std::exchange(events, {});
    }

    config::input_t original_input;
    std::vector<std::string> events;
  };
}

TEST_F(KeyboardStateTest, TracksClientModifierWhenItIsRemapped) {
  config::input.keybindings = { { VKEY_LMENU, VKEY_LWIN } };
  auto input = input::testing::make_input();

  press(input, VKEY_LMENU, MODIFIER_ALT);
  press(input, VKEY_SPACE, MODIFIER_ALT);
  release(input, VKEY_SPACE, MODIFIER_ALT);
  input::testing::release_held_keys();

  EXPECT_EQ(taken(), (std::vector<std::string> {
    "+91", "+32", "-32", "-91",
  }));
}

TEST_F(KeyboardStateTest, SupportsTheRightAltToWindowsMapping) {
  config::input.keybindings = { { VKEY_RMENU, VKEY_LWIN } };
  auto input = input::testing::make_input();

  press(input, VKEY_RMENU, MODIFIER_ALT);
  press(input, VKEY_SPACE, MODIFIER_ALT);
  release(input, VKEY_SPACE, MODIFIER_ALT);
  release(input, VKEY_RMENU);

  EXPECT_EQ(taken(), (std::vector<std::string> {
    "+91", "+32", "-32", "-91",
  }));
}

TEST_F(KeyboardStateTest, KeepsAltSetWhileLeftAltRemainsHeld) {
  config::input.keybindings = { { VKEY_RMENU, VKEY_LWIN } };
  auto input = input::testing::make_input();

  press(input, VKEY_LMENU, MODIFIER_ALT);
  press(input, VKEY_RMENU, MODIFIER_ALT);
  release(input, VKEY_RMENU, MODIFIER_ALT);
  press(input, VKEY_B, MODIFIER_ALT);
  release(input, VKEY_B, MODIFIER_ALT);
  release(input, VKEY_LMENU);

  EXPECT_EQ(taken(), (std::vector<std::string> {
    "+164", "+91", "-91", "+66", "-66", "-164",
  }));
}

TEST_F(KeyboardStateTest, KeepsAggregateModifierWhileEitherSideIsHeld) {
  config::input.keybindings = { { VKEY_RSHIFT, VKEY_LWIN } };
  auto input = input::testing::make_input();

  press(input, VKEY_LSHIFT, MODIFIER_SHIFT);
  press(input, VKEY_RSHIFT, MODIFIER_SHIFT);
  release(input, VKEY_RSHIFT, MODIFIER_SHIFT);
  press(input, VKEY_B, MODIFIER_SHIFT);
  release(input, VKEY_B, MODIFIER_SHIFT);
  release(input, VKEY_LSHIFT);
  press(input, VKEY_A, MODIFIER_SHIFT);
  release(input, VKEY_A, MODIFIER_SHIFT);

  EXPECT_EQ(taken(), (std::vector<std::string> {
    "+160", "+91", "-91", "+66", "-66", "-160",
    "+16", "+65", "-16", "-65",
  }));
}

TEST_F(KeyboardStateTest, KeepsControlAggregateWhileEitherSideIsHeld) {
  config::input.keybindings = { { VKEY_RCONTROL, VKEY_LWIN } };
  auto input = input::testing::make_input();

  press(input, VKEY_LCONTROL, MODIFIER_CTRL);
  press(input, VKEY_RCONTROL, MODIFIER_CTRL);
  release(input, VKEY_RCONTROL, MODIFIER_CTRL);
  press(input, VKEY_B, MODIFIER_CTRL);
  release(input, VKEY_B, MODIFIER_CTRL);
  release(input, VKEY_LCONTROL);
  press(input, VKEY_A, MODIFIER_CTRL);
  release(input, VKEY_A, MODIFIER_CTRL);

  EXPECT_EQ(taken(), (std::vector<std::string> {
    "+162", "+91", "-91", "+66", "-66", "-162",
    "+17", "+65", "-17", "-65",
  }));
}

TEST_F(KeyboardStateTest, ReleasesMappedHostKeyOnDisconnect) {
  config::input.keybindings = { { VKEY_LMENU, VKEY_LWIN } };
  auto input = input::testing::make_input();

  press(input, VKEY_LMENU, MODIFIER_ALT);
  input::testing::release_held_keys();

  EXPECT_EQ(taken(), (std::vector<std::string> { "+91", "-91" }));
}

TEST_F(KeyboardStateTest, IgnoresDuplicatePressesAndUnmatchedReleases) {
  auto input = input::testing::make_input();

  release(input, VKEY_Z);
  EXPECT_TRUE(taken().empty());

  press(input, VKEY_Z);
  press(input, VKEY_Z);
  EXPECT_EQ(taken(), (std::vector<std::string> { "+90" }));

  release(input, VKEY_Z);
  release(input, VKEY_Z);
  EXPECT_EQ(taken(), (std::vector<std::string> { "-90" }));
}

TEST_F(KeyboardStateTest, ForwardsModifierKeysWithoutSyntheticInjection) {
  auto input = input::testing::make_input();
  constexpr std::uint8_t claimed = MODIFIER_SHIFT | MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_META;
  constexpr std::array modifier_keys {
    VKEY_SHIFT, VKEY_CONTROL, VKEY_MENU,
    VKEY_LSHIFT, VKEY_RSHIFT, VKEY_LCONTROL, VKEY_RCONTROL,
    VKEY_LMENU, VKEY_RMENU,
  };

  for (const auto key : modifier_keys) {
    press(input, key, claimed);
    release(input, key, claimed);
  }

  std::vector<std::string> expected;
  expected.reserve(modifier_keys.size() * 2);
  for (const auto key : modifier_keys) {
    expected.push_back("+" + std::to_string(key));
    expected.push_back("-" + std::to_string(key));
  }
  EXPECT_EQ(taken(), expected);
}

TEST_F(KeyboardStateTest, InjectsClientModifiersWhenNoModifierKeysWereSent) {
  auto input = input::testing::make_input();
  constexpr std::array injectable {
    std::pair { static_cast<std::uint8_t>(MODIFIER_SHIFT), VKEY_SHIFT },
    std::pair { static_cast<std::uint8_t>(MODIFIER_CTRL), VKEY_CONTROL },
    std::pair { static_cast<std::uint8_t>(MODIFIER_ALT), VKEY_MENU },
  };

  for (unsigned combination = 1; combination < (1u << injectable.size()); ++combination) {
    std::uint8_t claimed = 0;
    std::vector<std::string> expected;
    for (std::size_t index = 0; index < injectable.size(); ++index) {
      if (combination & (1u << index)) {
        claimed |= injectable[index].first;
        expected.push_back("+" + std::to_string(injectable[index].second));
      }
    }
    expected.push_back("+66");
    for (std::size_t index = 0; index < injectable.size(); ++index) {
      if (combination & (1u << index)) {
        expected.push_back("-" + std::to_string(injectable[index].second));
      }
    }

    press(input, VKEY_B, claimed);
    EXPECT_EQ(taken(), expected);
    release(input, VKEY_B, claimed);
    EXPECT_EQ(taken(), (std::vector<std::string> { "-66" }));
  }
}

TEST_F(KeyboardStateTest, NeverSynthesizesMetaModifier) {
  auto input = input::testing::make_input();

  press(input, VKEY_A, MODIFIER_META);
  release(input, VKEY_A, MODIFIER_META);

  EXPECT_EQ(taken(), (std::vector<std::string> { "+65", "-65" }));
}

TEST_F(KeyboardStateTest, PropagatesPacketFlagsToSyntheticEvents) {
  auto input = input::testing::make_input();
  constexpr std::uint8_t flags = SS_KBE_FLAG_NON_NORMALIZED;

  press(input, VKEY_A, MODIFIER_SHIFT | MODIFIER_ALT, flags);
  const auto events = taken();
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events, (std::vector<std::string> {
    "+16:1", "+18:1", "+65:1", "-16:1", "-18:1",
  }));

  release(input, VKEY_A, MODIFIER_SHIFT | MODIFIER_ALT, flags);
  EXPECT_EQ(taken(), (std::vector<std::string> { "-65:1" }));
}

TEST_F(KeyboardStateTest, EmitsConfiguredKeybindingsAndLeavesUnmappedKeysUnchanged) {
  config::input.keybindings = {
    { VKEY_LWIN, VKEY_LMENU },
    { VKEY_RWIN, VKEY_RMENU },
    { VKEY_LMENU, VKEY_LWIN },
    { VKEY_RMENU, VKEY_RWIN },
  };
  auto input = input::testing::make_input();

  const std::array mappings {
    std::pair { VKEY_LWIN, VKEY_LMENU },
    std::pair { VKEY_RWIN, VKEY_RMENU },
    std::pair { VKEY_LMENU, VKEY_LWIN },
    std::pair { VKEY_RMENU, VKEY_RWIN },
  };
  for (const auto &[client, host] : mappings) {
    press(input, client);
    release(input, client);
    EXPECT_EQ(taken(), (std::vector<std::string> {
      "+" + std::to_string(host), "-" + std::to_string(host),
    }));
  }

  press(input, VKEY_A);
  release(input, VKEY_A);
  EXPECT_EQ(taken(), (std::vector<std::string> { "+65", "-65" }));
}

TEST_F(KeyboardStateTest, SwallowsDisplayShortcutAfterAllModifiersAreHeld) {
  auto input = input::testing::make_input();

  press(input, VKEY_LCONTROL, MODIFIER_CTRL);
  press(input, VKEY_LMENU, MODIFIER_CTRL | MODIFIER_ALT);
  press(input, VKEY_LSHIFT, MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT);
  press(input, VKEY_N, MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT);
  EXPECT_EQ(taken(), (std::vector<std::string> { "+162", "+164", "+160" }));

  press(input, VKEY_A, MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT);
  release(input, VKEY_A, MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT);
  EXPECT_EQ(taken(), (std::vector<std::string> { "+65", "-65" }));
}
