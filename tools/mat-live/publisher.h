#pragma once
#include "process_helpers.h"
#include "../../src/spatial_audio/native_mat_guard.h"
#include "../../src/spatial_audio/mat_capture_protocol.h"
#include "../../src/spatial_audio/encoded_audio_source.h"
#include <array>
#include <functional>
#include <sddl.h>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")
using spatial_audio::require;
struct Cycle {
  std::array<uint8_t, MAT_WIRE_CYCLE> bytes;
  uint64_t qpc;
};
struct Shared {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Cycle> queue;
  bool failed = false, ended = false, playing = false, ready = false;
  uint64_t encoder = 0, play_qpc = 0, last_cycle = 0, cycles = 0, acknowledged_leases = 0;
  std::string error, delivered_sha256;
};
// Metadata-only acceptance oracle for the post-PLAY stream; no payload storage.
class LeaseDigest {
  BCRYPT_ALG_HANDLE algorithm_ {};
  BCRYPT_HASH_HANDLE hash_ {};
  std::vector<BYTE> object_;
  void
  close() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    hash_ = nullptr;
    algorithm_ = nullptr;
  }

public:
  LeaseDigest() {
    try {
      DWORD size = 0, got = 0;
      require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "Lease SHA256 algorithm");
      require(BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, (PUCHAR) &size, sizeof(size), &got, 0) >= 0, "Lease SHA256 object");
      object_.resize(size);
      require(BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) >= 0, "Lease SHA256 init");
    }
    catch (...) {
      close();
      throw;
    }
  }
  ~LeaseDigest() { close(); }
  void
  update(const uint8_t *p, size_t n) { require(n <= MAXDWORD && BCryptHashData(hash_, const_cast<PUCHAR>(p), (ULONG) n, 0) >= 0, "Lease SHA256 update"); }
  std::string
  finish() {
    BYTE digest[32];
    require(BCryptFinishHash(hash_, digest, 32, 0) >= 0, "Lease SHA256 finish");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto b : digest) {
      out += hex[b >> 4];
      out += hex[b & 15];
    }
    return out;
  }
};
static uint64_t
Qpc() {
  LARGE_INTEGER n {};
  require(QueryPerformanceCounter(&n) != FALSE, "QPC");
  return n.QuadPart;
}
static void
Broken(Shared &s, const char *message) {
  std::lock_guard lock(s.mutex);
  s.failed = true;
  s.error = message;
  s.queue.clear();
  s.changed.notify_all();
}
static uintptr_t
LoadedTap(DWORD pid, const fs::path &expected) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
  require(snap != INVALID_HANDLE_VALUE, "Enumerate loaded tap identity");
  MODULEENTRY32W entry {};
  entry.dwSize = sizeof(entry);
  uintptr_t address = 0;
  try {
    require(Module32FirstW(snap, &entry) != FALSE, "Read loaded module list");
    do {
      if (_wcsicmp(entry.szModule, expected.filename().c_str())) continue;
      auto actual = fs::canonical(entry.szExePath);
      require(!address && _wcsicmp(expected.c_str(), actual.c_str()) == 0, "Loaded tap path differs; no reuse");
      address = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snap, &entry));
  }
  catch (...) {
    CloseHandle(snap);
    throw;
  }
  CloseHandle(snap);
  return address;
}
static bool
Io(HANDLE pipe, void *buffer, DWORD size, bool write, DWORD timeout, std::function<bool()> cancelled = [] { return false; }) {
  auto *b = static_cast<BYTE *>(buffer);
  DWORD done = 0;
  auto began = GetTickCount64();
  while (done < size && !interrupted && !cancelled()) {
    OVERLAPPED o {};
    o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!o.hEvent) return false;
    DWORD n = 0;
    BOOL okay = write ? WriteFile(pipe, b + done, size - done, &n, &o) : ReadFile(pipe, b + done, size - done, &n, &o);
    if (!okay && GetLastError() == ERROR_IO_PENDING) {
      while (WaitForSingleObject(o.hEvent, 20) == WAIT_TIMEOUT && !interrupted && !cancelled() && GetTickCount64() - began < timeout) {}
      if (interrupted || cancelled() || GetTickCount64() - began >= timeout) {
        CancelIoEx(pipe, &o);
        GetOverlappedResult(pipe, &o, &n, TRUE);
        CloseHandle(o.hEvent);
        return false;
      }
      okay = GetOverlappedResult(pipe, &o, &n, FALSE);
    }
    CloseHandle(o.hEvent);
    if (!okay || !n) return false;
    done += n;
  }
  return done == size;
}
static PSECURITY_DESCRIPTOR
PipeAcl() {
  HANDLE token {};
  require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "Pipe token");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> bytes(size);
  bool okay = GetTokenInformation(token, TokenUser, bytes.data(), size, &size) != FALSE;
  CloseHandle(token);
  require(okay, "Pipe token user");
  LPWSTR sid {};
  require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &sid) != FALSE, "Pipe SID");
  std::wstring acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR result {};
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &result, nullptr) != FALSE, "Private pipe ACL");
  return result;
}
static void
Serve(Shared &shared, const std::wstring &name, mat_capture::Descriptor descriptor) {
  auto acl = PipeAcl();
  SECURITY_ATTRIBUTES attributes { sizeof(attributes), acl, FALSE };
  try {
    while (!interrupted) {
      {
        std::lock_guard lock(shared.mutex);
        if (shared.failed || shared.ended) break;
      }
      HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 131072, 4096, 0, &attributes);
      require(pipe != INVALID_HANDLE_VALUE, "Exclusive local MAT pipe");
      try {
        OVERLAPPED o {};
        o.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(o.hEvent != nullptr, "Pipe connect event");
        BOOL connected = ConnectNamedPipe(pipe, &o);
        DWORD code = connected ? 0 : GetLastError();
        if (!connected && code == ERROR_PIPE_CONNECTED) connected = TRUE;
        if (!connected && code == ERROR_IO_PENDING) {
          while (WaitForSingleObject(o.hEvent, 20) == WAIT_TIMEOUT && !interrupted) {
            std::lock_guard lock(shared.mutex);
            if (shared.failed || shared.ended) break;
          }
          DWORD n = 0;
          if (interrupted) { CancelIoEx(pipe, &o); }
          else {
            std::lock_guard lock(shared.mutex);
            if (shared.failed || shared.ended) CancelIoEx(pipe, &o);
          }
          connected = GetOverlappedResult(pipe, &o, &n, TRUE);
        }
        CloseHandle(o.hEvent);
        const auto unavailable = [&] {std::lock_guard lock(shared.mutex);return shared.failed||shared.ended; };
        char command[8];
        require(connected && Io(pipe, command, 8, false, 2000, unavailable) && !memcmp(command, "MCAPOPEN", 8), "Live source lease open");
        {
          std::lock_guard lock(shared.mutex);
          require(shared.ready && !shared.failed && GetTickCount64() - shared.last_cycle < 2000, "Native MAT not ready");
          descriptor.encoder = shared.encoder;
        }
        uint8_t header[mat_capture::descriptor_bytes];
        mat_capture::encode_descriptor(header, descriptor);
        require(Io(pipe, header, sizeof(header), true, 2000, unavailable), "Live source descriptor");
        require(Io(pipe, command, 8, false, 30000, unavailable) && !memcmp(command, "MCAPPLAY", 8), "Live source lease play timeout/disconnect");
        {
          std::lock_guard lock(shared.mutex);
          require(!shared.failed && shared.ready && GetTickCount64() - shared.last_cycle < 2000, "Source stale at PLAY");
          shared.queue.clear();
          shared.play_qpc = Qpc();
          shared.playing = true;
        }
        uint64_t sequence = 0;
        LeaseDigest digest;
        while (!interrupted) {
          Cycle cycle {};
          bool end = false;
          {
            std::unique_lock lock(shared.mutex);
            shared.changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return shared.failed || shared.ended || !shared.queue.empty(); });
            require(!shared.failed, "Native capture failed (no EOS)");
            if (shared.queue.empty()) {
              if (shared.ended)
                end = true;
              else {
                require(GetTickCount64() - shared.last_cycle < 2000, "Native capture stalled");
                continue;
              }
            }
            else {
              cycle = std::move(shared.queue.front());
              shared.queue.pop_front();
            }
          }
          uint8_t record[mat_capture::record_bytes];
          mat_capture::encode_record(record, end ? mat_capture::end_kind : mat_capture::cycle_kind, sequence, end ? Qpc() : cycle.qpc,
            end ? nullptr : cycle.bytes.data(), end ? 0 : cycle.bytes.size());
          require(Io(pipe, record, sizeof(record), true, 2000) && (end || Io(pipe, cycle.bytes.data(), (DWORD) cycle.bytes.size(), true, 2000)), "Live source consumer disconnected/backpressure");
          if (end) {
            // DisconnectNamedPipe discards buffered unread bytes.
            // Keep this lease alive until the client has consumed
            // the exact EOS; a completed WriteFile is insufficient.
            // Never use unbounded FlushFileBuffers on a lost peer.
            uint8_t ack[mat_capture::end_ack_bytes];
            require(Io(pipe, ack, sizeof(ack), false, 2000) && mat_capture::valid_end_ack(ack, sequence), "Live source end acknowledgement failed/timed out");
            auto sha = digest.finish();
            {
              std::lock_guard lock(shared.mutex);
              ++shared.acknowledged_leases;
              shared.delivered_sha256 = sha;
            }
            printf("LEASE_END_ACK cycles=%llu sha256=%s\n", sequence, sha.c_str());
            break;
          }
          digest.update(cycle.bytes.data(), cycle.bytes.size());
          ++sequence;
        }
      }
      catch (const std::exception &e) {
        printf("LEASE_CLOSED %s\n", e.what());
      }
      {
        std::lock_guard lock(shared.mutex);
        shared.playing = false;
        shared.queue.clear();
      }
      CancelIoEx(pipe, nullptr);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }
  }
  catch (...) {
    LocalFree(acl);
    throw;
  }
  LocalFree(acl);
}
