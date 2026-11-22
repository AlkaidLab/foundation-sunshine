// Foundation Sunshine installer shell: the user-level installer for the Inno engine. Modes in one exe:
//   (default)   the web front end (installer_web.inc): the pages run in the setup UI process (Tauri 2 + WebView2,
//               appended as a second payload) and this process drives them; without the WebView2 Runtime or the UI
//               payload, or when the UI does not load, the native window below.
//   --native    the native Win32 + Direct2D/DirectWrite Material 3 window: welcome, components, UAC wait, progress,
//               restart, failure, done.
//   --worker    the elevated worker started with ShellExecuteExW(runas): protected run directory,
//               extracts the embedded Inno engine, runs it silently, writes events and the result.
//   --preview D renders every page to PNG files in directory D (no window, touches nothing else).
#ifndef UNICODE
  #define UNICODE
#endif
#ifndef _UNICODE
  #define _UNICODE
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <windowsx.h>

#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "installer_dir.h"
#include "installer_dir_win.h"
#include "installer_failure.h"
#include "installer_guide.h"
#include "installer_json.h"
#include "installer_maintenance.h"
#include "installer_state.h"
#include "installer_ui.h"
#include "installer_webview2.h"

namespace {

  constexpr wchar_t kWindowClass[] = L"FoundationSunshineInstallerShell";
  constexpr wchar_t kWindowTitle[] = L"Foundation Sunshine";
  constexpr wchar_t kRunRoot[] = L"FoundationSunshine\\install-runs";
  constexpr UINT_PTR kTimer = 7;
  constexpr UINT_PTR kCloseTimer = 8;  // the window closes a moment after the Panel was started from the done page
  constexpr UINT kCloseAfterLaunchMs = 1500;
  constexpr unsigned char kIconPng[] = {
#include "installer_icon.inc"
  };

  struct run_context {
    std::wstring id;
    std::wstring directory;
    std::wstring events;
    std::wstring result;
    std::wstring self;
    std::wstring components;  // comma list for /COMPONENTS, empty = engine default
    std::wstring tasks;  // "desktopicon", "none" or empty
    std::wstring shell_pid;  // pid of the user-level window; the engine must not kill it
    bool dir_given = false;  // --dir or /DIR= was on the command line, even with an empty value: an empty folder is refused, not read as "no folder"
    std::wstring dir;  // the install folder to pass as /DIR= (empty = the engine's own: the installed copy's, or Program Files\\Sunshine); checked again by the worker
    bool dir_unsafe_ok = false;  // a warning about the folder (installer_dir.h: a parent that others can replace, a user folder) was confirmed: the window's --dir-unsafe-ok, the silent /ALLOWUNSAFEPARENT
    std::wstring worker_copy;  // the copy of this exe the elevated worker runs from
    std::wstring selftest_directory;  // --selftest-worker: run the worker steps up to the engine in this directory, no install
  };

  run_context g_run;

