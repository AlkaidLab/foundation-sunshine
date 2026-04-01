/**
 * @file src/subprocess/ipc_pipe.cpp
 * @brief Platform-specific implementation of named pipe IPC.
 */
#include "ipc_pipe.h"

#include "subprocess_logging.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>

  #include <cerrno>
  #include <cstring>
#endif

namespace subprocess {
  namespace ipc {
    // Use chrono_literals only in the implementation
    using namespace std::chrono_literals;

    const char *
    result_to_string(result_e result) {
      switch (result) {
        case result_e::success:
          return "success";
        case result_e::error_create_pipe:
          return "error_create_pipe";
        case result_e::error_connect:
          return "error_connect";
        case result_e::error_timeout:
          return "error_timeout";
        case result_e::error_disconnected:
          return "error_disconnected";
        case result_e::error_invalid_message:
          return "error_invalid_message";
        case result_e::error_write:
          return "error_write";
        case result_e::error_read:
          return "error_read";
        default:
          return "unknown";
      }
    }

    // =====================================================
    // pipe_server_t implementation
    // =====================================================

    pipe_server_t::pipe_server_t() = default;

    pipe_server_t::~pipe_server_t() {
      stop_receive_loop();
      close();
    }

    result_e
    pipe_server_t::create(uint32_t session_id) {
      pipe_name_ = get_pipe_name(session_id);

#ifdef _WIN32
      // Create named pipe with overlapped I/O support
      HANDLE handle = CreateNamedPipeA(
        pipe_name_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,       // Max instances
        65536,   // Output buffer size
        65536,   // Input buffer size
        0,       // Default timeout
        nullptr  // Default security
      );

      if (handle == INVALID_HANDLE_VALUE) {
        SUBPROCESS_LOG(error) << "Failed to create named pipe: " << GetLastError();
        return result_e::error_create_pipe;
      }

      pipe_handle_ = handle;
      SUBPROCESS_LOG(debug) << "Created named pipe: " << pipe_name_;
      return result_e::success;
#else
      // On Unix, we use FIFO (named pipes)
      // Remove existing pipe if any
      unlink(pipe_name_.c_str());

      if (mkfifo(pipe_name_.c_str(), 0600) != 0) {
        SUBPROCESS_LOG(error) << "Failed to create FIFO: " << strerror(errno);
        return result_e::error_create_pipe;
      }

      SUBPROCESS_LOG(debug) << "Created FIFO: " << pipe_name_;
      return result_e::success;
#endif
    }

    result_e
    pipe_server_t::wait_for_connection(int timeout_ms) {
#ifdef _WIN32
      if (!pipe_handle_) {
        return result_e::error_create_pipe;
      }

      HANDLE handle = static_cast<HANDLE>(pipe_handle_);

      // Create event for overlapped operation
      OVERLAPPED overlapped = {};
      overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
      if (!overlapped.hEvent) {
        SUBPROCESS_LOG(error) << "Failed to create event: " << GetLastError();
        return result_e::error_create_pipe;
      }

      // Start async connect
      BOOL connected = ConnectNamedPipe(handle, &overlapped);
      DWORD error = GetLastError();

      if (!connected) {
        if (error == ERROR_IO_PENDING) {
          // Wait for connection with timeout
          DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
          CloseHandle(overlapped.hEvent);

          if (wait_result == WAIT_TIMEOUT) {
            CancelIo(handle);
            SUBPROCESS_LOG(debug) << "Connection timeout";
            return result_e::error_timeout;
          }
          else if (wait_result != WAIT_OBJECT_0) {
            SUBPROCESS_LOG(error) << "Wait failed: " << GetLastError();
            return result_e::error_connect;
          }

          // Check if connection succeeded
          DWORD bytes;
          if (!GetOverlappedResult(handle, &overlapped, &bytes, FALSE)) {
            error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED) {
              SUBPROCESS_LOG(error) << "GetOverlappedResult failed: " << error;
              return result_e::error_connect;
            }
          }
        }
        else if (error != ERROR_PIPE_CONNECTED) {
          CloseHandle(overlapped.hEvent);
          SUBPROCESS_LOG(error) << "ConnectNamedPipe failed: " << error;
          return result_e::error_connect;
        }
      }
      else {
        CloseHandle(overlapped.hEvent);
      }

