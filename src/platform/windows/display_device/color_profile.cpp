/**
 * @file src/platform/windows/display_device/color_profile.cpp
 * @brief Reversible Windows Advanced Color profile associations.
 */

// Windows must be included before the other Windows SDK headers.
#include <windows.h>

// standard includes
#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// lib includes
#include <icm.h>
#include <sddl.h>
#include <nlohmann/json.hpp>

// local includes
#include "color_profile.h"
#include "src/crypto.h"
#include "src/display_device/color_profile.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "windows_utils.h"

namespace display_device::win_color_profile {

  namespace {

    // Older MinGW headers stop at CPST_CUSTOM_WORKING_SPACE even though the
    // runtime API and current Windows SDK define STANDARD (7) and EXTENDED (8).
    constexpr auto extended_display_color_mode = static_cast<COLORPROFILESUBTYPE>(8);

    using add_display_association_fn = HRESULT(WINAPI *)(
      WCS_PROFILE_MANAGEMENT_SCOPE,
      PCWSTR,
      LUID,
      UINT32,
      BOOL,
      BOOL);
    using remove_display_association_fn = HRESULT(WINAPI *)(
      WCS_PROFILE_MANAGEMENT_SCOPE,
      PCWSTR,
      LUID,
      UINT32,
      BOOL);
    using set_display_default_fn = HRESULT(WINAPI *)(
      WCS_PROFILE_MANAGEMENT_SCOPE,
      PCWSTR,
      COLORPROFILETYPE,
      COLORPROFILESUBTYPE,
      LUID,
      UINT32);
    using get_display_list_fn = HRESULT(WINAPI *)(
      WCS_PROFILE_MANAGEMENT_SCOPE,
      LUID,
      UINT32,
      LPWSTR **,
      PDWORD);
    using get_display_default_fn = HRESULT(WINAPI *)(
      WCS_PROFILE_MANAGEMENT_SCOPE,
      LUID,
      UINT32,
      COLORPROFILETYPE,
      COLORPROFILESUBTYPE,
      LPWSTR *);
    using get_display_user_scope_fn = HRESULT(WINAPI *)(
      LUID,
      UINT32,
      WCS_PROFILE_MANAGEMENT_SCOPE *);
    using get_color_directory_fn = BOOL(WINAPI *)(PCWSTR, PWSTR, PDWORD);
    using set_use_per_user_profiles_fn = BOOL(WINAPI *)(PCWSTR, DWORD, BOOL);

    template <typename Function>
    Function
    load_function(HMODULE module, const char *name) noexcept {
      static_assert(sizeof(Function) == sizeof(FARPROC));
      return std::bit_cast<Function>(GetProcAddress(module, name));
    }

