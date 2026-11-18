/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <dxgi1_2.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>

// local includes
#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/globals.h"
#include "src/log_text.h"
#include "src/logging.h"
#include "src/process.h"
#include <boost/program_options/parsers.hpp>
#include <vector>

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
  HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 }
  // compare with __declspec(uuid(...)) for the struct above.
};

template <>
constexpr auto
__mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {

  // UAC bypass for WGC capture mode.
  // WGC cannot capture UAC prompts, so we temporarily set ConsentPromptBehaviorAdmin=0
  // to auto-elevate without prompting during capture, and restore the original value when capture ends.
  namespace secure_desktop {
    static constexpr const wchar_t *kPoliciesKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    static constexpr const wchar_t *kValueName = L"ConsentPromptBehaviorAdmin";

    static std::mutex state_mutex;
    static int wgc_ref_count = 0;
    static DWORD original_value = 5;  // Default: prompt for consent on secure desktop
    static bool modified = false;
    static std::once_flag recovery_flag;

    static std::filesystem::path
    backup_file_path() {
      wchar_t temp[MAX_PATH];
      GetTempPathW(MAX_PATH, temp);
      return std::filesystem::path(temp) / L"sunshine_uac_consent_backup";
    }

    /**
     * @brief Valid range for ConsentPromptBehaviorAdmin: 0-5.
     */
    static bool
    is_valid_consent_value(DWORD value) {
      return value <= 5;
    }

    /**
     * @brief Save original value to a temp file for crash recovery.
     * @return true if backup was successfully written and flushed.
     */
    static bool
    save_backup(DWORD value) {
      try {
        auto path = backup_file_path();
        std::ofstream f(path, std::ios::trunc);
        if (f) {
          f << value;
          f.flush();
          if (f.good()) {
            return true;
          }
        }
        BOOST_LOG(error) << "Failed to write crash recovery backup file"sv;
      }
      catch (...) {
        BOOST_LOG(error) << "Exception writing crash recovery backup file"sv;
      }
      return false;
    }

    /**
     * @brief Remove the backup file.
     */
    static void
    remove_backup() {
      try {
        std::filesystem::remove(backup_file_path());
      }
      catch (...) {
      }
    }

    /**
     * @brief Restore from crash recovery backup if it exists.
     */
    static void
    recover_from_crash() {
      try {
        auto path = backup_file_path();
        if (!std::filesystem::exists(path)) return;

        std::ifstream f(path);
        DWORD saved_value;
        if (!(f >> saved_value)) {
          BOOST_LOG(warning) << "Corrupt crash recovery backup file, removing"sv;
          remove_backup();
          return;
        }

        if (!is_valid_consent_value(saved_value)) {
          BOOST_LOG(error) << "Invalid ConsentPromptBehaviorAdmin value "sv << saved_value << " in backup, rejecting"sv;
          remove_backup();
          return;
        }

        HKEY hkey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPoliciesKey, 0, KEY_WRITE, &hkey) == ERROR_SUCCESS) {
          auto result = RegSetValueExW(hkey, kValueName, 0, REG_DWORD, (const BYTE *) &saved_value, sizeof(saved_value));
          RegCloseKey(hkey);
          if (result == ERROR_SUCCESS) {
            BOOST_LOG(info) << "Restored ConsentPromptBehaviorAdmin to "sv << saved_value << " from crash recovery backup"sv;
            remove_backup();
          }
          else {
            BOOST_LOG(error) << "Failed to restore ConsentPromptBehaviorAdmin from crash backup, will retry next startup"sv;
          }
        }
        else {
          BOOST_LOG(error) << "Failed to open registry for crash recovery, will retry next startup"sv;
        }
      }
      catch (...) {
      }
    }

    /**
     * @brief Disable UAC prompts by setting ConsentPromptBehaviorAdmin to 0 (elevate without prompting).
     * @return true if the value was modified.
     */
    static bool
    disable() {
      std::lock_guard<std::mutex> lock(state_mutex);

      if (wgc_ref_count++ > 0) {
        // Another WGC instance already disabled it
        return false;
      }

      // If a prior restore() failed, the registry is still at 0 and original_value
      // holds the real pre-modification value. Don't re-read or overwrite it.
      if (modified) {
        BOOST_LOG(info) << "ConsentPromptBehaviorAdmin still modified from prior session (pending restore), reusing original value "sv << original_value;
        return false;
      }

      DWORD value, size = sizeof(value);
      auto read_result = RegGetValueW(HKEY_LOCAL_MACHINE, kPoliciesKey, kValueName, RRF_RT_REG_DWORD, nullptr, &value, &size);
      if (read_result != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Failed to read ConsentPromptBehaviorAdmin (error 0x"sv << util::hex(read_result).to_string_view() << "), aborting UAC disable"sv;
        return false;
      }
      original_value = value;

      if (value == 0) {
        BOOST_LOG(info) << "ConsentPromptBehaviorAdmin is already 0 (auto-elevate)"sv;
        return false;
      }

      // Persist backup BEFORE modifying registry — ensures crash recovery is possible
      if (!save_backup(original_value)) {
        BOOST_LOG(error) << "Cannot persist backup file, aborting UAC disable to prevent irrecoverable state"sv;
        return false;
      }

      HKEY hkey;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPoliciesKey, 0, KEY_WRITE, &hkey) != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Cannot disable UAC prompts: insufficient privileges. Run Sunshine elevated to enable this feature."sv;
        remove_backup();
        return false;
      }

      DWORD new_value = 0;
      auto result = RegSetValueExW(hkey, kValueName, 0, REG_DWORD, (const BYTE *) &new_value, sizeof(new_value));
      RegCloseKey(hkey);

      if (result == ERROR_SUCCESS) {
        modified = true;
        BOOST_LOG(info) << "Set ConsentPromptBehaviorAdmin to 0 for WGC capture (original value: "sv << original_value << ")"sv;
        return true;
      }

      BOOST_LOG(warning) << "Failed to set ConsentPromptBehaviorAdmin [0x"sv << util::hex(result).to_string_view() << ']';
      remove_backup();
      return false;
    }

    /**
     * @brief Restore the original ConsentPromptBehaviorAdmin setting.
     */
    static void
    restore() {
      std::lock_guard<std::mutex> lock(state_mutex);

      if (--wgc_ref_count > 0) {
        // Other WGC instances still active
        return;
      }

      if (!modified) {
        return;
      }

      HKEY hkey;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPoliciesKey, 0, KEY_WRITE, &hkey) == ERROR_SUCCESS) {
        auto result = RegSetValueExW(hkey, kValueName, 0, REG_DWORD, (const BYTE *) &original_value, sizeof(original_value));
        RegCloseKey(hkey);
        if (result == ERROR_SUCCESS) {
          BOOST_LOG(info) << "Restored ConsentPromptBehaviorAdmin to "sv << original_value;
          modified = false;
          remove_backup();
        }
        else {
          BOOST_LOG(error) << "Failed to restore ConsentPromptBehaviorAdmin, backup preserved for next startup"sv;
        }
      }
      else {
        BOOST_LOG(error) << "Failed to open registry for restore, backup preserved for next startup"sv;
      }
    }
  }  // namespace secure_desktop

  void
  recover_secure_desktop() {
    std::call_once(secure_desktop::recovery_flag, secure_desktop::recover_from_crash);
  }

  /**
   * @brief Find a window by title (case-insensitive fuzzy matching).
   * @param window_title The window title to search for.
   * @return HWND of the found window, or nullptr if not found.
   *
   * @note This function uses multiple matching strategies:
   *       1. Direct substring match
   *       2. Match after removing spaces
   *       3. Word-based match (all search words appear in window title)
   *       4. Fuzzy character sequence match (characters appear in order)
   *       It skips invisible windows and windows without titles.
   */
  static HWND
  find_window_by_title(const std::string &window_title) {
    if (window_title.empty()) {
      return nullptr;
    }

    std::wstring search_title = platf::from_utf8(window_title);

    // Convert to lowercase for case-insensitive comparison
    std::transform(search_title.begin(), search_title.end(), search_title.begin(), ::towlower);

    // Split search title into words for word-based matching
    std::vector<std::wstring> search_words = platf::split_words(search_title);
    std::wstring search_title_no_spaces = search_title;
    search_title_no_spaces.erase(std::remove(search_title_no_spaces.begin(), search_title_no_spaces.end(), L' '), search_title_no_spaces.end());

    struct EnumData {
      std::wstring search_title;
      std::wstring search_title_no_spaces;
      std::vector<std::wstring> search_words;
      HWND found_hwnd;
      HWND best_match_hwnd;  // Best fuzzy match found so far
      int best_match_score;  // Higher is better
    } enum_data { search_title, search_title_no_spaces, search_words, nullptr, nullptr, 0 };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
      auto *data = reinterpret_cast<EnumData *>(lParam);

      // Check if window is visible
      if (!IsWindowVisible(hwnd)) {
        return TRUE;  // Continue enumeration
      }

      // Skip minimized windows as they may not capture properly
      if (IsIconic(hwnd)) {
        return TRUE;  // Continue enumeration
      }

      // Get window title length first
      int title_length = GetWindowTextLengthW(hwnd);
      if (title_length == 0) {
        return TRUE;  // Continue enumeration - no title
      }

      // Get window title
      std::wstring window_text(title_length + 1, L'\0');
      if (GetWindowTextW(hwnd, &window_text[0], title_length + 1) == 0) {
        return TRUE;  // Continue enumeration
      }
      window_text.resize(title_length);  // Remove null terminator from string

      // Convert to lowercase for case-insensitive comparison
      std::transform(window_text.begin(), window_text.end(), window_text.begin(), ::towlower);

      // Strategy 1: Direct substring match (highest priority)
      if (window_text.find(data->search_title) != std::wstring::npos) {
        data->found_hwnd = hwnd;
        return FALSE;  // Stop enumeration - exact match found
      }

      // Strategy 2: Match after removing spaces
      std::wstring window_text_no_spaces = window_text;
      window_text_no_spaces.erase(std::remove(window_text_no_spaces.begin(), window_text_no_spaces.end(), L' '), window_text_no_spaces.end());
      if (window_text_no_spaces.find(data->search_title_no_spaces) != std::wstring::npos) {
        data->found_hwnd = hwnd;
        return FALSE;  // Stop enumeration - good match found
      }

      // Strategy 3: Word-based matching (check if all search words appear in window title)
      if (!data->search_words.empty()) {
        bool all_words_found = true;
        for (const auto &word : data->search_words) {
          if (word.length() < 2) {
            continue;  // Skip very short words
          }
          if (window_text.find(word) == std::wstring::npos &&
              window_text_no_spaces.find(word) == std::wstring::npos) {
            all_words_found = false;
            break;
          }
        }
        if (all_words_found && data->search_words.size() > 0) {
          // Calculate a simple score based on word matches
          int score = static_cast<int>(data->search_words.size() * 10);
          if (score > data->best_match_score) {
            data->best_match_hwnd = hwnd;
            data->best_match_score = score;
          }
        }
      }

      // Strategy 4: Fuzzy character sequence matching (lowest priority, but still useful)
      if (platf::fuzzy_match(window_text, data->search_title)) {
        // Calculate score based on how close the match is
        int score = static_cast<int>(data->search_title.length() * 5);
        if (score > data->best_match_score) {
          data->best_match_hwnd = hwnd;
          data->best_match_score = score;
        }
      }

      return TRUE;  // Continue enumeration
    },
      reinterpret_cast<LPARAM>(&enum_data));

    // If exact match found, return it
    if (enum_data.found_hwnd != nullptr) {
      return enum_data.found_hwnd;
    }

    // Otherwise, return the best fuzzy match if we found one
    if (enum_data.best_match_hwnd != nullptr && enum_data.best_match_score > 0) {
      wchar_t actual_title[256] = { 0 };
      GetWindowTextW(enum_data.best_match_hwnd, actual_title, sizeof(actual_title) / sizeof(actual_title[0]));
      BOOST_LOG(debug) << "Using fuzzy match: "sv << logging::quoted_name(platf::to_utf8(actual_title)) << " for search "sv << logging::quoted_name(window_title) << " (score: "sv << enum_data.best_match_score << ")";
      return enum_data.best_match_hwnd;
    }

    return nullptr;
  }

  /**
   * @brief True when @p hwnd belongs to one of the shell hosts that use the
   *        generic Windows.UI.Core.CoreWindow class.
   *
   * Windows.UI.Core.CoreWindow is also the class of ordinary UWP applications
   * (Settings, Calculator, Store apps), so the class name alone is not a valid
   * shell filter. Only the specific shell processes are excluded here.
   */
  static bool
  is_shell_core_window(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
      return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
      return false;
    }

    wchar_t image_path[MAX_PATH] = {};
    DWORD image_path_size = static_cast<DWORD>(std::size(image_path));
    const bool queried = QueryFullProcessImageNameW(process, 0, image_path, &image_path_size) != FALSE;
    CloseHandle(process);
    if (!queried || image_path_size == 0) {
      return false;
    }

    std::wstring_view image_name { image_path, image_path_size };
    if (const auto slash = image_name.find_last_of(L"\\/"); slash != std::wstring_view::npos) {
      image_name.remove_prefix(slash + 1);
    }

    std::wstring lower_name { image_name };
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::towlower);

    static constexpr std::wstring_view shell_processes[] = {
      L"explorer.exe",
      L"startmenuexperiencehost.exe",
      L"shellexperiencehost.exe",
      L"searchhost.exe",
      L"searchapp.exe",
      L"textinputhost.exe",
      L"lockapp.exe",
      L"peopleexperiencehost.exe",
    };
    for (const auto shell_process : shell_processes) {
      if (lower_name == shell_process) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Plan4: decide whether a top-level window may be followed by the
   *        per-session window-follow capture.
   *
   * Conservative by design: shell surfaces, tool windows, cloaked windows,
   * minimized windows and Sunshine's own process are never followed.
   */
  static bool
  is_window_capture_eligible(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
      return false;
    }

    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex_style & WS_EX_TOOLWINDOW) != 0) {
      return false;
    }

    if (GetWindowTextLengthW(hwnd) <= 0) {
      return false;
    }

    RECT rect {};
    if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
      return false;
    }

    // Skip DWM-cloaked surfaces: they have no user-visible content.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
      return false;
    }

    wchar_t class_name[256] = {};
    if (GetClassNameW(hwnd, class_name, std::size(class_name)) > 0) {
      static constexpr std::wstring_view shell_classes[] = {
        L"Progman",
        L"WorkerW",
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"TaskListThumbnailWnd",
        L"MultitaskingViewFrame",
        L"ForegroundStaging",
      };
      const std::wstring_view cls { class_name };
      for (const auto shell_class : shell_classes) {
        if (cls == shell_class) {
          return false;
        }
      }

      // CoreWindow is shared by the shell and ordinary UWP apps; filter only
      // the known shell hosts instead of rejecting every UWP window.
      if (cls == L"Windows.UI.Core.CoreWindow" && is_shell_core_window(hwnd)) {
        return false;
      }
    }

    return true;
  }

  /**
   * @brief Plan4: first eligible window owned by @p process_id, preferring the
   *        current foreground window.
   */
  static HWND
  find_window_for_process_id(DWORD process_id) {
    if (process_id == 0) {
      return nullptr;
    }

    struct enum_state_t {
      DWORD process_id;
      HWND foreground_match;
      HWND first_match;
    } state { process_id, nullptr, nullptr };

    EnumWindows(
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        auto *data = reinterpret_cast<enum_state_t *>(lparam);
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid != data->process_id || !is_window_capture_eligible(hwnd)) {
          return TRUE;
        }
        if (!data->first_match) {
          data->first_match = hwnd;
        }
        if (GetForegroundWindow() == hwnd) {
          data->foreground_match = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&state));

    return state.foreground_match ? state.foreground_match : state.first_match;
  }

  /**
   * @brief Plan4: eligible foreground top-level window, or nullptr.
   */
  static HWND
  find_eligible_foreground_window() {
    HWND foreground = GetForegroundWindow();
    if (is_window_capture_eligible(foreground)) {
      return foreground;
    }

    // A child/owned popup can be reported as foreground; try its root window.
    HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    if (root && root != foreground && is_window_capture_eligible(root)) {
      return root;
    }
    return nullptr;
  }

  /**
   * @brief Plan4: creation time of @p process_id in Unix-epoch milliseconds,
   *        or 0 when it cannot be queried (process gone / access denied).
   */
  static std::int64_t
  process_start_unix_ms(DWORD process_id) {
    if (process_id == 0) {
      return 0;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
      return 0;
    }

    FILETIME creation {}, exit {}, kernel {}, user {};
    std::int64_t result = 0;
    if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
      ULARGE_INTEGER ticks {};
      ticks.LowPart = creation.dwLowDateTime;
      ticks.HighPart = creation.dwHighDateTime;
      // FILETIME counts 100ns intervals since 1601-01-01 UTC.
      constexpr std::uint64_t unix_epoch_offset_100ns = 116444736000000000ULL;
      if (ticks.QuadPart > unix_epoch_offset_100ns) {
        result = static_cast<std::int64_t>((ticks.QuadPart - unix_epoch_offset_100ns) / 10000ULL);
      }
    }

    CloseHandle(process);
    return result;
  }

  /**
   * @brief Plan4: true when @p process_id is a descendant of @p root_process_id.
   */
  static bool
  is_process_descendant_of(DWORD root_process_id, DWORD process_id) {
    if (root_process_id == 0 || process_id == 0 || root_process_id == process_id) {
      return false;
    }

    std::vector<std::pair<DWORD, DWORD>> pid_to_parent;  // pid -> parent pid
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return false;
    }

    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
      do {
        pid_to_parent.emplace_back(
          static_cast<DWORD>(entry.th32ProcessID),
          static_cast<DWORD>(entry.th32ParentProcessID));
      } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    auto find_parent = [&pid_to_parent](DWORD pid) -> DWORD {
      for (const auto &pair : pid_to_parent) {
        if (pair.first == pid) {
          return pair.second;
        }
      }
      return 0;
    };

    DWORD current = process_id;
    for (int guard = 0; guard < 64; ++guard) {
      const DWORD parent = find_parent(current);
      if (parent == 0) {
        return false;
      }
      if (parent == root_process_id) {
        return true;
      }
      current = parent;
    }
    return false;
  }

  /**
   * @brief Plan4: process filter for window-follow candidates.
   *
   * Only the Sunshine-launched application process tree, plus processes started
   * around/after the override was enabled, may be followed. This keeps a
   * game-exit transient (desktop, browser, launcher or any unrelated app) from
   * being streamed to the client.
   */
  static bool
  is_override_pid_allowed(DWORD process_id, std::uint32_t root_process_id, std::int64_t enabled_at_unix_ms) {
    if (process_id == 0) {
      return false;
    }

    if (root_process_id != 0 &&
        (process_id == root_process_id || is_process_descendant_of(root_process_id, process_id))) {
      return true;
    }

    if (enabled_at_unix_ms > 0) {
      const auto started_at = process_start_unix_ms(process_id);
      // 10 seconds of slack: the game may have started slightly before the
      // control client's (delayed) enable request.
      if (started_at != 0 && started_at + 10000 >= enabled_at_unix_ms) {
        return true;
      }
    }

    return false;
  }

  /**
   * @brief Plan4: topmost eligible top-level window in Z-order, optionally
   *        restricted to descendants of the Sunshine-launched app process.
   */
  static HWND
  find_topmost_override_window(std::uint32_t root_process_id, std::int64_t enabled_at_unix_ms, bool descendants_only) {
    struct state_t {
      std::uint32_t root_process_id;
      std::int64_t enabled_at_unix_ms;
      bool descendants_only;
      HWND result;
    } state { root_process_id, enabled_at_unix_ms, descendants_only, nullptr };

    EnumWindows(
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        auto *data = reinterpret_cast<state_t *>(lparam);
        if (!is_window_capture_eligible(hwnd)) {
          return TRUE;
        }
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (!is_override_pid_allowed(window_pid, data->root_process_id, data->enabled_at_unix_ms)) {
          return TRUE;
        }
        if (data->descendants_only) {
          if (data->root_process_id == 0 || window_pid == data->root_process_id ||
              !is_process_descendant_of(data->root_process_id, window_pid)) {
            return TRUE;
          }
        }
        data->result = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&state));

    return state.result;
  }

  /**
   * @brief Plan4: eligible foreground window for window-follow mode.
   *
   * The game window (a descendant of the Sunshine-launched app) is preferred
   * over the launcher's own fullscreen window: capturing Playnite itself looks
   * like the whole screen and made the capture flap between Playnite and the
   * game. When there is no foreground window at all (locked/disconnected RDP
   * session), the topmost eligible window in Z-order is used instead.
   */
  static HWND
  find_override_foreground_window(std::uint32_t root_process_id, std::int64_t enabled_at_unix_ms) {
    HWND child_window = find_topmost_override_window(root_process_id, enabled_at_unix_ms, true);

    HWND candidate = find_eligible_foreground_window();
    if (candidate) {
      DWORD candidate_pid = 0;
      GetWindowThreadProcessId(candidate, &candidate_pid);
      if (is_override_pid_allowed(candidate_pid, root_process_id, enabled_at_unix_ms)) {
        const bool is_launcher = root_process_id != 0 && candidate_pid == root_process_id;
        return is_launcher && child_window ? child_window : candidate;
      }
    }

    if (child_window) {
      return child_window;
    }

    return find_topmost_override_window(root_process_id, enabled_at_unix_ms, false);
  }

  /**
   * @brief Plan4: resolve the requested window-follow target.
   *
   * Returns nullptr when no eligible window is currently available; callers
   * fall back to display capture and retry later.
   */
  static HWND
  resolve_window_override_target(
    const streaming::window_capture_override_t &override,
    std::uint32_t root_process_id,
    std::int64_t enabled_at_unix_ms) {
    switch (override.mode) {
      case streaming::window_capture_mode_e::title: {
        if (override.window_title.empty()) {
          return find_override_foreground_window(root_process_id, enabled_at_unix_ms);
        }
        HWND hwnd = find_window_by_title(override.window_title);
        return is_window_capture_eligible(hwnd) ? hwnd : nullptr;
      }
      case streaming::window_capture_mode_e::pid:
        return find_window_for_process_id(override.process_id);
      case streaming::window_capture_mode_e::foreground:
      default:
        return find_override_foreground_window(root_process_id, enabled_at_unix_ms);
    }
  }

  /**
   * @brief Plan4: try to bring a captured window to the foreground.
   *
   * Sunshine is a background process, so Windows' foreground lock normally
   * rejects SetForegroundWindow(). run_command() grants ASFW_ANY before spawning
   * the app, but that does not cover windows created later, so retry here with
   * the AttachThreadInput helper. Mouse and gamepad input follow the foreground
   * window, therefore the captured game window must become foreground for the
   * client to control it.
   */
  static void
  focus_capture_window(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
      return;
    }

    if (IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_RESTORE);
    }

    // Locked/disconnected sessions reject SetForegroundWindow(); raising the
    // window in the Z-order still makes it visible in display capture and lets
    // the injected mouse/gamepad input reach it in active sessions.
    BringWindowToTop(hwnd);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    if (SetForegroundWindow(hwnd)) {
      return;
    }

    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD current_thread = GetCurrentThreadId();
    bool attached = false;
    if (foreground_thread != 0 && foreground_thread != current_thread) {
      attached = AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
    }

    SetForegroundWindow(hwnd);

    if (attached) {
      AttachThreadInput(current_thread, foreground_thread, FALSE);
    }
  }

  /**
   * @brief Plan4: publish the actually captured window into the session state
   *        so control/status requests can report it.
   */
  static void
  fill_observed_window(HWND hwnd, streaming::window_capture_observed_t &observed) {
    observed = {};
    if (!hwnd) {
      return;
    }
    observed.active = true;
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, std::size(title));
    observed.window_title = platf::to_utf8(title);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    observed.process_id = static_cast<std::uint32_t>(pid);
    observed.hwnd = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));
  }

  /**
   * @brief Query the visible window rectangle in physical virtual-screen pixels.
   *
   * Sunshine opts into per-monitor DPI awareness before creating the display
   * backend, so GetWindowRect()/DWM return physical pixels even when the window
   * moves to a monitor with a different scale. DWMWA_EXTENDED_FRAME_BOUNDS
   * excludes the invisible resize border and better matches what WGC captures.
   */
  static bool
  query_window_screen_rect(HWND hwnd, RECT &rect) {
    if (!hwnd || !IsWindow(hwnd)) {
      return false;
    }

    RECT candidate {};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &candidate, sizeof(candidate))) &&
        candidate.right > candidate.left && candidate.bottom > candidate.top) {
      rect = candidate;
    }
    else if (!GetWindowRect(hwnd, &candidate) ||
             candidate.right <= candidate.left || candidate.bottom <= candidate.top) {
      return false;
    }
    else {
      rect = candidate;
    }

    const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtual_width <= 0 || virtual_height <= 0) {
      return false;
    }

    // Clamp to the current virtual desktop. A window straddling monitors must
    // never produce an absolute pointer outside the normalized 0..65535 range.
    rect.left = std::clamp<LONG>(rect.left, virtual_x, virtual_x + virtual_width);
    rect.top = std::clamp<LONG>(rect.top, virtual_y, virtual_y + virtual_height);
    rect.right = std::clamp<LONG>(rect.right, virtual_x, virtual_x + virtual_width);
    rect.bottom = std::clamp<LONG>(rect.bottom, virtual_y, virtual_y + virtual_height);
    return rect.right > rect.left && rect.bottom > rect.top;
  }

  void
  wgc_capture_t::refresh_window_capture_rect() {
    if (!display) {
      return;
    }

    if (!captured_window_hwnd) {
      std::lock_guard lock(display->window_capture_rect_mutex);
      display->has_window_capture_rect = false;
      display->window_capture_rect = {};
      return;
    }

    RECT rect {};
    if (!query_window_screen_rect(captured_window_hwnd, rect)) {
      // Keep the last known rectangle while the window is temporarily
      // unqueryable; is_window_valid() will request a reinit if it disappears.
      return;
    }

    const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);

    std::lock_guard lock(display->window_capture_rect_mutex);
    display->window_capture_rect.offset_x = rect.left - virtual_x;
    display->window_capture_rect.offset_y = rect.top - virtual_y;
    display->window_capture_rect.width = rect.right - rect.left;
    display->window_capture_rect.height = rect.bottom - rect.top;
    display->has_window_capture_rect = display->window_capture_rect.valid();

    // Keep the absolute-input environment in sync with monitor topology changes
    // (hot-plug, resolution change, DPI change) while the window is captured.
    display->env_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    display->env_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  }

  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;

    if (config::video.wgc_disable_secure_desktop) {
      secure_desktop::restore();
    }
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int
  wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    this->display = display;
    frame_counter = display->frame_rate_source();
    frame_counter->kind.store(perf::capture_source_kind_e::wgc_arrivals, std::memory_order_relaxed);
    display->captures_monitor = true;
    {
      std::lock_guard lock(display->window_capture_rect_mutex);
      display->has_window_capture_rect = false;
      display->window_capture_rect = {};
    }
    // WGC is not supported in service mode (running as SYSTEM user)
    // Fail fast to avoid unnecessary attempts and potential deadlocks
    if (is_running_as_system_user) {
      BOOST_LOG(error) << "WGC capture is not available in service mode (running as SYSTEM user). Use DDX capture instead."sv;
      return -1;
    }

    // IsSupported() throws when the activation factory is missing (a Windows edition without the media features, a broken installation): the host ended in
    // std::terminate on it, the caller falls back to the other capture when this returns -1
    bool supported = false;
    try {
      supported = winrt::GraphicsCaptureSession::IsSupported();
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not available on this device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    if (!supported) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
      return -1;
    }

    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;

    if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
      BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    try {
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    // as<>() throws when the object cannot be queried (a device that was lost, an older Windows build): nothing above this line catches it, and the exception ended the host
    uwp_device = d3d_comhandle.try_as<winrt::IDirect3DDevice>();
    if (uwp_device == nullptr) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: the device has no WinRT Direct3D interface"sv;
      return -1;
    }

    auto capture_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (capture_factory == nullptr) {
      BOOST_LOG(error) << "Failed to get GraphicsCaptureItem factory"sv;
      return -1;
    }

    // Determine capture target: window or display
    bool capture_window = false;
    std::string window_title;

    // Priority 0 (Plan4): per-session window-follow override. Session-scoped:
    // only this display instance is affected, global settings stay untouched.
    window_override = config.capture_target ? config.capture_target->window_override()
                                           : streaming::window_capture_override_t {};
    window_override_active = window_override.enabled;
    capture_target_handle = config.capture_target;
    override_root_pid = proc::proc.get_app_pid();
    override_enabled_at_unix_ms = capture_target_handle ? capture_target_handle->window_override_enabled_at_unix_ms() : 0;
    override_poll_time = std::chrono::steady_clock::now();
    override_last_switch = std::chrono::steady_clock::time_point {};

    if (window_override_active) {
      capture_window = true;
      window_title = window_override.mode == streaming::window_capture_mode_e::title ? window_override.window_title : std::string {};
      desired_window_title = window_title;
    }
    // Priority 1: Check capture_target configuration from global config
    else if (config::video.capture_target == "window") {
      capture_window = true;
      window_title = config::video.window_title;
      desired_window_title = window_title;

      // If window_title is empty, try to derive from running app
      if (window_title.empty()) {
        int running_app_id = proc::proc.running();
        if (running_app_id > 0) {
          // Try to extract from the app command (executable path)
          std::string app_cmd = proc::proc.get_app_cmd(running_app_id);
          if (!app_cmd.empty()) {
            std::vector<std::string> parts;
            try {
              parts = boost::program_options::split_winmain(app_cmd);
            }
            catch (...) {
              // Ignore parsing errors
            }

            if (!parts.empty() && parts[0].find("://") == std::string::npos) {
              std::string exe_path = parts[0];
              size_t last_slash = exe_path.find_last_of("/\\");
              std::string filename = (last_slash != std::string::npos) ? exe_path.substr(last_slash + 1) : exe_path;
              size_t last_dot = filename.find_last_of('.');
              window_title = (last_dot != std::string::npos) ? filename.substr(0, last_dot) : filename;

              if (!window_title.empty()) {
                BOOST_LOG(info) << "Window title not specified, using executable filename: "sv << logging::quoted_name(window_title) << " (from: "sv << logging::quoted_name(app_cmd) << ")";
              }
            }
          }

          // Fallback to app name if still empty
          if (window_title.empty()) {
            window_title = proc::proc.get_app_name(running_app_id);
            if (!window_title.empty()) {
              BOOST_LOG(info) << "Window title not specified, using app name: "sv << logging::quoted_name(window_title);
            }
          }

          if (!window_title.empty()) {
            desired_window_title = window_title;
          }
        }
      }
    }
    // Priority 2: Check "window:" prefix in display_name (backward compatibility)
    else if (config.display_name.length() > 7 && config.display_name.substr(0, 7) == "window:") {
      capture_window = true;
      window_title = config.display_name.substr(7);
      desired_window_title = window_title;
    }
    else {
      desired_window_title.clear();
    }

    if (capture_window) {
      if (window_override_active) {
        // Short retry window: capture must not block the stream for long.
        // When the game window is not up yet we fall back to display capture;
        // poll_window_override() reinitializes as soon as a window appears.
        constexpr int max_override_retries = 4;
        constexpr int override_retry_interval_ms = 250;
        HWND target_hwnd = nullptr;
        for (int retry = 0; retry < max_override_retries; ++retry) {
          target_hwnd = resolve_window_override_target(window_override, override_root_pid, override_enabled_at_unix_ms);
          if (target_hwnd) {
            break;
          }
          if (retry < max_override_retries - 1) {
            Sleep(override_retry_interval_ms);
          }
        }

        if (!target_hwnd) {
          BOOST_LOG(info) << "Window-follow: no eligible window yet; using display capture until one appears (mode="
                          << static_cast<int>(window_override.mode) << ')';
          capture_window = false;

          streaming::window_capture_observed_t observed;
          observed.fallback_to_display = true;
          observed.fallback_reason =
            window_override.mode == streaming::window_capture_mode_e::title ? "window_not_found"
            : (window_override.mode == streaming::window_capture_mode_e::pid ? "process_window_not_found"
                                                                            : "no_eligible_foreground_window");
          if (capture_target_handle) {
            capture_target_handle->set_observed_window(observed);
          }
        }
        else {
          wchar_t actual_title[256] = {};
          GetWindowTextW(target_hwnd, actual_title, std::size(actual_title));
          DWORD target_pid = 0;
          GetWindowThreadProcessId(target_hwnd, &target_pid);
          BOOST_LOG(info) << "Window-follow: capturing window ["sv << platf::to_utf8(actual_title)
                          << "] pid="sv << target_pid << " (mode="sv << static_cast<int>(window_override.mode) << ')';

          // Deliberately do NOT maximize or force foreground here: the point of
          // the override is to leave the host desktop interaction untouched
          // while the client still receives the window fullscreen.
          if (FAILED(status = capture_factory->CreateForWindow(target_hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
            BOOST_LOG(error) << "Window-follow: failed to create capture item for window [0x"
                            << util::hex(status).to_string_view() << "], falling back to display capture"sv;
            capture_window = false;
            streaming::window_capture_observed_t observed;
            observed.fallback_to_display = true;
            observed.fallback_reason = "create_capture_item_failed";
            if (capture_target_handle) {
              capture_target_handle->set_observed_window(observed);
            }
          }
          else {
            captured_window_hwnd = target_hwnd;
            display->captures_monitor = false;
            refresh_window_capture_rect();
            // The client's mouse/gamepad input is routed to the foreground
            // window; make the captured game window the foreground window.
            focus_capture_window(target_hwnd);
            auto window_size = item.Size();
            window_capture_width = static_cast<int>(window_size.Width);
            window_capture_height = static_cast<int>(window_size.Height);
            BOOST_LOG(info) << "Window-follow: window capture initialized ("sv << window_capture_width << 'x' << window_capture_height
                            << ", display "sv << display->width << 'x' << display->height << ')';
            streaming::window_capture_observed_t observed;
            fill_observed_window(target_hwnd, observed);
            if (capture_target_handle) {
              capture_target_handle->set_observed_window(observed);
            }
          }
        }
      }
      else if (window_title.empty()) {
        BOOST_LOG(warning) << "Window capture requested but window_title is empty and no app is running. Falling back to display capture."sv;
        capture_window = false;
      }
      else {
        // Retry window lookup with timeout - window might be starting up
        constexpr int max_retries = 20;
        constexpr int retry_interval_ms = 500;
        HWND target_hwnd = nullptr;

        for (int retry = 0; retry < max_retries; ++retry) {
          target_hwnd = find_window_by_title(window_title);
          if (target_hwnd && IsWindow(target_hwnd) && IsWindowVisible(target_hwnd) && !IsIconic(target_hwnd)) {
            break;
          }
          target_hwnd = nullptr;

          if (retry < max_retries - 1) {
            BOOST_LOG(info) << "Window not found yet: "sv << logging::quoted_name(window_title) << ", retrying in "sv << retry_interval_ms << "ms ("sv << (retry + 1) << "/"sv << max_retries << ")..."sv;
            Sleep(retry_interval_ms);
          }
        }

        if (!target_hwnd) {
          BOOST_LOG(warning) << "Window not found or invalid after "sv << max_retries << " attempts: "sv << logging::quoted_name(window_title) << ". Falling back to display capture."sv;
          capture_window = false;
        }
        else {
          wchar_t actual_title[256] = {};
          GetWindowTextW(target_hwnd, actual_title, std::size(actual_title));
          BOOST_LOG(info) << "Capturing window: "sv << logging::quoted_name(platf::to_utf8(actual_title)) << " (searched for: "sv << logging::quoted_name(window_title) << ")";

          // Maximize the window first for better capture experience
          if (!IsZoomed(target_hwnd)) {
            BOOST_LOG(info) << "Maximizing window for capture..."sv;
            ShowWindow(target_hwnd, SW_MAXIMIZE);
            Sleep(500);
          }

          SetForegroundWindow(target_hwnd);
          Sleep(100);

          if (FAILED(status = capture_factory->CreateForWindow(target_hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
            BOOST_LOG(error) << "Failed to create capture item for window [0x"sv << util::hex(status).to_string_view() << ']';
            return -1;
          }

          captured_window_hwnd = target_hwnd;
          // A window can move between monitors while this capture stays open.
          // It has no fixed current monitor for display-scale control.
          display->captures_monitor = false;
          refresh_window_capture_rect();

          auto window_size = item.Size();
          window_capture_width = static_cast<int>(window_size.Width);
          window_capture_height = static_cast<int>(window_size.Height);

          // Log window details for debugging
          RECT window_rect = {}, client_rect = {};
          if (GetWindowRect(target_hwnd, &window_rect) && GetClientRect(target_hwnd, &client_rect)) {
            BOOST_LOG(info) << "Window geometry - Window: "sv
                            << (window_rect.right - window_rect.left) << 'x' << (window_rect.bottom - window_rect.top)
                            << ", Client: "sv << (client_rect.right - client_rect.left) << 'x' << (client_rect.bottom - client_rect.top)
                            << ", WGC initial: "sv << window_capture_width << 'x' << window_capture_height
                            << ", Display: "sv << display->width << 'x' << display->height;
          }

          BOOST_LOG(info) << "Window capture initialized with size: "sv << window_capture_width << 'x' << window_capture_height;
        }
      }
    }

    // If not capturing window (either not requested or fallback), capture display
    if (!capture_window) {
      captured_window_hwnd = nullptr;  // Not capturing a window
      display->captures_monitor = true;
      {
        std::lock_guard lock(display->window_capture_rect_mutex);
        display->has_window_capture_rect = false;
        display->window_capture_rect = {};
      }
      window_capture_width = 0;
      window_capture_height = 0;
      if (display->output == nullptr) {
        BOOST_LOG(error) << "Display output is null, cannot capture monitor"sv;
        return -1;
      }
      DXGI_OUTPUT_DESC output_desc;
      display->output->GetDesc(&output_desc);
      BOOST_LOG(info) << "Capturing display: ["sv << platf::to_utf8(output_desc.DeviceName) << ']';
      if (FAILED(status = capture_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    }

    display->capture_format =
      select_wgc_capture_format(config.effective_frame_pipeline_policy().capture);

    // Use the actual capture item size for frame pool creation
    auto item_size = item.Size();

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item_size);
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({ this, &wgc_capture_t::on_frame_arrived });
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      }
      else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }

    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);
      }
      else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may be capped to 60fps on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }

    try {
      capture_session.StartCapture();
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    // Disable UAC prompts so WGC can operate without interruption (if enabled in config)
    if (config::video.wgc_disable_secure_desktop) {
      secure_desktop::disable();
    }
    else {
      BOOST_LOG(info) << "WGC capture active. UAC prompts will not be auto-elevated. "
                         "Set wgc_disable_secure_desktop=true in config to auto-elevate UAC during WGC capture."sv;
    }

    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void
  wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    const auto source_collection_epoch = perf::frame_rate_collection_epoch();
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame { nullptr };
    try {
      frame = sender.TryGetNextFrame();
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      // Count arrivals before the latest-frame mailbox can overwrite a frame.
      frame_counter->record(1, source_collection_epoch);
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e
  wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    refresh_window_capture_rect();
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      }
      else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    // the surface and the time of a frame throw when the device was lost under the capture; the capture thread handles an error by starting over
    try {
      auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
      if (capture_access == nullptr) {
        return capture_e::error;
      }
      capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
      out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to read the captured frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      // A lost or reset device and a frame that was closed under the capture are recoverable: the capture starts over. Anything else stops it.
      switch (static_cast<std::int32_t>(e.code())) {
        case static_cast<std::int32_t>(DXGI_ERROR_DEVICE_REMOVED):
        case static_cast<std::int32_t>(DXGI_ERROR_DEVICE_RESET):
        case static_cast<std::int32_t>(DXGI_ERROR_DEVICE_HUNG):
        case static_cast<std::int32_t>(DXGI_ERROR_DRIVER_INTERNAL_ERROR):
        case static_cast<std::int32_t>(DXGI_ERROR_ACCESS_LOST):
        case static_cast<std::int32_t>(0x80000013L):  // RO_E_CLOSED
          return capture_e::reinit;
        default:
          return capture_e::error;
      }
    }
    return capture_e::ok;
  }

  capture_e
  wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int
  wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    }
    catch (winrt::hresult_error &) {
      return -1;
    }
  }

  bool
  wgc_capture_t::is_window_valid() const {
    // If not capturing a window, always return true
    if (captured_window_hwnd == nullptr) {
      return true;
    }
    // Check if window is still valid
    if (IsWindow(captured_window_hwnd) == FALSE) {
      return false;
    }
    // Check if window is minimized - minimized windows may not capture properly
    if (IsIconic(captured_window_hwnd) != FALSE) {
      return false;
    }
    // Check if window is visible - invisible windows may not capture properly
    if (IsWindowVisible(captured_window_hwnd) == FALSE) {
      return false;
    }
    return true;
  }

  capture_e
  wgc_capture_t::poll_window_override() {
    if (!window_override_active) {
      return capture_e::ok;
    }

    // Foreground/window probing is cheap but this runs per captured frame, so
    // keep it at a few Hz. A short debounce prevents flapping between windows
    // (e.g. transient popups) during reinitialization.
    constexpr auto poll_interval = std::chrono::milliseconds(250);
    constexpr auto switch_debounce = std::chrono::milliseconds(500);
    const auto now = std::chrono::steady_clock::now();
    if (now - override_poll_time < poll_interval) {
      return capture_e::ok;
    }
    override_poll_time = now;

    HWND desired = resolve_window_override_target(window_override, override_root_pid, override_enabled_at_unix_ms);

    if (captured_window_hwnd != nullptr) {
      // Follow window changes only when a different, eligible window exists.
      // Keeping the current window is the safe default when the requested
      // target temporarily disappears (alt-tab, UAC prompt, etc).
      if (desired != nullptr && desired != captured_window_hwnd && now - override_last_switch >= switch_debounce) {
        wchar_t title[256] = {};
        GetWindowTextW(desired, title, std::size(title));
        BOOST_LOG(info) << "Window-follow: switching captured window to ["sv << platf::to_utf8(title) << ']';
        override_last_switch = now;
        return capture_e::reinit;
      }
      return capture_e::ok;
    }

    // Currently falling back to display capture: reinitialize as soon as an
    // eligible window exists so the client switches from desktop to the game.
    if (desired != nullptr && now - override_last_switch >= switch_debounce) {
      override_last_switch = now;
      BOOST_LOG(info) << "Window-follow: window available again, reinitializing window capture"sv;
      return capture_e::reinit;
    }

    return capture_e::ok;
  }

  std::shared_ptr<img_t>
  display_wgc_ram_t::alloc_img() {
    auto img = std::make_shared<img_t>();

    // For window capture, use window capture dimensions; for display capture, use display dimensions
    int img_width = dup.window_capture_width > 0 ? dup.window_capture_width : width;
    int img_height = dup.window_capture_height > 0 ? dup.window_capture_height : height;

    img->width = img_width;
    img->height = img_height;
    img->pixel_pitch = get_pixel_pitch();
    img->row_pitch = img->pixel_pitch * img->width;
    img->data = nullptr;

    return img;
  }

  int
  display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    // WGC frames are typically delivered in the current display orientation.
    // If we keep DXGI rotation enabled here, later stages may apply an extra rotation,
    // causing flipped/upside-down/stretched output on client after a monitor rotation.
    if (display_rotation != DXGI_MODE_ROTATION_UNSPECIFIED &&
        display_rotation != DXGI_MODE_ROTATION_IDENTITY) {
      BOOST_LOG(info) << "WGC: disabling DXGI rotation handling for oriented frames";
      display_rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
      width_before_rotation = width;
      height_before_rotation = height;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e
  display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    // Plan4: react to window-follow target changes before touching the frame
    // queue; reinit lets the capture thread rebuild the backend.
    if (const auto override_status = dup.poll_window_override(); override_status != capture_e::ok) {
      return override_status;
    }

    // Check if window is still valid (if capturing a window)
    if (!dup.is_window_valid()) {
      BOOST_LOG(warning) << "Captured window is no longer valid, reinitializing capture"sv;
      return capture_e::reinit;
    }

    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      // If we're capturing a window and getting timeouts/errors, check if window is still valid
      if (dup.captured_window_hwnd != nullptr) {
        // Simplified: Any error or timeout means window might have changed, check validity
        if (!dup.is_window_valid()) {
          BOOST_LOG(warning) << "Captured window is no longer valid, reinitializing capture"sv;
          return capture_e::reinit;
        }
      }
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Get the actual captured frame dimensions
    int frame_width = static_cast<int>(desc.Width);
    int frame_height = static_cast<int>(desc.Height);

    // For window capture, a size change invalidates the staging texture, the
    // image pool and the encoder/SWS session dimensions. Never mutate img in
    // place: the memory pool may still contain buffers for the old height, and
    // a same-width/increased-height frame would then write past the allocation.
    if (dup.captured_window_hwnd != nullptr) {
      if (wgc_window_size_requires_reinit(
            dup.window_capture_width, dup.window_capture_height,
            frame_width, frame_height)) {
        BOOST_LOG(info) << "Window capture size changed: "sv << dup.window_capture_width << 'x' << dup.window_capture_height
                        << " -> "sv << frame_width << 'x' << frame_height << ", reinitializing capture"sv;
        return capture_e::reinit;
      }
    }
    else {
      // For display capture, check against display dimensions
      // WGC frames are typically in display orientation (after rotation),
      // so compare against `width`/`height` to avoid reinit loops on rotation.
      if (frame_width != width || frame_height != height) {
        BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << frame_width << 'x' << frame_height << ']';
        return capture_e::reinit;
      }
    }

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';
      BOOST_LOG(info) << "Creating staging texture: "sv << frame_width << 'x' << frame_height;

      D3D11_TEXTURE2D_DESC t {};
      t.Width = frame_width;
      t.Height = frame_height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // Check if the captured frame size matches our staging texture
    D3D11_TEXTURE2D_DESC staging_desc;
    texture->GetDesc(&staging_desc);

    if (frame_width != static_cast<int>(staging_desc.Width) || frame_height != static_cast<int>(staging_desc.Height)) {
      BOOST_LOG(info) << "Capture size mismatch - frame: "sv << frame_width << 'x' << frame_height
                      << ", staging: "sv << staging_desc.Width << 'x' << staging_desc.Height
                      << ", recreating staging texture"sv;
      // Reset texture to force recreation with new size on next iteration
      texture.reset();
      // Don't reinit the whole capture, just recreate the staging texture next frame
      return capture_e::timeout;
    }

    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    // Copy data using actual frame dimensions
    std::copy_n((std::uint8_t *) img_info.pData, frame_height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e
  display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
