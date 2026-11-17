/**
 * @file src/platform/windows/ds5/ds5_native_client.h
 * @brief Native Windows DualSense client.
 */
#pragma once

#include <cstdint>
#include <memory>

#include <boost/atomic.hpp>

#include "src/platform/common.h"

namespace platf::ds5 {
  class native_client_t {
  public:
    native_client_t();
    ~native_client_t();

    native_client_t(const native_client_t &) = delete;
    native_client_t &operator=(const native_client_t &) = delete;

    bool owns(int global_index) const;
    /** @brief Reports whether the native audio path is active. */
    bool audio_haptics_active() const noexcept;
    /** @brief Create one virtual DualSense for this gamepad index (several may live at once). */
    int alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue, bool audio_haptics);
    void free(int global_index);
    void submit_input(int global_index, const gamepad_state_t &state);
    void submit_touch(const gamepad_touch_t &touch);
    void submit_motion(const gamepad_motion_t &motion);
    void submit_battery(const gamepad_battery_t &battery);
    /** @brief Apply the headset and microphone state a client reported (control message 0x5516). */
    void submit_status(const gamepad_ds5_status_t &status);
    /** @brief Send the saved lightbar state and extension values once to every active pad (after a tuning save). */
    void push_tuning_state();
    /** @brief One short test rumble (restore false) or its end (restore true) on every live pad; returns the pads that took it. */
    unsigned test_rumble(std::uint8_t left, std::uint8_t right, bool restore);
    /** @brief Test hook: the time source of motion samples in microseconds; nullptr restores the system clock. */
    static void set_motion_clock(std::int64_t (*clock)());

  private:
    struct impl_t;

    // 必须比 _impl 后析构，保证 reader 退出前发布目标始终有效。
    // 分配、释放及输入提交仍在输入线程串行执行，控制线程只读取这个原子状态。
    boost::atomic<bool> published_audio_haptics { false };
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace platf::ds5
