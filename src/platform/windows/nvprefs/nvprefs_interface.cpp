/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.cpp
 * @brief Definitions for nvidia preferences interface.
 */
// standard includes
#include <cassert>

// local includes
#include "driver_settings.h"
#include "nvprefs_interface.h"
#include "undo_file.h"

namespace {

  const auto sunshine_program_data_folder = "Sunshine";
  const auto nvprefs_undo_file_name = "nvprefs_undo.json";

}  // namespace

namespace nvprefs {

  namespace {

    bool
    undo_data_empty(const undo_data_t &undo_data) {
      return !undo_data.get_opengl_swapchain() && !undo_data.get_game_profile() && !undo_data.get_base_extras();
    }

    void
    merge_setting_undo(std::optional<undo_data_t::data_t::setting_undo_t> &current,
        const std::optional<undo_data_t::data_t::setting_undo_t> &incoming) {
      if (!incoming) {
        return;
      }
      if (current && incoming->undo_value && *incoming->undo_value == current->our_value) {
        current->our_value = incoming->our_value;
        return;
      }
      current = incoming;
    }

    void
    merge_game_profile_undo(undo_data_t &target, const undo_data_t::data_t::game_profile_t &incoming) {
      auto current = target.get_game_profile();
      if (!current || current->profile_name != incoming.profile_name || current->exe_path != incoming.exe_path) {
        target.set_game_profile(incoming);
        return;
      }

      current->profile_was_created = current->profile_was_created || incoming.profile_was_created;
      current->application_was_added = current->application_was_added || incoming.application_was_added;
      merge_setting_undo(current->vsync, incoming.vsync);
      merge_setting_undo(current->frl, incoming.frl);
      merge_setting_undo(current->pstate, incoming.pstate);
      merge_setting_undo(current->prerender, incoming.prerender);
      target.set_game_profile(*current);
    }

    void
    merge_base_extras_undo(undo_data_t &target, const undo_data_t::data_t::base_extras_t &incoming) {
      auto current = target.get_base_extras();
      if (!current) {
        target.set_base_extras(incoming);
        return;
      }

      merge_setting_undo(current->vsync, incoming.vsync);
      merge_setting_undo(current->frl, incoming.frl);
      merge_setting_undo(current->pstate, incoming.pstate);
      merge_setting_undo(current->prerender, incoming.prerender);
      target.set_base_extras(*current);
    }

