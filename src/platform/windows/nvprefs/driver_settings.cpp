/**
 * @file src/platform/windows/nvprefs/driver_settings.cpp
 * @brief Definitions for nvidia driver settings.
 */
// local includes
#include "driver_settings.h"
#include "nvprefs_common.h"
#include "../misc.h"

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

  // -----------------------------------------------------------------
  // Stream-time game optimizations (per-game application profile +
  // optional BASE profile mirror). See driver_settings.h for rationale.
  // -----------------------------------------------------------------
  namespace {

    constexpr auto sunshine_game_profile_name = L"SunshineStreamGame";

    // Compute target FRL (frame rate limiter) value for the current client.
    NvU32
    compute_frl_fps(int client_fps, const nvprefs_options &opts) {
      if (opts.nv_frl_fps_override > 0) {
        return static_cast<NvU32>(opts.nv_frl_fps_override);
      }
      int v = client_fps + opts.nv_frl_fps_offset;
      if (v < 1) v = 1;
      return static_cast<NvU32>(v);
    }

    struct desired_settings_t {
      std::optional<NvU32> vsync;
      std::optional<NvU32> frl;
      std::optional<NvU32> pstate;
      std::optional<NvU32> prerender;
    };

    desired_settings_t
    compute_desired(const nvprefs_options &opts, int client_fps) {
      desired_settings_t d;
      if (opts.nv_force_vsync) d.vsync = VSYNCMODE_FORCEON;
      if (opts.nv_lock_frame_rate) d.frl = compute_frl_fps(client_fps, opts);
      if (opts.nv_prefer_max_performance) d.pstate = PREFERRED_PSTATE_PREFER_MAX;
      // PRERENDERLIMIT == 1 matches NVIDIA Control Panel "Low Latency Mode = On".
      // Value 0 means "use the 3D application setting" so leaving it untouched
      // when the option is off is equivalent to default driver behavior.
      if (opts.nv_low_latency_mode) d.prerender = 1;
      return d;
    }

    // Apply a single uint32 setting to a profile. Records the previous value
    // (or std::nullopt if the setting wasn't set on the profile) into undo_out
    // ONLY when we actually change the value. No-op when current == desired.
    bool
    apply_uint_setting(NvDRSSessionHandle session,
        NvDRSProfileHandle profile,
        NvU32 setting_id,
        NvU32 desired_value,
        std::optional<undo_data_t::data_t::setting_undo_t> &undo_out,
        const std::wstring &log_label) {
      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER1;
      NvAPI_Status status = NvAPI_DRS_GetSetting(session, profile, setting_id, &setting);

      std::optional<uint32_t> previous;
      bool already_at_desired = false;
      if (status == NVAPI_OK) {
        if (setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
          previous = setting.u32CurrentValue;
        }
        if (setting.u32CurrentValue == desired_value && setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION) {
          already_at_desired = true;
        }
      }
      else if (status != NVAPI_SETTING_NOT_FOUND) {
        nvapi_error_message(status);
        error_message(std::wstring(L"NvAPI_DRS_GetSetting() failed for ") + log_label);
        return false;
      }

      if (already_at_desired) {
        // No change to make and nothing to undo.
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
        error_message(std::wstring(L"NvAPI_DRS_SetSetting() failed for ") + log_label);
        return false;
      }

      undo_out = undo_data_t::data_t::setting_undo_t { desired_value, previous };
      info_message(std::wstring(L"Set ") + log_label + L" on profile");
      return true;
    }

    // Restore a single uint32 setting using saved undo data.
    // Skips silently when the user has manually changed the value since we
    // wrote it (the original value would be lost anyway).
    bool
    restore_uint_setting(NvDRSSessionHandle session,
        NvDRSProfileHandle profile,
        NvU32 setting_id,
        const undo_data_t::data_t::setting_undo_t &undo,
        const std::wstring &log_label) {
      NVDRS_SETTING setting = {};
      setting.version = NVDRS_SETTING_VER1;
      NvAPI_Status status = NvAPI_DRS_GetSetting(session, profile, setting_id, &setting);
      if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
        nvapi_error_message(status);
        error_message(std::wstring(L"NvAPI_DRS_GetSetting() failed while restoring ") + log_label);
        return false;
      }
      if (status == NVAPI_OK) {
        const bool ours = setting.settingLocation == NVDRS_CURRENT_PROFILE_LOCATION && setting.u32CurrentValue == undo.our_value;
        if (!ours) {
          info_message(std::wstring(log_label) + L" was changed externally, skipping restore");
          return true;
        }
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
          error_message(std::wstring(L"NvAPI_DRS_SetSetting() failed while restoring ") + log_label);
          return false;
        }
      }
      else {
        status = NvAPI_DRS_DeleteProfileSetting(session, profile, setting_id);
        if (status != NVAPI_OK && status != NVAPI_SETTING_NOT_FOUND) {
          nvapi_error_message(status);
          error_message(std::wstring(L"NvAPI_DRS_DeleteProfileSetting() failed while restoring ") + log_label);
          return false;
        }
      }
      info_message(std::wstring(L"Restored ") + log_label);
      return true;
    }

    // Apply the four configurable settings to the given profile. Returns false
    // on a hard NvAPI failure (caller should bail without saving).
    bool
    apply_desired_to_profile(NvDRSSessionHandle session,
        NvDRSProfileHandle profile,
        const desired_settings_t &d,
        std::optional<undo_data_t::data_t::setting_undo_t> &vsync_undo,
        std::optional<undo_data_t::data_t::setting_undo_t> &frl_undo,
        std::optional<undo_data_t::data_t::setting_undo_t> &pstate_undo,
        std::optional<undo_data_t::data_t::setting_undo_t> &prerender_undo) {
      if (d.vsync && !apply_uint_setting(session, profile, VSYNCMODE_ID, *d.vsync, vsync_undo, L"VSYNCMODE")) {
        return false;
      }
      if (d.frl && !apply_uint_setting(session, profile, FRL_FPS_ID, *d.frl, frl_undo, L"FRL_FPS")) {
        return false;
      }
      if (d.pstate && !apply_uint_setting(session, profile, PREFERRED_PSTATE_ID, *d.pstate, pstate_undo, L"PREFERRED_PSTATE")) {
        return false;
      }
      if (d.prerender && !apply_uint_setting(session, profile, PRERENDERLIMIT_ID, *d.prerender, prerender_undo, L"PRERENDERLIMIT")) {
        return false;
      }
      return true;
    }

  }  // namespace

  bool
  driver_settings_t::check_and_modify_game_profile(const std::wstring &exe_name, int client_fps, std::optional<undo_data_t::data_t::game_profile_t> &undo_out) {
    undo_out.reset();
    if (!session_handle) return false;

    const auto opts = get_nvprefs_options();
    if (!opts.nv_optimize_game) {
      // Game optimizations disabled by user, no-op.
      return true;
    }
    if (exe_name.empty()) {
      // Cannot match any profile without an exe — skip silently.
      return true;
    }

    const desired_settings_t desired = compute_desired(opts, client_fps);
    if (!desired.vsync && !desired.frl && !desired.pstate && !desired.prerender) {
      // All sub-options off, nothing to do.
      return true;
    }

    NvAPI_Status status;

    // 1. Try to find an existing profile that already owns this exe.
    NvAPI_UnicodeString app_name = {};
    fill_nvapi_string(app_name, exe_name.c_str());

    NvDRSProfileHandle profile_handle = 0;
    bool profile_was_created = false;
    bool application_was_added = false;
    std::wstring profile_name_used;

    NVDRS_APPLICATION app_info = {};
    app_info.version = NVDRS_APPLICATION_VER;
    status = NvAPI_DRS_FindApplicationByName(session_handle, app_name, &profile_handle, &app_info);
    if (status == NVAPI_OK) {
      // Found — fetch the profile name for the undo manifest.
      NVDRS_PROFILE existing = {};
      existing.version = NVDRS_PROFILE_VER;
      if (NvAPI_DRS_GetProfileInfo(session_handle, profile_handle, &existing) == NVAPI_OK) {
        profile_name_used.assign(reinterpret_cast<const wchar_t *>(existing.profileName));
      }
    }
    else {
      // No profile owns this exe: get/create our SunshineStreamGame profile and add the exe to it.
      NvAPI_UnicodeString profile_name = {};
      fill_nvapi_string(profile_name, sunshine_game_profile_name);
      status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);
      if (status != NVAPI_OK) {
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
      profile_name_used = sunshine_game_profile_name;

      NVDRS_APPLICATION application = {};
      application.version = NVDRS_APPLICATION_VER_V1;
      status = NvAPI_DRS_GetApplicationInfo(session_handle, profile_handle, app_name, &application);
      if (status != NVAPI_OK) {
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
    }

    undo_data_t::data_t::game_profile_t pending;
    pending.profile_name = platf::to_utf8(profile_name_used);
    pending.exe_path = platf::to_utf8(exe_name);
    pending.profile_was_created = profile_was_created;
    pending.application_was_added = application_was_added;

    if (!apply_desired_to_profile(session_handle, profile_handle, desired, pending.vsync, pending.frl, pending.pstate, pending.prerender)) {
      // A NvAPI write failed mid-way; populate undo_out so the caller can still
      // attempt to roll back what we did manage to set.
      undo_out = pending;
      return false;
    }

    undo_out = pending;
    info_message(std::wstring(L"Applied stream optimizations to game profile for ") + exe_name);
    return true;
  }

  bool
  driver_settings_t::restore_game_profile_to_undo(const undo_data_t::data_t::game_profile_t &undo_data) {
    if (!session_handle) return false;

    NvAPI_Status status;

    // Locate the profile that should own this exe right now. Prefer
    // FindApplicationByName because the user may have moved the application
    // between profiles since we wrote the settings.
    const std::wstring exe_name = platf::from_utf8(undo_data.exe_path);
    if (exe_name.empty()) {
      info_message("game_profile undo entry missing exe_path, skipping");
      return true;
    }

    NvAPI_UnicodeString app_name = {};
    fill_nvapi_string(app_name, exe_name.c_str());

    NvDRSProfileHandle profile_handle = 0;
    NVDRS_APPLICATION app_info = {};
    app_info.version = NVDRS_APPLICATION_VER;
    status = NvAPI_DRS_FindApplicationByName(session_handle, app_name, &profile_handle, &app_info);
    if (status != NVAPI_OK) {
      // Fall back to the named profile we (might have) created.
      const std::wstring saved_name = platf::from_utf8(undo_data.profile_name);
      NvAPI_UnicodeString profile_name = {};
      fill_nvapi_string(profile_name, saved_name.c_str());
      status = NvAPI_DRS_FindProfileByName(session_handle, profile_name, &profile_handle);
      if (status != NVAPI_OK) {
        info_message(std::wstring(L"No profile found for ") + exe_name + L" during restore, skipping");
        return true;
      }
    }

    // Best-effort restore: keep going even if individual setting writes fail,
    // so that subsequent settings + profile/application cleanup still run.
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

    if (undo_data.application_was_added) {
      status = NvAPI_DRS_DeleteApplication(session_handle, profile_handle, app_name);
      if (status != NVAPI_OK && status != NVAPI_EXECUTABLE_NOT_FOUND) {
        nvapi_error_message(status);
        error_message(std::wstring(L"NvAPI_DRS_DeleteApplication() failed for ") + exe_name);
        // Non-fatal: the user can clean up manually if it ever happens.
      }
    }

    if (undo_data.profile_was_created) {
      // Only delete the profile we created if it has no other applications attached.
      NVDRS_PROFILE info = {};
      info.version = NVDRS_PROFILE_VER;
      if (NvAPI_DRS_GetProfileInfo(session_handle, profile_handle, &info) == NVAPI_OK && info.numOfApps == 0) {
        status = NvAPI_DRS_DeleteProfile(session_handle, profile_handle);
        if (status != NVAPI_OK) {
          nvapi_error_message(status);
          error_message("NvAPI_DRS_DeleteProfile() SunshineStreamGame failed");
          // Non-fatal.
        }
      }
    }

    return ok;
  }

  bool
  driver_settings_t::check_and_modify_base_extras(int client_fps, std::optional<undo_data_t::data_t::base_extras_t> &undo_out) {
    undo_out.reset();
    if (!session_handle) return false;

    const auto opts = get_nvprefs_options();
    if (!opts.nv_optimize_game || !opts.nv_apply_to_base_profile) {
      return true;
    }

    const desired_settings_t desired = compute_desired(opts, client_fps);
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
      undo_out = pending;
      return false;
    }
    undo_out = pending;
    info_message("Applied stream optimizations to BASE driver profile");
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
