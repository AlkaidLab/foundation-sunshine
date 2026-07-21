/**
 * @file src/input_activity.cpp
 * @brief Definitions for input activity detection used by the VRR input activity boost.
 */
// lib includes
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>

// local includes
#include "config.h"
#include "input_activity.h"
#include "utility.h"

namespace input::activity {

  bool
  tracker_t::evaluate(_NV_INPUT_HEADER *payload) {
    switch (util::endian::little(payload->magic)) {
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
      case UTF8_TEXT_EVENT_MAGIC:
        return config::input.keyboard;
      default:
        return false;
    }
  }

}  // namespace input::activity
