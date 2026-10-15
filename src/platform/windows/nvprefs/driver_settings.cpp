/**
 * @file src/platform/windows/nvprefs/driver_settings.cpp
 * @brief Definitions for nvidia driver settings.
 */
// local includes
#include "driver_settings.h"
#include "nvprefs_common.h"

namespace {

  const auto sunshine_application_profile_name = L"SunshineStream";
  const auto sunshine_application_path = L"sunshine.exe";

  void
  nvapi_error_message(NvAPI_Status status) {
    NvAPI_ShortString message = {};
    NvAPI_GetErrorMessage(status, message);
    nvprefs::error_message(std::string("NvAPI error: ") + message);
  }

  void
  fill_nvapi_string(NvAPI_UnicodeString &dest, const wchar_t *src) {
    static_assert(sizeof(NvU16) == sizeof(wchar_t));
    memcpy_s(dest, NVAPI_UNICODE_STRING_MAX * sizeof(NvU16), src, (wcslen(src) + 1) * sizeof(wchar_t));
  }

}  // namespace

namespace nvprefs {

  driver_settings_t::~driver_settings_t() {
    if (session_handle) {
      NvAPI_DRS_DestroySession(session_handle);
    }
  }

  bool
  driver_settings_t::init() {
    if (session_handle) return true;

    NvAPI_Status status;

    status = NvAPI_Initialize();
    if (status != NVAPI_OK) {
      info_message("NvAPI_Initialize() failed, ignore if you don't have NVIDIA video card");
      return false;
    }

    status = NvAPI_DRS_CreateSession(&session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_CreateSession() failed");
      destroy();
      return false;
    }

    return load_settings();
  }

  void
  driver_settings_t::destroy() {
    if (session_handle) {
      NvAPI_DRS_DestroySession(session_handle);
      session_handle = 0;
    }
    NvAPI_Unload();
  }

  bool
  driver_settings_t::load_settings() {
    if (!session_handle) return false;

    NvAPI_Status status = NvAPI_DRS_LoadSettings(session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_LoadSettings() failed");
      destroy();
      return false;
    }

    return true;
  }

  bool
  driver_settings_t::save_settings() {
    if (!session_handle) return false;

    NvAPI_Status status = NvAPI_DRS_SaveSettings(session_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_SaveSettings() failed");
      return false;
    }

    return true;
  }