      connected_ = true;
      SUBPROCESS_LOG(info) << "Client connected to IPC pipe";
      return result_e::success;
#else
      // On Unix, open the FIFO for read+write (non-blocking initially)
      int fd = open(pipe_name_.c_str(), O_RDWR | O_NONBLOCK);
      if (fd < 0) {
        SUBPROCESS_LOG(error) << "Failed to open FIFO: " << strerror(errno);
        return result_e::error_create_pipe;
      }

      pipe_handle_ = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
      connected_ = true;
      SUBPROCESS_LOG(info) << "IPC pipe ready";
      return result_e::success;
#endif
    }

    result_e
    pipe_server_t::send_message(message_type_e type, const void *payload, size_t payload_length) {
      if (!connected_) {
        return result_e::error_disconnected;
      }

      std::lock_guard<std::mutex> lock(write_mutex_);

      message_header_t header = make_header(type, static_cast<uint32_t>(payload_length), sequence_number_++);

#ifdef _WIN32
      HANDLE handle = static_cast<HANDLE>(pipe_handle_);

      // Write header
      DWORD written;
      if (!WriteFile(handle, &header, sizeof(header), &written, nullptr) || written != sizeof(header)) {
        SUBPROCESS_LOG(error) << "Failed to write header: " << GetLastError();
        return result_e::error_write;
      }

      // Write payload if any
      if (payload && payload_length > 0) {
        if (!WriteFile(handle, payload, static_cast<DWORD>(payload_length), &written, nullptr) ||
            written != payload_length) {
          SUBPROCESS_LOG(error) << "Failed to write payload: " << GetLastError();
          return result_e::error_write;
        }
      }

      return result_e::success;
#else
      int fd = static_cast<int>(reinterpret_cast<intptr_t>(pipe_handle_));

      // Write header
      ssize_t written = write(fd, &header, sizeof(header));
      if (written != sizeof(header)) {
        SUBPROCESS_LOG(error) << "Failed to write header: " << strerror(errno);
        return result_e::error_write;
      }

      // Write payload if any
      if (payload && payload_length > 0) {
        written = write(fd, payload, payload_length);
        if (written != static_cast<ssize_t>(payload_length)) {
          SUBPROCESS_LOG(error) << "Failed to write payload: " << strerror(errno);
          return result_e::error_write;
        }
      }

      return result_e::success;
#endif
    }

    result_e
    pipe_server_t::receive_message(message_header_t &header, std::vector<uint8_t> &payload, int timeout_ms) {
      if (!connected_) {
        return result_e::error_disconnected;
      }

#ifdef _WIN32
      HANDLE handle = static_cast<HANDLE>(pipe_handle_);

      // Create event for overlapped read
      OVERLAPPED overlapped = {};
      overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
      if (!overlapped.hEvent) {
        return result_e::error_read;
      }

      auto cleanup = [&]() {
        CloseHandle(overlapped.hEvent);
      };

      // Read header
      DWORD read_bytes = 0;
      BOOL success = ReadFile(handle, &header, sizeof(header), &read_bytes, &overlapped);

      if (!success) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
          DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
          if (wait_result == WAIT_TIMEOUT) {
            CancelIo(handle);
            cleanup();
            return result_e::error_timeout;
          }
          else if (wait_result != WAIT_OBJECT_0) {
            cleanup();
            return result_e::error_read;
          }

          if (!GetOverlappedResult(handle, &overlapped, &read_bytes, FALSE)) {
            cleanup();
            connected_ = false;
            return result_e::error_disconnected;
          }
        }
        else {
          cleanup();
          connected_ = false;
          return result_e::error_disconnected;
        }
      }

      if (read_bytes != sizeof(header)) {
        cleanup();
        return result_e::error_read;
      }

      // Validate header
      if (!validate_header(header)) {
        cleanup();
        return result_e::error_invalid_message;
      }

      // Read payload if any
      if (header.payload_length > 0) {
        payload.resize(header.payload_length);
        ResetEvent(overlapped.hEvent);

        success = ReadFile(handle, payload.data(), header.payload_length, &read_bytes, &overlapped);
        if (!success) {
          DWORD error = GetLastError();
          if (error == ERROR_IO_PENDING) {
            DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
            if (wait_result == WAIT_TIMEOUT) {
              CancelIo(handle);
              cleanup();
              return result_e::error_timeout;
            }
            else if (wait_result != WAIT_OBJECT_0) {
              cleanup();
              return result_e::error_read;
            }

            if (!GetOverlappedResult(handle, &overlapped, &read_bytes, FALSE)) {
              cleanup();
              connected_ = false;
              return result_e::error_disconnected;
            }
          }
          else {
            cleanup();
            connected_ = false;
            return result_e::error_disconnected;
          }
        }

        if (read_bytes != header.payload_length) {
          cleanup();
          return result_e::error_read;
        }
      }
      else {
        payload.clear();
      }

      cleanup();
      return result_e::success;
