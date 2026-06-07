/**
 * @file src/platform/windows/nvprefs/driver_settings.cpp
 * @brief Definitions for nvidia driver settings.
 */
// standard library headers
#include <string>

// local includes
#include "driver_settings.h"
#include "nvprefs_common.h"

namespace {

  const auto sunshine_application_profile_name = L"SunshineStream";
  const auto sunshine_application_path = L"sunshine.exe";
  const auto sunshine_game_profile_name = L"SunshineStreamGame";

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

  std::string
  wide_to_utf8(const std::wstring &value) {
    if (value.empty()) {
      return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
      return {};
    }

    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
  }

  std::wstring
  utf8_to_wide(const std::string &value) {
    if (value.empty()) {
      return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
      return {};
    }

    std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
  }

  NvU32
  compute_frl_fps(int client_fps, const nvprefs::nvprefs_options &options) {
    if (options.nv_frl_fps_override > 0) {
      return static_cast<NvU32>(options.nv_frl_fps_override);
    }

    int value = client_fps + options.nv_frl_fps_offset;
    if (value < 1) {
      value = 1;
    }

    return static_cast<NvU32>(value);
  }

  struct desired_settings_t {
    std::optional<NvU32> vsync;
    std::optional<NvU32> frl;
    std::optional<NvU32> pstate;
    std::optional<NvU32> prerender;
  };

  desired_settings_t
  compute_desired(const nvprefs::nvprefs_options &options, int client_fps) {
    desired_settings_t desired;
    if (options.nv_force_vsync) {
      desired.vsync = VSYNCMODE_FORCEON;
    }
    if (options.nv_lock_frame_rate) {
      desired.frl = compute_frl_fps(client_fps, options);
    }
    if (options.nv_prefer_max_performance) {
      desired.pstate = PREFERRED_PSTATE_PREFER_MAX;
    }
    if (options.nv_low_latency_mode) {
      desired.prerender = 1;
    }
    return desired;
  }

  bool
  apply_uint_setting(NvDRSSessionHandle session,
      NvDRSProfileHandle profile,
      NvU32 setting_id,
      NvU32 desired_value,
      std::optional<nvprefs::undo_data_t::data_t::setting_undo_t> &undo_out,
      const std::wstring &log_label) {
    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER1;
    NvAPI_Status status = NvAPI_DRS_GetSetting(session, profile, setting_id, &setting);

    std::optional<uint32_t> previous;
    bool already_at_desired = false;
    if (status == NVAPI_OK) {
      if (setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
        previous = setting.u32CurrentValue;
        already_at_desired = setting.u32CurrentValue == desired_value;
      }
    }
    else if (status != NVAPI_SETTING_NOT_FOUND) {
      nvapi_error_message(status);
      nvprefs::error_message(std::wstring(L"NvAPI_DRS_GetSetting() failed for ") + log_label);
      return false;
    }

    if (already_at_desired) {
      return true;
    }

    setting = {};
    setting.version = NVDRS_SETTING_VER1;
    setting.settingId = setting_id;
    setting.settingType = NVDRS_DWORD_TYPE;
    setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
    setting.u32CurrentValue = desired_value;
    status = NvAPI_DRS_SetSetting(session, profile, &setting);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      nvprefs::error_message(std::wstring(L"NvAPI_DRS_SetSetting() failed for ") + log_label);
      return false;
    }

