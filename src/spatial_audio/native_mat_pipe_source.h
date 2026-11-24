#pragma once
#include "encoded_audio_source.h"
#include "mat_capture_protocol.h"
#include "mat_capture_reader.h"
#include "mat_live_status.h"
#include "mat_auto_source.h"
#include <chrono>
#include <functional>
#include <memory>
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <filesystem>
namespace spatial_audio {
  class NativeMatPipeSource final: public EncodedAudioSource {
    HANDLE pipe_ = INVALID_HANDLE_VALUE, publisher_ = nullptr, engine_ = nullptr;
    mat_capture::Descriptor native_ {};
    State state_ = State::preparing;
    uint64_t next_ = 0, last_qpc_ = 0;
    std::string error_;
    bool status_leased_ = false, status_running_ = false;
    std::function<bool()> cancelled_ = [] { return false; };
    void
    close() noexcept {
      if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
      }
      if (publisher_) {
        CloseHandle(publisher_);
        publisher_ = nullptr;
      }
      if (engine_) {
        CloseHandle(engine_);
        engine_ = nullptr;
      }
      if (status_leased_) {
        mat_live_status.released(status_running_);
        status_leased_ = false;
        status_running_ = false;
      }
    }
    static uint64_t
    created(HANDLE p) {
      FILETIME c {}, e {}, k {}, u {};
      require(GetProcessTimes(p, &c, &e, &k, &u) != 0, "MAT process creation time");
      return (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }
    static std::wstring
    image(HANDLE p) {
      wchar_t path[32768];
      DWORD n = 32768;
      require(QueryFullProcessImageNameW(p, 0, path, &n) != 0, "MAT publisher/engine image");
      return std::wstring(path, n);
    }
    void
    io(void *data, DWORD bytes, bool writing) {
      auto *p = static_cast<uint8_t *>(data);
      DWORD offset = 0;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (offset < bytes) {
        require(!cancelled_(), "MAT pipe cancelled");
        OVERLAPPED o {};
        o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(o.hEvent != nullptr, "MAT pipe I/O event");
        DWORD n = 0;
        BOOL okay = writing ? WriteFile(pipe_, p + offset, bytes - offset, &n, &o) : ReadFile(pipe_, p + offset, bytes - offset, &n, &o);
        DWORD code = okay ? 0 : GetLastError();
        if (!okay && code == ERROR_IO_PENDING) {
          while (WaitForSingleObject(o.hEvent, 20) == WAIT_TIMEOUT) {
            if (cancelled_() || std::chrono::steady_clock::now() >= deadline) break;
          }
          if (cancelled_() || std::chrono::steady_clock::now() >= deadline) {
            CancelIoEx(pipe_, &o);
            GetOverlappedResult(pipe_, &o, &n, TRUE);
            CloseHandle(o.hEvent);
            throw std::runtime_error("MAT pipe I/O timeout/cancel");
          }
          okay = GetOverlappedResult(pipe_, &o, &n, FALSE);
        }
        CloseHandle(o.hEvent);
        require(okay && n && n <= bytes - offset, "MAT pipe disconnected/I/O failure");
        offset += n;
      }
    }

  public:
    NativeMatPipeSource() = default;
    NativeMatPipeSource(const NativeMatPipeSource &) = delete;
    NativeMatPipeSource &
    operator=(const NativeMatPipeSource &) = delete;
    ~NativeMatPipeSource() override { stop(); }
    void
    prepare(const std::string &name, const std::string &producer) {
      require(state_ == State::preparing, "Native MAT source lease already prepared/closed");
      try {
        require(!name.empty() && name.starts_with("\\\\.\\pipe\\SunshineMatLive-") &&
                  (!producer.empty() || name == mat_auto_pipe),
          "Local native MAT source required");
        require(name.find_first_of("\r\n") == std::string::npos, "MAT pipe name");
        pipe_ = CreateFileW(std::filesystem::path(name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        require(pipe_ != INVALID_HANDLE_VALUE, "Native MAT capture is not armed or is leased");
        ULONG pid = 0;
        require(GetNamedPipeServerProcessId(pipe_, &pid), "Native MAT pipe publisher PID");
        publisher_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        require(publisher_ != nullptr, "MAT publisher process");
        if (producer.empty()) require(trusted_mat_auto_publisher(publisher_), "Untrusted automatic MAT source");
        char command[8];
        memcpy(command, "MCAPOPEN", 8);
        io(command, 8, true);
        uint8_t header[mat_capture::descriptor_bytes];
        io(header, sizeof(header), false);
        require(mat_capture::decode_descriptor(header, native_), "Native MAT descriptor (test/file sources forbidden)");
        require(pid == native_.publisher_pid, "Native MAT pipe publisher PID");
        engine_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, native_.engine_pid);
        require(publisher_ && engine_ && created(publisher_) == native_.publisher_created && created(engine_) == native_.engine_created,
          "MAT publisher/engine identity changed");
        if (!producer.empty()) {
          auto expected = std::filesystem::canonical(producer).wstring();
          auto actual = std::filesystem::canonical(image(publisher_)).wstring();
          require(_wcsicmp(expected.c_str(), actual.c_str()) == 0, "MAT pipe belongs to unexpected producer image");
        }
        wchar_t system[MAX_PATH];
        require(GetSystemDirectoryW(system, MAX_PATH) > 0, "Windows system directory");
        auto target = (std::filesystem::path(system) / L"audiodg.exe").wstring();
        require(_wcsicmp(image(engine_).c_str(), target.c_str()) == 0, "Native MAT engine image");
        state_ = State::idle;
        mat_live_status.acquired();
        status_leased_ = true;
      }
      catch (const std::exception &e) {
        error_ = e.what();
        mat_live_status.failed(error_);
        state_ = State::fault;
        close();
        throw;
      }
    }
    const mat_capture::Descriptor &
    identity() const { return native_; }
    Descriptor
    descriptor() const override { return {}; }
    void
    cancel_when(std::function<bool()> fn) { cancelled_ = std::move(fn); }
    bool
    start() override {
      if (state_ != State::idle) return false;
      try {
        char command[8];
        memcpy(command, "MCAPPLAY", 8);
        io(command, 8, true);
        state_ = State::running;
        mat_live_status.started();
        status_running_ = true;
        return true;
      }
      catch (const std::exception &e) {
        error_ = e.what();
        if (!cancelled_()) mat_live_status.failed(error_);
        state_ = State::fault;
        close();
        return false;
      }
    }
    Read
    read(Frame &out) override {
      if (state_ == State::stopped) return Read::ended;
      if (state_ != State::running) return Read::failed;
      try {
        require(WaitForSingleObject(publisher_, 0) == WAIT_TIMEOUT && WaitForSingleObject(engine_, 0) == WAIT_TIMEOUT, "Native MAT owner exited");
        auto status = mat_capture::read_record([this](void *p, size_t n, bool writing) { io(p, (DWORD) n, writing); }, next_, last_qpc_, native_.epoch, native_.encoder, out);
        if (status == Read::ended) {
          state_ = State::stopped;
          close();
        }
        return status;
      }
      catch (const std::exception &e) {
        error_ = e.what();
        if (!cancelled_()) mat_live_status.failed(error_);
        state_ = State::fault;
        close();
        return Read::failed;
      }
    }
    void
    stop() override {
      close();
      if (state_ != State::fault) state_ = State::stopped;
    }
    State
    state() const override { return state_; }
    std::string
    error() const override { return error_; }
  };
}  // namespace spatial_audio
#endif