#else
      int fd = static_cast<int>(reinterpret_cast<intptr_t>(pipe_handle_));

      // Poll for data with timeout
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLIN;

      int ret = poll(&pfd, 1, timeout_ms);
      if (ret == 0) {
        return result_e::error_timeout;
      }
      else if (ret < 0) {
        SUBPROCESS_LOG(error) << "Poll failed: " << strerror(errno);
        return result_e::error_read;
      }

      // Read header
      ssize_t read_bytes = read(fd, &header, sizeof(header));
      if (read_bytes != sizeof(header)) {
        if (read_bytes == 0) {
          connected_ = false;
          return result_e::error_disconnected;
        }
        return result_e::error_read;
      }

      // Validate header
      if (!validate_header(header)) {
        return result_e::error_invalid_message;
      }

      // Read payload if any
      if (header.payload_length > 0) {
        payload.resize(header.payload_length);
        read_bytes = read(fd, payload.data(), header.payload_length);
        if (read_bytes != static_cast<ssize_t>(header.payload_length)) {
          return result_e::error_read;
        }
      }
      else {
        payload.clear();
      }

      return result_e::success;
#endif
    }

    void
    pipe_server_t::start_receive_loop(message_callback_t callback) {
      if (running_) {
        return;
      }

      running_ = true;
      receive_thread_ = std::thread([this, callback]() {
        while (running_ && connected_) {
          message_header_t header;
          std::vector<uint8_t> payload;

          auto result = receive_message(header, payload, 1000);
          if (result == result_e::error_timeout) {
            continue;
          }
          else if (result != result_e::success) {
            SUBPROCESS_LOG(warning) << "IPC receive error: " << result_to_string(result);
            break;
          }

          if (!callback(header, payload)) {
            break;
          }
        }
      });
    }

    void
    pipe_server_t::stop_receive_loop() {
      running_ = false;
      if (receive_thread_.joinable()) {
        receive_thread_.join();
      }
    }

    bool
    pipe_server_t::is_connected() const {
      return connected_;
    }

    void
    pipe_server_t::close() {
#ifdef _WIN32
      if (pipe_handle_) {
        DisconnectNamedPipe(static_cast<HANDLE>(pipe_handle_));
        CloseHandle(static_cast<HANDLE>(pipe_handle_));
        pipe_handle_ = nullptr;
      }
#else
      if (pipe_handle_) {
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(pipe_handle_));
        ::close(fd);
        pipe_handle_ = nullptr;
      }
      if (!pipe_name_.empty()) {
        unlink(pipe_name_.c_str());
      }
#endif
      connected_ = false;
    }

    // =====================================================
    // pipe_client_t implementation
    // =====================================================

    pipe_client_t::pipe_client_t() = default;

    pipe_client_t::~pipe_client_t() {
      stop_receive_loop();
      disconnect();
    }

    result_e
    pipe_client_t::connect(uint32_t session_id, int timeout_ms) {
      pipe_name_ = get_pipe_name(session_id);

#ifdef _WIN32
      auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
        HANDLE handle = CreateFileA(
          pipe_name_.c_str(),
          GENERIC_READ | GENERIC_WRITE,
          0,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OVERLAPPED,
          nullptr);

        if (handle != INVALID_HANDLE_VALUE) {
          // Set pipe to message mode
          DWORD mode = PIPE_READMODE_MESSAGE;
          if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
            CloseHandle(handle);
            SUBPROCESS_LOG(error) << "Failed to set pipe mode: " << GetLastError();
            return result_e::error_connect;
          }

          pipe_handle_ = handle;
          connected_ = true;
          SUBPROCESS_LOG(info) << "Connected to IPC pipe: " << pipe_name_;
          return result_e::success;
        }

        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
          // Pipe not yet created, wait and retry
          std::this_thread::sleep_for(100ms);
          continue;
        }
        else if (error == ERROR_PIPE_BUSY) {
          // Wait for pipe to become available
          if (!WaitNamedPipeA(pipe_name_.c_str(), timeout_ms)) {
            break;
          }
          continue;
        }
        else {
          SUBPROCESS_LOG(error) << "Failed to connect to pipe: " << error;
          return result_e::error_connect;
        }
      }

      return result_e::error_timeout;
