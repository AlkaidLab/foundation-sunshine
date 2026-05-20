/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.h
 * @brief Declarations for nvidia preferences interface.
 */
#pragma once

// standard library headers
#include <memory>
#include <string>

namespace nvprefs {

  class nvprefs_interface {
  public:
    nvprefs_interface();
    ~nvprefs_interface();

    bool
    load();

    void
    unload();

    bool
    restore_from_and_delete_undo_file_if_exists();

    bool
    modify_application_profile();

    bool
    modify_global_profile();

    bool
    owning_undo_file();

    bool
    restore_global_profile();

    /**
     * @brief Apply per-stream NVIDIA driver optimizations: per-game profile
     *        (always when feature is on) and BASE profile (when the user
     *        opted into nv_apply_to_base_profile). Updates and persists the
     *        undo manifest so a Sunshine crash mid-stream still allows the
     *        next launch to roll the changes back.
     * @param exe_name Lower-cased basename of the running game executable.
     *                 Empty string skips the per-game profile leg silently.
     * @param client_fps Client refresh rate, used to derive FRL target.
     */
    bool
    apply_stream_optimizations(const std::wstring &exe_name, int client_fps);

    /**
     * @brief Reverse a previous apply_stream_optimizations(). Safe to call
     *        even if no optimizations were applied.
     */
    bool
    restore_stream_optimizations();

  private:
    struct impl;
    std::unique_ptr<impl> pimpl;
  };

}  // namespace nvprefs
