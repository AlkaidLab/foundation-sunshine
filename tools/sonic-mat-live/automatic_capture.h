#pragma once
#include "../../src/spatial_audio/mat_installation.h"
#include <stdexcept>
#include <thread>
#include <tlhelp32.h>
#include <vector>

namespace sonic_live {
  // The monitor owns only local handles. Capture still uses its existing
  // cooperative shutdown and verifies original hook bytes before returning.
  class CaptureOwner {
    HANDLE owner_ = nullptr, stop_ = nullptr, finished_ = nullptr;
    std::thread monitor_;

  public:
    CaptureOwner(DWORD pid, uint64_t created, const wchar_t *stop): owner_(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)),
                                                                    stop_(OpenEventW(SYNCHRONIZE, FALSE, stop)), finished_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
      if (!owner_ || !stop_ || !finished_ || native_mat_guard::created(owner_) != created || WaitForSingleObject(owner_, 0) != WAIT_TIMEOUT) {
        close();
        throw std::runtime_error("MAT owner identity or stop event is unavailable");
      }
    }
    CaptureOwner(const CaptureOwner &) = delete;
    ~CaptureOwner() {
      if (finished_) SetEvent(finished_);
      if (monitor_.joinable()) monitor_.join();
      close();
    }
    HANDLE
    process() const { return owner_; }
    bool
    cancelled() const {
      return WaitForSingleObject(owner_, 0) != WAIT_TIMEOUT || WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT;
    }
    void
    monitor(volatile LONG &interrupted) {
      monitor_ = std::thread([this, &interrupted] {
        HANDLE events[] = { finished_, owner_, stop_ };
        const auto result = WaitForMultipleObjects(3, events, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0) InterlockedExchange(&interrupted, 1);
      });
    }

  private:
    void
    close() {
      for (auto h : { owner_, stop_, finished_ })
        if (h) CloseHandle(h);
      owner_ = stop_ = finished_ = nullptr;
    }
  };

  struct EngineIdentity {
    DWORD pid;
    uint64_t created;
  };
  inline EngineIdentity
  automatic_engine() {
    std::vector<EngineIdentity> engines;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot enumerate the audio engine");
    PROCESSENTRY32W entry { sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"audiodg.exe")) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (process) {
          if (native_mat_guard::system_engine(process) && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            engines.push_back({ entry.th32ProcessID, native_mat_guard::created(process) });
          CloseHandle(process);
        }
        else
          engines.push_back({ 0, 0 });  // An unreadable candidate cannot be ruled out.
      } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    // No endpoint switching and no attempt to inject several engines to find one.
    if (engines.size() != 1 || !engines.front().pid) throw std::runtime_error("MAT needs one unambiguous Windows audio engine");
    return engines.front();
  }
}  // namespace sonic_live
