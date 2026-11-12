/**
 * @file src/platform/linux/input/touch_plan.h
 * @brief Translation of client touch events into synthetic touchscreen contact actions.
 * @details Kept free of inputtino and libevdev so the mapping can be unit tested without a
 *          uinput device. The Windows path is the reference: a contact exists only between
 *          DOWN/MOVE and UP/CANCEL, hover marks a pointer in-range without contacting, and a
 *          cancelled gesture releases what it had down (`populate_common_pointer_info` and
 *          `touch_update` in src/platform/windows/input.cpp).
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "src/platform/common.h"

namespace platf::touch {
  /**
   * @brief What a client touch event means for the host's contact set.
   */
  enum class action_e {
    ignore,  ///< No change to the contact set
    place,  ///< Create or update this pointer's contact
    release,  ///< Release this pointer's contact, if it has one
    release_all,  ///< Release every contact this client placed
  };

  /// Contacts the synthetic touchscreen can report (the device has 16 MT slots).
  inline constexpr std::size_t kMaxContacts = 16;
  /// Windows sends 512/1024 for a pointer whose client reported no pressure or distance.
  inline constexpr float kDefaultPressure = 0.5f;

  struct plan_t {
    action_e action = action_e::ignore;
    /// Whether a contact should be placed right after `release_all` freed the table.
    bool place_after_release = false;
    /// Contact orientation in degrees, -90..90; 0 when the client reported none.
    int orientation = 0;
    /// Contact pressure, 0..1.
    float pressure = 0.0f;
  };

  /**
   * @brief Whether the event type is one this backend knows how to translate.
   * @details Windows logs a warning for anything else rather than guessing.
   */
  constexpr bool
  known_event(std::uint8_t event_type) {
    switch (event_type) {
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        return true;
      default:
        return false;
    }
  }

  /**
   * @brief Convert the client's 0..360 contact rotation to the device's -90..90 range.
   * @details This mirrors the mapping the backend has always used (measured from the Y axis),
   *          but keeps the client's "no rotation reported" sentinel out of the maths: run
   *          through it, 65535 would clamp to a claim of 90 degrees.
   */
  inline int
  orientation_for(std::uint16_t rotation) {
    if (rotation == LI_ROT_UNKNOWN) {
      return 0;
    }

    int adjusted = rotation;

    if (adjusted > 90 && adjusted < 270) {
      // Lower hemisphere
      adjusted = 180 - adjusted;
    }

    if (adjusted > 90) {
      adjusted -= 360;
    }
    else if (adjusted < -90) {
      adjusted += 360;
    }

    return std::clamp(adjusted, -90, 90);
  }

  /**
   * @brief Pressure to hand the device for a contacting pointer.
   * @details Windows substitutes 512/1024 for a client that reports nothing, so that a plain
   *          finger does not read as zero pressure.
   */
  constexpr float
  pressure_for(float pressure_or_distance) {
    return pressure_or_distance != 0.0f ? pressure_or_distance : kDefaultPressure;
  }

  /**
   * @brief Decide what to do with a client touch event.
   * @param touch The client's event.
   * @param contact_held Whether this pointer currently has a contact down.
   * @param contact_count Contacts this client has down right now.
   */
  inline plan_t
  plan_for(const touch_input_t &touch, bool contact_held, std::size_t contact_count) {
    plan_t plan;

    switch (touch.eventType) {
      case LI_TOUCH_EVENT_HOVER:
        // In range but not touching: on its own it is never a contact. If we are holding one
        // for this pointer, the client is telling us it stopped touching.
        if (contact_held) {
          plan.action = action_e::release;
        }
        return plan;

      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        if (!contact_held && contact_count >= kMaxContacts) {
          // Windows cancels every active touch when it runs out of pointer entries.
          plan.action = action_e::release_all;
          plan.place_after_release = true;
        }
        else {
          plan.action = action_e::place;
        }
        plan.orientation = orientation_for(touch.rotation);
        plan.pressure = pressure_for(touch.pressureOrDistance);
        return plan;

      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        plan.action = action_e::release;
        return plan;

      case LI_TOUCH_EVENT_CANCEL_ALL:
        plan.action = action_e::release_all;
        return plan;

      case LI_TOUCH_EVENT_BUTTON_ONLY:
      default:
        // A button-only pointer is not a contact, and an unknown event means nothing to us.
        return plan;
    }
  }
}  // namespace platf::touch
