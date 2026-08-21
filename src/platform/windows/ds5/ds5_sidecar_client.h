/**
 * @file src/platform/windows/ds5/ds5_sidecar_client.h
 * @brief Narrow named-pipe client for the optional DualSense sidecar.
 */
#pragma once

#include <memory>

#include "src/platform/common.h"

namespace platf::ds5 {
  class sidecar_client_t {
  public:
    sidecar_client_t();
    ~sidecar_client_t();

    sidecar_client_t(const sidecar_client_t &) = delete;
    sidecar_client_t &operator=(const sidecar_client_t &) = delete;

    bool configured() const;
    bool owns(int global_index) const;
    int alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue, bool audio_haptics,
              bool genshin_compatibility = false);
    void free(int global_index);
    void submit_input(int global_index, const gamepad_state_t &state);
    void submit_touch(const gamepad_touch_t &touch);
    void submit_motion(const gamepad_motion_t &motion);
    void submit_battery(const gamepad_battery_t &battery);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace platf::ds5
