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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "installer_json.h"
#include "installer_state.h"
#include "installer_maintenance.h"
#include "installer_ui.h"

namespace {

  constexpr wchar_t kWindowClass[] = L"FoundationSunshineInstallerShell";
  constexpr wchar_t kWindowTitle[] = L"Foundation Sunshine";
  constexpr wchar_t kRunRoot[] = L"FoundationSunshine\\install-runs";
  constexpr UINT_PTR kTimer = 7;
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
    std::wstring worker_copy;  // the copy of this exe the elevated worker runs from
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
    std::array<char, 1024 * 1024> buffer {};
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
  // the pv value of the Evergreen Runtime client key, per machine (WOW6432Node on 64-bit Windows) or per user;
  // missing, empty or 0.0.0.0 means not installed.
  bool
  webview2_runtime_present() {
    // test switch, as the Panel's SUNSHINE_GUI_FORCE_NO_WEBVIEW2: behave as if the Runtime were missing
    if (GetEnvironmentVariableW(L"FOUNDATION_SETUP_FORCE_NO_WEBVIEW2", nullptr, 0) != 0) return false;
    constexpr wchar_t kClient[] = L"\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
    const std::wstring machine = std::wstring(L"SOFTWARE\\WOW6432Node") + kClient;
    const std::wstring machine32 = std::wstring(L"SOFTWARE") + kClient;
    const std::wstring user = std::wstring(L"Software") + kClient;
    const auto has = [](HKEY root, const std::wstring &key) {
      wchar_t buffer[64] {};
      DWORD size = sizeof(buffer) - sizeof(wchar_t);
      if (RegGetValueW(root, key.c_str(), L"pv", RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return false;
      const std::wstring pv = buffer;
      return !pv.empty() && pv != L"0.0.0.0";
    };
    return has(HKEY_LOCAL_MACHINE, machine) || has(HKEY_LOCAL_MACHINE, machine32) || has(HKEY_CURRENT_USER, user);
  }

  // A named section of the appended payload (append_installer_payload.py): <name>-offset= and <name>-size= after the
  // engine's offset= and size= in the footer. "ui" is the setup UI, "wv2" the WebView2 Evergreen bootstrapper.
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

  int
  worker_main() {
    const std::wstring id = g_run.id;
    const std::wstring root = program_data() + L"\\" + kRunRoot;
    g_run.directory = root + L"\\" + id;
    g_run.events = g_run.directory + L"\\events.jsonl";
    g_run.result = g_run.directory + L"\\install-result.json";
    if (!make_directory_tree(g_run.directory) || !protect_run_directory(g_run.directory)) return 5;
    const std::string id_utf8(id.begin(), id.end());
    append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 +
                                 "\",\"seq\":1,\"phase\":\"worker\",\"state\":\"running\",\"result_code\":0,\"message_key\":\"WorkerStarted\"}\n");
    const std::wstring engine = g_run.directory + L"\\sunshine-engine.exe";
    const std::wstring log_path = g_run.directory + L"\\inno.log";
    if (!extract_payload(g_run.self, engine)) {
      write_text_atomic(g_run.result, "{\"schema\":1,\"state\":\"failed\",\"result_code\":101,\"message_key\":\"PayloadInvalid\"}\n");
      return 101;
    }
    append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 +
                                 "\",\"seq\":2,\"phase\":\"engine\",\"state\":\"running\",\"result_code\":0,\"message_key\":\"EngineStarted\"}\n");
    DWORD code = 1;
    if (!run_engine(engine, log_path, g_run.directory, id, &code)) code = 101;
    // The Panel runs on WebView2: a successful install without the Runtime gets it from the bootstrapper appended to
    // this package (Microsoft Learn "Distribute your app and the WebView2 Runtime": MicrosoftEdgeWebview2Setup.exe
    // /silent /install, per machine from this elevated process). Offline it fails: that is logged, never an install failure.
    if ((code == 0 || code == 3010) && !webview2_runtime_present()) {
      uint64_t wv2_offset = 0, wv2_size = 0;
      std::string wv2_state = "missing";
      DWORD wv2_code = 0;
      const std::wstring setup = g_run.directory + L"\\MicrosoftEdgeWebview2Setup.exe";
      if (read_named_payload(g_run.self, "wv2", &wv2_offset, &wv2_size) && extract_range(g_run.self, wv2_offset, wv2_size, setup)) {
        append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"seq\":5,\"phase\":\"webview2\",\"state\":\"running\",\"result_code\":0,\"message_key\":\"WebView2Bootstrapper\"}\n");
        std::wstring command = L"\"" + setup + L"\" /silent /install";
        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');
        STARTUPINFOW startup {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process {};
        if (CreateProcessW(setup.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, g_run.directory.c_str(), &startup, &process)) {
          if (WaitForSingleObject(process.hProcess, 10 * 60 * 1000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &wv2_code);
          else wv2_code = WAIT_TIMEOUT;
          CloseHandle(process.hThread);
          CloseHandle(process.hProcess);
        }
        else
          wv2_code = GetLastError();
        wv2_state = webview2_runtime_present() ? "installed" : "missing";
      }
      append_event(g_run.events, "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"seq\":6,\"phase\":\"webview2\",\"state\":\"" +
                                   (wv2_state == "installed" ? "done" : "failed") + "\",\"result_code\":" + std::to_string(wv2_code) +
                                   ",\"message_key\":\"WebView2Runtime-" + wv2_state + "\"}\n");
    }
    const auto state = installer_state::classify_exit_code(code);
    const std::string state_name(installer_state::name(state));
    const std::string result = "{\"schema\":1,\"run\":\"" + id_utf8 + "\",\"state\":\"" + state_name +
                               "\",\"result_code\":" + std::to_string(code) + "}\n";
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
  // The floor: Windows 10 (any build, 1507 / 10240). Below it nothing of Foundation Sunshine runs; say so and stop.
  if (windows_build() == 0) {
    if (!has_silent_switch(command_line) && command_line.find(L"--worker") == std::wstring::npos) {
      const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
      MessageBoxW(nullptr, zh ? L"Foundation Sunshine 需要 Windows 10 或更高版本。" : L"Foundation Sunshine needs Windows 10 or later.",
        L"Foundation Sunshine", MB_OK | MB_ICONINFORMATION);
    }
    return 1150;  // ERROR_OLD_WIN_VERSION
  }
  if (command_line.find(L"--worker") != std::wstring::npos) {
    g_run.self = module_path();
    const std::wstring id = command_value(command_line, L"--run-id");
    if (id.empty()) return 1223;
    g_run.id = id;
    g_run.components = only_component_chars(command_value(command_line, L"--components"));
    for (const wchar_t ch : command_value(command_line, L"--shell-pid"))
      if (ch >= L'0' && ch <= L'9') g_run.shell_pid.push_back(ch);
    const std::wstring tasks = command_value(command_line, L"--tasks");
    g_run.tasks = tasks == L"desktopicon" || tasks == L"none" ? tasks : L"";
    return worker_main();
  }
  // An updater (the Panel's, an old Panel's, a script) starts the package the way it started the
  // old Inno package: /VERYSILENT or /SILENT. Run the engine unattended at once, with no window;
  // the engine keeps the previous install folder and components, and its exit code is returned.
  if (has_silent_switch(command_line)) {
    g_run.self = module_path();
    g_run.id = make_run_id();
    return worker_main();
  }
  const size_t bench = command_line.find(L"--bench");
  if (bench != std::wstring::npos) {
    g_bench = true;
    g_bench_output = command_value(command_line, L"--bench");
    if (g_bench_output == L"--bench") g_bench_output.clear();
    return shell_main();
  }
  if (command_line.find(L"--selftest-window ") != std::wstring::npos) {
    g_selftest_out = command_value(command_line, L"--selftest-window");
    while (!g_selftest_out.empty() && g_selftest_out.front() == L'"') g_selftest_out.erase(g_selftest_out.begin());
    while (!g_selftest_out.empty() && g_selftest_out.back() == L'"') g_selftest_out.pop_back();
    return shell_main();  // the native window, always
  }
  const size_t strip = command_line.find(L"--strip ");
  if (strip != std::wstring::npos) {
    std::wstring dir = command_line.substr(strip + 8);
    while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
    return strip_main(dir);
  }
  const size_t preview = command_line.find(L"--preview ");
  if (preview != std::wstring::npos) {
    std::wstring dir = command_line.substr(preview + 10);
    while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
    while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
    return preview_main(dir);
  }
  if (command_line.find(L"--native") == std::wstring::npos) {
    const int code = web_main();
    if (code != kWebFallback) return code;
  }
  return shell_main();
}