  bool
  driver_settings_t::restore_global_profile_to_undo(const undo_data_t &undo_data) {
    if (!session_handle) return false;

    // Restore only entries still carrying our explicit user override. Deleting an
    // override restores the driver's inherited/predefined value.
    const auto restore_setting = [&](NvU32 id, const auto &data) {
      if (!data) return true;
      NvDRSProfileHandle profile = 0;
      auto status = NvAPI_DRS_GetBaseProfile(session_handle, &profile);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        return false;
      }
      NVDRS_SETTING setting {};
      setting.version = NVDRS_SETTING_VER;
      status = NvAPI_DRS_GetSetting(session_handle, profile, id, &setting);
      if (status == NVAPI_SETTING_NOT_FOUND) return true;
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        return false;
      }
      if (setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION ||
          setting.isCurrentPredefined || setting.u32CurrentValue != data->our_value) {
        info_message("Global driver setting changed externally; preserving the current value");
        return true;
      }
      if (data->undo_value) {
        setting = {};
        setting.version = NVDRS_SETTING_VER;
        setting.settingId = id;
        setting.settingType = NVDRS_DWORD_TYPE;
        setting.u32CurrentValue = *data->undo_value;
        status = NvAPI_DRS_SetSetting(session_handle, profile, &setting);
      }
      else {
        status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile, id);
      }
      if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
        nvapi_error_message(status);
        return false;
      }
      return true;
    };
    return restore_setting(OGL_CPL_PREFER_DXPRESENT_ID, undo_data.get_opengl_swapchain()) &&
           restore_setting(VSYNCMODE_ID, undo_data.get_vsync());
  }

  bool
  driver_settings_t::check_and_modify_global_profile(std::optional<undo_data_t> &undo_data, bool opengl_vulkan_on_dxgi, bool force_vsync) {
    if (!session_handle) return false;
    undo_data.reset();
    if (!opengl_vulkan_on_dxgi && !force_vsync) return true;

    NvDRSProfileHandle profile = 0;
    auto status = NvAPI_DRS_GetBaseProfile(session_handle, &profile);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      return false;
    }
    const auto apply_setting = [&](NvU32 id, NvU32 value, bool vsync) {
      NVDRS_SETTING setting {};
      setting.version = NVDRS_SETTING_VER;
      auto result = NvAPI_DRS_GetSetting(session_handle, profile, id, &setting);
      if (result != NVAPI_OK && result != NVAPI_SETTING_NOT_FOUND) {
        nvapi_error_message(result);
        return false;
      }
      if (result == NVAPI_OK && setting.u32CurrentValue == value) return true;
      const std::optional<uint32_t> previous =
        result == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && !setting.isCurrentPredefined ?
          std::optional<uint32_t> { setting.u32CurrentValue } :
          std::nullopt;
      if (!undo_data) undo_data.emplace();
      if (vsync)
        undo_data->set_vsync(value, previous);
      else
        undo_data->set_opengl_swapchain(value, previous);
      setting = {};
      setting.version = NVDRS_SETTING_VER;
      setting.settingId = id;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.u32CurrentValue = value;
      result = NvAPI_DRS_SetSetting(session_handle, profile, &setting);
      if (result != NVAPI_OK) {
        nvapi_error_message(result);
        return false;
      }
      return true;
    };
    if (opengl_vulkan_on_dxgi &&
        !apply_setting(OGL_CPL_PREFER_DXPRESENT_ID, OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, false)) return false;
    return !force_vsync || apply_setting(VSYNCMODE_ID, VSYNCMODE_FORCEON, true);
  }

  bool
  driver_settings_t::verify_global_vsync() {
    // Re-read the persisted DRS database, not the staged session settings.
    if (!load_settings()) return false;
    NvDRSProfileHandle profile = 0;
    if (NvAPI_DRS_GetBaseProfile(session_handle, &profile) != NVAPI_OK) return false;
    NVDRS_SETTING setting {};
    setting.version = NVDRS_SETTING_VER;
    return NvAPI_DRS_GetSetting(session_handle, profile, VSYNCMODE_ID, &setting) == NVAPI_OK &&
           setting.u32CurrentValue == VSYNCMODE_FORCEON;
  }

  bool
  driver_settings_t::check_and_modify_application_profile(bool &modified, bool sunshine_high_power_mode) {
    if (!session_handle) return false;

    modified = false;
    NvAPI_Status status;

    NvAPI_UnicodeString profile_name = {};
    fill_nvapi_string(profile_name, sunshine_application_profile_name);

    NvDRSProfileHandle profile_handle = 0;
    status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);

    if (status != NVAPI_OK) {
      // Create application profile if missing
      NVDRS_PROFILE profile = {};
      profile.version = NVDRS_PROFILE_VER1;
      fill_nvapi_string(profile.profileName, sunshine_application_profile_name);
      status = NvAPI_DRS_CreateProfile(session_handle, &profile, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_CreateProfile() failed");
        return false;
      }
      modified = true;
    }

    NvAPI_UnicodeString sunshine_path = {};
    fill_nvapi_string(sunshine_path, sunshine_application_path);

    NVDRS_APPLICATION application = {};
    application.version = NVDRS_APPLICATION_VER_V1;
    status = NvAPI_DRS_GetApplicationInfo(session_handle, profile_handle, sunshine_path, &application);

    if (status != NVAPI_OK) {
      // Add application to application profile if missing
      application.version = NVDRS_APPLICATION_VER_V1;
      application.isPredefined = 0;
      fill_nvapi_string(application.appName, sunshine_application_path);
      fill_nvapi_string(application.userFriendlyName, sunshine_application_path);
      fill_nvapi_string(application.launcher, L"");

      status = NvAPI_DRS_CreateApplication(session_handle, profile_handle, &application);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_CreateApplication() failed");
        return false;
      }
      modified = true;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER1;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID, &setting);

    if (!sunshine_high_power_mode) {
      if (status == NVAPI_OK &&
          setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
        // User requested to not use high power mode for sunshine.exe,
        // remove the setting from application profile if it's been set previously

        status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID);
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
          nvapi_error_message(status);
          error_message("NvAPI_DRS_DeleteProfileSetting() PREFERRED_PSTATE failed");
          return false;
        }
        modified = true;

        info_message(std::wstring(L"Removed PREFERRED_PSTATE for ") + sunshine_application_path);
      }
    }
    else if (status != NVAPI_OK ||
             setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION ||
             setting.u32CurrentValue != PREFERRED_PSTATE_PREFER_MAX) {
      // Set power setting if needed
      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = PREFERRED_PSTATE_ID;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = PREFERRED_PSTATE_PREFER_MAX;

      status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_SetSetting() PREFERRED_PSTATE failed");
        return false;
      }
      modified = true;

      info_message(std::wstring(L"Changed PREFERRED_PSTATE to PREFERRED_PSTATE_PREFER_MAX for ") + sunshine_application_path);
    }

    return true;
  }

}  // namespace nvprefs
