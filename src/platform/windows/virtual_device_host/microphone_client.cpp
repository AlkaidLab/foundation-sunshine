/**
 * @file src/platform/windows/virtual_device_host/microphone_client.cpp
 * @brief Bounded, lifecycle-owned SDS5 virtual microphone transport.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <span>
#include <thread>
#include <vector>

#include "microphone_client.h"
#include "protocol.h"
#include "src/logging.h"
#include "src/platform/windows/ds5/ds5_sidecar_client.h"
#include "src/platform/windows/misc.h"

namespace platf::virtual_device_host {
  using namespace std::chrono_literals;
  using namespace std::literals;
  namespace wire = protocol;

  namespace {
    constexpr DWORD write_stall_timeout_ms = 5000;
    constexpr std::size_t maximum_queued_frames = 2880;  // 60 ms at 48 kHz.

    std::mutex published_status_mutex;
    microphone_status_t published_status;

    struct message_t {
      wire::message_e type;
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

    std::int32_t read_i32(const std::uint8_t *p) {
      return static_cast<std::int32_t>(read_u32(p));
    }

    void write_u16(std::uint8_t *p, std::uint16_t value) {
      wire::write_u16(p, value);
    }

    void write_u32(std::uint8_t *p, std::uint32_t value) {
      wire::write_u32(p, value);
    }

    void write_u64(std::uint8_t *p, std::uint64_t value) {
      write_u32(p, static_cast<std::uint32_t>(value));
      write_u32(p + 4, static_cast<std::uint32_t>(value >> 32));
    }

    bool transfer_exact(HANDLE pipe, HANDLE stop_event, void *buffer, std::size_t size, bool write,
                        DWORD timeout = INFINITE) {
      std::size_t offset = 0;
      while (offset < size) {
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) return false;
        OVERLAPPED operation {};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        DWORD count = 0;
        auto *bytes = static_cast<std::uint8_t *>(buffer) + offset;
        const auto remaining = static_cast<DWORD>(size - offset);
        const auto started = write ?
                               WriteFile(pipe, bytes, remaining, &count, &operation) :
                               ReadFile(pipe, bytes, remaining, &count, &operation);
        bool completed = started != FALSE;
        if (!completed && GetLastError() == ERROR_IO_PENDING) {
          const std::array waits { operation.hEvent, stop_event };
          const auto waited = WaitForMultipleObjects(
            static_cast<DWORD>(waits.size()), waits.data(), FALSE, timeout);
          if (waited == WAIT_OBJECT_0) {
            completed = GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE;
          }
          else {
            CancelIoEx(pipe, &operation);
            WaitForSingleObject(operation.hEvent, INFINITE);
            completed = false;
          }
        }
        CloseHandle(operation.hEvent);
        if (!completed || count == 0) return false;
        offset += count;
      }
      return true;
    }

    std::string state_name(std::uint8_t state) {
      static constexpr std::array names {
        "absent"sv, "creating"sv, "enumerating"sv, "idle"sv,
        "host_capturing"sv, "remote_active"sv, "destroying"sv, "device_faulted"sv,
      };
      return state < names.size() ? std::string(names[state]) : "unknown"s;
    }

    void publish(const microphone_status_t &status) {
      std::lock_guard lock(published_status_mutex);
      published_status = status;
    }
  }  // namespace

  struct microphone_client_t::impl_t {
    struct pcm_block_t {
      std::vector<std::int16_t> samples;
      std::uint32_t sequence = 0;
      std::uint64_t timestamp_us = 0;
      std::uint16_t flags = 0;
    };

    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<pcm_block_t> queue;
    std::size_t queued_frames = 0;
    std::atomic<bool> stop_requested { false };
    bool startup_finished = false;
    bool startup_success = false;
    bool flush_requested = false;
    std::uint64_t flush_ticket = 0;
    std::uint64_t flush_completed = 0;
    bool stream_started = false;
    bool discontinuity = false;
    std::uint32_t core_dropped_frames = 0;
    std::uint32_t sidecar_dropped_frames = 0;
    std::uint32_t sequence = 0;
    std::uint32_t generation = 0;
    std::uint32_t next_request_id = 1;
    microphone_status_t status;

    impl_t() {
      status.component_available = ds5::refresh_component_availability();
      publish(status);
    }

    ~impl_t() {
      stop();
      if (stop_event) CloseHandle(stop_event);
    }

    void set_error(std::string code, std::int32_t error = 0) {
      std::lock_guard lock(mutex);
      status.online = false;
      status.state = "device_faulted";
      status.error_code = std::move(code);
      status.last_error = error;
      publish(status);
    }

    bool send(wire::message_e type, std::uint32_t request_id,
              std::span<const std::uint8_t> payload) {
      std::vector<std::uint8_t> frame(wire::HEADER_SIZE + payload.size());
      const auto header = wire::encode_header(
        type, static_cast<std::uint32_t>(payload.size()), request_id);
      std::copy(header.begin(), header.end(), frame.begin());
      std::copy(payload.begin(), payload.end(), frame.begin() + wire::HEADER_SIZE);
      return pipe != INVALID_HANDLE_VALUE && transfer_exact(
        pipe, stop_event, frame.data(), frame.size(), true, write_stall_timeout_ms);
    }

    bool receive(message_t &message) {
      std::array<std::uint8_t, wire::HEADER_SIZE> header {};
      if (pipe == INVALID_HANDLE_VALUE ||
          !transfer_exact(pipe, stop_event, header.data(), header.size(), false) ||
          read_u32(header.data()) != wire::MAGIC ||
          read_u16(header.data() + 4) != wire::VERSION) {
        return false;
      }
      const auto payload_size = read_u32(header.data() + 8);
      if (payload_size > wire::MAX_PAYLOAD) return false;
      message.type = static_cast<wire::message_e>(read_u16(header.data() + 6));
      message.request_id = read_u32(header.data() + 12);
      message.payload.resize(payload_size);
      return payload_size == 0 || transfer_exact(
        pipe, stop_event, message.payload.data(), message.payload.size(), false,
        write_stall_timeout_ms);
    }

    void dispatch(const message_t &message) {
      if (message.type != wire::message_e::mic_status ||
          message.payload.size() != wire::MIC_STATUS_PAYLOAD_SIZE) return;
      std::lock_guard lock(mutex);
      status.generation = read_u32(message.payload.data());
      status.state = state_name(message.payload[4]);
      status.host_streaming = message.payload[5] != 0;
      status.buffered_bytes = read_u32(message.payload.data() + 8);
      status.underruns = read_u32(message.payload.data() + 12);
      sidecar_dropped_frames = read_u32(message.payload.data() + 16);
      status.dropped_frames = core_dropped_frames + sidecar_dropped_frames;
      status.submit_errors = read_u32(message.payload.data() + 20);
      status.last_error = read_i32(message.payload.data() + 24);
      status.device_created = status.state != "absent";
      publish(status);
    }

    bool transact(wire::message_e request_type, std::span<const std::uint8_t> payload,
                  wire::message_e reply_type, message_t &reply) {
      const auto request_id = next_request_id++;
      if (!send(request_type, request_id, payload)) return false;
      while (true) {
        message_t message;
        if (!receive(message)) return false;
        if (message.request_id == request_id && message.type == reply_type) {
          reply = std::move(message);
          return true;
        }
        if (message.type == wire::message_e::error && message.request_id == request_id) {
          return false;
        }
        dispatch(message);
      }
      return false;
    }

    void drain_available_status() {
      while (pipe != INVALID_HANDLE_VALUE) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) ||
            available < wire::HEADER_SIZE) return;
        message_t message;
        if (!receive(message)) {
          set_error("MIC_USBIP_TRANSPORT_LOST");
          return;
        }
        dispatch(message);
      }
    }

    bool launch_and_connect() {
      const auto executable = ds5::trusted_component_path();
      if (!stop_event || !executable) {
        set_error("MIC_USBIP_COMPONENT_UNAVAILABLE");
        return false;
      }
      const auto fail_launch = [&](std::string code, std::int32_t last_error) {
        BOOST_LOG(error) << "USB/IP virtual microphone host startup failed: "sv
                         << code << " ("sv << last_error << ')';
        close_transport();
        set_error(std::move(code), last_error);
        return false;
      };
      std::random_device random;
      const auto pipe_name = "sunshine-mic-v1-"s + std::to_string(GetCurrentProcessId()) + "-" +
                             std::to_string(random()) + std::to_string(random());
      const auto pipe_path = platf::from_utf8(R"(\\.\pipe\)"s + pipe_name);
      const auto executable_w = executable->wstring();
      auto command = L"\"" + executable_w + L"\" --pipe " + platf::from_utf8(pipe_name) +
                     L" --enable-composite-microphone-prototype";
      std::vector<wchar_t> mutable_command(command.begin(), command.end());
      mutable_command.push_back(L'\0');

      STARTUPINFOW startup { sizeof(startup) };
      PROCESS_INFORMATION process_info {};
      job = CreateJobObjectW(nullptr, nullptr);
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (!job || !SetInformationJobObject(
                    job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return fail_launch("MIC_USBIP_JOB_CREATE_FAILED", static_cast<std::int32_t>(GetLastError()));
      }
      if (!CreateProcessW(executable_w.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                          executable->parent_path().c_str(), &startup, &process_info)) {
        return fail_launch("MIC_USBIP_PROCESS_START_FAILED", static_cast<std::int32_t>(GetLastError()));
      }
      if (!AssignProcessToJobObject(job, process_info.hProcess)) {
        const auto error = static_cast<std::int32_t>(GetLastError());
        TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return fail_launch("MIC_USBIP_JOB_ASSIGN_FAILED", error);
      }
      ResumeThread(process_info.hThread);
      CloseHandle(process_info.hThread);
      process = process_info.hProcess;

      const auto deadline = std::chrono::steady_clock::now() + 10s;
      do {
        pipe = CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return true;
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
          DWORD exit_code = ERROR_PROCESS_ABORTED;
          GetExitCodeProcess(process, &exit_code);
          return fail_launch("MIC_USBIP_COMPONENT_EXITED", static_cast<std::int32_t>(exit_code));
        }
        WaitNamedPipeW(pipe_path.c_str(), 100);
      } while (!stop_requested.load() && std::chrono::steady_clock::now() < deadline);
      return fail_launch("MIC_USBIP_PIPE_TIMEOUT", static_cast<std::int32_t>(GetLastError()));
    }

    bool negotiate_and_create() {
      if (!launch_and_connect()) {
        return false;
      }
      message_t reply;
      std::array<std::uint8_t, 4> hello {};
      if (!transact(wire::message_e::hello, hello, wire::message_e::hello_reply, reply) ||
          reply.payload.size() != 4) {
        set_error("MIC_USBIP_NEGOTIATION_FAILED");
        return false;
      }
      constexpr auto required = wire::CAP_VIRTUAL_MICROPHONE |
                                wire::CAP_PERSISTENT_DEVICE_HOST |
                                wire::CAP_MICROPHONE_STATUS;
      if ((read_u32(reply.payload.data()) & required) != required) {
        set_error("MIC_USBIP_COMPONENT_UPDATE_REQUIRED");
        return false;
      }
      std::array<std::uint8_t, wire::MIC_CREATE_PAYLOAD_SIZE> create {};
      write_u32(create.data(), wire::MICROPHONE_SAMPLE_RATE_HZ);
      create[4] = wire::MICROPHONE_CHANNELS;
      create[5] = wire::MICROPHONE_BITS_PER_SAMPLE;
      if (!transact(wire::message_e::mic_create, create,
                    wire::message_e::mic_create_reply, reply) ||
          reply.payload.size() != wire::MIC_CREATE_REPLY_PAYLOAD_SIZE) {
        set_error("MIC_USBIP_CREATE_FAILED");
        return false;
      }
      const auto result = read_i32(reply.payload.data());
      if (result != static_cast<std::int32_t>(wire::mic_result_e::success)) {
        set_error("MIC_USBIP_CREATE_FAILED", result);
        return false;
      }
      {
        std::lock_guard lock(mutex);
        generation = read_u32(reply.payload.data() + 4);
        status.online = true;
        status.device_created = true;
        status.generation = generation;
        status.state = "idle";
        status.error_code.clear();
        publish(status);
      }
      return true;
    }

    std::vector<std::uint8_t> encode_pcm(pcm_block_t &&block) {
      const auto frames = static_cast<std::uint16_t>(block.samples.size());
      std::vector<std::uint8_t> payload(
        wire::MIC_PCM_HEADER_SIZE + block.samples.size() * sizeof(std::int16_t));
      write_u32(payload.data(), generation);
      write_u32(payload.data() + 4, block.sequence);
      write_u64(payload.data() + 8, block.timestamp_us);
      write_u16(payload.data() + 16, frames);
      write_u16(payload.data() + 18, block.flags);
      std::memcpy(payload.data() + wire::MIC_PCM_HEADER_SIZE,
                  block.samples.data(), block.samples.size() * sizeof(std::int16_t));
      return payload;
    }

    void complete_flush(std::uint64_t ticket, bool success) {
      std::lock_guard lock(mutex);
      if (!success) {
        status.error_code = "MIC_USBIP_FLUSH_FAILED";
        status.online = false;
        publish(status);
      }
      flush_completed = std::max(flush_completed, ticket);
      wake.notify_all();
    }

    void run() {
      const auto started = negotiate_and_create();
      {
        std::lock_guard lock(mutex);
        startup_success = started;
        startup_finished = true;
        wake.notify_all();
      }
      if (!started) {
        close_transport();
        return;
      }

      while (true) {
        pcm_block_t block;
        bool have_block = false;
        bool do_flush = false;
        std::uint64_t ticket = 0;
        {
          std::unique_lock lock(mutex);
          wake.wait_for(lock, 100ms, [&] {
            return stop_requested.load() || flush_requested || !queue.empty();
          });
          if (stop_requested.load()) break;
          if (flush_requested) {
            do_flush = true;
            flush_requested = false;
            ticket = flush_ticket;
            queue.clear();
            queued_frames = 0;
            sequence = 0;
            stream_started = false;
            discontinuity = false;
          }
          else if (!queue.empty()) {
            block = std::move(queue.front());
            queued_frames -= block.samples.size();
            queue.pop_front();
            block.sequence = sequence++;
            block.timestamp_us = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (!stream_started) {
              block.flags |= wire::mic_stream_start;
              stream_started = true;
            }
            if (discontinuity) {
              block.flags |= wire::mic_discontinuity;
              discontinuity = false;
            }
            have_block = true;
          }
        }

        if (do_flush) {
          message_t reply;
          const auto success = transact(
            wire::message_e::mic_flush, {}, wire::message_e::mic_flush_reply, reply) &&
            reply.payload.size() == wire::MIC_OPERATION_REPLY_PAYLOAD_SIZE &&
            read_i32(reply.payload.data()) ==
              static_cast<std::int32_t>(wire::mic_result_e::success);
          complete_flush(ticket, success);
          if (!success) break;
        }
        else if (have_block) {
          const auto payload = encode_pcm(std::move(block));
          if (!send(wire::message_e::mic_pcm, 0, payload)) {
            set_error("MIC_USBIP_TRANSPORT_LOST");
            break;
          }
          drain_available_status();
        }
        else {
          drain_available_status();
        }
      }

      if (generation != 0 && pipe != INVALID_HANDLE_VALUE) {
        message_t reply;
        transact(wire::message_e::mic_destroy, {}, wire::message_e::mic_destroy_reply, reply);
        send(wire::message_e::shutdown, 0, {});
      }
      close_transport();
      std::lock_guard lock(mutex);
      status.online = false;
      status.device_created = false;
      status.host_streaming = false;
      status.state = "absent";
      publish(status);
      flush_completed = flush_ticket;
      wake.notify_all();
    }

    bool start() {
      std::unique_lock lock(mutex);
      if (worker.joinable()) {
        if (status.online) return true;
        lock.unlock();
        worker.join();
        lock.lock();
      }
      ResetEvent(stop_event);
      stop_requested = false;
      startup_finished = false;
      startup_success = false;
      flush_requested = false;
      queue.clear();
      queued_frames = 0;
      sequence = 0;
      generation = 0;
      stream_started = false;
      discontinuity = false;
      core_dropped_frames = 0;
      sidecar_dropped_frames = 0;
      status.component_available = ds5::refresh_component_availability();
      status.online = false;
      status.device_created = false;
      status.host_streaming = false;
      status.state = "absent";
      status.error_code.clear();
      publish(status);
      worker = std::thread([this] { run(); });
      if (!wake.wait_for(lock, 15s, [&] { return startup_finished; })) {
        status.error_code = "MIC_USBIP_START_TIMEOUT";
        publish(status);
        lock.unlock();
        stop();
        return false;
      }
      return startup_success;
    }

    int write_pcm(const std::int16_t *samples, std::size_t frame_count) {
      if (!samples || frame_count == 0) return 0;
      std::lock_guard lock(mutex);
      if (!status.online || stop_requested.load()) return -2;
      std::size_t offset = 0;
      while (offset < frame_count) {
        const auto count = std::min<std::size_t>(
          wire::MAX_MIC_PCM_FRAMES, frame_count - offset);
        while (!queue.empty() && queued_frames + count > maximum_queued_frames) {
          queued_frames -= queue.front().samples.size();
          core_dropped_frames += static_cast<std::uint32_t>(queue.front().samples.size());
          status.dropped_frames = core_dropped_frames + sidecar_dropped_frames;
          queue.pop_front();
          discontinuity = true;
        }
        pcm_block_t block;
        block.samples.assign(samples + offset, samples + offset + count);
        queued_frames += count;
        queue.emplace_back(std::move(block));
        offset += count;
      }
      publish(status);
      wake.notify_one();
      return static_cast<int>(frame_count * sizeof(std::int16_t));
    }

    bool flush() {
      std::unique_lock lock(mutex);
      if (!status.online || stop_requested.load()) return false;
      const auto ticket = ++flush_ticket;
      flush_requested = true;
      wake.notify_one();
      return wake.wait_for(lock, 5s, [&] {
        return flush_completed >= ticket || !status.online;
      }) && flush_completed >= ticket;
    }

    bool online() const {
      std::lock_guard lock(mutex);
      return status.online;
    }

    void close_transport() {
      if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
      }
      if (process) {
        if (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT) {
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
    }

    void stop() {
      {
        std::lock_guard lock(mutex);
        stop_requested = true;
        wake.notify_all();
      }
      if (stop_event) SetEvent(stop_event);
      if (worker.joinable()) worker.join();
      close_transport();
    }
  };

  microphone_status_t microphone_status() {
    std::lock_guard lock(published_status_mutex);
    auto result = published_status;
    result.component_available = ds5::component_available();
    return result;
  }

  microphone_client_t::microphone_client_t():
      _impl(std::make_unique<impl_t>()) {}

  microphone_client_t::~microphone_client_t() = default;

  bool microphone_client_t::start() {
    return _impl->start();
  }

  int microphone_client_t::write_pcm(const std::int16_t *samples, std::size_t frame_count) {
    return _impl->write_pcm(samples, frame_count);
  }

  bool microphone_client_t::flush() {
    return _impl->flush();
  }

  bool microphone_client_t::online() const {
    return _impl->online();
  }

  microphone_client_t &persistent_microphone_client() {
    static microphone_client_t client;
    return client;
  }
}  // namespace platf::virtual_device_host
