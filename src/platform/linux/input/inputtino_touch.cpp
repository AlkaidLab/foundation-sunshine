/**
 * @file src/platform/linux/input/inputtino_touch.cpp
 * @brief Definitions for inputtino touch input handling.
 */
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

#include "inputtino_common.h"
#include "inputtino_touch.h"
#include "touch_plan.h"

using namespace std::literals;

namespace platf::touch {
  namespace {
    /**
     * @brief Release every contact this client currently has down.
     */
    void
    release_contacts(client_input_raw_t *raw) {
      for (auto pointer_id : raw->active_touch_pointers) {
        (*raw->touch).release_finger(pointer_id);
      }

      raw->active_touch_pointers.clear();
    }
  }  // namespace

  void
  update(client_input_raw_t *raw, const touch_port_t &touch_port, const touch_input_t &touch) {
    // The coordinates the client sent are already normalized to this client's touch port,
    // which is the whole display on Linux: the synthetic device covers the display and takes
    // 0..1, so there is no pixel space to map into here (unlike the Windows path, where the
    // touch port is a pixel rectangle inside the virtual desktop).
    (void) touch_port;

    if (!raw->touch) {
      return;
    }

    if (!known_event(touch.eventType)) {
      BOOST_LOG(warning) << "Unknown touch event: "sv << (int) touch.eventType;
      return;
    }

    const bool contact_held = raw->active_touch_pointers.count(touch.pointerId) != 0;
    const auto plan = plan_for(touch, contact_held, raw->active_touch_pointers.size());

    if (plan.action == action_e::release_all) {
      release_contacts(raw);
    }

    switch (plan.action) {
      case action_e::place:
      case action_e::release_all:
        // `release_all` only carries a placement when the pointer table overflowed and the
        // new contact has to start on a freed slot.
        if (plan.action == action_e::place || plan.place_after_release) {
          (*raw->touch).place_finger(touch.pointerId, touch.x, touch.y, plan.pressure, plan.orientation);
          raw->active_touch_pointers.insert(touch.pointerId);
        }
        break;

      case action_e::release:
        // Only a pointer we hold a contact for can be lifted: the client can repeat an UP for
        // a pointer it already lifted, and releasing an unknown pointer would corrupt the
        // state of whichever pointer holds that slot.
        if (contact_held) {
          (*raw->touch).release_finger(touch.pointerId);
          raw->active_touch_pointers.erase(touch.pointerId);
        }
        break;

      case action_e::ignore:
        break;
    }
  }
}  // namespace platf::touch