#else
      auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
        int fd = open(pipe_name_.c_str(), O_RDWR | O_NONBLOCK);
        if (fd >= 0) {
          pipe_handle_ = reinterpret_cast<void *>(static_cast<intptr_t>(fd));
          connected_ = true;
          SUBPROCESS_LOG(info) << "Connected to IPC pipe: " << pipe_name_;
          return result_e::success;
        }

        if (errno == ENOENT) {
          // FIFO not yet created, wait and retry
          std::this_thread::sleep_for(100ms);
          continue;
        }

        SUBPROCESS_LOG(error) << "Failed to open FIFO: " << strerror(errno);
        return result_e::error_connect;
      }

      return result_e::error_timeout;
#endif
    }

    result_e
    pipe_client_t::send_message(message_type_e type, const void *payload, size_t payload_length) {
      if (!connected_) {
        return result_e::error_disconnected;
      }

      std::lock_guard<std::mutex> lock(write_mutex_);

      message_header_t header = make_header(type, static_cast<uint32_t>(payload_length), sequence_number_++);

#ifdef _WIN32
      HANDLE handle = static_cast<HANDLE>(pipe_handle_);

      // Write header
      DWORD written;
      if (!WriteFile(handle, &header, sizeof(header), &written, nullptr) || written != sizeof(header)) {
        SUBPROCESS_LOG(error) << "Failed to write header: " << GetLastError();
        return result_e::error_write;
      }

      // Write payload if any
      if (payload && payload_length > 0) {
        if (!WriteFile(handle, payload, static_cast<DWORD>(payload_length), &written, nullptr) ||
            written != payload_length) {
          SUBPROCESS_LOG(error) << "Failed to write payload: " << GetLastError();
          return result_e::error_write;
        }
      }

      return result_e::success;
#else
      int fd = static_cast<int>(reinterpret_cast<intptr_t>(pipe_handle_));

      // Write header
      ssize_t written = write(fd, &header, sizeof(header));
      if (written != sizeof(header)) {
        SUBPROCESS_LOG(error) << "Failed to write header: " << strerror(errno);
        return result_e::error_write;
      }

      // Write payload if any
      if (payload && payload_length > 0) {
        written = write(fd, payload, payload_length);
        if (written != static_cast<ssize_t>(payload_length)) {
          SUBPROCESS_LOG(error) << "Failed to write payload: " << strerror(errno);
          return result_e::error_write;
        }
      }

      return result_e::success;
#endif
    }

    result_e
    pipe_client_t::receive_message(message_header_t &header, std::vector<uint8_t> &payload, int timeout_ms) {
      if (!connected_) {
        return result_e::error_disconnected;
      }

#ifdef _WIN32
      HANDLE handle = static_cast<HANDLE>(pipe_handle_);

      // Create event for overlapped read
      OVERLAPPED overlapped = {};
      overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
      if (!overlapped.hEvent) {
        return result_e::error_read;
      }

      auto cleanup = [&]() {
        CloseHandle(overlapped.hEvent);
      };

      // Read header
      DWORD read_bytes = 0;
      BOOL success = ReadFile(handle, &header, sizeof(header), &read_bytes, &overlapped);

      if (!success) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
          DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
          if (wait_result == WAIT_TIMEOUT) {
            CancelIo(handle);
            cleanup();
            return result_e::error_timeout;
          }
          else if (wait_result != WAIT_OBJECT_0) {
            cleanup();
            return result_e::error_read;
          }

          if (!GetOverlappedResult(handle, &overlapped, &read_bytes, FALSE)) {
            cleanup();
            connected_ = false;
            return result_e::error_disconnected;
          }
        }
        else {
          cleanup();
          connected_ = false;
          return result_e::error_disconnected;
        }
      }

      if (read_bytes != sizeof(header)) {
        cleanup();
        return result_e::error_read;
      }

      // Validate header
      if (!validate_header(header)) {
        cleanup();
        return result_e::error_invalid_message;
      }

      // Read payload if any
      if (header.payload_length > 0) {
        payload.resize(header.payload_length);
        ResetEvent(overlapped.hEvent);

        success = ReadFile(handle, payload.data(), header.payload_length, &read_bytes, &overlapped);
        if (!success) {
          DWORD error = GetLastError();
          if (error == ERROR_IO_PENDING) {
            DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
            if (wait_result == WAIT_TIMEOUT) {
              CancelIo(handle);
              cleanup();
              return result_e::error_timeout;
            }
            else if (wait_result != WAIT_OBJECT_0) {
              cleanup();
              return result_e::error_read;
            }

            if (!GetOverlappedResult(handle, &overlapped, &read_bytes, FALSE)) {
              cleanup();
              connected_ = false;
              return result_e::error_disconnected;
            }
          }
          else {
            cleanup();
            connected_ = false;
            return result_e::error_disconnected;
          }
        }

        if (read_bytes != header.payload_length) {
          cleanup();
          return result_e::error_read;
        }
      }
      else {
        payload.clear();
      }

      cleanup();
      return result_e::success;
