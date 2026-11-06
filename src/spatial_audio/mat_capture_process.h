#pragma once
#include "mat_installation.h"
#include <memory>
#include <mutex>
#include <shellapi.h>
#include <stdexcept>
#include <thread>
#include <wtsapi32.h>

namespace spatial_audio {
  class MatCaptureProcess {
    struct State {
      std::mutex mutex;
      HANDLE stop = nullptr, process = nullptr;
      std::string error;
      ~State() {
        if (process) CloseHandle(process);
        if (stop) CloseHandle(stop);
      }
    };
    std::shared_ptr<State> state_;

    static std::wstring
    session_user() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
      auto sid = [](HANDLE token) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        LPWSTR text = nullptr;
        std::wstring result;
        if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
          result = text;
          LocalFree(text);
        }
        return result;
      };
      auto user = sid(token);
      CloseHandle(token);
      if (user == L"S-1-5-18") {
        DWORD session = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !WTSQueryUserToken(session, &token)) return {};
        user = sid(token);
        CloseHandle(token);
      }
      return user;
    }

  public:
    ~MatCaptureProcess() { stop(); }
    void
    start() {
      if (state_) return;
      state_ = std::make_shared<State>();
      auto state = state_;
      try {
        const auto host = std::filesystem::path(native_mat_guard::image(GetCurrentProcess()));
        const auto image = host.parent_path() / L"tools/mat-capture/sunshine-plugin-mat-capture.exe";
        if (!mat_installed_image(image)) throw std::runtime_error("MAT capture is missing or outside a protected installation. Reinstall Sunshine.");
        const auto user = session_user();
        if (user.empty()) throw std::runtime_error("MAT cannot identify the signed-in Windows user.");
        BYTE random[16];
        if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) throw std::runtime_error("MAT stop event creation failed.");
        std::wstring event = L"Local\\SunshineMatLiveOwner-";
        for (auto b : random) {
          event += L"0123456789abcdef"[b >> 4];
          event += L"0123456789abcdef"[b & 15];
        }
        PSECURITY_DESCRIPTOR acl = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x100002;;;AU)", SDDL_REVISION_1, &acl, nullptr))
          throw std::runtime_error("MAT stop event permissions failed.");
        SECURITY_ATTRIBUTES sa { sizeof(sa), acl, FALSE };
        state->stop = CreateEventW(&sa, TRUE, FALSE, event.c_str());
        const auto error = GetLastError();
        LocalFree(acl);
        if (!state->stop || error == ERROR_ALREADY_EXISTS) throw std::runtime_error("MAT stop event creation failed.");
        const std::wstring args = L"--automatic " + std::to_wstring(GetCurrentProcessId()) + L" " +
                                  std::to_wstring(native_mat_guard::created(GetCurrentProcess())) + L" " + event + L" " + user;
        // UAC must not block the web UI or streaming server. The child checks
        // both the exact owner process and this stop event before attaching.
        std::thread([state, image, args] {
          HANDLE token = nullptr;
          TOKEN_ELEVATION elevation {};
          DWORD size = 0;
          bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                          GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
          if (token) CloseHandle(token);
          HANDLE process = nullptr;
          DWORD error = ERROR_SUCCESS;
          if (elevated) {
            auto command = L"\"" + image.wstring() + L"\" " + args;
            STARTUPINFOW startup { sizeof(startup) };
            PROCESS_INFORMATION info {};
            if (CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, image.parent_path().c_str(), &startup, &info)) {
              CloseHandle(info.hThread);
              process = info.hProcess;
            }
            else
              error = GetLastError();
          }
          else {
            const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            SHELLEXECUTEINFOW info { sizeof(info) };
            info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.lpVerb = L"runas";
            info.lpFile = image.c_str();
            info.lpParameters = args.c_str();
            info.nShow = SW_HIDE;
            if (ShellExecuteExW(&info))
              process = info.hProcess;
            else
              error = GetLastError();
            if (SUCCEEDED(com)) CoUninitialize();
          }
          std::lock_guard lock(state->mutex);
          state->process = process;
          if (!process) state->error = error == ERROR_CANCELLED ? "MAT authorization was cancelled. Apply the MAT setting again to retry." :
                                                                  "MAT capture could not start (Windows error " + std::to_string(error) + ").";
        }).detach();
      }
      catch (const std::exception &e) {
        state->error = e.what();
      }
    }
    void
    stop() {
      if (state_ && state_->stop) SetEvent(state_->stop);
    }
    std::string
    error() const {
      if (!state_) return {};
      std::lock_guard lock(state_->mutex);
      if (!state_->error.empty()) return state_->error;
      if (state_->process && WaitForSingleObject(state_->process, 0) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(state_->process, &code);
        if (code == 4) return "MAT cannot identify a single Windows audio engine on this setup.";
        return code == 3 ? "This Windows audio engine or Dolby Access version is not supported for MAT." :
                           "MAT capture stopped (code " + std::to_string(code) + "). Check the audio engine and apply the MAT setting again to retry.";
      }
      return {};
    }
  };
  inline MatCaptureProcess mat_capture_process;
}  // namespace spatial_audio
