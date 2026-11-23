/**
 * @file src/platform/windows/unicode_input.h
 * @brief Key event order for text typed through KEYEVENTF_UNICODE.
 *
 * Every KEYEVENTF_UNICODE event is delivered as a key message whose virtual key
 * is VK_PACKET; the character travels in the scan code field and becomes the
 * WM_CHAR of the matching key-down.  Applications that follow key state by
 * virtual key (Qt, CEF and Chromium editors, WeChat) therefore see one
 * VK_PACKET key.  When all key-downs of a text run come first and the key-ups
 * only afterwards, that key never goes up between characters: the first
 * character is typed once and the rest is taken for auto-repeat of the held key
 * ("hello" arrives as "hhhhh").  Each UTF-16 unit therefore gets its key-up
 * immediately after its key-down, and the units of a surrogate pair follow one
 * another so a character is never split by another character.
 *
 * Sources: Microsoft Learn, SendInput, KEYBDINPUT (KEYEVENTF_UNICODE) and
 * "Virtual-Key Codes" (VK_PACKET).  The pure function below has no Windows
 * dependency so the order can be tested on any host.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace platf::win_unicode {
  /// One keyboard event of a text run: the UTF-16 unit goes into KEYBDINPUT::wScan.
  struct key_event_t {
    std::uint16_t unit;
    bool up;

    bool
    operator==(const key_event_t &other) const {
      return unit == other.unit && up == other.up;
    }
  };

  /**
   * @brief Events for typing `units` (UTF-16) as text, key-up directly after its key-down.
   * @param units The UTF-16 code units.
   * @param count Number of code units.
   * @return Down, up for every unit in text order; empty for no units.
   */
  inline std::vector<key_event_t>
  interleaved_key_events(const std::uint16_t *units, std::size_t count) {
    std::vector<key_event_t> events;
    if (!units) {
      return events;
    }
    events.reserve(count * 2);
    for (std::size_t i = 0; i < count; i++) {
      events.push_back({ units[i], false });
      events.push_back({ units[i], true });
    }
    return events;
  }
}  // namespace platf::win_unicode
