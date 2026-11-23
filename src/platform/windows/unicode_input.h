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
 * SendInput can insert only a prefix of a batch.  Microsoft Learn, SendInput,
 * "Return value": the function returns the number of events it successfully
 * inserted into the input stream, which is less than the array size when a
 * thread blocked the input (UIPI, or another desktop is the input desktop), and
 * zero when none was inserted.  The caller has to send the remaining events
 * itself, and when it cannot, the key-ups of the keys already pressed
 * (pending_key_ups) so VK_PACKET is not left down.
 *
 * Sources: Microsoft Learn, SendInput, KEYBDINPUT (KEYEVENTF_UNICODE) and
 * "Virtual-Key Codes" (VK_PACKET).  The pure functions below have no Windows
 * dependency so the order can be tested on any host.
 */
#pragma once

#include <cstddef>
#include <iterator>
#include <cstdint>
#include <span>
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
   * @tparam Unit A 16-bit character type (`char16_t`, `wchar_t` on Windows, `std::uint16_t`).
   *              Each element is converted by value; no storage is read through another type.
   * @param units The UTF-16 code units.
   * @return Down, up for every unit in text order; empty for no units.
   */
  template <typename Unit>
  std::vector<key_event_t>
  interleaved_key_events(std::span<const Unit> units) {
    static_assert(sizeof(Unit) == sizeof(std::uint16_t), "a UTF-16 code unit is 16 bits wide");
    std::vector<key_event_t> events;
    events.reserve(units.size() * 2);
    for (const Unit unit : units) {
      const auto value = static_cast<std::uint16_t>(unit);
      events.push_back({ value, false });
      events.push_back({ value, true });
    }
    return events;
  }

  /**
   * @brief Key-ups still owed after only the first `injected` events of `events` went out.
   * @details A key whose down was injected without its up stays pressed; the result releases
   *          each of them, the most recently pressed first.  Nothing is owed when `injected`
   *          is zero, covers the whole run, or ends right after a key-up.
   * @param events The full event list of the run.
   * @param injected How many events from the start were inserted into the input stream.
   * @return The key-up events to send; empty when no key is left down.
   */
  inline std::vector<key_event_t>
  pending_key_ups(const std::vector<key_event_t> &events, std::size_t injected) {
    std::vector<key_event_t> held;
    if (injected > events.size()) {
      injected = events.size();
    }
    for (std::size_t i = 0; i < injected; i++) {
      if (!events[i].up) {
        held.push_back({ events[i].unit, true });
        continue;
      }
      // An up releases the most recent down of the same unit.
      for (auto it = held.rbegin(); it != held.rend(); ++it) {
        if (it->unit == events[i].unit) {
          held.erase(std::next(it).base());
          break;
        }
      }
    }
    return std::vector<key_event_t>(held.rbegin(), held.rend());
  }
}  // namespace platf::win_unicode
