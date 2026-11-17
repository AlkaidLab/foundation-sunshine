/**
 * @file src/platform/windows/audio_vb_cable_win.cpp
 * @brief The Windows side of the VB-CABLE install and enable jobs (see src/audio_vb_cable.h).
 *
 * Sources (docs/authority/source-index.tsv): WinVerifyTrust (Authenticode check of the setup program before it is started),
 * CreateProcessW (the setup program, tar.exe and pnputil.exe run without a shell and with full paths), pnputil
 * /enable-device (the Device Manager's own enable), URLDownloadToFileW (the download, as the original host does it).
 */
#include <windows.h>

#include <sddl.h>
#include <wincrypt.h>
#include <softpub.h>
#include <urlmon.h>
#include <wintrust.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "src/audio_devices.h"
#include "src/audio_vb_cable.h"
#include "src/platform/windows/misc.h"

namespace audio_devices::vb_cable {
  namespace {
    constexpr auto setup_timeout = std::chrono::seconds(180);
    constexpr auto tool_timeout = std::chrono::seconds(120);
    constexpr std::uintmax_t max_download_bytes = 64ull * 1024 * 1024;

    std::wstring
    wide(const std::string &text) {
      return platf::from_utf8(text);
    }

    std::wstring
    system32(const wchar_t *file) {
      wchar_t dir[MAX_PATH] = {};
      const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
      return std::wstring(dir, n) + L"\\" + file;
    }

    /// Runs a program without a shell, hidden, and waits. nullopt on a timeout (that process only is ended) or when it cannot start.
    std::optional<int>
    run(const std::wstring &exe, const std::wstring &arguments, const std::wstring &directory, std::chrono::seconds timeout) {
      std::wstring command = L"\"" + exe + L"\" " + arguments;
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process {};
      if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &process)) {
        return std::nullopt;
      }
      const DWORD waited = WaitForSingleObject(process.hProcess, static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
      std::optional<int> code;
      if (waited == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(process.hProcess, &exit_code);
        code = static_cast<int>(exit_code);
      }
      else {
        TerminateProcess(process.hProcess, 1);
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      return code;
    }

    struct system_environment_t: environment_t {
      bool
      elevated() override {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        TOKEN_ELEVATION elevation {};
        DWORD size = 0;
        const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
        CloseHandle(token);
        return ok;
      }

      std::string
      state() override {
        std::vector<endpoint_t> endpoints;
        if (!enumerate_endpoints(endpoints)) return "unavailable";
        std::vector<driver_t> drivers;
        const bool read = enumerate_vb_cable_drivers(drivers);
        return summarize_vb_cable(endpoints, read ? &drivers : nullptr).state;
      }

      std::optional<std::string>
      disabled_instance_id() override {
        std::vector<driver_t> drivers;
        if (!enumerate_vb_cable_drivers(drivers)) return std::nullopt;
        for (const auto &driver : drivers) {
          if (is_vb_cable_driver(driver) && driver.disabled && !driver.instance_id.empty()) return driver.instance_id;
        }
        return std::nullopt;
      }

      bool
      download(std::string_view url, const std::string &zip_path) override {
        // urlmon is loaded by name like the original host does; the OS validates the TLS certificate of download.vb-audio.com
        HMODULE urlmon = LoadLibraryW(L"urlmon.dll");
        if (!urlmon) return false;
        auto fetch = reinterpret_cast<decltype(&URLDownloadToFileW)>(GetProcAddress(urlmon, "URLDownloadToFileW"));
        const bool ok = fetch && fetch(nullptr, wide(std::string(url)).c_str(), wide(zip_path).c_str(), 0, nullptr) == S_OK;
        FreeLibrary(urlmon);
        if (!ok) return false;
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::path(wide(zip_path)), ec);
        if (ec || size < 1024 || size > max_download_bytes) return false;
        char magic[2] = {};
        std::ifstream in(std::filesystem::path(wide(zip_path)), std::ios::binary);
        in.read(magic, 2);
        return in.gcount() == 2 && magic[0] == 'P' && magic[1] == 'K';
      }

      bool
      extract(const std::string &zip_path, const std::string &directory) override {
        const auto code = run(system32(L"tar.exe"), L"-xf \"" + wide(zip_path) + L"\" -C \"" + wide(directory) + L"\"", {}, tool_timeout);
        return code && *code == 0;
      }

      std::string
      find_setup(const std::string &directory) override {
        std::error_code ec;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(std::filesystem::path(wide(directory)), ec)) {
          if (entry.is_regular_file(ec) && _wcsicmp(entry.path().filename().c_str(), wide(std::string(setup_file)).c_str()) == 0) {
            return platf::to_utf8(entry.path().wstring());
          }
        }
        return {};
      }

      signature_t
      verify_signature(const std::string &file) override {
        const auto path = wide(file);
        WINTRUST_FILE_INFO file_info {};
        file_info.cbStruct = sizeof(file_info);
        file_info.pcwszFilePath = path.c_str();
        WINTRUST_DATA data {};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &file_info;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        signature_t result;
        result.valid = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS;
        if (CRYPT_PROVIDER_DATA *provider = WTHelperProvDataFromStateData(data.hWVTStateData)) {
          if (CRYPT_PROVIDER_SGNR *signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0)) {
            if (CRYPT_PROVIDER_CERT *cert = WTHelperGetProvCertFromChain(signer, 0)) {
              wchar_t subject[512] = {};
              if (CertNameToStrW(X509_ASN_ENCODING, &cert->pCert->pCertInfo->Subject, CERT_X500_NAME_STR, subject, 512)) result.signer = platf::to_utf8(std::wstring(subject));
            }
          }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        return result;
      }

      std::optional<int>
      run_setup(const std::string &setup_path) override {
        const auto path = std::filesystem::path(wide(setup_path));
        // the same switch the original install-vsink.bat uses; the host is a service, so nothing may open a window
        return run(path.wstring(), L"/S", path.parent_path().wstring(), setup_timeout);
      }

      std::optional<int>
      run_pnputil(const std::string &arguments) override {
        return run(system32(L"pnputil.exe"), wide(arguments), {}, tool_timeout);
      }

      std::string
      work_directory() override {
        // a directory only SYSTEM and the administrators can write: what is verified is what is run
        wchar_t root[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableW(L"ProgramData", root, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};
        const auto directory = std::filesystem::path(root) / L"Sunshine-VB-Cable" / std::to_wstring(GetTickCount64());
        SECURITY_ATTRIBUTES attributes { sizeof(attributes), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)", SDDL_REVISION_1, &attributes.lpSecurityDescriptor, nullptr)) return {};
        std::error_code ec;
        std::filesystem::create_directories(directory.parent_path(), ec);
        const bool created = CreateDirectoryW(directory.c_str(), &attributes) != 0;
        LocalFree(attributes.lpSecurityDescriptor);
        return created ? platf::to_utf8(directory.generic_wstring()) : std::string {};
      }

      void
      remove_directory(const std::string &directory) override {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(wide(directory)), ec);
      }

      std::string
      settle_state() override {
        // the endpoints of a new or re-enabled device appear a moment after the step ends
        std::string current;
        for (int i = 0; i < 15; ++i) {
          current = state();
          if (current == "ready") break;
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return current;
      }
    };
  }  // namespace

  std::unique_ptr<environment_t>
  make_system_environment() {
    return std::make_unique<system_environment_t>();
  }

  job_runner_t &
  runner() {
    static auto *instance = new job_runner_t();
    return *instance;
  }
}  // namespace audio_devices::vb_cable
