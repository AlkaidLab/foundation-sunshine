/**
 * @file src/platform/windows/display_vram_internal.h
 * @brief Shared implementation types for D3D11 VRAM capture backends.
 */
#pragma once

#include <atomic>
#include <memory>

#include "display.h"

namespace platf::dxgi {
  /**
   * D3D11-backed image shared by capture backends and hardware encoders.
   *
   * This is intentionally kept out of display.h because it is an
   * implementation detail of the Windows VRAM capture path.
   */
  struct img_d3d_t: public platf::img_t {
    texture2d_t capture_texture;
    render_target_t capture_rt;
    keyed_mutex_t capture_mutex;

    HANDLE encoder_texture_handle = {};
    bool dummy = false;
    bool blank = true;
    std::uint32_t id = 0;
    DXGI_FORMAT format;
    bool linear_gamma = false;

    // Borrowed VDD frames use the producer's shared texture directly.
    bool borrowed_vdd_texture = false;
    bool borrowed_vdd_frame = false;
    keyed_mutex_t borrowed_vdd_mutex;
    std::shared_ptr<std::atomic<UINT64>> borrowed_vdd_inflight_counter;
    UINT32 borrowed_vdd_slot = 0;
    UINT64 encoder_acquire_key = 0;
    UINT64 encoder_release_key = 0;
    UINT64 producer_release_key = 0;

    void
    note_borrowed_vdd_frame_returned();

    void
    mark_borrowed_vdd_consumed();

    bool
    release_borrowed_vdd_after_convert(IDXGIKeyedMutex *encoder_mutex);

    bool
    abandon_borrowed_vdd_frame(bool log_busy = true, DWORD timeout_ms = 0);

    ~img_d3d_t() override;
  };

  /**
   * Scoped key-0 lock used while the capture device writes an image.
   */
  struct texture_lock_helper {
    keyed_mutex_t mutex;
    bool locked = false;

    texture_lock_helper(const texture_lock_helper &) = delete;
    texture_lock_helper &
    operator=(const texture_lock_helper &) = delete;

    texture_lock_helper(texture_lock_helper &&other);

    texture_lock_helper &
    operator=(texture_lock_helper &&other);

    texture_lock_helper(IDXGIKeyedMutex *mutex);

    ~texture_lock_helper();

    bool
    lock();
  };
}  // namespace platf::dxgi
