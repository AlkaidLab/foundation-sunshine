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
  const auto continue_name = L"Local\\sunshine-ds5-test-continue-" + parent_pid;
  const auto continue_event = OpenEventW(SYNCHRONIZE, FALSE, continue_name.c_str());
  if (!continue_event) return 2;
  const auto crash_once_name = L"Local\\sunshine-ds5-test-crash-once-" + parent_pid;
  const auto crash_once_event = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                           crash_once_name.c_str());
  const auto recovered_name = L"Local\\sunshine-ds5-test-recovered-" + parent_pid;
  const auto recovered_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, recovered_name.c_str());
  const auto marker_name = L"Local\\sunshine-ds5-test-marker-" + parent_pid;
  const auto marker_event = OpenEventW(EVENT_MODIFY_STATE, FALSE, marker_name.c_str());
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
      if (!reply(pipe, 2, request_id, std::vector<std::uint8_t>(4))) break;
    } else if (type == 3 && payload.size() == 4) {
      std::vector<std::uint8_t> response(8);
      response[0] = payload[0];
      if (!reply(pipe, 4, request_id, response)) break;
      if (crash_once_event && WaitForSingleObject(crash_once_event, 0) == WAIT_TIMEOUT) {
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

  if (marker_event) CloseHandle(marker_event);
  if (recovered_event) CloseHandle(recovered_event);
  if (crash_once_event) CloseHandle(crash_once_event);
  CloseHandle(continue_event);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  return 0;
}
