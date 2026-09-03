/**
 * @file src/text_context/bridge.h
 * @brief Correlates user-session text-focus observations with remote input.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace text_context {
  using session_id = std::uint64_t;
  using payload_t = std::vector<std::uint8_t>;

  constexpr std::uint8_t kWireVersion = 1;
  constexpr std::size_t kWireSize = 76;

  enum class source_e : std::uint8_t {
    input_pane = 1,
    uia = 2,
  };

  enum class cause_e : std::uint8_t {
    unknown = 0,
    remote_touch = 1,
    remote_mouse = 2,
  };

  struct screen_rect_t {
    std::int32_t left {};
    std::int32_t top {};
    std::int32_t right {};
    std::int32_t bottom {};

    bool valid() const { return right > left && bottom > top; }
  };

  struct observation_t {
    source_e source {source_e::input_pane};
    bool active {false};
    bool editable {false};
    bool password {false};
    bool multiline {false};
    bool pane_visible {false};
    bool auto_show {false};
    std::optional<screen_rect_t> element_rect;
    std::optional<screen_rect_t> caret_rect;
  };

  struct outbound_msg_t {
    session_id target {};
    payload_t bytes;
  };

  class bridge_t {
  public:
    static bridge_t &instance();

    void session_started(session_id sid);
    void session_stopped(session_id sid);

    void record_touch(session_id sid, std::uint8_t event_type, std::uint32_t pointer_id,
                      std::int32_t screen_x, std::int32_t screen_y,
                      std::int32_t capture_left, std::int32_t capture_top,
                      std::uint32_t capture_width, std::uint32_t capture_height);
    void record_mouse_button(session_id sid, bool release,
                             std::int32_t screen_x, std::int32_t screen_y,
                             std::int32_t capture_left, std::int32_t capture_top,
                             std::uint32_t capture_width, std::uint32_t capture_height);

    /// Returns true when the observation consumed a matching remote input.
    bool observe(const observation_t &observation);
    void drain_outbound(std::deque<outbound_msg_t> &out);

    void notify_gui_alive(bool input_pane, bool uia);
    bool gui_alive() const;
    bool input_pane_available() const;
    bool uia_available() const;

    bridge_t(const bridge_t &) = delete;
    bridge_t &operator=(const bridge_t &) = delete;

  private:
    bridge_t();
    ~bridge_t();
    bool correlate_locked(const observation_t &observation);
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace text_context
