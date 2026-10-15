/**
 * @file src/platform/windows/nvprefs/nvprefs_interface.cpp
 * @brief Definitions for nvidia preferences interface.
 */
// standard includes
#include <cassert>
#include <dxgi.h>
#include <mutex>

// local includes
#include "driver_settings.h"
#include "nvprefs_interface.h"
#include "undo_file.h"

namespace {

  const auto sunshine_program_data_folder = "Sunshine";
  const auto nvprefs_undo_file_name = "nvprefs_undo.json";

}  // namespace

namespace nvprefs {

  struct nvprefs_interface::impl {
    std::mutex mutex;
    bool loaded = false;
    unsigned vsync_users = 0;
    std::string vsync_state = "idle";

    ~impl();
    bool
    load();
    void
    unload();
    bool
    restore_from_and_delete_undo_file_if_exists();
    bool
    modify_application_profile(const nvprefs_options &options);
    bool
    modify_global_profile(const nvprefs_options &options, bool force_vsync = false);
    bool
    owning_undo_file();
    bool
    restore(bool manage_driver_settings);
    bool
    restore_global_profile(bool vsync_only = false);
    void
    release_vsync();
    driver_settings_t driver_settings;
    std::filesystem::path undo_folder_path;
    std::filesystem::path undo_file_path;
    std::optional<undo_data_t> undo_data;
    std::optional<undo_file_t> undo_file;
  };

  nvprefs_interface::nvprefs_interface(): pimpl(std::make_shared<impl>()) {}
  nvprefs_interface::~nvprefs_interface() = default;

  nvprefs_interface::impl::~impl() {
    if (owning_undo_file() && load()) restore_global_profile();
    unload();
  }

  bool
  nvprefs_interface::impl::load() {
    if (!loaded) {
      // Check %ProgramData% variable, need it for storing undo file
      wchar_t program_data_env[MAX_PATH];
      auto get_env_result = GetEnvironmentVariableW(L"ProgramData", program_data_env, MAX_PATH);
      if (get_env_result == 0 || get_env_result >= MAX_PATH || !std::filesystem::is_directory(program_data_env)) {
        error_message("Missing or malformed %ProgramData% environment variable");
        return false;
      }

      // Prepare undo file path variables
      undo_folder_path = std::filesystem::path(program_data_env) / sunshine_program_data_folder;
      undo_file_path = undo_folder_path / nvprefs_undo_file_name;

      // Dynamically load nvapi library and load driver settings
      loaded = driver_settings.init();
    }

    return loaded;
  }

  void
  nvprefs_interface::impl::unload() {
    if (loaded) {
      // Unload dynamically loaded nvapi library
      driver_settings.destroy();
      loaded = false;
    }
  }