    undo_out = nvprefs::undo_data_t::data_t::setting_undo_t { desired_value, previous };
    nvprefs::info_message(std::wstring(L"Set ") + log_label + L" on NVIDIA profile");
    return true;
  }

  bool
  restore_uint_setting(NvDRSSessionHandle session,
      NvDRSProfileHandle profile,
      NvU32 setting_id,
      const nvprefs::undo_data_t::data_t::setting_undo_t &undo,
      const std::wstring &log_label) {
    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER1;
    NvAPI_Status status = NvAPI_DRS_GetSetting(session, profile, setting_id, &setting);
    if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
      nvapi_error_message(status);
      nvprefs::error_message(std::wstring(L"NvAPI_DRS_GetSetting() failed while restoring ") + log_label);
      return false;
    }

    if (status == NVAPI_SETTING_NOT_FOUND) {
      nvprefs::info_message(std::wstring(log_label) + L" is no longer present, skipping restore");
      return true;
    }

    const bool ours = setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.u32CurrentValue == undo.our_value;
    if (!ours) {
      nvprefs::info_message(std::wstring(log_label) + L" was changed externally, skipping restore");
      return true;
    }

    if (undo.undo_value) {
      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = setting_id;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = *undo.undo_value;
      status = NvAPI_DRS_SetSetting(session, profile, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        nvprefs::error_message(std::wstring(L"NvAPI_DRS_SetSetting() failed while restoring ") + log_label);
        return false;
      }
    }
    else {
      status = NvAPI_DRS_DeleteProfileSetting(session, profile, setting_id);
      if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
        nvapi_error_message(status);
        nvprefs::error_message(std::wstring(L"NvAPI_DRS_DeleteProfileSetting() failed while restoring ") + log_label);
        return false;
      }
    }

    nvprefs::info_message(std::wstring(L"Restored ") + log_label);
    return true;
  }

  bool
  apply_desired_to_profile(NvDRSSessionHandle session,
      NvDRSProfileHandle profile,
      const desired_settings_t &desired,
      std::optional<nvprefs::undo_data_t::data_t::setting_undo_t> &vsync_undo,
      std::optional<nvprefs::undo_data_t::data_t::setting_undo_t> &frl_undo,
      std::optional<nvprefs::undo_data_t::data_t::setting_undo_t> &pstate_undo,
      std::optional<nvprefs::undo_data_t::data_t::setting_undo_t> &prerender_undo) {
    if (desired.vsync && !apply_uint_setting(session, profile, VSYNCMODE_ID, *desired.vsync, vsync_undo, L"VSYNCMODE")) {
      return false;
    }
    if (desired.frl && !apply_uint_setting(session, profile, FRL_FPS_ID, *desired.frl, frl_undo, L"FRL_FPS")) {
      return false;
    }
    if (desired.pstate && !apply_uint_setting(session, profile, PREFERRED_PSTATE_ID, *desired.pstate, pstate_undo, L"PREFERRED_PSTATE")) {
      return false;
    }
    if (desired.prerender && !apply_uint_setting(session, profile, PRERENDERLIMIT_ID, *desired.prerender, prerender_undo, L"PRERENDERLIMIT")) {
      return false;
    }

    return true;
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

    const auto &swapchain_data = undo_data.get_opengl_swapchain();
    if (swapchain_data) {
      NvAPI_Status status;

      NvDRSProfileHandle profile_handle = 0;
      status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetBaseProfile() failed");
        return false;
      }

      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER;
      status = NvAPI_DRS_GetSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID, &setting);

      if (status == NVAPI_OK && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.u32CurrentValue == swapchain_data->our_value) {
        if (swapchain_data->undo_value) {
          setting = {};
          setting.version = NVDRS_SETTING_VER1;
          setting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
          setting.settingType = NVDRS_DWORD_TYPE;
          setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
          setting.u32CurrentValue = *swapchain_data->undo_value;

          status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);

          if (status != NVAPI_OK) {
            nvapi_error_message(status);
            error_message("NvAPI_DRS_SetSetting() OGL_CPL_PREFER_DXPRESENT failed");
            return false;
          }
        }
        else {
          status = NvAPI_DRS_DeleteProfileSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID);

          if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
            nvapi_error_message(status);
            error_message("NvAPI_DRS_DeleteProfileSetting() OGL_CPL_PREFER_DXPRESENT failed");
            return false;
          }
        }

        info_message("Restored OGL_CPL_PREFER_DXPRESENT for base profile");
      }
      else if (status == NVAPI_OK || status == NVAPI_SETTING_NOT_FOUND) {
        info_message("OGL_CPL_PREFER_DXPRESENT has been changed from our value in base profile, not restoring");
      }
      else {
        error_message("NvAPI_DRS_GetSetting() OGL_CPL_PREFER_DXPRESENT failed");
        return false;
      }
    }

    return true;
  }

  bool
  driver_settings_t::check_and_modify_global_profile(std::optional<undo_data_t> &undo_data) {
    if (!session_handle) return false;

    undo_data.reset();
    NvAPI_Status status;

    if (!get_nvprefs_options().opengl_vulkan_on_dxgi) {
      // User requested to leave OpenGL/Vulkan DXGI swapchain setting alone
      return true;
    }

    NvDRSProfileHandle profile_handle = 0;
    status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetBaseProfile() failed");
      return false;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, OGL_CPL_PREFER_DXPRESENT_ID, &setting);

    // Remember current OpenGL/Vulkan DXGI swapchain setting and change it if needed
    if (status == NVAPI_SETTING_NOT_FOUND || (status == NVAPI_OK && setting.u32CurrentValue != OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED)) {
      undo_data = undo_data_t();
      if (status == NVAPI_OK) {
        undo_data->set_opengl_swapchain(OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, setting.u32CurrentValue);
      }
      else {
        undo_data->set_opengl_swapchain(OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED, std::nullopt);
      }

      setting = {};
      setting.version = NVDRS_SETTING_VER1;
      setting.settingId = OGL_CPL_PREFER_DXPRESENT_ID;
      setting.settingType = NVDRS_DWORD_TYPE;
      setting.settingLocation = NVDRS_CURRENT_PROFILE_LOCATION;
      setting.u32CurrentValue = OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED;

      status = NvAPI_DRS_SetSetting(session_handle, profile_handle, &setting);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_SetSetting() OGL_CPL_PREFER_DXPRESENT failed");
        return false;
      }

      info_message("Changed OGL_CPL_PREFER_DXPRESENT to OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED for base profile");
    }
    else if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetSetting() OGL_CPL_PREFER_DXPRESENT failed");
      return false;
    }

    return true;
  }

  bool
  driver_settings_t::check_and_modify_application_profile(bool &modified) {
    if (!session_handle) return false;

    modified = false;
    NvAPI_Status status;

    NvAPI_UnicodeString profile_name = {};
    fill_nvapi_string(profile_name, sunshine_application_profile_name);

    NvDRSProfileHandle profile_handle = 0;
    status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);

    if (status == NVAPI_PROFILE_NOT_FOUND) {
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
    else if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_FindProfileByName() failed");
      return false;
    }

    NvAPI_UnicodeString sunshine_path = {};
    fill_nvapi_string(sunshine_path, sunshine_application_path);

    NVDRS_APPLICATION application = {};
    application.version = NVDRS_APPLICATION_VER_V1;
    status = NvAPI_DRS_GetApplicationInfo(session_handle, profile_handle, sunshine_path, &application);

    if (status == NVAPI_EXECUTABLE_NOT_FOUND) {
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
    else if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetApplicationInfo() failed");
      return false;
    }

    NVDRS_SETTING setting = {};
    setting.version = NVDRS_SETTING_VER1;
    status = NvAPI_DRS_GetSetting(session_handle, profile_handle, PREFERRED_PSTATE_ID, &setting);

    if (!get_nvprefs_options().sunshine_high_power_mode) {
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
      else if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetSetting() PREFERRED_PSTATE failed");
        return false;
      }
    }
    else if (status == NVAPI_SETTING_NOT_FOUND ||
             (status == NVAPI_OK &&
                 (setting.settingLocation != NVDRS_CURRENT_PROFILE_LOCATION ||
                     setting.u32CurrentValue != PREFERRED_PSTATE_PREFER_MAX))) {
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
    else if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetSetting() PREFERRED_PSTATE failed");
      return false;
    }

    return true;
  }

  bool
  driver_settings_t::check_and_modify_game_profile(const std::wstring &exe_name, int client_fps, std::optional<undo_data_t::data_t::game_profile_t> &undo_out) {
    undo_out.reset();
    if (!session_handle) return false;

    const auto options = get_nvprefs_options();
    if (!options.nv_optimize_game || exe_name.empty()) {
      return true;
    }

    const desired_settings_t desired = compute_desired(options, client_fps);
    if (!desired.vsync && !desired.frl && !desired.pstate && !desired.prerender) {
      return true;
    }

    NvAPI_UnicodeString app_name = {};
    fill_nvapi_string(app_name, exe_name.c_str());

    NvDRSProfileHandle profile_handle = 0;
    bool profile_was_created = false;
    bool application_was_added = false;
    std::wstring profile_name_used;

    NVDRS_APPLICATION app_info = {};
    app_info.version = NVDRS_APPLICATION_VER;
    NvAPI_Status status = NvAPI_DRS_FindApplicationByName(session_handle, app_name, &profile_handle, &app_info);
    if (status == NVAPI_OK) {
      NVDRS_PROFILE existing = {};
      existing.version = NVDRS_PROFILE_VER;
      status = NvAPI_DRS_GetProfileInfo(session_handle, profile_handle, &existing);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message(std::wstring(L"NvAPI_DRS_GetProfileInfo() failed for ") + exe_name);
        return false;
      }
      profile_name_used.assign(reinterpret_cast<const wchar_t *>(existing.profileName));
    }
    else if (status == NVAPI_EXECUTABLE_NOT_FOUND) {
      NvAPI_UnicodeString profile_name = {};
      fill_nvapi_string(profile_name, sunshine_game_profile_name);
      status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);
      if (status == NVAPI_PROFILE_NOT_FOUND) {
        NVDRS_PROFILE profile = {};
        profile.version = NVDRS_PROFILE_VER1;
        fill_nvapi_string(profile.profileName, sunshine_game_profile_name);
        status = NvAPI_DRS_CreateProfile(session_handle, &profile, &profile_handle);
        if (status != NVAPI_OK) {
          nvapi_error_message(status);
          error_message("NvAPI_DRS_CreateProfile() SunshineStreamGame failed");
          return false;
        }
        profile_was_created = true;
      }
      else if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_FindProfileByName() SunshineStreamGame failed");
        return false;
      }

      profile_name_used = sunshine_game_profile_name;

      NVDRS_APPLICATION application = {};
      application.version = NVDRS_APPLICATION_VER_V1;
      status = NvAPI_DRS_GetApplicationInfo(session_handle, profile_handle, app_name, &application);
      if (status == NVAPI_EXECUTABLE_NOT_FOUND) {
        application = {};
        application.version = NVDRS_APPLICATION_VER_V1;
        application.isPredefined = 0;
        fill_nvapi_string(application.appName, exe_name.c_str());
        fill_nvapi_string(application.userFriendlyName, exe_name.c_str());
        fill_nvapi_string(application.launcher, L"");

        status = NvAPI_DRS_CreateApplication(session_handle, profile_handle, &application);
        if (status != NVAPI_OK) {
          nvapi_error_message(status);
          error_message(std::wstring(L"NvAPI_DRS_CreateApplication() failed for ") + exe_name);
          return false;
        }
        application_was_added = true;
      }
      else if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message(std::wstring(L"NvAPI_DRS_GetApplicationInfo() failed for ") + exe_name);
        return false;
      }
    }
    else {
      nvapi_error_message(status);
      error_message(std::wstring(L"NvAPI_DRS_FindApplicationByName() failed for ") + exe_name);
      return false;
    }

    undo_data_t::data_t::game_profile_t pending;
    pending.profile_name = wide_to_utf8(profile_name_used);
    pending.exe_path = wide_to_utf8(exe_name);
    pending.profile_was_created = profile_was_created;
    pending.application_was_added = application_was_added;

    if (!apply_desired_to_profile(session_handle, profile_handle, desired, pending.vsync, pending.frl, pending.pstate, pending.prerender)) {
      if (pending.profile_was_created || pending.application_was_added || pending.vsync || pending.frl || pending.pstate || pending.prerender) {
        undo_out = pending;
      }
      return false;
    }

    if (pending.profile_was_created || pending.application_was_added || pending.vsync || pending.frl || pending.pstate || pending.prerender) {
      undo_out = pending;
    }
    info_message(std::wstring(L"Applied stream NVIDIA profile optimizations for ") + exe_name);
    return true;
  }

  bool
  driver_settings_t::restore_game_profile_to_undo(const undo_data_t::data_t::game_profile_t &undo_data) {
    if (!session_handle) return false;

    const std::wstring exe_name = utf8_to_wide(undo_data.exe_path);
    if (exe_name.empty()) {
      info_message("game_profile undo entry missing exe_path, skipping");
      return true;
    }

    NvAPI_UnicodeString app_name = {};
    fill_nvapi_string(app_name, exe_name.c_str());

    NvDRSProfileHandle profile_handle = 0;
    NVDRS_APPLICATION app_info = {};
    app_info.version = NVDRS_APPLICATION_VER;
    NvAPI_Status status = NvAPI_DRS_FindApplicationByName(session_handle, app_name, &profile_handle, &app_info);
    if (status == NVAPI_EXECUTABLE_NOT_FOUND) {
      const std::wstring saved_profile_name = utf8_to_wide(undo_data.profile_name);
      if (saved_profile_name.empty()) {
        info_message(std::wstring(L"No saved NVIDIA profile name for ") + exe_name + L" during restore, skipping");
        return true;
      }
      NvAPI_UnicodeString profile_name = {};
      fill_nvapi_string(profile_name, saved_profile_name.c_str());
      status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);
      if (status == NVAPI_PROFILE_NOT_FOUND) {
        info_message(std::wstring(L"No NVIDIA profile found for ") + exe_name + L" during restore, skipping");
        return true;
      }
    }

    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message(std::wstring(L"NvAPI_DRS_FindApplicationByName() failed while restoring ") + exe_name);
      return false;
    }

    bool ok = true;
    if (undo_data.vsync && !restore_uint_setting(session_handle, profile_handle, VSYNCMODE_ID, *undo_data.vsync, L"VSYNCMODE")) {
      ok = false;
    }
    if (undo_data.frl && !restore_uint_setting(session_handle, profile_handle, FRL_FPS_ID, *undo_data.frl, L"FRL_FPS")) {
      ok = false;
    }
    if (undo_data.pstate && !restore_uint_setting(session_handle, profile_handle, PREFERRED_PSTATE_ID, *undo_data.pstate, L"PREFERRED_PSTATE")) {
      ok = false;
    }
    if (undo_data.prerender && !restore_uint_setting(session_handle, profile_handle, PRERENDERLIMIT_ID, *undo_data.prerender, L"PRERENDERLIMIT")) {
      ok = false;
    }

    const std::wstring cleanup_profile_name = utf8_to_wide(undo_data.profile_name);
    NvDRSProfileHandle cleanup_profile_handle = 0;
    bool cleanup_profile_found = false;
    if (!cleanup_profile_name.empty()) {
      NvAPI_UnicodeString profile_name = {};
      fill_nvapi_string(profile_name, cleanup_profile_name.c_str());
      status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &cleanup_profile_handle);
      if (status == NVAPI_OK) {
        cleanup_profile_found = true;
      }
      else if (status == NVAPI_PROFILE_NOT_FOUND) {
        info_message("Original game profile not found during cleanup");
      }
      else {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_FindProfileByName() failed during game profile cleanup");
        ok = false;
      }
    }

    if (undo_data.application_was_added && cleanup_profile_found) {
      status = NvAPI_DRS_DeleteApplication(session_handle, cleanup_profile_handle, app_name);
      if (status != NVAPI_OK && status != NVAPI_EXECUTABLE_NOT_FOUND) {
        nvapi_error_message(status);
        error_message(std::wstring(L"NvAPI_DRS_DeleteApplication() failed for ") + exe_name);
        ok = false;
      }
    }

    if (undo_data.profile_was_created && cleanup_profile_found) {
      NVDRS_PROFILE info = {};
      info.version = NVDRS_PROFILE_VER;
      status = NvAPI_DRS_GetProfileInfo(session_handle, cleanup_profile_handle, &info);
      if (status != NVAPI_OK) {
        nvapi_error_message(status);
        error_message("NvAPI_DRS_GetProfileInfo() SunshineStreamGame failed");
        ok = false;
      }
      else if (info.numOfApps == 0) {
        status = NvAPI_DRS_DeleteProfile(session_handle, cleanup_profile_handle);
        if (status != NVAPI_OK) {
          nvapi_error_message(status);
          error_message("NvAPI_DRS_DeleteProfile() SunshineStreamGame failed");
          ok = false;
        }
      }
    }

    return ok;
  }

  bool
  driver_settings_t::check_and_modify_base_extras(int client_fps, std::optional<undo_data_t::data_t::base_extras_t> &undo_out) {
    undo_out.reset();
    if (!session_handle) return false;

    const auto options = get_nvprefs_options();
    if (!options.nv_optimize_game || !options.nv_apply_to_base_profile) {
      return true;
    }

    const desired_settings_t desired = compute_desired(options, client_fps);
    if (!desired.vsync && !desired.frl && !desired.pstate && !desired.prerender) {
      return true;
    }

    NvDRSProfileHandle profile_handle = 0;
    NvAPI_Status status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetBaseProfile() failed for base extras");
      return false;
    }

    undo_data_t::data_t::base_extras_t pending;
    if (!apply_desired_to_profile(session_handle, profile_handle, desired, pending.vsync, pending.frl, pending.pstate, pending.prerender)) {
      if (pending.vsync || pending.frl || pending.pstate || pending.prerender) {
        undo_out = pending;
      }
      return false;
    }

    if (pending.vsync || pending.frl || pending.pstate || pending.prerender) {
      undo_out = pending;
    }
    info_message("Applied stream optimizations to BASE NVIDIA profile");
    return true;
  }

  bool
  driver_settings_t::restore_base_extras_to_undo(const undo_data_t::data_t::base_extras_t &undo_data) {
    if (!session_handle) return false;

    NvDRSProfileHandle profile_handle = 0;
    NvAPI_Status status = NvAPI_DRS_GetBaseProfile(session_handle, &profile_handle);
    if (status != NVAPI_OK) {
      nvapi_error_message(status);
      error_message("NvAPI_DRS_GetBaseProfile() failed for base extras restore");
      return false;
    }

    bool ok = true;
    if (undo_data.vsync && !restore_uint_setting(session_handle, profile_handle, VSYNCMODE_ID, *undo_data.vsync, L"VSYNCMODE (base)")) {
      ok = false;
    }
    if (undo_data.frl && !restore_uint_setting(session_handle, profile_handle, FRL_FPS_ID, *undo_data.frl, L"FRL_FPS (base)")) {
      ok = false;
    }
    if (undo_data.pstate && !restore_uint_setting(session_handle, profile_handle, PREFERRED_PSTATE_ID, *undo_data.pstate, L"PREFERRED_PSTATE (base)")) {
      ok = false;
    }
    if (undo_data.prerender && !restore_uint_setting(session_handle, profile_handle, PRERENDERLIMIT_ID, *undo_data.prerender, L"PRERENDERLIMIT (base)")) {
      ok = false;
    }

    return ok;
  }

}  // namespace nvprefs