    /**
     * @brief Dynamically loaded modern Windows Color System entry points.
     *
     * Dynamic loading is intentional. MinGW's import library does not expose
     * these modern functions yet, and it lets us report ERROR_PROC_NOT_FOUND on
     * an unsupported Windows build rather than preventing Sunshine from loading.
     */
    class color_profile_api_t {
    public:
      color_profile_api_t() {
        module = LoadLibraryExW(L"mscms.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) {
          load_status = HRESULT_FROM_WIN32(GetLastError());
          return;
        }

        add_display_association = load_function<add_display_association_fn>(
          module, "ColorProfileAddDisplayAssociation");
        remove_display_association = load_function<remove_display_association_fn>(
          module, "ColorProfileRemoveDisplayAssociation");
        set_display_default = load_function<set_display_default_fn>(
          module, "ColorProfileSetDisplayDefaultAssociation");
        get_display_list = load_function<get_display_list_fn>(
          module, "ColorProfileGetDisplayList");
        get_display_default = load_function<get_display_default_fn>(
          module, "ColorProfileGetDisplayDefault");
        get_display_user_scope = load_function<get_display_user_scope_fn>(
          module, "ColorProfileGetDisplayUserScope");
        get_color_directory = load_function<get_color_directory_fn>(module, "GetColorDirectoryW");
        set_use_per_user_profiles = load_function<set_use_per_user_profiles_fn>(module, "WcsSetUsePerUserProfiles");

        if (!add_display_association ||
            !remove_display_association ||
            !set_display_default ||
            !get_display_list ||
            !get_display_default ||
            !get_display_user_scope || !get_color_directory || !set_use_per_user_profiles) {
          load_status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
      }

      ~color_profile_api_t() {
        if (module) {
          FreeLibrary(module);
        }
      }

      color_profile_api_t(const color_profile_api_t &) = delete;
      color_profile_api_t &
      operator=(const color_profile_api_t &) = delete;

      [[nodiscard]] HRESULT
      status() const {
        return load_status;
      }

      add_display_association_fn add_display_association { nullptr };
      remove_display_association_fn remove_display_association { nullptr };
      set_display_default_fn set_display_default { nullptr };
      get_display_list_fn get_display_list { nullptr };
      get_display_default_fn get_display_default { nullptr };
      get_display_user_scope_fn get_display_user_scope { nullptr };
      get_color_directory_fn get_color_directory { nullptr };
      set_use_per_user_profiles_fn set_use_per_user_profiles { nullptr };

    private:
      HMODULE module { nullptr };
      HRESULT load_status { S_OK };
    };

    struct display_target_t {
      LUID adapter_id {};
      UINT32 source_id {};
      std::wstring display_name;
    };

    struct handle_closer_t {
      void
      operator()(void *handle) const noexcept {
        if (handle) {
          CloseHandle(handle);
        }
      }
    };

    struct local_free_t {
      template <typename T>
      void
      operator()(T *memory) const noexcept {
        if (memory) {
          LocalFree(memory);
        }
      }
    };

    using unique_handle_t = std::unique_ptr<void, handle_closer_t>;
    using unique_local_string_t = std::unique_ptr<wchar_t, local_free_t>;
    using unique_local_string_list_t = std::unique_ptr<LPWSTR, local_free_t>;

    std::mutex operation_mutex;

    color_profile_api_t &
    color_profile_api() {
      static color_profile_api_t api;
      return api;
    }

    std::string
    hresult_string(HRESULT status) {
      std::ostringstream stream;
      stream << "0x"
             << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
             << static_cast<std::uint32_t>(status);
      return stream.str();
    }

    void
    log_failure(
      std::string_view operation,
      HRESULT status,
      std::string_view device_id,
      std::string_view profile = {}) {
      BOOST_LOG(error) << "Advanced Color profile " << operation
                       << " failed for display " << device_id
                       << (profile.empty() ? "" : ", profile ")
                       << profile
                       << ", HRESULT=" << hresult_string(status);
      platf::print_status("Advanced Color profile " + std::string { operation }, status);
    }

    bool
    is_missing(HRESULT status) {
      return status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
             status == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
             status == HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION);
    }

    bool
    is_already_associated(HRESULT status) {
      return status == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
             status == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    }

    HRESULT
    associate_default(const color_profile_api_t &api, const display_target_t &target, const std::wstring &profile) {
      auto status = api.add_display_association(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
        profile.c_str(), target.adapter_id, target.source_id, TRUE, TRUE);
      if (is_already_associated(status)) {
        status = api.set_display_default(WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
          profile.c_str(), CPT_ICC, extended_display_color_mode, target.adapter_id, target.source_id);
      }
      return status;
    }

    HRESULT
    resolve_active_target(const std::string &device_id, display_target_t &target) {
      if (device_id.empty()) {
        return E_INVALIDARG;
      }

      const auto display_data = w_utils::query_display_config(w_utils::ACTIVE_ONLY_DEVICES);
      if (!display_data) {
        return E_FAIL;
      }

      const auto *path = w_utils::get_active_path(device_id, display_data->paths);
      if (!path) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
      }

