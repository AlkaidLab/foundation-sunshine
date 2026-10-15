/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.h
 * @brief Serialized NVIDIA preferences transactions and session ownership.
 */
#pragma once
#include <memory>
#include <string>

namespace nvprefs {
  class nvprefs_interface {
  public:
    struct status_t {
      bool supported;
      std::string state;
      bool detection_known { true };
    };

    nvprefs_interface();
    ~nvprefs_interface();
    nvprefs_interface(const nvprefs_interface &) = delete;
    nvprefs_interface &
    operator=(const nvprefs_interface &) = delete;

    // Configure Sunshine/OpenGL preferences and recover an abandoned journal.
    bool
    configure();
    // Restore owned changes or a journal left by a terminated Sunshine process.
    bool
    restore();
    // Call only for a resolved virtual-display launch, before starting the game.
    // The last lease restores VSync; pending RTSP tickets also hold a lease.
    std::shared_ptr<void>
    acquire_vsync();
    status_t
    status();

  private:
    struct impl;
    std::shared_ptr<impl> pimpl;
  };
}  // namespace nvprefs