#else
      int fd = static_cast<int>(reinterpret_cast<intptr_t>(pipe_handle_));

      // Poll for data with timeout
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLIN;

      int ret = poll(&pfd, 1, timeout_ms);
      if (ret == 0) {
        return result_e::error_timeout;
      }
      else if (ret < 0) {
        SUBPROCESS_LOG(error) << "Poll failed: " << strerror(errno);
        return result_e::error_read;
      }

      // Read header
      ssize_t read_bytes = read(fd, &header, sizeof(header));
      if (read_bytes != sizeof(header)) {
        if (read_bytes == 0) {
          connected_ = false;
          return result_e::error_disconnected;
        }
        return result_e::error_read;
      }

      // Validate header
      if (!validate_header(header)) {
        return result_e::error_invalid_message;
      }

      // Read payload if any
      if (header.payload_length > 0) {
        payload.resize(header.payload_length);
        read_bytes = read(fd, payload.data(), header.payload_length);
        if (read_bytes != static_cast<ssize_t>(header.payload_length)) {
          return result_e::error_read;
        }
      }
      else {
        payload.clear();
      }

      return result_e::success;
#endif
    }

    void
    pipe_client_t::start_receive_loop(message_callback_t callback) {
      if (running_) {
        return;
      }

      running_ = true;
      receive_thread_ = std::thread([this, callback]() {
        while (running_ && connected_) {
          message_header_t header;
          std::vector<uint8_t> payload;

          auto result = receive_message(header, payload, 1000);
          if (result == result_e::error_timeout) {
            continue;
          }
          else if (result != result_e::success) {
            SUBPROCESS_LOG(warning) << "IPC receive error: " << result_to_string(result);
            break;
          }

          if (!callback(header, payload)) {
            break;
          }
        }
      });
    }

    void
    pipe_client_t::stop_receive_loop() {
      running_ = false;
      if (receive_thread_.joinable()) {
        receive_thread_.join();
      }
    }

    bool
    pipe_client_t::is_connected() const {
      return connected_;
    }

    void
    pipe_client_t::disconnect() {
#ifdef _WIN32
      if (pipe_handle_) {
        CloseHandle(static_cast<HANDLE>(pipe_handle_));
        pipe_handle_ = nullptr;
      }
#else
      if (pipe_handle_) {
        int fd = static_cast<int>(reinterpret_cast<intptr_t>(pipe_handle_));
        ::close(fd);
        pipe_handle_ = nullptr;
      }
#endif
      connected_ = false;
    }

  }  // namespace ipc
}  // namespace subprocess