  std::wstring
  module_path() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
      const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (length == 0) return {};
      if (length + 1 < buffer.size()) return { buffer.data(), length };
      buffer.resize(buffer.size() * 2);
    }
  }

  std::wstring
  program_data() {
    std::vector<wchar_t> buffer(512);
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return L"C:\\ProgramData";
    return { buffer.data(), length };
  }

  std::wstring
  make_run_id() {
    wchar_t id[64] {};
    swprintf_s(id, L"%llx-%lu", static_cast<unsigned long long>(GetTickCount64()), GetCurrentProcessId());
    return id;
  }

  bool
  make_directory_tree(const std::wstring &path) {
    if (CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash == 0) return false;
    if (!make_directory_tree(path.substr(0, slash))) return false;
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
  }

  bool
  protect_run_directory(const std::wstring &path) {
    // Authority: installer-ms-setnamedsecurityinfow-20261107; the run directory
    // exposes only read access to the interactive user and full access to the
    // elevated worker and service identities.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GR;;;IU)", SDDL_REVISION_1, &descriptor, nullptr)) {
      return false;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    const bool valid = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != FALSE;
    const DWORD status = valid ? SetNamedSecurityInfoW(
                                   const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                   dacl, nullptr) :
                                 ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    return status == ERROR_SUCCESS;
  }

  bool
  write_text_atomic(const std::wstring &path, const std::string &text) {
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
                       written == text.size();
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote) {
      DeleteFileW(temporary.c_str());
      return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  }

  bool
  append_event(const std::wstring &path, const std::string &line) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE &&
                    written == line.size();
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
  }

  std::string
  read_file(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size {};
    std::string content;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 1024 * 1024) {
      content.resize(static_cast<size_t>(size.QuadPart));
      DWORD read = 0;
      if (!ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &read, nullptr)) content.clear();
      content.resize(read);
    }
    CloseHandle(file);
    return content;
  }

  bool
  read_payload_footer(const std::wstring &path, uint64_t *offset, uint64_t *size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER file_size {};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 128) {
      CloseHandle(file);
      return false;
    }
    const DWORD read_size = static_cast<DWORD>(std::min<LONGLONG>(file_size.QuadPart, 1024));
    LARGE_INTEGER position {};
    position.QuadPart = file_size.QuadPart - read_size;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
      CloseHandle(file);
      return false;
    }
    std::string footer(read_size, '\0');
    DWORD actual = 0;
    const bool ok = ReadFile(file, footer.data(), read_size, &actual, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok) return false;
    footer.resize(actual);
    constexpr std::string_view marker = "ALKAID-SHELL-PAYLOAD-V1\n";
    const size_t marker_pos = footer.rfind(marker);
    if (marker_pos == std::string::npos) return false;
    const auto parse = [&](const std::string_view key, uint64_t *value) {
      const size_t begin = footer.find(key, marker_pos + marker.size());
      if (begin == std::string::npos) return false;
      const size_t number_begin = begin + key.size();
      char *end = nullptr;
      *value = std::strtoull(footer.c_str() + number_begin, &end, 10);
      return end != footer.c_str() + number_begin;
    };
    if (!parse("offset=", offset) || !parse("size=", size)) return false;
    return *offset > 0 && *size > 0 && *offset + *size <= static_cast<uint64_t>(file_size.QuadPart);
  }

  bool
  extract_payload(const std::wstring &source, const std::wstring &destination) {
    uint64_t offset = 0;
    uint64_t size = 0;
    if (!read_payload_footer(source, &offset, &size)) return false;
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
      if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
      if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
      return false;
    }
    LARGE_INTEGER position {};
    position.QuadPart = static_cast<LONGLONG>(offset);
    bool ok = SetFilePointerEx(input, position, nullptr, FILE_BEGIN) != FALSE;
    // Heap, not stack: a 1 MiB local here is inlined into worker_main by Clang, and the 1 MiB default stack reserve of
    // LLD overflowed in the prologue before the worker wrote its first event (issue #1326).
    std::vector<char> buffer(1024 * 1024);
    uint64_t remaining = size;
    while (ok && remaining != 0) {
      const DWORD requested = static_cast<DWORD>(std::min<uint64_t>(remaining, buffer.size()));
      DWORD read = 0;
      ok = ReadFile(input, buffer.data(), requested, &read, nullptr) != FALSE && read != 0;
      if (!ok) break;
      DWORD written = 0;
      ok = WriteFile(output, buffer.data(), read, &written, nullptr) != FALSE && written == read;
      remaining -= read;
    }
    FlushFileBuffers(output);
    CloseHandle(input);
    CloseHandle(output);
    if (!ok || remaining != 0) DeleteFileW(destination.c_str());
    return ok && remaining == 0;
  }

  // Authority: installer-ms-webview2-distribution-20261109 ("Detect if a WebView2 Runtime is already installed"):
  // the pv value of the Evergreen Runtime client key, per machine (HKLM, the 32-bit view is WOW6432Node) or per user (HKCU);
  // missing, empty or 0.0.0.0 means not installed. Both registry views of both roots are read, so that the answer does not
  // depend on whether this process is 32 or 64 bit.
  bool
  webview2_runtime_present() {
    // test switch, as the Panel's SUNSHINE_GUI_FORCE_NO_WEBVIEW2: behave as if the Runtime were missing
    if (GetEnvironmentVariableW(L"FOUNDATION_SETUP_FORCE_NO_WEBVIEW2", nullptr, 0) != 0) return false;
    constexpr wchar_t kClient[] = L"\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const std::wstring machine = std::wstring(L"SOFTWARE") + kClient;
    const std::wstring user = std::wstring(L"Software") + kClient;
    const auto has = [](HKEY root, const std::wstring &key, REGSAM view) {
      HKEY handle = nullptr;
      if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | view, &handle) != ERROR_SUCCESS) return false;
      wchar_t buffer[128] {};
      DWORD size = sizeof(buffer) - sizeof(wchar_t);
      DWORD type = 0;
      const bool read = RegQueryValueExW(handle, L"pv", nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ;
      RegCloseKey(handle);
      return read && installer_webview2::pv_means_installed(buffer);
    };
    for (const REGSAM view : { KEY_WOW64_64KEY, KEY_WOW64_32KEY })
      if (has(HKEY_LOCAL_MACHINE, machine, view) || has(HKEY_CURRENT_USER, user, view)) return true;
    return false;
  }

  struct winhttp_handle {
    HINTERNET handle = nullptr;
    winhttp_handle() = default;
    explicit winhttp_handle(HINTERNET h): handle(h) {}
    winhttp_handle(const winhttp_handle &) = delete;
    winhttp_handle &operator=(const winhttp_handle &) = delete;
    ~winhttp_handle() {
      if (handle != nullptr) WinHttpCloseHandle(handle);
    }
  };

  // Authority: installer-ms-winhttp-20261118 (WinHTTP: WinHttpOpen, WinHttpOpenRequest with WINHTTP_FLAG_SECURE, the redirect
  // policy option). One HTTPS GET into a new file; redirects are followed (go.microsoft.com redirects to Microsoft's CDN) but
  // never from https to http; the size is capped; `deadline` (GetTickCount64 time) is checked between two reads, so that a
  // server sending a trickle cannot hold the install for hours; one blocking WinHTTP call may still wait for its own timeout
  // (10 to 15 s; a second attempt that starts with 30 s left may spend 50 s on resolve, connect, send and the response header),
  // so the worst case is about 110 s; a failed or
  // partial download leaves no file. `error` is the Win32 error, or the HTTP status when it was not 200.
  bool
  download_https(const std::wstring &url, const std::wstring &destination, const std::uint64_t max_bytes, const ULONGLONG deadline, DWORD *error) {
    *error = 0;
    const auto fail = [&](const DWORD code) {
      *error = code != 0 ? code : ERROR_GEN_FAILURE;
      return false;
    };
    URL_COMPONENTS parts {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0) return fail(ERROR_INVALID_PARAMETER);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);  // the path and the query follow each other
    constexpr DWORD kAutomaticProxy = 4;  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, Windows 8.1 and later
    winhttp_handle session(WinHttpOpen(L"FoundationSunshineSetup/1", kAutomaticProxy, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.handle == nullptr) session.handle = WinHttpOpen(L"FoundationSunshineSetup/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.handle == nullptr) return fail(GetLastError());
    WinHttpSetTimeouts(session.handle, 10000, 10000, 15000, 15000);  // each wait; the total is bounded by `deadline` plus one of them
    winhttp_handle connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
    if (connection.handle == nullptr) return fail(GetLastError());
    winhttp_handle request(WinHttpOpenRequest(connection.handle, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (request.handle == nullptr) return fail(GetLastError());
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.handle, nullptr)) return fail(GetLastError());
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) return fail(GetLastError());
    if (status != 200) return fail(status);
    HANDLE file = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(GetLastError());
    std::vector<char> buffer(64 * 1024);  // heap, as in extract_range
    std::uint64_t total = 0;
    DWORD problem = 0;
    for (;;) {
      if (GetTickCount64() > deadline) {
        problem = ERROR_TIMEOUT;
        break;
      }
      DWORD got = 0;
      if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
        problem = GetLastError();
        break;
      }
      if (got == 0) break;
      total += got;
      if (total > max_bytes) {
        problem = ERROR_FILE_TOO_LARGE;
        break;
      }
      DWORD written = 0;
      if (!WriteFile(file, buffer.data(), got, &written, nullptr) || written != got) {
        problem = GetLastError();
        break;
      }
    }
    if (problem == 0 && total == 0) problem = ERROR_NO_DATA;
    FlushFileBuffers(file);
    CloseHandle(file);
    if (problem != 0) {
      DeleteFileW(destination.c_str());
      return fail(problem);
    }
    return true;
  }

  // Authority: installer-ms-winverifytrust-20261118 (WinVerifyTrust, WINTRUST_ACTION_GENERIC_VERIFY_V2, the WTHelper functions).
  // The embedded Authenticode signature must verify, and the organisation of the signing (leaf) certificate must be exactly
  // "Microsoft Corporation". Revocation is not queried: a failing OCSP/CRL server must not decide an install, and the file
  // comes over TLS from Microsoft's own link.
  bool
  verify_signed_by_microsoft(const std::wstring &path, DWORD *error) {
    *error = 0;
    WINTRUST_FILE_INFO file {};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data {};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const HWND no_window = static_cast<HWND>(INVALID_HANDLE_VALUE);
    const LONG status = WinVerifyTrust(no_window, &action, &data);
    bool ok = false;
    if (status != ERROR_SUCCESS) {
      *error = static_cast<DWORD>(status);
    }
    else {
      *error = 0x800B0004;  // TRUST_E_SUBJECT_NOT_TRUSTED: valid, but not Microsoft's
      CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData);
      CRYPT_PROVIDER_SGNR *signer = provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
      CRYPT_PROVIDER_CERT *leaf = signer != nullptr ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
      if (leaf != nullptr && leaf->pCert != nullptr) {
        wchar_t organization[256] {};
        if (CertGetNameStringW(leaf->pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char *>(szOID_ORGANIZATION_NAME), organization, 255) > 1 &&
            installer_webview2::signer_is_microsoft(organization)) {
          ok = true;
          *error = 0;
        }
      }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(no_window, &action, &data);
    return ok;
  }

  // The Evergreen Bootstrapper, per machine from this elevated process: MicrosoftEdgeWebview2Setup.exe /silent /install
  // (installer-ms-webview2-distribution-20261109). true when it started and finished within the 10 minutes; the registry,
  // not its exit code, tells whether the Runtime is there.
  bool
  run_webview2_bootstrapper(const std::wstring &setup, const std::wstring &directory, DWORD *code) {
    std::wstring command = L"\"" + setup + L"\" /silent /install";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(setup.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process)) {
      *code = GetLastError();
      return false;
    }
    bool finished = true;
    if (WaitForSingleObject(process.hProcess, 10 * 60 * 1000) == WAIT_OBJECT_0) {
      GetExitCodeProcess(process.hProcess, code);
    }
    else {
      *code = WAIT_TIMEOUT;
      finished = false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return finished;
  }

  // The steps of installer_webview2::ensure_runtime on this machine: Microsoft's fixed link, two attempts within 90 s for a download.
  installer_webview2::steps
  webview2_steps(const std::wstring &directory, std::function<void(std::string_view)> progress) {
    const std::wstring setup = directory + L"\\MicrosoftEdgeWebview2Setup.exe";
    installer_webview2::steps s;
    s.runtime_present = [] { return webview2_runtime_present(); };
    s.download = [setup](unsigned long *code) {
      const std::string_view link = installer_webview2::kBootstrapperUrl;
      const std::wstring url(link.begin(), link.end());  // ASCII
      DWORD error = 0;
      // One deadline for both attempts: the install of the host is finished and waits for this step.
      const ULONGLONG deadline = GetTickCount64() + 90 * 1000;
      for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt != 0) {
          if (GetTickCount64() + 3000 + 30000 >= deadline) break;  // a second attempt needs time to finish
          Sleep(3000);
        }
        DeleteFileW(setup.c_str());
        if (download_https(url, setup, installer_webview2::kMaxDownloadBytes, deadline, &error)) return true;
      }
      *code = error;
      return false;
    };
    s.verify_signature = [setup](unsigned long *code) {
      DWORD error = 0;
      const bool ok = verify_signed_by_microsoft(setup, &error);
      *code = error;
      return ok;
    };
    s.run_bootstrapper = [setup, directory](unsigned long *code) {
      DWORD exit_code = 0;
      const bool finished = run_webview2_bootstrapper(setup, directory, &exit_code);
      *code = exit_code;
      return finished;
    };
    s.discard = [setup] { DeleteFileW(setup.c_str()); };
    s.progress = std::move(progress);
    return s;
  }

  // A named section of the appended payload (append_installer_payload.py): <name>-offset= and <name>-size= after the
  // engine's offset= and size= in the footer. "ui" is the setup UI.
  bool
  read_named_payload(const std::wstring &path, const char *name, uint64_t *offset, uint64_t *size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER file_size {};
    bool ok = GetFileSizeEx(file, &file_size) && file_size.QuadPart > 1024;
    std::string footer;
    if (ok) {
      const DWORD n = static_cast<DWORD>(std::min<LONGLONG>(file_size.QuadPart, 1024));
      LARGE_INTEGER position {};
      position.QuadPart = file_size.QuadPart - n;
      footer.resize(n);
      DWORD actual = 0;
      ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) && ReadFile(file, footer.data(), n, &actual, nullptr);
      footer.resize(actual);
    }
    CloseHandle(file);
    if (!ok) return false;
    const size_t marker = footer.rfind("ALKAID-SHELL-PAYLOAD-V1\n");
    if (marker == std::string::npos) return false;
    const auto value = [&](const char *key, uint64_t *out) {
      const size_t b = footer.find(key, marker);
      if (b == std::string::npos) return false;
      *out = std::strtoull(footer.c_str() + b + std::strlen(key), nullptr, 10);
      return true;
    };
    return value(("\n" + std::string(name) + "-offset=").c_str(), offset) && value(("\n" + std::string(name) + "-size=").c_str(), size) && *offset > 0 && *size > 0 &&
           *offset + *size <= static_cast<uint64_t>(file_size.QuadPart);
  }

  bool
  extract_range(const std::wstring &source, uint64_t offset, uint64_t size, const std::wstring &destination) {
    HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE;
    if (ok) {
      LARGE_INTEGER position {};
      position.QuadPart = static_cast<LONGLONG>(offset);
      ok = SetFilePointerEx(input, position, nullptr, FILE_BEGIN) != FALSE;
      std::vector<char> buffer(1024 * 1024);
      uint64_t remaining = size;
      while (ok && remaining) {
        DWORD read = 0, written = 0;
        const DWORD want = static_cast<DWORD>(std::min<uint64_t>(remaining, buffer.size()));
        ok = ReadFile(input, buffer.data(), want, &read, nullptr) && read && WriteFile(output, buffer.data(), read, &written, nullptr) && written == read;
        remaining -= read;
      }
      ok = ok && remaining == 0;
    }
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!ok) DeleteFileW(destination.c_str());
    return ok;
  }

  bool
  run_engine(const std::wstring &engine, const std::wstring &log_path, const std::wstring &run_dir,
    const std::wstring &run_id, DWORD *exit_code) {
    std::wstring command = L"\"" + engine + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /LOG=\"" + log_path +
                           L"\" /SUNSHINE_RUN_DIR=" + run_dir + L" /SUNSHINE_RUN_ID=" + run_id;
    // The component list comes from the user-level shell; only [a-z,] survives so it cannot
    // inject switches into the engine command line.
    if (!g_run.components.empty()) command += L" /COMPONENTS=\"" + g_run.components + L"\"";
    if (!g_run.shell_pid.empty()) command += L" /SUNSHINE_SHELL_PID=" + g_run.shell_pid;
    // The folder was checked by prepare_install_dir() just before: it holds no quote, so the quoted form cannot be left early.
    if (!g_run.dir.empty()) command += installer_dir::engine_switch(g_run.dir);
    if (g_run.tasks == L"desktopicon")
      command += L" /MERGETASKS=\"desktopicon\"";
    else if (g_run.tasks == L"none")
      command += L" /MERGETASKS=\"!desktopicon\"";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(engine.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
          run_dir.c_str(), &startup, &process)) {
      return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    const bool got_code = GetExitCodeProcess(process.hProcess, exit_code) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return got_code;
  }

  std::wstring
  command_value(const std::wstring &command_line, const std::wstring &key) {
    const size_t marker = command_line.find(key + L" ");
    if (marker == std::wstring::npos) return {};
    const size_t begin = marker + key.size() + 1;
    const size_t end = command_line.find_first_of(L" \t", begin);
    return command_line.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
  }

  std::wstring
  only_component_chars(const std::wstring &value) {
    std::wstring out;
    for (const wchar_t ch : value)
      if ((ch >= L'a' && ch <= L'z') || ch == L',') out.push_back(ch);
    return out;
  }

  bool
  has_silent_switch(const std::wstring &command_line) {
    std::wstring upper = command_line;
    for (wchar_t &ch : upper)
      if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    return upper.find(L" /VERYSILENT") != std::wstring::npos || upper.find(L" /SILENT") != std::wstring::npos;
  }

  // The install folder the person chose (--dir) or the silent switch gave (/DIR=), made ready before the engine starts. The worker never takes the shell's word:
  // it checks the folder again with the same rules (installer_dir.h), refuses a second place for an installed copy (to move, uninstall and install again), and
  // creates the folder, and any missing folder above it, with the protection of Program Files (installer_dir_win.h). 0 = ready, otherwise the exit code (110) after
  // install-result.json said which rule it was ("dir_error": the code of the setup UI, so that the window can show it on the first page).
  int
  prepare_install_dir(const std::string &id_utf8) {
    if (!g_run.dir_given) return 0;
    if (g_run.dir.empty()) {
      write_text_atomic(g_run.result, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"state\":\"failed\",\"result_code\":110,\"message_key\":\"InstallDirRefused\",\"dir_error\":\"invalid\"}\n");
      return 110;
    }
    g_run.dir = installer_dir::normalise(g_run.dir);
    const auto refuse = [&](const std::string &reason, const std::string &detail) {
      write_text_atomic(g_run.result, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"state\":\"failed\",\"result_code\":110,\"message_key\":\"InstallDirRefused\",\"dir_error\":\"" + reason + "\"" + detail + "}\n");
      append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"seq\":4,\"phase\":\"engine\",\"state\":\"failed\",\"result_code\":110,\"message_key\":\"InstallDirRefused\"}\n");
      return 110;
    };
    const std::wstring installed = installer_maintenance::install_dir();
    if (!installed.empty() && GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES && !installer_dir::same_text(installer_dir::normalise(installed), g_run.dir))
      return refuse("no-access", ",\"detail\":\"installed_elsewhere\"");
    if (installer_dir::same_text(g_run.dir, installer_dir_win::default_dir())) {
      g_run.dir.clear();  // the default is the engine's own choice: Program Files\\Sunshine, or the installed copy (an installed copy elsewhere was refused above)
      g_run.dir_given = false;
      return 0;
    }
    DWORD win32 = 0;
    if (const auto problem = installer_dir_win::prepare(g_run.dir, &win32, g_run.dir_unsafe_ok)) {
      // a warning that was not confirmed is refused like a problem, and says what would allow it (a silent run has nobody to ask)
      const std::string detail = installer_dir::is_warning(*problem) ? ",\"detail\":\"needs_allowunsafeparent\"" : ",\"win32\":" + std::to_string(win32);
      return refuse(std::string(installer_dir::code(*problem)), detail);
    }
    return 0;
  }

  int
  worker_main() {
    const std::wstring id = g_run.id;
    const bool selftest = !g_run.selftest_directory.empty();
    const std::wstring root = program_data() + L"\\" + kRunRoot;
    g_run.directory = selftest ? g_run.selftest_directory : root + L"\\" + id;
    g_run.events = g_run.directory + L"\\events.jsonl";
    g_run.result = g_run.directory + L"\\install-result.json";
    if (!make_directory_tree(g_run.directory) || (!selftest && !protect_run_directory(g_run.directory))) return 5;
    const std::string id_utf8(id.begin(), id.end());
    append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 +
                                 "\",\"seq\":1,\"phase\":\"worker\",\"state\":\"running\",\"result_code\":0,\"message_key\":\"WorkerStarted\"}\n");
    const std::wstring engine = g_run.directory + L"\\sunshine-engine.exe";
    const std::wstring log_path = g_run.directory + L"\\inno.log";
    if (!extract_payload(g_run.self, engine)) {
      write_text_atomic(g_run.result, "{\"schema\":1,\"state\":\"failed\",\"result_code\":101,\"message_key\":\"PayloadInvalid\"" + installer_failure::result_fields(installer_failure::step::payload, 101) + "}\n");
      return 101;
    }
    if (selftest) {
      // The same entry, run directory and payload extraction as an install, without starting the engine.
      write_text_atomic(g_run.result, "{\"schema\":1,\"state\":\"selftest-ok\",\"result_code\":0}\n");
      return 0;
    }
    if (const int refused = prepare_install_dir(id_utf8); refused != 0) return refused;
    append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 +
                                 "\",\"seq\":2,\"phase\":\"engine\",\"state\":\"running\",\"result_code\":0,\"message_key\":\"EngineStarted\"}\n");
    DWORD code = 1;
    if (!run_engine(engine, log_path, g_run.directory, id, &code)) code = 101;
    // The Panel runs on WebView2. A successful install on a machine without the Runtime fetches Microsoft's Evergreen
    // Bootstrapper from its fixed link, checks its Authenticode signature (Microsoft Corporation) and runs it: the installer
    // does not carry the bootstrapper. A failure (offline, a proxy, a bad signature, the bootstrapper failing) is never an
    // install failure: it is recorded in the events and in install-result.json with the reason and the manual download link,
    // and the Panel reports the missing Runtime when it starts.
    std::string webview2_fields;
    if (code == 0 || code == 3010) {
      const auto progress = [&](std::string_view what) {
        append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"seq\":5,\"phase\":\"webview2\",\"state\":\"running\",\"result_code\":0,\"message_key\":\"WebView2" +
                                     (what == "download" ? "Download" : "Install") + "\"}\n");
      };
      const auto wv2 = installer_webview2::ensure_runtime(webview2_steps(g_run.directory, progress));
      webview2_fields = installer_webview2::result_fields(wv2);
      if (wv2.what != installer_webview2::outcome::already_installed) {
        append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"seq\":6,\"phase\":\"webview2\",\"state\":\"" +
                                     (installer_webview2::runtime_available(wv2.what) ? "done" : "failed") + "\",\"result_code\":" + std::to_string(wv2.code) +
                                     ",\"message_key\":\"WebView2Runtime-" + std::string(installer_webview2::outcome_word(wv2.what)) + "\"}\n");
      }
    }
    const auto state = installer_state::classify_exit_code(code);
    const std::string state_name(installer_state::name(state));
    // A failed install names its step and a code word; the host reports it at its next start (a retry that worked, or an install that got as far as the host).
    const std::string failure = installer_failure::is_failure_state(state_name) ? installer_failure::result_fields(installer_failure::step::engine, code) : std::string {};
    const std::string result = "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"state\":\"" + state_name +
                               "\",\"result_code\":" + std::to_string(code) + failure + webview2_fields + "}\n";
    write_text_atomic(g_run.result, result);
    // The decision to show the restart page, in the log: only an engine exit of 3010 asks for it.
    append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 +
                                 "\",\"seq\":3,\"phase\":\"restart\",\"state\":\"" +
                                 (code == 3010 ? "needed" : "not-needed") + "\",\"result_code\":" + std::to_string(code) +
                                 ",\"message_key\":\"" + (code == 3010 ? "EngineExit3010" : "EngineExitNoRestart") + "\"}\n");
    append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 +
                                 "\",\"seq\":4,\"phase\":\"engine\",\"state\":\"" + state_name +
                                 "\",\"result_code\":" + std::to_string(code) + ",\"message_key\":\"EngineFinished\"}\n");
    return static_cast<int>(code);
  }

