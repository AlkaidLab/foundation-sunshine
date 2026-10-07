/**
 * @file src/platform/windows/display_vram_internal.h
 * @brief Shared implementation types for D3D11 VRAM capture backends.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

#include "display.h"

namespace platf::dxgi {
  struct shared_yuv_plane_t {
    HANDLE shared_handle = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool ten_bit = false;
  };

  /**
   * D3D11 YUV output owned by the existing conversion/filter pipeline.
   *
   * PyroWave consumes this interface without duplicating Sunshine's HDR,
   * rotation, scaling, cursor, or optional enhancement shaders.
   */
  class shared_yuv_encode_device_t: public platf::encode_device_t {
  public:
    virtual ID3D11Device *d3d_device() noexcept = 0;
    virtual ID3D11DeviceContext *d3d_context() noexcept = 0;
    virtual const std::array<shared_yuv_plane_t, 3> &yuv_planes() const noexcept = 0;
  };

  [[nodiscard]] std::unique_ptr<shared_yuv_encode_device_t>
  make_shared_yuv_encode_device(std::shared_ptr<display_base_t> display, const ::video::config_t &config);

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
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
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