    bool
    ensure_undo_dir_and_file(const std::filesystem::path &dir, const std::filesystem::path &file, std::optional<undo_file_t> &out) {
      if (out) {
        return true;
      }
      if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error_message("Couldn't create undo folder");
        return false;
      }
      out = undo_file_t::create_new_file(file);
      if (!out) {
        error_message("Couldn't create undo file");
        return false;
      }
      return true;
    }

  }  // namespace

  struct nvprefs_interface::impl {
    bool loaded = false;
    driver_settings_t driver_settings;
    std::filesystem::path undo_folder_path;
    std::filesystem::path undo_file_path;
    std::optional<undo_data_t> undo_data;
    std::optional<undo_file_t> undo_file;
  };

  nvprefs_interface::nvprefs_interface():
      pimpl(new impl()) {
  }

  nvprefs_interface::~nvprefs_interface() {
    if (owning_undo_file() && load()) {
      restore_stream_optimizations();
      restore_global_profile();
    }
    unload();
  }

  bool
  nvprefs_interface::load() {
    if (!pimpl->loaded) {
      // Check %ProgramData% variable, need it for storing undo file
      wchar_t program_data_env[MAX_PATH];
      auto get_env_result = GetEnvironmentVariableW(L"ProgramData", program_data_env, MAX_PATH);
      if (get_env_result == 0 || get_env_result >= MAX_PATH || !std::filesystem::is_directory(program_data_env)) {
        error_message("Missing or malformed %ProgramData% environment variable");
        return false;
      }

      // Prepare undo file path variables
      pimpl->undo_folder_path = std::filesystem::path(program_data_env) / sunshine_program_data_folder;
      pimpl->undo_file_path = pimpl->undo_folder_path / nvprefs_undo_file_name;

      // Dynamically load nvapi library and load driver settings
      pimpl->loaded = pimpl->driver_settings.init();
    }

    return pimpl->loaded;
  }

  void
  nvprefs_interface::unload() {
    if (pimpl->loaded) {
      // Unload dynamically loaded nvapi library
      pimpl->driver_settings.destroy();
      pimpl->loaded = false;
    }
  }

  bool
  nvprefs_interface::restore_from_and_delete_undo_file_if_exists() {
    if (!pimpl->loaded) return false;

    // Check for undo file from previous improper termination
    bool access_denied = false;
    if (auto undo_file = undo_file_t::open_existing_file(pimpl->undo_file_path, access_denied)) {
      // Try to restore from the undo file
      info_message("Opened undo file from previous improper termination");
      bool delete_after_restore = false;
      if (auto undo_data = undo_file->read_undo_data()) {
        bool ok = pimpl->driver_settings.restore_global_profile_to_undo(*undo_data);
        if (auto game_profile = undo_data->get_game_profile()) {
          ok = pimpl->driver_settings.restore_game_profile_to_undo(*game_profile) && ok;
        }
        if (auto base_extras = undo_data->get_base_extras()) {
          ok = pimpl->driver_settings.restore_base_extras_to_undo(*base_extras) && ok;
        }

        if (ok && pimpl->driver_settings.save_settings()) {
          info_message("Restored driver settings from undo file - deleting the file");
          delete_after_restore = true;
        }
        else {
          error_message("Failed to fully restore driver settings from undo file, keeping the file for retry");
          return false;
        }
      }
      else {
        error_message("Couldn't read undo file, deleting the unreadable file");
        delete_after_restore = true;
      }

      if (delete_after_restore && !undo_file->delete_file()) {
        error_message("Couldn't delete undo file");
        return false;
      }
    }
    else if (access_denied) {
      error_message("Couldn't open undo file from previous improper termination, or confirm that there's no such file");
      return false;
    }

    return true;
  }

  bool
  nvprefs_interface::modify_application_profile() {
    if (!pimpl->loaded) return false;

    // Modify and save sunshine.exe application profile settings, if needed
    bool modified = false;
    if (!pimpl->driver_settings.check_and_modify_application_profile(modified)) {
      error_message("Failed to modify application profile settings");
      return false;
    }
    else if (modified) {
      if (pimpl->driver_settings.save_settings()) {
        info_message("Modified application profile settings");
      }
      else {
        error_message("Couldn't save application profile settings");
        return false;
      }
    }
    else {
      info_message("No need to modify application profile settings");
    }

    return true;
  }

  bool
  nvprefs_interface::modify_global_profile() {
    if (!pimpl->loaded) return false;

    // Modify but not save global profile settings, if needed
    std::optional<undo_data_t> undo_data;
    if (!pimpl->driver_settings.check_and_modify_global_profile(undo_data)) {
      error_message("Couldn't modify global profile settings");
      return false;
    }
    else if (!undo_data) {
      info_message("No need to modify global profile settings");
      return true;
    }

    auto make_undo_and_commit = [&]() -> bool {
      // Create and lock undo file if it hasn't been done yet
      if (!ensure_undo_dir_and_file(pimpl->undo_folder_path, pimpl->undo_file_path, pimpl->undo_file)) {
        return false;
      }

      assert(undo_data);
      if (pimpl->undo_data) {
        // Merge undo data if settings has been modified externally since our last modification
        pimpl->undo_data->merge(*undo_data);
      }
      else {
        pimpl->undo_data = undo_data;
      }

      // Write undo data to undo file
      if (!pimpl->undo_file->write_undo_data(*pimpl->undo_data)) {
        error_message("Couldn't write to undo file - deleting the file");
        if (!pimpl->undo_file->delete_file()) {
          error_message("Couldn't delete undo file");
        }
        return false;
      }

      // Save global profile settings
      if (!pimpl->driver_settings.save_settings()) {
        error_message("Couldn't save global profile settings");
        return false;
      }

      return true;
    };

    if (!make_undo_and_commit()) {
      // Revert settings modifications
      pimpl->driver_settings.load_settings();
      return false;
    }

    return true;
  }

  bool
  nvprefs_interface::owning_undo_file() {
    return pimpl->undo_file.has_value();
  }

  bool
  nvprefs_interface::restore_global_profile() {
    if (!pimpl->loaded || !pimpl->undo_data || !pimpl->undo_file) return false;

    // Restore global profile settings with undo data
    if (pimpl->driver_settings.restore_global_profile_to_undo(*pimpl->undo_data) &&
        pimpl->driver_settings.save_settings()) {
      pimpl->undo_data->clear_opengl_swapchain();

      if (undo_data_empty(*pimpl->undo_data)) {
        if (!pimpl->undo_file->delete_file()) {
          error_message("Couldn't delete undo file");
          return false;
        }
        pimpl->undo_data = std::nullopt;
        pimpl->undo_file = std::nullopt;
      }
      else if (!pimpl->undo_file->write_undo_data(*pimpl->undo_data)) {
        error_message("Couldn't update undo file after restoring global profile");
        return false;
      }
    }
    else {
      error_message("Couldn't restore global profile settings");
      return false;
    }

    return true;
  }

  bool
  nvprefs_interface::apply_stream_optimizations(const std::wstring &exe_name, int client_fps) {
    if (!pimpl->loaded) return false;

    std::optional<undo_data_t::data_t::game_profile_t> game_undo;
    std::optional<undo_data_t::data_t::base_extras_t> base_undo;

    const bool ok_game = pimpl->driver_settings.check_and_modify_game_profile(exe_name, client_fps, game_undo);
    if (!ok_game) {
      error_message("Couldn't fully apply game-profile optimizations");
    }
    const bool ok_base = pimpl->driver_settings.check_and_modify_base_extras(client_fps, base_undo);
    if (!ok_base) {
      error_message("Couldn't fully apply base-profile optimizations");
    }

    if (!game_undo && !base_undo) {
      return ok_game && ok_base;
    }

    if (!pimpl->undo_data) {
      pimpl->undo_data = undo_data_t {};
    }
    if (game_undo) {
      merge_game_profile_undo(*pimpl->undo_data, *game_undo);
    }
    if (base_undo) {
      merge_base_extras_undo(*pimpl->undo_data, *base_undo);
    }

    if (!ensure_undo_dir_and_file(pimpl->undo_folder_path, pimpl->undo_file_path, pimpl->undo_file)) {
      pimpl->driver_settings.load_settings();
      return false;
    }
    if (!pimpl->undo_file->write_undo_data(*pimpl->undo_data)) {
      error_message("Couldn't write stream-optimization undo data");
      pimpl->driver_settings.load_settings();
      return false;
    }
    if (!pimpl->driver_settings.save_settings()) {
      error_message("Couldn't save driver settings after stream optimizations");
      return false;
    }

    return ok_game && ok_base;
  }

  bool
  nvprefs_interface::restore_stream_optimizations() {
    if (!pimpl->loaded || !pimpl->undo_data) return true;

    bool ok = true;
    bool game_restored = false;
    bool base_restored = false;

    if (auto game_profile = pimpl->undo_data->get_game_profile()) {
      if (pimpl->driver_settings.restore_game_profile_to_undo(*game_profile)) {
        game_restored = true;
      }
      else {
        ok = false;
      }
    }
    if (auto base_extras = pimpl->undo_data->get_base_extras()) {
      if (pimpl->driver_settings.restore_base_extras_to_undo(*base_extras)) {
        base_restored = true;
      }
      else {
        ok = false;
      }
    }

    if (!game_restored && !base_restored) {
      return ok;
    }

    if (!pimpl->driver_settings.save_settings()) {
      error_message("Couldn't save driver settings after restoring stream optimizations");
      return false;
    }

    if (game_restored) {
      pimpl->undo_data->clear_game_profile();
    }
    if (base_restored) {
      pimpl->undo_data->clear_base_extras();
    }

    if (pimpl->undo_file && undo_data_empty(*pimpl->undo_data)) {
      if (!pimpl->undo_file->delete_file()) {
        error_message("Couldn't delete now-empty undo file");
        return false;
      }
      pimpl->undo_data = std::nullopt;
      pimpl->undo_file = std::nullopt;
    }
    else if (pimpl->undo_file && !pimpl->undo_file->write_undo_data(*pimpl->undo_data)) {
      error_message("Couldn't update undo file after restoring stream optimizations");
      return false;
    }

    return ok;
  }

  void
  nvprefs_interface::release_undo_file_for_later_restore() {
    pimpl->undo_data = std::nullopt;
    pimpl->undo_file = std::nullopt;
  }

}  // namespace nvprefs