  bool
  nvprefs_interface::impl::restore_from_and_delete_undo_file_if_exists() {
    if (!loaded) return false;

    // Check for undo file from previous improper termination
    bool access_denied = false;
    if (auto undo_file = undo_file_t::open_existing_file(undo_file_path, access_denied)) {
      // Try to restore from the undo file
      info_message("Opened undo file from previous improper termination");
      if (auto undo_data = undo_file->read_undo_data()) {
        if (driver_settings.restore_global_profile_to_undo(*undo_data) && driver_settings.save_settings()) {
          info_message("Restored global profile settings from undo file - deleting the file");
        }
        else {
          error_message("Failed to restore global profile settings; retaining the undo file for retry");
          return false;
        }
      }
      else {
        error_message("Couldn't read undo file; retaining it for recovery");
        return false;
      }

      if (!undo_file->delete_file()) {
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
  nvprefs_interface::impl::modify_application_profile(const nvprefs_options &options) {
    if (!loaded || !options.manage_driver_settings) return false;

    // Modify and save sunshine.exe application profile settings, if needed
    bool modified = false;
    if (!driver_settings.check_and_modify_application_profile(modified, options.sunshine_high_power_mode)) {
      error_message("Failed to modify application profile settings");
      return false;
    }
    else if (modified) {
      if (driver_settings.save_settings()) {
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
  nvprefs_interface::impl::modify_global_profile(const nvprefs_options &options, bool force_vsync) {
    if (!loaded || !options.manage_driver_settings) return false;

    // Modify but not save global profile settings, if needed
    std::optional<undo_data_t> pending_undo;
    if (!driver_settings.check_and_modify_global_profile(pending_undo, options.opengl_vulkan_on_dxgi, force_vsync)) {
      error_message("Couldn't modify global profile settings");
      driver_settings.load_settings();
      return false;
    }
    else if (!pending_undo) {
      info_message("No need to modify global profile settings");
      return true;
    }

    auto make_undo_and_commit = [&]() -> bool {
      // Create and lock undo file if it hasn't been done yet
      if (!undo_file) {
        // Prepare Sunshine folder in ProgramData if it doesn't exist
        if (!CreateDirectoryW(undo_folder_path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
          error_message("Couldn't create undo folder");
          return false;
        }

        // Create undo file to handle improper termination of nvprefs.exe
        undo_file = undo_file_t::create_new_file(undo_file_path);
        if (!undo_file) {
          error_message("Couldn't create undo file");
          return false;
        }
      }

      assert(pending_undo);
      if (undo_data) {
        // Merge undo data if settings has been modified externally since our last modification
        undo_data->merge(*pending_undo);
      }
      else {
        undo_data = pending_undo;
      }

      // Write undo data to undo file
      if (!undo_file->write_undo_data(*undo_data)) {
        error_message("Couldn't write undo data; refusing to commit driver changes");
        return false;
      }

      // Save global profile settings
      if (!driver_settings.save_settings()) {
        error_message("Couldn't save global profile settings");
        return false;
      }

      return true;
    };

    if (!make_undo_and_commit()) {
      // Revert settings modifications
      driver_settings.load_settings();
      return false;
    }

    return true;
  }

  bool
  nvprefs_interface::impl::owning_undo_file() {
    return undo_file.has_value();
  }

  bool
  nvprefs_interface::impl::restore_global_profile(bool vsync_only) {
    if (!loaded || !undo_data || !undo_file) return false;

    undo_data_t restore_data = *undo_data;
    if (vsync_only) {
      restore_data = {};
      const auto vsync = undo_data->get_vsync();
      if (!vsync) return true;
      restore_data.set_vsync(vsync->our_value, vsync->undo_value);
    }
    // Restore global profile settings with undo data.
    if (driver_settings.restore_global_profile_to_undo(restore_data) &&
        driver_settings.save_settings()) {
      if (vsync_only) {
        auto remaining = *undo_data;
        remaining.clear_vsync();
        if (!remaining.empty()) {
          if (!undo_file->write_undo_data(remaining)) return false;
          undo_data = remaining;
          return true;
        }
      }
      // Global profile settings successfully restored, can delete undo file
      if (!undo_file->delete_file()) {
        error_message("Couldn't delete undo file");
        return false;
      }
      undo_data = std::nullopt;
      undo_file = std::nullopt;
    }
    else {
      error_message("Couldn't restore global profile settings");
      return false;
    }

    return true;
  }

  bool
  nvprefs_interface::configure() {
    std::lock_guard lock { pimpl->mutex };
    const auto options = get_nvprefs_options();
    if (!options.manage_driver_settings) {
      // Recover abandoned changes, but keep an active session's ownership until
      // its last lease ends even when management was disabled in the meantime.
      if (pimpl->vsync_users == 0) pimpl->restore(options.manage_driver_settings);
      return false;
    }
    if (!pimpl->load()) return false;
    auto unload = util::fail_guard([&]() { pimpl->unload(); });
    if (!pimpl->owning_undo_file() && !pimpl->restore_from_and_delete_undo_file_if_exists()) return false;
    const bool application_ok = pimpl->modify_application_profile(options);
    // Discard any partial application edits before an independent global save.
    if (!application_ok && !pimpl->driver_settings.load_settings()) return false;
    const bool global_ok = pimpl->modify_global_profile(options);
    return application_ok && global_ok;
  }

  bool
  nvprefs_interface::restore() {
    std::lock_guard lock { pimpl->mutex };
    return pimpl->restore(get_nvprefs_options().manage_driver_settings);
  }

  bool
  nvprefs_interface::impl::restore(bool manage_driver_settings) {
    if (!owning_undo_file() && !manage_driver_settings) {
      wchar_t program_data[MAX_PATH] {};
      const auto length = GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
      if (!length || length >= MAX_PATH) return false;
      const auto path = std::filesystem::path(program_data) / sunshine_program_data_folder / nvprefs_undo_file_name;
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
      }
    }
    if (!load()) return false;
    auto unload_guard = util::fail_guard([&]() { unload(); });
    const bool restored = owning_undo_file() ? restore_global_profile() :
                                               restore_from_and_delete_undo_file_if_exists();
    if (restored && vsync_users == 0) vsync_state = "idle";
    return restored;
  }

  std::shared_ptr<void>
  nvprefs_interface::acquire_vsync() {
    std::lock_guard lock { pimpl->mutex };
    const auto options = get_nvprefs_options();
    if (!options.manage_driver_settings || !options.vdd_force_vsync) return {};
    if (pimpl->vsync_users == 0) {
      pimpl->vsync_state = "error";
      if (!pimpl->load()) return {};
      auto unload = util::fail_guard([&]() { pimpl->unload(); });
      if ((!pimpl->owning_undo_file() && !pimpl->restore_from_and_delete_undo_file_if_exists()) ||
          !pimpl->modify_global_profile(options, true) || !pimpl->driver_settings.verify_global_vsync()) {
        error_message("Virtual-display VSync compatibility failed; continuing without the workaround");
        // SaveSettings can fail after making changes. Keep the undo record if
        // rollback cannot complete, and retry at shutdown or next startup.
        if (pimpl->owning_undo_file()) pimpl->restore_global_profile(true);
        return {};
      }
    }
    struct lease_t {
      std::shared_ptr<impl> state;
      ~lease_t() {
        if (state) state->release_vsync();
      }
    };
    auto lease = std::make_shared<lease_t>();
    ++pimpl->vsync_users;
    pimpl->vsync_state = "active";
    lease->state = pimpl;
    info_message("NVIDIA global VSync compatibility active; games must initialize their driver after this point");
    return lease;
  }

  void
  nvprefs_interface::impl::release_vsync() {
    std::lock_guard lock { mutex };
    if (--vsync_users != 0) return;
    vsync_state = "idle";
    if (!undo_data || !undo_data->get_vsync()) return;
    // Restoration is allowed even after the management option was disabled.
    if (!load()) {
      vsync_state = "error";
      return;
    }
    auto unload_guard = util::fail_guard([&]() { unload(); });
    if (!restore_global_profile(true)) {
      vsync_state = "error";
      error_message("Couldn't restore global VSync; recovery data retained");
    }
    else {
      info_message("Restored NVIDIA global VSync after the last virtual-display session");
    }
  }

  nvprefs_interface::status_t
  nvprefs_interface::status() {
    // Read hardware capability without loading or changing DRS preferences.
    bool supported = false;
    bool detection_known = false;
    IDXGIFactory1 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
      detection_known = true;
      for (UINT index = 0;; ++index) {
        IDXGIAdapter1 *adapter = nullptr;
        const auto result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
          detection_known = false;
          break;
        }
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc)))
          detection_known = false;
        else if (desc.VendorId == 0x10DE) {
          supported = true;
          detection_known = true;
        }
        adapter->Release();
        if (supported) break;
      }
      factory->Release();
    }
    std::lock_guard lock { pimpl->mutex };
    return { supported, pimpl->vsync_state, detection_known };
  }

}  // namespace nvprefs
