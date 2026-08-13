/**
 * @file src/platform/windows/ds5/ds5_sidecar_client.cpp
 * @brief Lifecycle-owned client for Sunshine.Ds5Sidecar protocol v1.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <random>
#include <span>
#include <thread>
#include <vector>

#include "ds5_sidecar_client.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"

namespace platf::ds5 {
  using namespace std::chrono_literals;
  using namespace std::literals;

  namespace {
    constexpr std::uint32_t MAGIC = 0x35534453;
    constexpr std::uint16_t VERSION = 1;
    constexpr std::size_t HEADER_SIZE = 16;
    constexpr std::uint32_t MAX_PAYLOAD = 1024 * 1024;

    enum class message_e: std::uint16_t {
      hello = 1,
      hello_reply = 2,
      attach = 3,
      attach_reply = 4,
      detach = 5,
      detach_reply = 6,
      input = 7,
      touch = 8,
      motion = 9,
      battery = 10,
      rumble = 101,
      adaptive_triggers = 102,
      led = 103,
      haptics_pcm = 104,
      error = 255,
    };

    struct message_t {
      message_e type;
      std::uint32_t request_id;
      std::vector<std::uint8_t> payload;
    };

    std::uint16_t read_u16(const std::uint8_t *p) {
      return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }

    std::uint32_t read_u32(const std::uint8_t *p) {
      return static_cast<std::uint32_t>(p[0]) |
             (static_cast<std::uint32_t>(p[1]) << 8) |
             (static_cast<std::uint32_t>(p[2]) << 16) |
             (static_cast<std::uint32_t>(p[3]) << 24);
    }

    std::uint64_t read_u64(const std::uint8_t *p) {
      return static_cast<std::uint64_t>(read_u32(p)) |
             (static_cast<std::uint64_t>(read_u32(p + 4)) << 32);
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

    bool read_exact(HANDLE pipe, std::span<std::uint8_t> destination) {
      std::size_t offset = 0;
      while (offset < destination.size()) {
        DWORD count = 0;
        if (!ReadFile(pipe, destination.data() + offset,
                      static_cast<DWORD>(destination.size() - offset), &count, nullptr) || count == 0) {
          return false;
        }
        offset += count;
      }
      return true;
    }

    bool write_exact(HANDLE pipe, std::span<const std::uint8_t> source) {
      std::size_t offset = 0;
      while (offset < source.size()) {
        DWORD count = 0;
        if (!WriteFile(pipe, source.data() + offset,
                       static_cast<DWORD>(source.size() - offset), &count, nullptr) || count == 0) {
          return false;
        }
        offset += count;
      }
      return true;
    }
  }  // namespace

  struct sidecar_client_t::impl_t {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    std::thread reader;
    std::mutex write_mutex;
    std::atomic_bool stopping { false };
    std::atomic_bool online { false };
    int global_index = -1;
    std::uint8_t client_index = 0;
    feedback_queue_t feedback_queue;
    std::uint32_t next_request_id = 1;

    ~impl_t() {
      close();
    }

    bool send(message_e type, std::uint32_t request_id, std::span<const std::uint8_t> payload) {
      std::vector<std::uint8_t> frame(HEADER_SIZE + payload.size());
      write_u32(frame.data(), MAGIC);
      write_u16(frame.data() + 4, VERSION);
      write_u16(frame.data() + 6, static_cast<std::uint16_t>(type));
      write_u32(frame.data() + 8, static_cast<std::uint32_t>(payload.size()));
      write_u32(frame.data() + 12, request_id);
      std::copy(payload.begin(), payload.end(), frame.begin() + HEADER_SIZE);
      std::lock_guard lock(write_mutex);
      return pipe != INVALID_HANDLE_VALUE && write_exact(pipe, frame);
    }

    bool receive(message_t &message) {
      std::array<std::uint8_t, HEADER_SIZE> header {};
      if (!read_exact(pipe, header) || read_u32(header.data()) != MAGIC ||
          read_u16(header.data() + 4) != VERSION) {
        return false;
      }
      const auto payload_size = read_u32(header.data() + 8);
      if (payload_size > MAX_PAYLOAD) {
        return false;
      }
      message.type = static_cast<message_e>(read_u16(header.data() + 6));
      message.request_id = read_u32(header.data() + 12);
      message.payload.resize(payload_size);
      return read_exact(pipe, message.payload);
    }

    bool transact(message_e request_type, std::span<const std::uint8_t> payload,
                  message_e reply_type, message_t &reply) {
      const auto request_id = next_request_id++;
      if (!send(request_type, request_id, payload) || !receive(reply)) {
        return false;
      }
      if (reply.type == message_e::error) {
        std::string reason;
        if (reply.payload.size() >= 8) {
          const auto length = std::min<std::size_t>(read_u32(reply.payload.data() + 4), reply.payload.size() - 8);
          reason.assign(reinterpret_cast<const char *>(reply.payload.data() + 8), length);
        }
        BOOST_LOG(error) << "DualSense sidecar rejected request: "sv << reason;
        return false;
      }
      return reply.type == reply_type && reply.request_id == request_id;
    }

    bool launch_and_connect() {
      const auto executable = std::filesystem::path(config::input.ds5_sidecar_path);
      if (executable.empty() || !std::filesystem::is_regular_file(executable)) {
        BOOST_LOG(error) << "DualSense sidecar is not installed or ds5_sidecar_path is invalid"sv;
        return false;
      }

      std::random_device random;
      const auto pipe_name = "sunshine-ds5-v1-"s + std::to_string(GetCurrentProcessId()) + "-" +
                             std::to_string(random()) + std::to_string(random());
      const auto pipe_path = platf::from_utf8(R"(\\.\pipe\)"s + pipe_name);
      auto executable_w = executable.wstring();
      auto command = L"\"" + executable_w + L"\" --pipe " + platf::from_utf8(pipe_name);
      std::vector<wchar_t> mutable_command(command.begin(), command.end());
      mutable_command.push_back(L'\0');

      STARTUPINFOW startup { sizeof(startup) };
      PROCESS_INFORMATION process_info {};

      job = CreateJobObjectW(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits {};
      job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                            &job_limits, sizeof(job_limits))) {
        BOOST_LOG(error) << "Failed to create the DualSense sidecar lifecycle job: "sv << GetLastError();
        if (job) {
          CloseHandle(job);
          job = nullptr;
        }
        return false;
      }

      if (!CreateProcessW(executable_w.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, executable.parent_path().c_str(),
                          &startup, &process_info)) {
        BOOST_LOG(error) << "Failed to launch DualSense sidecar: "sv << GetLastError();
        CloseHandle(job);
        job = nullptr;
        return false;
      }
      if (!AssignProcessToJobObject(job, process_info.hProcess)) {
        BOOST_LOG(error) << "Failed to assign the DualSense sidecar to its lifecycle job: "sv << GetLastError();
        TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        CloseHandle(job);
        job = nullptr;
        return false;
      }
      ResumeThread(process_info.hThread);
      CloseHandle(process_info.hThread);
      process = process_info.hProcess;

      const auto deadline = std::chrono::steady_clock::now() + 10s;
      do {
        pipe = CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
          return true;
        }
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
          BOOST_LOG(error) << "DualSense sidecar exited before opening its pipe"sv;
          return false;
        }
        WaitNamedPipeW(pipe_path.c_str(), 100);
      } while (std::chrono::steady_clock::now() < deadline);

      BOOST_LOG(error) << "Timed out connecting to DualSense sidecar pipe"sv;
      return false;
    }

    bool attach(const gamepad_id_t &id, bool audio_haptics) {
      if (!launch_and_connect()) {
        return false;
      }

      message_t reply;
      std::array<std::uint8_t, 4> hello {};
      write_u32(hello.data(), 0);
      if (!transact(message_e::hello, hello, message_e::hello_reply, reply)) {
        return false;
      }

      std::array<std::uint8_t, 4> attach_payload {
        static_cast<std::uint8_t>(id.globalIndex),
        id.clientRelativeIndex,
        static_cast<std::uint8_t>(audio_haptics ? 1 : 0),
        0,
      };
      if (!transact(message_e::attach, attach_payload, message_e::attach_reply, reply) ||
          reply.payload.size() != 8 || reply.payload[0] != attach_payload[0]) {
        return false;
      }

      global_index = id.globalIndex;
      client_index = id.clientRelativeIndex;
      online = true;
      reader = std::thread([this] { read_loop(); });
      BOOST_LOG(info) << "DualSense sidecar attached controller "sv << id.globalIndex
                      << (reply.payload[1] ? " with native four-channel haptics" : " (HID only)");
      return true;
    }

    void read_loop() {
      message_t message;
      while (!stopping && receive(message)) {
        const auto &p = message.payload;
        switch (message.type) {
          case message_e::rumble:
            if (p.size() == 6 && p[0] == global_index) {
              feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(
                p[1], read_u16(p.data() + 2), read_u16(p.data() + 4)));
            }
            break;
          case message_e::adaptive_triggers:
            if (p.size() == 26 && p[0] == global_index) {
              std::array<std::uint8_t, 10> left, right;
              std::copy_n(p.data() + 6, 10, left.begin());
              std::copy_n(p.data() + 16, 10, right.begin());
              feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(
                p[1], p[2], p[3], p[4], left, right));
            }
            break;
          case message_e::led:
            if (p.size() == 5 && p[0] == global_index) {
              feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(p[1], p[2], p[3], p[4]));
            }
            break;
          case message_e::haptics_pcm:
            if (p.size() >= 24 && p[0] == global_index) {
              const auto frames = read_u16(p.data() + 4);
              const auto pcm_size = static_cast<std::size_t>(frames) * 4;
              if (p[3] == 2 && p[6] == 16 && read_u32(p.data() + 20) == 48000 &&
                  frames <= 240 && p.size() == 24 + pcm_size) {
                feedback_queue->raise(gamepad_feedback_msg_t::make_ds5_haptics_pcm(
                  p[1], p[2], frames, read_u32(p.data() + 8), read_u64(p.data() + 12),
                  p.data() + 24, pcm_size));
              }
            }
            break;
          case message_e::error:
            BOOST_LOG(warning) << "DualSense sidecar reported an asynchronous error"sv;
            break;
          default:
            break;
        }
      }
      online = false;
      if (!stopping) {
        BOOST_LOG(warning) << "DualSense sidecar disconnected unexpectedly"sv;
      }
    }

    void close() {
      if (stopping.exchange(true)) {
        return;
      }
      online = false;
      if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
      }
      if (reader.joinable()) {
        reader.join();
      }
      if (process) {
        if (WaitForSingleObject(process, 5000) == WAIT_TIMEOUT) {
          BOOST_LOG(warning) << "DualSense sidecar did not exit after owner disconnect; terminating owned child"sv;
          TerminateProcess(process, ERROR_PROCESS_ABORTED);
          WaitForSingleObject(process, 1000);
        }
        CloseHandle(process);
        process = nullptr;
      }
      if (job) {
        CloseHandle(job);
        job = nullptr;
      }
      global_index = -1;
    }
  };

  sidecar_client_t::sidecar_client_t():
      _impl(std::make_unique<impl_t>()) {}

  sidecar_client_t::~sidecar_client_t() = default;

  bool sidecar_client_t::configured() const {
    return config::input.ds5_enabled && !config::input.ds5_sidecar_path.empty();
  }

  bool sidecar_client_t::owns(int global_index) const {
    return _impl->global_index == global_index;
  }

  int sidecar_client_t::alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue, bool audio_haptics) {
    if (!configured() || _impl->global_index >= 0) {
      return -1;
    }
    _impl->feedback_queue = std::move(feedback_queue);
    if (_impl->attach(id, audio_haptics)) {
      return 0;
    }
    _impl = std::make_unique<impl_t>();
    return -1;
  }

  void sidecar_client_t::free(int global_index) {
    if (_impl->global_index == global_index) {
      _impl->close();
      _impl = std::make_unique<impl_t>();
    }
  }

  void sidecar_client_t::submit_input(int global_index, const gamepad_state_t &state) {
    if (!owns(global_index)) return;
    std::array<std::uint8_t, 20> payload {};
    payload[0] = static_cast<std::uint8_t>(global_index);
    write_u32(payload.data() + 4, state.buttonFlags);
    payload[8] = state.lt;
    payload[9] = state.rt;
    write_u16(payload.data() + 12, static_cast<std::uint16_t>(state.lsX));
    write_u16(payload.data() + 14, static_cast<std::uint16_t>(state.lsY));
    write_u16(payload.data() + 16, static_cast<std::uint16_t>(state.rsX));
    write_u16(payload.data() + 18, static_cast<std::uint16_t>(state.rsY));
    _impl->send(message_e::input, 0, payload);
  }

  void sidecar_client_t::submit_touch(const gamepad_touch_t &touch) {
    if (!owns(touch.id.globalIndex)) return;
    std::array<std::uint8_t, 20> payload {};
    payload[0] = static_cast<std::uint8_t>(touch.id.globalIndex);
    payload[1] = touch.eventType;
    write_u32(payload.data() + 4, touch.pointerId);
    write_u32(payload.data() + 8, std::bit_cast<std::uint32_t>(touch.x));
    write_u32(payload.data() + 12, std::bit_cast<std::uint32_t>(touch.y));
    write_u32(payload.data() + 16, std::bit_cast<std::uint32_t>(touch.pressure));
    _impl->send(message_e::touch, 0, payload);
  }

  void sidecar_client_t::submit_motion(const gamepad_motion_t &motion) {
    if (!owns(motion.id.globalIndex)) return;
    std::array<std::uint8_t, 16> payload {};
    payload[0] = static_cast<std::uint8_t>(motion.id.globalIndex);
    payload[1] = motion.motionType;
    write_u32(payload.data() + 4, std::bit_cast<std::uint32_t>(motion.x));
    write_u32(payload.data() + 8, std::bit_cast<std::uint32_t>(motion.y));
    write_u32(payload.data() + 12, std::bit_cast<std::uint32_t>(motion.z));
    _impl->send(message_e::motion, 0, payload);
  }

  void sidecar_client_t::submit_battery(const gamepad_battery_t &battery) {
    if (!owns(battery.id.globalIndex)) return;
    std::array<std::uint8_t, 4> payload {
      static_cast<std::uint8_t>(battery.id.globalIndex), battery.state, battery.percentage, 0
    };
    _impl->send(message_e::battery, 0, payload);
  }
}  // namespace platf::ds5
