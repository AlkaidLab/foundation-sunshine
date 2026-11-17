/**
 * @file src/ds5/touchpad_mapper.h
 * @brief Allocation-free DualSense touchpad to host pointer mapping.
 *
 * The host receives one controller touch contact per packet.  This mapper
 * turns those contacts into relative pointer movement, two-finger scrolling or
 * a right-button gesture without allocating and without touching the report
 * path.  Native precision-touchpad injection stays in the platform backend;
 * this header covers the host-pointer decision only.
 *
 * Sources: docs/architecture/ds5-platform-design.zh.md section 16.4.7 on
 * branch docs/ds5-tuning-profile (commit 29f7673fb, read 2026-10-31) for the
 * key semantics; ds5-linux-hid-playstation-20261031 for the two-contact
 * touchpad model and the 1920x1080 native touchpad coordinate space.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ds5::tuning::runtime {
  enum class TouchpadOutput : std::uint32_t {
    controller = 0,
    host_pointer = 1,
    precision = 2,
    off = 3,
  };

  enum class TouchpadClickAction : std::uint32_t {
    touchpad_click = 0,
    left = 1,
    right = 2,
    none = 3,
  };

  enum class TouchpadTwoFingerAction : std::uint32_t {
    scroll = 0,
    right_click = 1,
    none = 2,
  };

  enum class TouchPhase : std::uint8_t {
    down = 0,
    move = 1,
    up = 2,
    cancel = 3,
  };

  struct touchpad_control_t {
    TouchpadOutput output { TouchpadOutput::controller };
    float sensitivity { 1.0f };
    float scroll_speed { 1.0f };
    bool reverse_scroll { false };
    TouchpadClickAction click { TouchpadClickAction::touchpad_click };
    TouchpadTwoFingerAction two_finger { TouchpadTwoFingerAction::scroll };
    int surface_width { 1920 };
    int surface_height { 1080 };
  };

  /**
   * @brief One host-pointer action produced by a touch or button transition.
   *
   * `scroll` uses the high-resolution wheel unit accepted by
   * platf::scroll(); 120 is one notch on Windows.  `mouse_button` uses the
   * client input button numbers (1 left, 3 right).
   */
  struct touchpad_pointer_event_t {
    int move_x { 0 };
    int move_y { 0 };
    int scroll { 0 };
    int mouse_button { 0 };
    bool mouse_release { false };
    bool consume { false };
  };

  class touchpad_pointer_t {
  public:
    static constexpr std::size_t max_contacts = 2;

    void
    reset() {
      contacts_ = {};
      two_finger_ = false;
      right_down_ = false;
      button_down_ = false;
      scroll_remainder_ = 0.0f;
      mid_x_ = 0.0f;
      mid_y_ = 0.0f;
    }

    touchpad_pointer_event_t
    touch(TouchPhase phase, std::uint32_t pointer_id, float x, float y, const touchpad_control_t &control) {
      touchpad_pointer_event_t event;
      event.consume = true;

      if (phase == TouchPhase::cancel) {
        end_two_finger(event);
        contacts_ = {};
        scroll_remainder_ = 0.0f;
        return event;
      }

      auto *contact = find(pointer_id);
      if (phase == TouchPhase::down) {
        if (contact == nullptr) contact = claim(pointer_id);
        if (contact == nullptr) return event;
        const bool was_two_finger = two_finger_;
        contact->active = true;
        contact->x = x;
        contact->y = y;
        if (!was_two_finger && active_count() >= 2) begin_two_finger(control, event);
        return event;
      }

      if (contact == nullptr) {
        // A lost down event must not leave the pointer stuck.  The first move
        // after a drop is treated as a fresh contact without emitting motion.
        if (phase != TouchPhase::move) return event;
        contact = claim(pointer_id);
        if (contact == nullptr) return event;
        contact->active = true;
        contact->x = x;
        contact->y = y;
        return event;
      }

      if (phase == TouchPhase::up) {
        contact->active = false;
        if (active_count() < 2) end_two_finger(event);
        if (active_count() == 0) scroll_remainder_ = 0.0f;
        return event;
      }

      if (two_finger_) {
        contact->x = x;
        contact->y = y;
        handle_two_finger(control, event);
        return event;
      }

      const auto delta_x = static_cast<float>(x - contact->x) * static_cast<float>(control.surface_width) * control.sensitivity;
      const auto delta_y = static_cast<float>(y - contact->y) * static_cast<float>(control.surface_height) * control.sensitivity;
      contact->x = x;
      contact->y = y;
      event.move_x = static_cast<int>(std::lround(delta_x));
      event.move_y = static_cast<int>(std::lround(delta_y));
      return event;
    }

    touchpad_pointer_event_t
    button(bool pressed, const touchpad_control_t &control) {
      touchpad_pointer_event_t event;
      if (pressed == button_down_) return event;
      button_down_ = pressed;

      switch (control.click) {
        case TouchpadClickAction::left:
          event.mouse_button = 1;
          event.mouse_release = !pressed;
          event.consume = true;
          break;
        case TouchpadClickAction::right:
          event.mouse_button = 3;
          event.mouse_release = !pressed;
          event.consume = true;
          break;
        case TouchpadClickAction::none:
          event.consume = true;
          break;
        case TouchpadClickAction::touchpad_click:
          event.consume = false;
          break;
      }
      return event;
    }

  private:
    struct contact_t {
      bool active { false };
      std::uint32_t id { 0 };
      float x { 0.0f };
      float y { 0.0f };
    };

    contact_t *
    find(std::uint32_t pointer_id) {
      for (auto &contact : contacts_) {
        if (contact.active && contact.id == pointer_id) return &contact;
      }
      return nullptr;
    }

    contact_t *
    claim(std::uint32_t pointer_id) {
      for (auto &contact : contacts_) {
        if (!contact.active) {
          contact = { true, pointer_id, 0.0f, 0.0f };
          return &contact;
        }
      }
      return nullptr;
    }

    std::size_t
    active_count() const {
      return static_cast<std::size_t>(std::count_if(contacts_.begin(), contacts_.end(), [](const contact_t &contact) { return contact.active; }));
    }

    void
    midpoint(float &x, float &y) const {
      float sum_x = 0.0f;
      float sum_y = 0.0f;
      std::size_t count = 0;
      for (const auto &contact : contacts_) {
        if (!contact.active) continue;
        sum_x += contact.x;
        sum_y += contact.y;
        ++count;
      }
      x = count ? sum_x / static_cast<float>(count) : 0.0f;
      y = count ? sum_y / static_cast<float>(count) : 0.0f;
    }

    void
    begin_two_finger(const touchpad_control_t &control, touchpad_pointer_event_t &event) {
      two_finger_ = true;
      scroll_remainder_ = 0.0f;
      midpoint(mid_x_, mid_y_);
      if (control.two_finger == TouchpadTwoFingerAction::right_click && !right_down_) {
        right_down_ = true;
        event.mouse_button = 3;
        event.mouse_release = false;
      }
    }

    void
    handle_two_finger(const touchpad_control_t &control, touchpad_pointer_event_t &event) {
      float x = 0.0f;
      float y = 0.0f;
      midpoint(x, y);
      const auto delta_y = y - mid_y_;
      mid_x_ = x;
      mid_y_ = y;

      if (control.two_finger != TouchpadTwoFingerAction::scroll) return;

      // A full-height two-finger sweep equals eight wheel notches.  Keep the
      // fractional remainder so slow gestures still produce smooth scrolling.
      auto units = -delta_y * 8.0f * 120.0f * control.scroll_speed;
      if (control.reverse_scroll) units = -units;
      scroll_remainder_ += units;
      const auto ticks = static_cast<int>(std::lround(scroll_remainder_));
      if (ticks != 0) {
        event.scroll = ticks;
        scroll_remainder_ -= static_cast<float>(ticks);
      }
    }

    void
    end_two_finger(touchpad_pointer_event_t &event) {
      if (!two_finger_) return;
      two_finger_ = false;
      scroll_remainder_ = 0.0f;
      if (right_down_) {
        right_down_ = false;
        event.mouse_button = 3;
        event.mouse_release = true;
      }
    }

    std::array<contact_t, max_contacts> contacts_ {};
    bool two_finger_ { false };
    bool right_down_ { false };
    bool button_down_ { false };
    float scroll_remainder_ { 0.0f };
    float mid_x_ { 0.0f };
    float mid_y_ { 0.0f };
  };
}  // namespace ds5::tuning::runtime
