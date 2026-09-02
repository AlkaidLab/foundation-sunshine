/**
 * @file src/platform/windows/touch_keyboard_session.cpp
 * @brief Windows implementation of the session-scoped touch keyboard
 *        AutoInvoke registry transaction.
 *
 * Sunshine may run elevated (SYSTEM service) or as a plain user process:
 * - plain user: values are written through HKCU;
 * - SYSTEM: the active console session user is resolved and values are
 *   written into HKEY_USERS\<sid>, because the service's own HKCU belongs
 *   to S-1-5-18 and is invisible to the user's shell.
 *
 * Undo journal: platf::appdata()/touch_keyboard_undo.json captures the
 * pre-session state of every touched value (including "did not exist") so
 * an abnormal termination can be repaired by the next end_session() call.
 */

#include "src/touch_keyboard_session.h"

#include "src/logging.h"
#include "src/platform/common.h"

#include <nlohmann/json.hpp>

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <Windows.h>
#include <sddl.h>
#include <WtsApi32.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace touch_kb {

  namespace {

    constexpr auto REG_SUBKEY = L"SOFTWARE\\Microsoft\\TabletTip\\1.7";

    struct key_def {
      const wchar_t *name;
      DWORD desired;
      // Desired value for keys whose semantics are enums rather than flags.
      // PreventTouchKeyboardAutoInvoke stays 0 (neutral) and is only written
      // when it was non-zero before the session.
      bool write_only_if_previously_present = false;
    };

    // Full-variable-isolation ablation (2026-09-02): with all of these keys
    // zeroed and TabTip restarted, the attached virtual touchscreen no longer
    // auto-invokes the touch keyboard; restoring them restores invocation.
    constexpr key_def KEYS[] = {
      { L"EnableDesktopModeAutoInvoke", 1 },
      { L"EnableDesktopModeDockedAutoInvoke", 1 },
      { L"EnableInDesktopMode", 1 },
      { L"EnableInDesktopModeTabletKeyboard", 1 },
      { L"EnableInDesktopModeAutoInvoke", 1 },
      { L"EnableDockedModeAutoInvoke", 1 },
      { L"EnableTouchKeyboardAutoInvokeInDesktopMode", 1 },
      { L"EnableTouchKeyboardAutoInvoke", 1 },
      { L"TouchKeyboardTapInvoke", 2 },
      { L"PreventTouchKeyboardAutoInvoke", 0, true },
    };

    struct captured_value {
      bool exists;
      DWORD value;
    };

    std::wstring
    to_wstring(const std::string &utf8) {
      if (utf8.empty()) {
        return {};
      }
      int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), nullptr, 0);
      std::wstring out(size, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), out.data(), size);
      return out;
    }

    std::string
    to_utf8(const std::wstring &wide) {
      if (wide.empty()) {
        return {};
      }
      int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int) wide.size(), nullptr, 0, nullptr, nullptr);
      std::string out(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int) wide.size(), out.data(), size, nullptr, nullptr);
      return out;
    }

    bool
    running_as_system() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
      }
      BYTE sid[SECURITY_MAX_SID_SIZE];
      DWORD size = sizeof(sid);
      BOOL result = GetTokenInformation(token, TokenUser, sid, size, &size);
      CloseHandle(token);
      if (!result) {
        return false;
      }
      return IsWellKnownSid(sid, WinLocalSystemSid);
    }

    struct hive_t {
      HKEY root;
      std::wstring prefix;  // "HKEY_USERS\<sid>" form for the undo journal
    };

    /**
     * Registry root plus per-user prefix of the active console session user.
     * The prefix is empty when running as the interactive user (HKCU).
     */
    std::optional<hive_t>
    resolve_hive() {
      const DWORD session_id = WTSGetActiveConsoleSessionId();
      if (session_id == 0xFFFFFFFF) {
        return std::nullopt;
      }

      LPWSTR user_buf = nullptr;
      LPWSTR domain_buf = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, WTSUserName, &user_buf, &bytes)) {
        BOOST_LOG(warning) << "touch_kb: WTSQuerySessionInformation(user) failed: " << GetLastError();
        return std::nullopt;
      }
      WTSFreeMemory(user_buf);
      // Re-query for the domain (each query returns its own buffer).
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, WTSDomainName, &domain_buf, &bytes)) {
        BOOST_LOG(warning) << "touch_kb: WTSQuerySessionInformation(domain) failed: " << GetLastError();
        return std::nullopt;
      }
      std::wstring domain = domain_buf;
      WTSFreeMemory(domain_buf);

      // Re-query the user name: the first buffer was freed.
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id, WTSUserName, &user_buf, &bytes)) {
        return std::nullopt;
      }
      std::wstring user = user_buf;
      WTSFreeMemory(user_buf);

      BYTE sid[SECURITY_MAX_SID_SIZE];
      DWORD sid_size = sizeof(sid);
      wchar_t referenced_domain[256];
      DWORD ref_size = 256;
      SID_NAME_USE use;
      const std::wstring account = domain + L"\\" + user;
      if (!LookupAccountNameW(nullptr, account.c_str(), sid, &sid_size,
                              referenced_domain, &ref_size, &use)) {
        BOOST_LOG(warning) << "touch_kb: LookupAccountName failed: " << GetLastError();
        return std::nullopt;
      }

      LPWSTR sid_string = nullptr;
      if (!ConvertSidToStringSidW(sid, &sid_string)) {
        return std::nullopt;
      }
      hive_t hive { HKEY_USERS, std::wstring(L"HKEY_USERS\\") + sid_string };
      LocalFree(sid_string);
      return hive;
    }

    hive_t
    get_hive() {
      if (running_as_system()) {
        auto resolved = resolve_hive();
        if (resolved) {
          return *resolved;
        }
        BOOST_LOG(warning) << "touch_kb: unable to resolve the console user hive";
        return { HKEY_USERS, L"HKEY_USERS" };
      }
      return { HKEY_CURRENT_USER, L"HKEY_CURRENT_USER" };
    }

    std::filesystem::path
    undo_path() {
      return platf::appdata() / "touch_keyboard_undo.json";
    }

    std::optional<captured_value>
    read_value(HKEY key, const wchar_t *name) {
      DWORD type = 0;
      DWORD data = 0;
      DWORD size = sizeof(data);
      const LONG rc = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, &type, &data, &size);
      if (rc == ERROR_SUCCESS) {
        return captured_value { true, data };
      }
      if (rc == ERROR_FILE_NOT_FOUND) {
        return captured_value { false, 0 };
      }
      return std::nullopt;
    }

    bool
    write_value(HKEY key, const wchar_t *name, DWORD value) {
      return RegSetKeyValueW(key, nullptr, name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
    }

    bool
    delete_value(HKEY key, const wchar_t *name) {
      const LONG rc = RegDeleteKeyValueW(key, nullptr, name);
      return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }

  }  // namespace

  bool
  start_session(bool effective) {
    if (!effective) {
      return false;
    }

    // Single owner: an existing undo journal means another stream session is
    // already running the transaction.  Skip rather than overwrite its
    // captured values; the running session's end_session() will restore.
    if (std::filesystem::exists(undo_path())) {
      BOOST_LOG(info) << "touch_kb: transaction already active; skipping";
      return true;
    }

    const auto hive = get_hive();
    std::wstring subkey = REG_SUBKEY;
    const wchar_t sep = L'\\';
    if (!hive.prefix.empty()) {
      subkey = hive.prefix.substr(hive.prefix.find(sep) + 1) + sep + REG_SUBKEY;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(hive.root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "touch_kb: cannot open [" << to_utf8(subkey) << "]: " << GetLastError();
      return false;
    }

    nlohmann::json undo_keys = nlohmann::json::array();
    bool all_written = true;
    for (const auto &def : KEYS) {
      const auto captured = read_value(key, def.name);
      if (!captured) {
        all_written = false;
        continue;
      }
      undo_keys.push_back({
        { "name", to_utf8(def.name) },
        { "exists", captured->exists },
        { "value", captured->value },
      });
      // Enum-style keys are only forced back to their desired value when the
      // value was present before; a missing PreventTouchKeyboardAutoInvoke
      // means the neutral default and creating it adds no information.
      if (def.write_only_if_previously_present && !captured->exists) {
        continue;
      }
      if (!write_value(key, def.name, def.desired)) {
        all_written = false;
      }
    }
    RegCloseKey(key);

    nlohmann::json undo = {
      { "hive", to_utf8(hive.prefix) },
      { "keys", undo_keys },
    };

    // CREATE_NEW is the atomic ownership grant; a concurrent transaction
    // loses here and skips instead of clobbering the captured values.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    HANDLE journal = CreateFileW(undo_path().c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (journal == INVALID_HANDLE_VALUE) {
      BOOST_LOG(warning) << "touch_kb: journal creation failed (concurrent transaction?), err="
                         << GetLastError();
      return false;
    }
    const auto content = undo.dump(2);
    DWORD written = 0;
    WriteFile(journal, content.c_str(), (DWORD) content.size(), &written, nullptr);
    CloseHandle(journal);

    BOOST_LOG(info) << "touch_kb: AutoInvoke key group applied (complete=" << all_written << ")";
    return all_written;
  }

  void
  end_session() {
    const auto path = undo_path();
    if (!std::filesystem::exists(path)) {
      return;
    }

    nlohmann::json undo;
    try {
      std::ifstream undo_file(path.string());
      undo = nlohmann::json::parse(undo_file);
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "touch_kb: undo journal unreadable, leaving values as-is: " << e.what();
      std::error_code ec;
      std::filesystem::remove(path, ec);
      return;
    }

    const auto hive_id = to_wstring(undo.value("hive", ""));
    // The journal stores "HKEY_USERS\<sid>" for elevated runs and
    // "HKEY_CURRENT_USER" for interactive runs.
    HKEY key = nullptr;
    const wchar_t prefix[] = L"HKEY_USERS\\";
    if (hive_id.rfind(prefix, 0) == 0) {
      const std::wstring sid_prefix = hive_id.substr(wcslen(prefix));
      RegOpenKeyExW(HKEY_USERS, (sid_prefix + L"\\" + REG_SUBKEY).c_str(), 0, KEY_SET_VALUE, &key);
    }
    else if (hive_id == L"HKEY_CURRENT_USER") {
      RegOpenKeyExW(HKEY_CURRENT_USER, REG_SUBKEY, 0, KEY_SET_VALUE, &key);
    }
    if (!key) {
      BOOST_LOG(warning) << "touch_kb: cannot reopen the registry key for restore";
      return;
    }

    bool all_restored = true;
    if (undo.contains("keys")) {
      for (const auto &entry : undo["keys"]) {
        const auto name = to_wstring(entry.value("name", ""));
        if (name.empty() || !key) {
          continue;
        }
        bool ok = false;
        if (entry.value("exists", false)) {
          const auto value = (DWORD) entry.value("value", 0);
          ok = write_value(key, name.c_str(), value);
        }
        else {
          ok = delete_value(key, name.c_str());
        }
        all_restored &= ok;
      }
    }
    if (key) {
      RegCloseKey(key);
    }

    // Keep the journal on partial failure so the next session can retry the
    // restore; deleting it here would strand the user's original values.
    if (!all_restored) {
      BOOST_LOG(warning) << "touch_kb: registry restore incomplete; keeping the undo journal";
      return;
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    BOOST_LOG(info) << "touch_kb: AutoInvoke key group restored";
  }

}  // namespace touch_kb
