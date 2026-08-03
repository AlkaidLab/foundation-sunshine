/**
 * @file src/input_activity.cpp
 * @brief Definitions for input activity detection used by the VRR input activity boost.
 */
// lib includes
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>

// local includes
#include "config.h"
#include "cursor_channel.h"
#include "input_activity.h"
#include "utility.h"

namespace input::activity {

  bool
  tracker_t::evaluate(_NV_INPUT_HEADER *payload) {
    switch (util::endian::little(payload->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5: {
        const auto *mouse = reinterpret_cast<PNV_REL_MOUSE_MOVE_PACKET>(payload);
        return config::input.mouse &&
               cursor_channel::local_mode_active() &&
               (util::endian::big(mouse->deltaX) != 0 ||
                util::endian::big(mouse->deltaY) != 0);
      }
      case MOUSE_MOVE_ABS_MAGIC:
        return config::input.mouse && cursor_channel::local_mode_active();
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return config::input.mouse;
      case SCROLL_MAGIC_GEN5: {
        const auto *scroll = reinterpret_cast<PNV_SCROLL_PACKET>(payload);
        return config::input.mouse &&
               (util::endian::big(scroll->scrollAmt1) != 0 ||
                util::endian::big(scroll->scrollAmt2) != 0);
      }
      case SS_HSCROLL_MAGIC:
        return config::input.mouse &&
               util::endian::big(reinterpret_cast<PSS_HSCROLL_PACKET>(payload)->scrollAmount) != 0;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
      case UTF8_TEXT_EVENT_MAGIC:
        return config::input.keyboard;
      default:
        return false;
    }
  }

}  // namespace input::activity
