/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

#include <functional>

#include "platform/common.h"
#include "platform/gamepad_selection.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  void
  print(void *input);
  void
  reset(std::shared_ptr<input_t> &input);
  void
  passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  [[nodiscard]] std::unique_ptr<platf::deinit_t>
  init();

  bool
  probe_gamepads();

  std::shared_ptr<input_t>
  alloc(safe::mail_t mail, std::uint64_t session_id, std::string client_gamepad, std::function<void()> on_pen_barrel_roll = {});

  /**
   * @brief 会话 PlayStation 手柄的只读汇总（供 /api/runtime 会话回复）。
   * @param input 会话输入上下文；空指针返回未分配的默认值。
   * @return 第一个上报为 PlayStation 的已分配手柄的活动虚拟类型与 DualSense 身份上报。
   */
  platf::gamepad_selection::pad_facts_t
  pad_summary(const std::shared_ptr<input_t> &input);

  /**
   * @brief 检查当前会话是否已成功分配 DualSense。
   * @param input 会话输入上下文。
   * @return 至少一个 DualSense 手柄分配成功时返回 true。
   */
  bool
  has_ds5_gamepad(const std::shared_ptr<input_t> &input);

  /**
   * @brief 检查当前会话的 DualSense 音频触觉是否仍可用。
   * @param input 会话输入上下文。
   * @return 已协商音频触觉且未降级或断线时返回 true。
   */
  bool
  has_ds5_audio_haptics(const std::shared_ptr<input_t> &input);

  /**
   * @brief 记录 DualSense 客户端上报的耳机和麦克风状态（控制消息 0x5516）。
   * @param input 会话输入上下文。
   * @param controller 线上的手柄编号。
   * @param valid_mask ds5::control_extension::in_* 位，只有置位的字段有效。
   *
   * 手柄尚未分配时状态先缓存，分配成功后写入虚拟设备的输入报告。
   */
  void
  ds5_input_status(const std::shared_ptr<input_t> &input, std::uint16_t controller, std::uint32_t valid_mask,
    std::uint8_t headset_present, std::uint8_t microphone_present, std::uint8_t microphone_muted);

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Physical size of the selected display in desktop pixels. UIA and caret
    // rectangles are reported in this coordinate frame, so remote text context
    // capture geometry must use these extents, not width/height (stream
    // resolution) or env_* (whole virtual desktop).
    int display_width, display_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit
    operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0 &&
             display_width != 0 && display_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float>
  scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);

#ifdef SUNSHINE_TESTS
  namespace testing {
    struct keyboard_event_t {
      std::uint16_t key_code;
      bool release;
      std::uint8_t flags;
    };

    std::shared_ptr<input_t>
    make_input();

    void
    set_keyboard_sink(std::function<void(const keyboard_event_t &)> sink);

    void
    send_keyboard_packet(std::shared_ptr<input_t> &input, std::uint16_t key_code,
                         std::uint8_t modifiers, std::uint8_t flags, bool release);

    void
    release_held_keys();

    void
    reset_keyboard_state();
  }  // namespace testing
#endif
}  // namespace input
