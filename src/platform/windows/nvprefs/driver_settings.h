/**
 * @file src/platform/windows/nvprefs/driver_settings.h
 * @brief Declarations for nvidia driver settings.
 */
#pragma once

// local includes first so standard library headers are pulled in before nvapi's SAL macros
#include "undo_data.h"

// nvapi headers
// disable clang-format header reordering
// as <NvApiDriverSettings.h> needs types from <nvapi.h>
// clang-format off

// With GCC/MinGW, nvapi_lite_salend.h (included transitively via nvapi_lite_d3dext.h)
// undefines all SAL annotation macros (e.g. __success, __in, __out, __inout) after
// nvapi_lite_salstart.h had defined them. This leaves NVAPI_INTERFACE and other macros
// that use SAL annotations broken for the rest of nvapi.h. Defining __NVAPI_EMPTY_SAL
// makes nvapi_lite_salend.h a no-op, preserving the SAL macro definitions throughout.
// After nvapi.h, we include nvapi_lite_salend.h explicitly (without __NVAPI_EMPTY_SAL)
// to clean up the SAL macros and prevent them from polluting subsequent includes.
#if defined(__GNUC__)
  #define __NVAPI_EMPTY_SAL
#endif

#include <nvapi.h>
#include <NvApiDriverSettings.h>

#if defined(__GNUC__)
  #undef __NVAPI_EMPTY_SAL
  // Clean up SAL macros that nvapi_lite_salstart.h defined and salend.h was
  // prevented from cleaning up (due to __NVAPI_EMPTY_SAL above).
  #include <nvapi_lite_salend.h>
#endif
// clang-format on

namespace nvprefs {

  class driver_settings_t {
  public:
    ~driver_settings_t();

    bool
    init();

    void
    destroy();

    bool
    load_settings();

    bool
    save_settings();

    bool
    restore_global_profile_to_undo(const undo_data_t &undo_data);

    bool
    check_and_modify_global_profile(std::optional<undo_data_t> &undo_data);

    bool
    check_and_modify_application_profile(bool &modified);

    /**
     * @brief Apply per-game stream optimizations (force VSync, FRL, max-perf,
     *        low-latency) to the NVIDIA application profile that owns the given
     *        executable. If no profile owns it, a SunshineStream-Game profile is
     *        created on demand.
     * @param exe_name Lower-cased basename of the running game executable
     *                 (NvAPI matches applications by basename).
     * @param client_fps Target frame rate reported by the streaming client; used
     *                   to derive the FRL value when nv_lock_frame_rate is on.
     * @param undo_out Out: filled with the data needed to restore the profile.
     *                 Already-existing data is overwritten — caller must merge
     *                 with any prior session's data.
     * @return true on success (including the no-op case).
     */
    bool
    check_and_modify_game_profile(const std::wstring &exe_name, int client_fps, std::optional<undo_data_t::data_t::game_profile_t> &undo_out);

    /**
     * @brief Reverse a previous check_and_modify_game_profile() using the saved
     *        undo data: restore each touched setting to its original value (or
     *        delete it if it didn't exist), remove the application from the
     *        profile if we added it, and delete the profile if we created it
     *        and it has no other applications.
     */
    bool
    restore_game_profile_to_undo(const undo_data_t::data_t::game_profile_t &undo_data);

    /**
     * @brief Apply the same stream optimizations to the BASE (global) driver
     *        profile, so apps Sunshine cannot detect also benefit. Restored on
     *        stream stop.
     * @param client_fps See check_and_modify_game_profile.
     */
    bool
    check_and_modify_base_extras(int client_fps, std::optional<undo_data_t::data_t::base_extras_t> &undo_out);

    /**
     * @brief Reverse a previous check_and_modify_base_extras() using the saved
     *        undo data.
     */
    bool
    restore_base_extras_to_undo(const undo_data_t::data_t::base_extras_t &undo_data);

  private:
    NvDRSSessionHandle session_handle = 0;
  };

}  // namespace nvprefs
