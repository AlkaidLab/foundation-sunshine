/**
 * @file src/platform/windows/nvprefs/nvprefs_common.h
 * @brief Declarations for common nvidia preferences.
 */
#pragma once

// sunshine utility header for generic smart pointers
#include "src/utility.h"

// winapi headers
// disable clang-format header reordering
// clang-format off
#include <windows.h>
#include <aclapi.h>
// clang-format on

namespace nvprefs {

  struct safe_handle: public util::safe_ptr_v2<void, BOOL, CloseHandle> {
    using util::safe_ptr_v2<void, BOOL, CloseHandle>::safe_ptr_v2;
    explicit
    operator bool() const {
      auto handle = get();
      return handle != NULL && handle != INVALID_HANDLE_VALUE;
    }
  };

  struct safe_hlocal_deleter {
    void
    operator()(void *p) {
      LocalFree(p);
    }
  };

  template <typename T>
  using safe_hlocal = util::uniq_ptr<std::remove_pointer_t<T>, safe_hlocal_deleter>;

  using safe_sid = util::safe_ptr_v2<void, PVOID, FreeSid>;

  void
  info_message(const std::wstring &message);

  void
  info_message(const std::string &message);

  void
  error_message(const std::wstring &message);

  void
  error_message(const std::string &message);

  struct nvprefs_options {
    bool opengl_vulkan_on_dxgi = true;
    bool sunshine_high_power_mode = true;

    // Stream-time game optimizations applied to the game's NVIDIA application
    // profile. All restored when the stream stops or via the undo file on the
    // next launch after a crash.
    bool nv_optimize_game = false;       // master switch for the per-game block
    bool nv_force_vsync = true;          // VSYNCMODE -> VSYNCMODE_FORCEON
    bool nv_lock_frame_rate = true;      // FRL_FPS -> client_fps + frl_offset (clamped >= 1)
    int  nv_frl_fps_offset = -2;         // delta added to client fps to derive FRL target
    int  nv_frl_fps_override = 0;        // if > 0, use this fps directly instead of client_fps + offset
    bool nv_prefer_max_performance = false;  // PREFERRED_PSTATE -> PREFERRED_PSTATE_PREFER_MAX
    bool nv_low_latency_mode = false;    // PRERENDERLIMIT -> 1 (matches NVIDIA "Low Latency Mode = On")

    // When true the same set of optimizations is also written to the BASE
    // (global) driver profile, so games launched outside Sunshine — or
    // sub-processes Sunshine cannot detect — also get the treatment until the
    // stream stops.
    bool nv_apply_to_base_profile = false;
  };

  nvprefs_options
  get_nvprefs_options();

}  // namespace nvprefs