      target.adapter_id = path->targetInfo.adapterId;
      target.source_id = path->sourceInfo.id;
      target.display_name = platf::from_utf8(w_utils::get_display_name(*path));
      return S_OK;
    }

    template <typename Callback>
    HRESULT
    run_as_profile_user(Callback &&callback) {
      std::lock_guard registry_lock { platf::user_registry_mutex() };
      if (!platf::initialize_user_registry()) return E_FAIL;
      HRESULT callback_status { E_UNEXPECTED };
      const auto guarded_callback = [&]() noexcept {
        try {
          callback_status = callback();
        }
        catch (const std::bad_alloc &) {
          callback_status = E_OUTOFMEMORY;
        }
        catch (const std::exception &exception) {
          BOOST_LOG(error) << "Advanced Color profile operation raised an exception: " << exception.what();
          callback_status = E_FAIL;
        }
        catch (...) {
          BOOST_LOG(error) << "Advanced Color profile operation raised an unknown exception.";
          callback_status = E_FAIL;
        }
      };

      if (!platf::is_running_as_system()) {
        guarded_callback();
        return callback_status;
      }

      SetLastError(ERROR_SUCCESS);
      unique_handle_t user_token { platf::retrieve_users_token(false) };
      if (!user_token) {
        const DWORD token_error = GetLastError();
        return HRESULT_FROM_WIN32(token_error == ERROR_SUCCESS ? ERROR_NO_TOKEN : token_error);
      }

      const std::error_code impersonation_error =
        platf::impersonate_current_user(user_token.get(), guarded_callback);
      if (impersonation_error) {
        // impersonate_current_user() reports a portable std::errc value, not a
        // Win32 error number, so don't reinterpret its numeric value as one.
        return E_ACCESSDENIED;
      }

      return callback_status;
    }

    HRESULT
    get_default_profile(
      const color_profile_api_t &api,
      const display_target_t &target,
      WCS_PROFILE_MANAGEMENT_SCOPE scope,
      bool &present,
      std::wstring &profile) {
      present = false;
      profile.clear();

      LPWSTR raw_profile { nullptr };
      const HRESULT status = api.get_display_default(
        scope,
        target.adapter_id,
        target.source_id,
        CPT_ICC,
        extended_display_color_mode,
        &raw_profile);
      unique_local_string_t profile_cleanup { raw_profile };

      if (is_missing(status)) {
        return S_OK;
      }
      if (FAILED(status)) {
        return status;
      }

      if (raw_profile && raw_profile[0] != L'\0') {
        present = true;
        profile = raw_profile;
      }
      return S_OK;
    }

    HRESULT
    get_associated_profiles(
      const color_profile_api_t &api,
      const display_target_t &target,
      WCS_PROFILE_MANAGEMENT_SCOPE scope,
      std::vector<std::wstring> &profiles) {
      profiles.clear();

      LPWSTR *raw_profiles { nullptr };
      DWORD profile_count {};
      const HRESULT status = api.get_display_list(
        scope,
        target.adapter_id,
        target.source_id,
        &raw_profiles,
        &profile_count);
      unique_local_string_list_t profiles_cleanup { raw_profiles };

      if (is_missing(status)) {
        return S_OK;
      }
      if (FAILED(status)) {
        return status;
      }
      if (profile_count != 0 && !raw_profiles) {
        return E_UNEXPECTED;
      }

      profiles.reserve(profile_count);
      for (DWORD index = 0; index < profile_count; ++index) {
        if (raw_profiles[index] && raw_profiles[index][0] != L'\0') {
          profiles.emplace_back(raw_profiles[index]);
        }
      }
      return S_OK;
    }

    bool
    profile_names_equal(std::wstring_view left, std::wstring_view right) {
      if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
          right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
      }

