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

    bool
    apply_stream_optimizations(const std::wstring &exe_name, int client_fps);

    bool
    restore_stream_optimizations();

    void
    release_undo_file_for_later_restore();

  private:
    struct impl;
    std::unique_ptr<impl> pimpl;
  };

}  // namespace nvprefs