// clang-format off
#include "installer_shell_ui.inc"
#include "installer_preview.inc"
#include "installer_web.inc"
// clang-format on

}  // namespace

int WINAPI
wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::wstring command_line = GetCommandLineW();
  // A mode is a whole argument of the command line, never a piece of one: /DIR="D:\\Tools--worker\\Sunshine" is a folder, not the worker.
  const std::vector<std::wstring> arguments = installer_dir::split_arguments(command_line);
  const auto has_argument = [&arguments](const wchar_t *name) {
    for (const auto &argument : arguments)
      if (argument == name) return true;
    return false;
  };
  // The floor: Windows 10 (any build, 1507 / 10240). Below it nothing of Foundation Sunshine runs; say so and stop.
  if (windows_build() == 0) {
    if (!has_silent_switch(command_line) && !has_argument(L"--worker")) {
      const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
      MessageBoxW(nullptr, zh ? L"Foundation Sunshine 需要 Windows 10 或更高版本。" : L"Foundation Sunshine needs Windows 10 or later.",
        L"Foundation Sunshine", MB_OK | MB_ICONINFORMATION);
    }
    return 1150;  // ERROR_OLD_WIN_VERSION
  }
  if (has_argument(L"--worker")) {
    g_run.self = module_path();
    const std::wstring id = command_value(command_line, L"--run-id");
    if (id.empty()) return 1223;
    g_run.id = id;
    g_run.components = only_component_chars(command_value(command_line, L"--components"));
    for (const wchar_t ch : command_value(command_line, L"--shell-pid"))
      if (ch >= L'0' && ch <= L'9') g_run.shell_pid.push_back(ch);
    g_run.dir = installer_dir::option_value(command_line, L"--dir");
    g_run.dir_given = has_argument(L"--dir");
    g_run.dir_unsafe_ok = has_argument(L"--dir-unsafe-ok");
    const std::wstring tasks = command_value(command_line, L"--tasks");
    g_run.tasks = tasks == L"desktopicon" || tasks == L"none" ? tasks : L"";
    return worker_main();
  }
  // CI smoke test of the worker: --selftest-worker <directory> runs worker_main up to and including the payload
  // extraction into that directory (scripts/ci/check-windows-release.py needs a package built by the same job).
  if (has_argument(L"--selftest-worker")) {
    g_run.self = module_path();
    g_run.id = make_run_id();
    g_run.selftest_directory = command_value(command_line, L"--selftest-worker");
    if (g_run.selftest_directory.empty()) return 1223;
    return worker_main();
  }
  // CI smoke test of the install location rules with this machine's real answers (drive type, file system, folders, permissions): --selftest-dir <path> [--pick] --out <file>
  // writes what the window would show for that path: typed as it is, or (--pick) as the folder picker's result with the product's folder name added. Nothing is created or changed.
  if (has_argument(L"--selftest-dir")) {
    const std::wstring raw = installer_dir::option_value(command_line, L"--selftest-dir");
    const std::wstring out = installer_dir::option_value(command_line, L"--out");
    if (out.empty()) return 1223;
    std::wstring path = installer_dir::normalise(installer_dir::tidy(raw));
    bool appended = false, redirected = false;
    if (has_argument(L"--pick")) {
      const installer_dir::pick chosen = installer_dir_win::resolve_pick(path);
      path = chosen.path;
      appended = chosen.appended;
      redirected = chosen.redirected;
    }
    const installer_dir::verdict verdict = installer_dir_win::verdict_for_window(path);  // what the window says, the default folder's own rule included
    const auto free = installer_dir_win::free_bytes_of(path);
    const std::wstring safe = verdict.warning != installer_dir::error::none ? installer_dir::safe_alternative(verdict.path, installer_dir_win::default_dir()) : std::wstring();
    const auto yes = [](bool x) { return x ? "true" : "false"; };
    const bool written = write_text_atomic(out, "{\"input\":" + json_text(utf8(raw)) + ",\"path\":" + json_text(utf8(path)) + ",\"appended\":" + yes(appended) + ",\"redirected\":" + yes(redirected) +
                             ",\"error\":" + json_text(std::string(installer_dir::code(verdict.problem))) + ",\"warning\":" + json_text(std::string(installer_dir::code(verdict.warning))) +
                             ",\"safe\":" + json_text(utf8(safe)) + ",\"free_bytes\":" + (free ? std::to_string(*free) : std::string("null")) + "}\n");
    return written ? 0 : 1223;  // an answer that could not be written is a failure, not an old file read as the new answer
  }
  // CI smoke test of the Runtime step: --selftest-webview2 <directory> downloads Microsoft's bootstrapper from the fixed link
  // into that directory and checks its signature (no run, nothing installed), then checks that an unsigned file, this
  // exe, is refused. Exit 0, or 20 (download failed), 21 (the file is not signed by Microsoft), 22 (an unsigned file passed).
  if (has_argument(L"--selftest-webview2")) {
    const std::wstring dir = command_value(command_line, L"--selftest-webview2");
    if (dir.empty() || !make_directory_tree(dir)) return 1223;
    auto s = webview2_steps(dir, nullptr);
    unsigned long code = 0;
    const bool fetched = s.download(&code);
    std::string text = "{\"download\":" + std::string(fetched ? "true" : "false") + ",\"download_code\":" + std::to_string(code);
    int exit_code = 0;
    if (!fetched) {
      exit_code = 20;
    }
    else {
      const bool signed_ok = s.verify_signature(&code);
      text += ",\"signed_by_microsoft\":" + std::string(signed_ok ? "true" : "false") + ",\"signature_code\":" + std::to_string(code);
      if (!signed_ok) exit_code = 21;
      s.discard();
    }
    DWORD unsigned_code = 0;
    const bool unsigned_passed = verify_signed_by_microsoft(module_path(), &unsigned_code);
    text += ",\"unsigned_refused\":" + std::string(unsigned_passed ? "false" : "true") + ",\"unsigned_code\":" + std::to_string(unsigned_code) + "}\n";
    if (unsigned_passed && exit_code == 0) exit_code = 22;
    write_text_atomic(dir + L"\\selftest-webview2.json", text);
    return exit_code;
  }
  // An updater (the Panel's, an old Panel's, a script) starts the package the way it started the
  // old Inno package: /VERYSILENT or /SILENT. Run the engine unattended at once, with no window;
  // the engine keeps the previous install folder and components, and its exit code is returned.
  if (has_silent_switch(command_line)) {
    g_run.self = module_path();
    g_run.id = make_run_id();
    // /DIR= goes to the engine as the person's own choice, after the same checks as the window's (docs/installer-silent-mode.md)
    if (const auto dir = installer_dir::dir_switch(command_line)) {
      g_run.dir = *dir;
      g_run.dir_given = true;  // /DIR="" is a folder that is refused, not "no folder"
    }
    // a silent run has nobody to ask: a folder with only a warning is installed into when the command line says so
    g_run.dir_unsafe_ok = installer_dir::has_switch(command_line, L"/ALLOWUNSAFEPARENT");
    return worker_main();
  }
  if (has_argument(L"--bench")) {
    g_bench = true;
    g_bench_output = command_value(command_line, L"--bench");
    if (g_bench_output == L"--bench") g_bench_output.clear();
    return shell_main();
  }
  if (has_argument(L"--selftest-window")) {
    g_selftest_out = command_value(command_line, L"--selftest-window");
    while (!g_selftest_out.empty() && g_selftest_out.front() == L'"') g_selftest_out.erase(g_selftest_out.begin());
    while (!g_selftest_out.empty() && g_selftest_out.back() == L'"') g_selftest_out.pop_back();
    return shell_main();  // the native window, always
  }
  if (has_argument(L"--strip")) {
    std::wstring dir = command_line.substr(command_line.find(L"--strip ") + 8);
    while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
    return strip_main(dir);
  }
  if (has_argument(L"--preview")) {
    std::wstring dir = command_line.substr(command_line.find(L"--preview ") + 10);
    while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
    return preview_main(dir);
  }
  if (!has_argument(L"--native")) {
    const int code = web_main();
    if (code != kWebFallback) return code;
  }
  return shell_main();
}
