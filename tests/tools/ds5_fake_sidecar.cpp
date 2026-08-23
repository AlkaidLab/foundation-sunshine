/**
 * @file tests/tools/ds5_fake_sidecar.cpp
 * @brief Minimal DS5 protocol peer that leaves the Core reader blocked until EOF.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
  constexpr std::uint32_t MAGIC = 0x35534453;
  constexpr std::size_t HEADER_SIZE = 16;

  std::uint16_t read_u16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
  }

  std::uint32_t read_u32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
  }

  void write_u16(std::uint8_t *p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
  }

  void write_u32(std::uint8_t *p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8);
    p[2] = static_cast<std::uint8_t>(value >> 16);
    p[3] = static_cast<std::uint8_t>(value >> 24);
  }

  bool transfer(HANDLE pipe, void *buffer, DWORD size, bool write) {
    DWORD count = 0;
    return (write ? WriteFile(pipe, buffer, size, &count, nullptr) :
                    ReadFile(pipe, buffer, size, &count, nullptr)) &&
           count == size;
  }

  bool reply(HANDLE pipe, std::uint16_t type, std::uint32_t request_id,
             const std::vector<std::uint8_t> &payload) {
    std::array<std::uint8_t, HEADER_SIZE> header {};
    write_u32(header.data(), MAGIC);
    write_u16(header.data() + 4, 1);
    write_u16(header.data() + 6, type);
    write_u32(header.data() + 8, static_cast<std::uint32_t>(payload.size()));
    write_u32(header.data() + 12, request_id);
    return transfer(pipe, header.data(), static_cast<DWORD>(header.size()), true) &&
           (payload.empty() || transfer(pipe, const_cast<std::uint8_t *>(payload.data()),
                                        static_cast<DWORD>(payload.size()), true));
  }
}

int main(int argc, char **argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--pipe") return 2;
  const std::string pipe_name(argv[2]);
  constexpr std::string_view prefix = "sunshine-ds5-v1-";
  const auto pid_end = pipe_name.find('-', prefix.size());
  if (!pipe_name.starts_with(prefix) || pid_end == std::string::npos) return 2;
  const std::wstring parent_pid(pipe_name.begin() + prefix.size(),
                                pipe_name.begin() + pid_end);
  std::array<wchar_t, 256> event_suffix_buffer {};
  const auto event_suffix_size = GetEnvironmentVariableW(
    L"SUNSHINE_DS5_TEST_EVENT_SUFFIX", event_suffix_buffer.data(),
    static_cast<DWORD>(event_suffix_buffer.size()));
  const auto event_suffix = event_suffix_size > 0 && event_suffix_size < event_suffix_buffer.size() ?
                              std::wstring(event_suffix_buffer.data(), event_suffix_size) :
                              parent_pid;
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + event_suffix;
  const auto continue_event = OpenEventW(SYNCHRONIZE, FALSE, continue_name.c_str());
  if (!continue_event) return 2;
  // The test process opts this peer into emitting async feedback ahead of the
  // attach reply, exercising the Core client's transaction multiplexing.
  const auto interleave = GetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_INTERLEAVE", nullptr, 0) > 0;
  const auto audio_policy_fallback =
    GetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_AUDIO_POLICY_FALLBACK", nullptr, 0) > 0;
  const auto legacy_capabilities =
    GetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_LEGACY_CAPABILITIES", nullptr, 0) > 0;
  const auto genshin_compatibility =
    GetEnvironmentVariableW(L"SUNSHINE_DS5_TEST_GENSHIN_COMPATIBILITY", nullptr, 0) > 0;
  const auto policy_once_name = L"Local\\sunshine-ds5-test-policy-once-" + event_suffix;
  const auto policy_once_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                            policy_once_name.c_str());
  const auto hid_fallback_name = L"Local\\sunshine-ds5-test-hid-fallback-" + event_suffix;
  const auto hid_fallback_event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                             hid_fallback_name.c_str());
  const auto crash_once_name = L"Local\\sunshine-ds5-test-crash-once-" + event_suffix;
  const auto crash_once_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                           crash_once_name.c_str());
  const auto crash_always_name = L"Local\\sunshine-ds5-test-crash-always-" + event_suffix;
  const auto crash_always_event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                             crash_always_name.c_str());
  const auto recovered_name = L"Local\\sunshine-ds5-test-recovered-" + event_suffix;
  const auto recovered_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, recovered_name.c_str());
  const auto recovery_started_name = L"Local\\sunshine-ds5-test-recovery-started-" + event_suffix;
  const auto recovery_started_event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                                  recovery_started_name.c_str());
  const auto recovery_wait_name = L"Local\\sunshine-ds5-test-recovery-wait-" + event_suffix;
  const auto recovery_wait_event = OpenEventW(SYNCHRONIZE, FALSE,
                                               recovery_wait_name.c_str());
  const auto marker_name = L"Local\\sunshine-ds5-test-marker-" + event_suffix;
  const auto marker_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, marker_name.c_str());
  const auto genshin_compatibility_name =
    L"Local\\sunshine-ds5-test-genshin-compatibility-" + event_suffix;
  const auto genshin_compatibility_event = OpenEventW(
    EVENT_MODIFY_STATE, FALSE, genshin_compatibility_name.c_str());

  // A crash-once test can hold the replacement process before it creates its
  // pipe, exposing the Core client's assigned-but-offline recovery window.
  if (crash_once_event && recovery_wait_event &&
      WaitForSingleObject(crash_once_event, 0) == WAIT_OBJECT_0) {
    if (recovery_started_event) SetEvent(recovery_started_event);
    if (WaitForSingleObject(recovery_wait_event, 5000) != WAIT_OBJECT_0) return 5;
  }

  const auto path = L"\\\\.\\pipe\\" + std::wstring(pipe_name.begin(), pipe_name.end());
  const auto pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                     1, 4096, 4096, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return 3;
  if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
    CloseHandle(pipe);
    return 4;
  }

  while (true) {
    std::array<std::uint8_t, HEADER_SIZE> header {};
    if (!transfer(pipe, header.data(), static_cast<DWORD>(header.size()), false)) break;
    const auto size = read_u32(header.data() + 8);
    std::vector<std::uint8_t> payload(size);
    if (size && !transfer(pipe, payload.data(), size, false)) break;
    const auto type = read_u16(header.data() + 6);
    const auto request_id = read_u32(header.data() + 12);
    if (type == 1) {
      std::vector<std::uint8_t> capabilities(4);
      std::uint32_t advertised_capabilities = 0;
      if (genshin_compatibility) {
        advertised_capabilities |= 1u << 8;
      }
      if (!legacy_capabilities) {
        advertised_capabilities |= 1u << 9;
      }
      write_u32(capabilities.data(), advertised_capabilities);
      if (!reply(pipe, 2, request_id, capabilities)) break;
    } else if (type == 3 && payload.size() == 4) {
      if (interleave) {
        std::vector<std::uint8_t> early(6);
        early[0] = payload[0];
        if (!reply(pipe, 101, 0, early)) break;
      }
      std::vector<std::uint8_t> response(8);
      response[0] = payload[0];
      if ((audio_policy_fallback || genshin_compatibility) && payload[2] == 1) {
        response[1] = 1;
      }
      if (!reply(pipe, 4, request_id, response)) break;
      if (genshin_compatibility && payload[2] == 1 && (payload[3] & 1) != 0 &&
          genshin_compatibility_event) {
        SetEvent(genshin_compatibility_event);
      }
      if (audio_policy_fallback && policy_once_event &&
          WaitForSingleObject(policy_once_event, 0) == WAIT_TIMEOUT) {
        // The first process accepts the composite attach, then reports the
        // same asynchronous violation as the real endpoint guard and exits.
        FlushFileBuffers(pipe);
        SetEvent(policy_once_event);
        if (!reply(pipe, 105, 0, { payload[0], payload[1], 0, 0 })) break;
        FlushFileBuffers(pipe);
        break;
      }
      if (audio_policy_fallback && policy_once_event && hid_fallback_event &&
          WaitForSingleObject(policy_once_event, 0) == WAIT_OBJECT_0 && payload[2] == 0) {
        SetEvent(hid_fallback_event);
      }
      if (crash_always_event) {
        // Ensure Core has consumed the attach reply before simulating the
        // post-attach crash; otherwise the test races pipe teardown.
        FlushFileBuffers(pipe);
        SetEvent(crash_always_event);
        break;
      }
      if (crash_once_event && WaitForSingleObject(crash_once_event, 0) == WAIT_TIMEOUT) {
        // The attach itself must succeed before the fake peer simulates an
        // asynchronous crash. Wait until Core consumes the reply so closing
        // the pipe cannot race the synchronous attach transaction.
        FlushFileBuffers(pipe);
        SetEvent(crash_once_event);
        break;
      }
      if (crash_once_event && recovered_event) {
        SetEvent(recovered_event);
      }
      // Let the test observe the Core reader's first blocked read before
      // sending a marker that forces one complete read-loop iteration.
      if (WaitForSingleObject(continue_event, 5000) != WAIT_OBJECT_0) break;
      std::vector<std::uint8_t> marker(6);
      marker[0] = payload[0];
      if (!reply(pipe, 101, 0, marker)) break;
      if (marker_event) SetEvent(marker_event);
    }
  }

  if (genshin_compatibility_event) CloseHandle(genshin_compatibility_event);
  if (marker_event) CloseHandle(marker_event);
  if (hid_fallback_event) CloseHandle(hid_fallback_event);
  if (policy_once_event) CloseHandle(policy_once_event);
  if (recovery_wait_event) CloseHandle(recovery_wait_event);
  if (recovery_started_event) CloseHandle(recovery_started_event);
  if (recovered_event) CloseHandle(recovered_event);
  if (crash_always_event) CloseHandle(crash_always_event);
  if (crash_once_event) CloseHandle(crash_once_event);
  CloseHandle(continue_event);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  return 0;
}