      return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()),
               right.data(), static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
    }

    HRESULT
    remove_advanced_association(
      const color_profile_api_t &api,
      const display_target_t &target,
      const std::wstring &profile) {
      const HRESULT status = api.remove_display_association(
        WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
        profile.c_str(),
        target.adapter_id,
        target.source_id,
        TRUE);
      return is_missing(status) ? S_OK : status;
    }

    HRESULT
    restore_association_without_default(
      const color_profile_api_t &api,
      const display_target_t &target,
      const std::wstring &profile) {
      HRESULT status = remove_advanced_association(api, target, profile);
      if (FAILED(status)) {
        return status;
      }

      status = api.add_display_association(
        WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
        profile.c_str(),
        target.adapter_id,
        target.source_id,
        FALSE,
        TRUE);
      return is_already_associated(status) ? S_OK : status;
    }

    HRESULT
    validate_state(const state_t &state) {
      if (state.version != state_t::current_version ||
          state.device_id.empty() ||
          state.applied_profile.empty() ||
          (state.original_default_present && state.original_default_profile.empty())) {
        return E_INVALIDARG;
      }
      return S_OK;
    }

    HRESULT
    profile_user_sid(std::string &sid) {
      HANDLE raw_token {};
      if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
        if (GetLastError() != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
          return HRESULT_FROM_WIN32(GetLastError());
        }
      }
      unique_handle_t token { raw_token };
      DWORD size {};
      GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
      if (size == 0) return HRESULT_FROM_WIN32(GetLastError());
      std::vector<std::uint8_t> buffer(size);
      if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) return HRESULT_FROM_WIN32(GetLastError());
      LPWSTR raw_sid {};
      if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &raw_sid)) return HRESULT_FROM_WIN32(GetLastError());
      unique_local_string_t cleanup { raw_sid };
      sid = platf::to_utf8(raw_sid);
      return sid.empty() ? E_FAIL : S_OK;
    }

    HRESULT
    read_profile(const std::string &profile_name, calibration_result_t &result) {
      result.stage = "profile_name";
      if (!color_profile::is_valid_profile_basename(profile_name)) return E_INVALIDARG;
      result.stage = "color_directory";
      auto &api = color_profile_api();
      if (FAILED(api.status())) return api.status();
      std::wstring directory(MAX_PATH, L'\0');
      DWORD bytes = static_cast<DWORD>(directory.size() * sizeof(wchar_t));
      if (!api.get_color_directory(nullptr, directory.data(), &bytes)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 64 * 1024) return HRESULT_FROM_WIN32(GetLastError());
        directory.resize(bytes / sizeof(wchar_t));
        if (!api.get_color_directory(nullptr, directory.data(), &bytes)) return HRESULT_FROM_WIN32(GetLastError());
      }
      directory.resize(wcslen(directory.c_str()));
      const auto path = std::filesystem::path { directory } / platf::from_utf8(profile_name);
      result.stage = "profile_read";
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error) return HRESULT_FROM_WIN32(error.value());
      if (size < 132 || size > 16 * 1024 * 1024) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      std::ifstream stream(path, std::ios::binary);
      std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
      if (!stream || !stream.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))) {
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
      }
      result.stage = "profile_parse";
      const auto luminance = hdr::parse_calibration_luminance(data);
      if (!luminance) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      auto fingerprint = util::hex(crypto::hash(std::string_view {
                                     reinterpret_cast<const char *>(data.data()), data.size() }),
        true)
                           .to_string();
      std::transform(fingerprint.begin(), fingerprint.end(), fingerprint.begin(), [](unsigned char c) { return std::tolower(c); });
      result.profile = hdr::calibration_profile_t { profile_name, std::move(fingerprint), *luminance };
      result.state = calibration_result_t::state_e::detected;
      result.stage.clear();
      return S_OK;
    }

    void
    finish_inspection(calibration_result_t &result, HRESULT status) {
      result.error = static_cast<std::uint32_t>(status);
      if (FAILED(status)) {
        result.profile.reset();
        result.state = status == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) || status == E_INVALIDARG ?
                         calibration_result_t::state_e::invalid :
                         calibration_result_t::state_e::unavailable;
      }
    }

  }  // namespace

  calibration_result_t
  read_hdr_calibration(const std::string &profile_name) {
    std::lock_guard lock { operation_mutex };
    calibration_result_t result;
    result.stage = "user_context";
    const HRESULT status = run_as_profile_user([&]() -> HRESULT {
      result.stage = "user_identity";
      const auto identity_status = profile_user_sid(result.user_sid);
      if (FAILED(identity_status)) return identity_status;
      return read_profile(profile_name, result);
    });
    finish_inspection(result, status);
    return result;
  }

  calibration_result_t
  current_hdr_calibration(const std::string &device_id) {
    std::lock_guard lock { operation_mutex };
    calibration_result_t result;
    result.stage = "target_resolution";
    display_target_t target;
    HRESULT status = resolve_active_target(device_id, target);
    if (SUCCEEDED(status)) {
      result.stage = "user_context";
      status = run_as_profile_user([&]() -> HRESULT {
        result.stage = "user_identity";
        auto status = profile_user_sid(result.user_sid);
        if (FAILED(status)) return status;
        auto &api = color_profile_api();
        result.stage = "color_api";
        if (FAILED(api.status())) return api.status();
        WCS_PROFILE_MANAGEMENT_SCOPE scope {};
        result.stage = "profile_scope";
        status = api.get_display_user_scope(target.adapter_id, target.source_id, &scope);
        if (FAILED(status)) return status;
        if (scope != WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE && scope != WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER) return E_UNEXPECTED;
        result.scope = static_cast<unsigned>(scope);
        bool present = false;
        std::wstring profile;
        result.stage = "profile_association";
        status = get_default_profile(api, target, scope, present, profile);
        if (FAILED(status)) return status;
        if (!present) {
          result.state = calibration_result_t::state_e::absent;
          result.stage.clear();
          return S_OK;
        }
        return read_profile(platf::to_utf8(profile), result);
      });
    }
    finish_inspection(result, status);
    return result;
  }

  bool
  apply_hdr_calibration(const std::string &device_id, const hdr::calibration_binding_t &binding) {
    std::lock_guard lock { operation_mutex };
    display_target_t target;
    HRESULT status = resolve_active_target(device_id, target);
    if (SUCCEEDED(status)) {
      status = run_as_profile_user([&]() -> HRESULT {
        calibration_result_t checked;
        auto status = profile_user_sid(checked.user_sid);
        if (FAILED(status)) return status;
        if (checked.user_sid != binding.user_sid) return E_ACCESSDENIED;
        status = read_profile(binding.profile.profile_name, checked);
        if (FAILED(status)) return status;
        if (!hdr::same_calibration(*checked.profile, binding.profile)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        auto &api = color_profile_api();
        const auto name = platf::from_utf8(binding.profile.profile_name);
        WCS_PROFILE_MANAGEMENT_SCOPE scope {};
        status = api.get_display_user_scope(target.adapter_id, target.source_id, &scope);
        if (FAILED(status)) return status;
        bool present = false;
        std::wstring current;
        status = get_default_profile(api, target, scope, present, current);
        if (FAILED(status)) return status;
        if (present && profile_names_equal(current, name)) return S_OK;
        // This association belongs to the client's dedicated VDD and survives
        // monitor recreation. Physical-screen overrides keep their existing undo path.
        if (scope == WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE) {
          // A recreated monitor can still select its system list. Selecting a
          // per-user default alone does not enable that list on modern WCS APIs.
          DISPLAY_DEVICEW monitor {};
          monitor.cb = sizeof(monitor);
          if (target.display_name.empty() || !EnumDisplayDevicesW(target.display_name.c_str(), 0, &monitor, 0)) return E_FAIL;
          if (!api.set_use_per_user_profiles(monitor.DeviceKey, CLASS_MONITOR, TRUE)) return HRESULT_FROM_WIN32(GetLastError());
        }
        status = associate_default(api, target, name);
        if (FAILED(status)) return status;
        status = api.get_display_user_scope(target.adapter_id, target.source_id, &scope);
        if (FAILED(status)) return status;
        status = get_default_profile(api, target, scope, present, current);
        if (FAILED(status)) return status;
        return present && profile_names_equal(current, name) ? S_OK : E_FAIL;
      });
    }
    if (FAILED(status)) log_failure("client calibration", status, device_id, binding.profile.profile_name);
    return SUCCEEDED(status);
  }

  void
  to_json(nlohmann::json &json, const state_t &state) {
    json = nlohmann::json {
      { "version", state.version },
      { "device_id", state.device_id },
      { "original_default_profile", state.original_default_profile },
      { "applied_profile", state.applied_profile },
      { "original_default_present", state.original_default_present },
      { "original_scope_was_current_user", state.original_scope_was_current_user },
      { "applied_profile_was_associated", state.applied_profile_was_associated }
    };
  }

  void
  from_json(const nlohmann::json &json, state_t &state) {
    state = {};
    state.version = json.value("version", state_t::current_version);
    state.device_id = json.value("device_id", std::string {});
    state.original_default_profile = json.value("original_default_profile", std::string {});
    state.applied_profile = json.value("applied_profile", std::string {});
    state.original_default_present = json.value("original_default_present", false);
    state.original_scope_was_current_user = json.value("original_scope_was_current_user", false);
    state.applied_profile_was_associated = json.value("applied_profile_was_associated", false);
  }

  std::optional<state_t>
  snapshot(const std::string &device_id, const std::string &applied_profile) {
    std::lock_guard lock { operation_mutex };

    if (device_id.empty() || applied_profile.empty()) {
      log_failure("snapshot validation", E_INVALIDARG, device_id, applied_profile);
      return std::nullopt;
    }

    display_target_t target;
    HRESULT status = resolve_active_target(device_id, target);
    if (FAILED(status)) {
      log_failure("target resolution", status, device_id, applied_profile);
      return std::nullopt;
    }

    const std::wstring applied_profile_wide = platf::from_utf8(applied_profile);
    if (applied_profile_wide.empty()) {
      status = HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
      log_failure("profile-name conversion", status, device_id, applied_profile);
      return std::nullopt;
    }

    state_t state;
    state.device_id = device_id;
    state.applied_profile = applied_profile;

    status = run_as_profile_user([&]() -> HRESULT {
      auto &api = color_profile_api();
      if (FAILED(api.status())) {
        return api.status();
      }

      WCS_PROFILE_MANAGEMENT_SCOPE original_scope {};
      HRESULT operation_status = api.get_display_user_scope(
        target.adapter_id,
        target.source_id,
        &original_scope);
      if (FAILED(operation_status)) {
        return operation_status;
      }
      if (original_scope != WCS_PROFILE_MANAGEMENT_SCOPE_SYSTEM_WIDE &&
          original_scope != WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER) {
        return E_UNEXPECTED;
      }
      state.original_scope_was_current_user =
        original_scope == WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER;

      std::wstring original_default;
      operation_status = get_default_profile(
        api,
        target,
        original_scope,
        state.original_default_present,
        original_default);
      if (FAILED(operation_status)) {
        return operation_status;
      }
      if (state.original_default_present) {
        state.original_default_profile = platf::to_utf8(original_default);
        if (state.original_default_profile.empty()) {
          return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
        }
      }

      std::vector<std::wstring> associated_profiles;
      operation_status = get_associated_profiles(
        api,
        target,
        WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
        associated_profiles);
      if (FAILED(operation_status)) {
        return operation_status;
      }

      state.applied_profile_was_associated = std::any_of(
        associated_profiles.begin(),
        associated_profiles.end(),
        [&](const std::wstring &profile) {
          return profile_names_equal(profile, applied_profile_wide);
        });
      return S_OK;
    });

    if (FAILED(status)) {
      log_failure("snapshot", status, device_id, applied_profile);
      return std::nullopt;
    }

    BOOST_LOG(info) << "Snapshotted Advanced Color profile state for display " << device_id
                    << ": original_default="
                    << (state.original_default_present ? state.original_default_profile : "<none>")
                    << ", original_scope="
                    << (state.original_scope_was_current_user ? "current-user" : "system-wide")
                    << ", applied_profile_was_associated="
                    << state.applied_profile_was_associated;
    return state;
  }

  bool
  apply(const state_t &state) {
    std::lock_guard lock { operation_mutex };

    HRESULT status = validate_state(state);
    if (FAILED(status)) {
      log_failure("apply validation", status, state.device_id, state.applied_profile);
      return false;
    }

    display_target_t target;
    status = resolve_active_target(state.device_id, target);
    if (FAILED(status)) {
      log_failure("target resolution", status, state.device_id, state.applied_profile);
      return false;
    }

    const std::wstring profile = platf::from_utf8(state.applied_profile);
    if (profile.empty()) {
      status = HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
      log_failure("profile-name conversion", status, state.device_id, state.applied_profile);
      return false;
    }

    status = run_as_profile_user([&]() -> HRESULT {
      auto &api = color_profile_api();
      if (FAILED(api.status())) {
        return api.status();
      }

      if (state.applied_profile_was_associated) {
        return api.set_display_default(
          WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
          profile.c_str(),
          CPT_ICC,
          extended_display_color_mode,
          target.adapter_id,
          target.source_id);
      }

      return associate_default(api, target, profile);
    });

    if (FAILED(status)) {
      log_failure("apply", status, state.device_id, state.applied_profile);
      return false;
    }

    BOOST_LOG(info) << "Applied Advanced Color profile " << state.applied_profile
                    << " to display " << state.device_id;
    return true;
  }

  bool
  restore(const state_t &state) {
    std::lock_guard lock { operation_mutex };

    HRESULT status = validate_state(state);
    if (FAILED(status)) {
      log_failure("restore validation", status, state.device_id, state.applied_profile);
      return false;
    }

    display_target_t target;
    status = resolve_active_target(state.device_id, target);
    if (FAILED(status)) {
      log_failure("target resolution", status, state.device_id, state.applied_profile);
      return false;
    }

    const std::wstring applied_profile = platf::from_utf8(state.applied_profile);
    const std::wstring original_default =
      state.original_default_present ? platf::from_utf8(state.original_default_profile) : std::wstring {};
    if (applied_profile.empty() ||
        (state.original_default_present && original_default.empty())) {
      status = HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
      log_failure("profile-name conversion", status, state.device_id, state.applied_profile);
      return false;
    }

    status = run_as_profile_user([&]() -> HRESULT {
      auto &api = color_profile_api();
      if (FAILED(api.status())) {
        return api.status();
      }

      // A current-user default can be restored directly. When the original
      // scope was system-wide (or no user default existed), the user override
      // must instead be cleared while preserving any pre-existing association.
      if (state.original_scope_was_current_user && state.original_default_present) {
        HRESULT operation_status = api.set_display_default(
          WCS_PROFILE_MANAGEMENT_SCOPE_CURRENT_USER,
          original_default.c_str(),
          CPT_ICC,
          extended_display_color_mode,
          target.adapter_id,
          target.source_id);
        if (FAILED(operation_status)) {
          return operation_status;
        }

        if (!state.applied_profile_was_associated &&
            !profile_names_equal(applied_profile, original_default)) {
          operation_status = remove_advanced_association(api, target, applied_profile);
        }
        return operation_status;
      }

      if (state.applied_profile_was_associated) {
        return restore_association_without_default(api, target, applied_profile);
      }
      return remove_advanced_association(api, target, applied_profile);
    });

    if (FAILED(status)) {
      log_failure("restore", status, state.device_id, state.applied_profile);
      return false;
    }

    BOOST_LOG(info) << "Restored Advanced Color profile state for display " << state.device_id
                    << " after applying " << state.applied_profile;
    return true;
  }

}  // namespace display_device::win_color_profile
