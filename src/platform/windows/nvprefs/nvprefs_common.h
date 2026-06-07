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
    bool nv_optimize_game = false;
    bool nv_force_vsync = true;
    bool nv_lock_frame_rate = true;
    int nv_frl_fps_offset = -2;
    int nv_frl_fps_override = 0;
    bool nv_prefer_max_performance = false;
    bool nv_low_latency_mode = false;
    bool nv_apply_to_base_profile = false;
  };

  void
  set_nvprefs_options(nvprefs_options options);

  nvprefs_options
  get_nvprefs_options();

}  // namespace nvprefs
