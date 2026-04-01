/**
 * @file src/subprocess/subprocess_manager.cpp
 * @brief Implementation of subprocess lifecycle management.
 */
#include "subprocess_manager.h"

#include <chrono>
#include <filesystem>

#include "src/logging.h"
#include "src/platform/common.h"

#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
#else
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace subprocess {

  const char *
  state_to_string(state_e state) {
    switch (state) {
      case state_e::stopped:
        return "stopped";
      case state_e::starting:
        return "starting";
      case state_e::ready:
        return "ready";
      case state_e::streaming:
        return "streaming";
      case state_e::stopping:
        return "stopping";
      case state_e::error:
        return "error";
      default:
        return "unknown";
    }
  }

  // =====================================================
  // subprocess_worker_t implementation
  // =====================================================

  subprocess_worker_t::subprocess_worker_t() = default;

  subprocess_worker_t::~subprocess_worker_t() {
    stop(5000);
  }

  bool
  subprocess_worker_t::start(const session_config_t &config, status_callback_t status_callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_running()) {
      BOOST_LOG(warning) << "Subprocess worker already running";
      return false;
    }

    config_ = config;
    status_callback_ = std::move(status_callback);
    state_ = state_e::starting;

    BOOST_LOG(info) << "Starting subprocess worker for session " << config.session_id;

    // Create IPC pipe server
    ipc_server_ = std::make_unique<ipc::pipe_server_t>();
    auto result = ipc_server_->create(config.session_id);
    if (result != ipc::result_e::success) {
      BOOST_LOG(error) << "Failed to create IPC pipe: " << ipc::result_to_string(result);
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, -1, "Failed to create IPC pipe");
      }
      return false;
    }

    // Launch subprocess
    if (!launch_process()) {
      BOOST_LOG(error) << "Failed to launch subprocess";
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, -2, "Failed to launch subprocess");
      }
      return false;
    }

    // Wait for subprocess to connect
    auto &sub_config = get_config();
    result = ipc_server_->wait_for_connection(sub_config.init_timeout_ms);
    if (result != ipc::result_e::success) {
      BOOST_LOG(error) << "Subprocess failed to connect: " << ipc::result_to_string(result);
      terminate_process();
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, -3, "Subprocess connection timeout");
      }
      return false;
    }

    // Send session configuration
    std::vector<uint8_t> payload;
    {
      ipc::init_session_payload_t init_payload {};
      init_payload.width = config.width;
      init_payload.height = config.height;
      init_payload.framerate = config.framerate;
      init_payload.bitrate = config.bitrate_kbps;
      init_payload.slices_per_frame = config.slices_per_frame;
      init_payload.num_ref_frames = config.num_ref_frames;
      init_payload.encoder_csc_mode = config.encoder_csc_mode;
      init_payload.video_format = config.video_format;
      init_payload.dynamic_range = config.dynamic_range;
      init_payload.chroma_sampling = config.chroma_sampling;
      init_payload.enable_intra_refresh = config.enable_intra_refresh;

      init_payload.audio_channels = config.audio_channels;
      init_payload.audio_mask = config.audio_mask;
      init_payload.audio_packet_duration = config.audio_packet_duration;
      init_payload.audio_high_quality = config.audio_high_quality ? 1 : 0;
      init_payload.audio_host_audio = config.audio_host_audio ? 1 : 0;

      init_payload.packet_size = config.packet_size;
      init_payload.min_fec_packets = config.min_fec_packets;
      init_payload.fec_percentage = config.fec_percentage;

      init_payload.encryption_flags = config.encryption_flags;
      std::copy(config.gcm_key.begin(), config.gcm_key.end(), init_payload.gcm_key);
      std::copy(config.iv.begin(), config.iv.end(), init_payload.iv);

      init_payload.display_name_length = static_cast<uint16_t>(config.display_name.size());

      // Serialize payload
      payload.resize(sizeof(init_payload) + config.display_name.size());
      std::memcpy(payload.data(), &init_payload, sizeof(init_payload));
      if (!config.display_name.empty()) {
        std::memcpy(payload.data() + sizeof(init_payload),
                    config.display_name.data(), config.display_name.size());
      }
    }

    result = ipc_server_->send_message(ipc::message_type_e::INIT_SESSION, payload.data(), payload.size());
    if (result != ipc::result_e::success) {
      BOOST_LOG(error) << "Failed to send init message: " << ipc::result_to_string(result);
      terminate_process();
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, -4, "Failed to send configuration");
      }
      return false;
    }

    // Wait for ready status
    ipc::message_header_t header;
    std::vector<uint8_t> response;
    result = ipc_server_->receive_message(header, response, sub_config.init_timeout_ms);
    if (result != ipc::result_e::success) {
      BOOST_LOG(error) << "Failed to receive ready status: " << ipc::result_to_string(result);
      terminate_process();
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, -5, "Subprocess initialization failed");
      }
      return false;
    }

    auto msg_type = static_cast<ipc::message_type_e>(header.type);
    if (msg_type == ipc::message_type_e::STATUS_ERROR) {
      int error_code = 0;
      std::string error_msg = "Unknown error";
      if (response.size() >= sizeof(ipc::status_error_payload_t)) {
        auto *err_payload = reinterpret_cast<ipc::status_error_payload_t *>(response.data());
        error_code = err_payload->error_code;
        if (err_payload->message_length > 0 &&
            response.size() >= sizeof(ipc::status_error_payload_t) + err_payload->message_length) {
          error_msg = std::string(reinterpret_cast<char *>(response.data() + sizeof(ipc::status_error_payload_t)),
                                  err_payload->message_length);
        }
      }
      BOOST_LOG(error) << "Subprocess reported error: " << error_msg << " (code: " << error_code << ")";
      terminate_process();
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, error_code, error_msg);
      }
      return false;
    }

    if (msg_type != ipc::message_type_e::STATUS_READY) {
      BOOST_LOG(error) << "Unexpected message type: " << static_cast<int>(header.type);
      terminate_process();
      state_ = state_e::error;
      if (status_callback_) {
        status_callback_(state_e::error, -6, "Unexpected response from subprocess");
      }
      return false;
    }

    state_ = state_e::ready;
    BOOST_LOG(info) << "Subprocess worker ready for session " << config.session_id;

    // Start message receive loop
    ipc_server_->start_receive_loop([this](const ipc::message_header_t &h, const std::vector<uint8_t> &p) {
      return handle_message(h, p);
    });

    // Start heartbeat monitoring
    heartbeat_running_ = true;
    last_heartbeat_ = std::chrono::steady_clock::now();
    heartbeat_thread_ = std::thread(&subprocess_worker_t::heartbeat_thread, this);

    if (status_callback_) {
      status_callback_(state_e::ready, 0, "");
    }

    return true;
  }

  void
  subprocess_worker_t::stop(int wait_timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (state_ == state_e::stopped) {
      return;
    }

    BOOST_LOG(info) << "Stopping subprocess worker";
    state_ = state_e::stopping;

    // Stop heartbeat
    heartbeat_running_ = false;
    if (heartbeat_thread_.joinable()) {
      heartbeat_thread_.join();
    }

    // Send shutdown message
    if (ipc_server_ && ipc_server_->is_connected()) {
      ipc_server_->send_message(ipc::message_type_e::SHUTDOWN);
      ipc_server_->stop_receive_loop();
    }

    // Wait for process to exit gracefully
    terminate_process();

    ipc_server_.reset();
    state_ = state_e::stopped;

    BOOST_LOG(info) << "Subprocess worker stopped";
  }

  void
  subprocess_worker_t::request_idr_frame() {
    if (!is_running() || !ipc_server_ || !ipc_server_->is_connected()) {
      return;
    }

    auto result = ipc_server_->send_message(ipc::message_type_e::REQUEST_IDR);
    if (result != ipc::result_e::success) {
      BOOST_LOG(warning) << "Failed to send IDR request: " << ipc::result_to_string(result);
    }
  }

  void
  subprocess_worker_t::change_bitrate(int new_bitrate_kbps) {
    if (!is_running() || !ipc_server_ || !ipc_server_->is_connected()) {
      return;
    }

    ipc::change_bitrate_payload_t payload;
    payload.new_bitrate_kbps = new_bitrate_kbps;

    auto result = ipc_server_->send_message(ipc::message_type_e::CHANGE_BITRATE, &payload, sizeof(payload));
    if (result != ipc::result_e::success) {
      BOOST_LOG(warning) << "Failed to send bitrate change: " << ipc::result_to_string(result);
    }
  }

  void
  subprocess_worker_t::invalidate_ref_frames(int64_t first_frame, int64_t last_frame) {
    if (!is_running() || !ipc_server_ || !ipc_server_->is_connected()) {
      return;
    }

    ipc::invalidate_refs_payload_t payload;
    payload.first_frame = first_frame;
    payload.last_frame = last_frame;

    auto result = ipc_server_->send_message(ipc::message_type_e::INVALIDATE_REFS, &payload, sizeof(payload));
    if (result != ipc::result_e::success) {
      BOOST_LOG(warning) << "Failed to send ref frame invalidation: " << ipc::result_to_string(result);
    }
  }

#ifdef _WIN32
  bool
  subprocess_worker_t::transfer_socket(uint8_t socket_type, uintptr_t socket_handle,
                                       const uint8_t *remote_addr, uint8_t addr_family, uint16_t remote_port) {
    if (!is_running() || !ipc_server_ || !ipc_server_->is_connected()) {
      return false;
    }

    // Get subprocess process ID
    DWORD process_id = 0;
    if (process_handle_) {
      process_id = GetProcessId(static_cast<HANDLE>(process_handle_));
    }

    if (process_id == 0) {
      BOOST_LOG(error) << "Cannot get subprocess process ID for socket transfer";
      return false;
    }

    // Duplicate socket for subprocess
    WSAPROTOCOL_INFOW protocol_info;
    if (WSADuplicateSocketW(static_cast<SOCKET>(socket_handle), process_id, &protocol_info) != 0) {
      BOOST_LOG(error) << "WSADuplicateSocket failed: " << WSAGetLastError();
      return false;
    }

    // Build socket info payload
    std::vector<uint8_t> payload;
    payload.resize(sizeof(ipc::socket_info_payload_t) + sizeof(protocol_info));

    auto *info = reinterpret_cast<ipc::socket_info_payload_t *>(payload.data());
    info->socket_type = socket_type;
    info->local_port = 0;  // Will be determined by subprocess
    info->remote_port = remote_port;
    info->address_family = addr_family;
    std::memcpy(info->remote_addr, remote_addr, (addr_family == AF_INET6) ? 16 : 4);
    info->protocol_info_length = sizeof(protocol_info);

    std::memcpy(payload.data() + sizeof(ipc::socket_info_payload_t), &protocol_info, sizeof(protocol_info));

    auto result = ipc_server_->send_message(ipc::message_type_e::SOCKET_INFO, payload.data(), payload.size());
    if (result != ipc::result_e::success) {
      BOOST_LOG(error) << "Failed to send socket info: " << ipc::result_to_string(result);
      return false;
    }

    BOOST_LOG(debug) << "Socket transferred to subprocess (type=" << (int) socket_type << ")";
    return true;
  }
#endif

  bool
  subprocess_worker_t::handle_message(const ipc::message_header_t &header, const std::vector<uint8_t> &payload) {
    auto msg_type = static_cast<ipc::message_type_e>(header.type);

    switch (msg_type) {
      case ipc::message_type_e::HEARTBEAT_ACK:
        last_heartbeat_ = std::chrono::steady_clock::now();
        break;

      case ipc::message_type_e::STATUS_STREAMING:
        state_ = state_e::streaming;
        if (status_callback_) {
          status_callback_(state_e::streaming, 0, "");
        }
        break;

      case ipc::message_type_e::STATUS_STOPPED:
        state_ = state_e::stopped;
        if (status_callback_) {
          status_callback_(state_e::stopped, 0, "");
        }
        return false;  // Stop receive loop

      case ipc::message_type_e::STATUS_ERROR: {
        int error_code = 0;
        std::string error_msg = "Unknown error";
        if (payload.size() >= sizeof(ipc::status_error_payload_t)) {
          auto *err = reinterpret_cast<const ipc::status_error_payload_t *>(payload.data());
          error_code = err->error_code;
          if (err->message_length > 0 &&
              payload.size() >= sizeof(ipc::status_error_payload_t) + err->message_length) {
            error_msg = std::string(reinterpret_cast<const char *>(payload.data() + sizeof(ipc::status_error_payload_t)),
                                    err->message_length);
          }
        }
        BOOST_LOG(error) << "Subprocess error: " << error_msg << " (code: " << error_code << ")";
        state_ = state_e::error;
        if (status_callback_) {
          status_callback_(state_e::error, error_code, error_msg);
        }
        return false;  // Stop receive loop
      }

      default:
        BOOST_LOG(debug) << "Received message type: " << static_cast<int>(header.type);
        break;
    }

    return true;
  }

  void
  subprocess_worker_t::heartbeat_thread() {
    auto &sub_config = get_config();

    while (heartbeat_running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sub_config.heartbeat_interval_ms));

      if (!heartbeat_running_ || !ipc_server_ || !ipc_server_->is_connected()) {
        break;
      }

      // Send heartbeat
      auto result = ipc_server_->send_message(ipc::message_type_e::HEARTBEAT);
      if (result != ipc::result_e::success) {
        BOOST_LOG(warning) << "Failed to send heartbeat: " << ipc::result_to_string(result);
        continue;
      }

      // Check for heartbeat timeout
      auto now = std::chrono::steady_clock::now();
      auto last = last_heartbeat_.load();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() > sub_config.heartbeat_timeout_ms) {
        BOOST_LOG(error) << "Subprocess heartbeat timeout";
        state_ = state_e::error;
        if (status_callback_) {
          status_callback_(state_e::error, -100, "Heartbeat timeout");
        }
        break;
      }
    }
  }

  bool
  subprocess_worker_t::launch_process() {
#ifdef _WIN32
    // Find sender executable
    auto &sub_config = get_config();
    std::filesystem::path sender_path;

    if (!sub_config.sender_executable.empty()) {
      sender_path = sub_config.sender_executable;
    }
    else {
      // Look in same directory as current executable
      wchar_t module_path[MAX_PATH];
      GetModuleFileNameW(nullptr, module_path, MAX_PATH);
      sender_path = std::filesystem::path(module_path).parent_path() / "sunshine-sender.exe";
    }

    if (!std::filesystem::exists(sender_path)) {
      BOOST_LOG(error) << "Sender executable not found: " << sender_path.string();
      return false;
    }

    // Build command line
    std::wstring cmdline = L"\"" + sender_path.wstring() + L"\" --session-id " + std::to_wstring(config_.session_id);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Create process
    if (!CreateProcessW(
          nullptr,
          cmdline.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_NO_WINDOW,
          nullptr,
          nullptr,
          &si,
          &pi)) {
      BOOST_LOG(error) << "CreateProcess failed: " << GetLastError();
      return false;
    }

    CloseHandle(pi.hThread);
    process_handle_ = pi.hProcess;

    BOOST_LOG(info) << "Launched subprocess (PID: " << pi.dwProcessId << ")";
    return true;
#else
    // Find sender executable
    auto &sub_config = get_config();
    std::filesystem::path sender_path;

    if (!sub_config.sender_executable.empty()) {
      sender_path = sub_config.sender_executable;
    }
    else {
      // Look in same directory as current executable
      char exe_path[PATH_MAX];
      ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
      if (len == -1) {
        BOOST_LOG(error) << "Failed to get executable path";
        return false;
      }
      exe_path[len] = '\0';
      sender_path = std::filesystem::path(exe_path).parent_path() / "sunshine-sender";
    }

    if (!std::filesystem::exists(sender_path)) {
      BOOST_LOG(error) << "Sender executable not found: " << sender_path.string();
      return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
      BOOST_LOG(error) << "fork() failed: " << strerror(errno);
      return false;
    }

    if (pid == 0) {
      // Child process
      std::string session_id_str = std::to_string(config_.session_id);
      execl(sender_path.c_str(), sender_path.c_str(),
            "--session-id", session_id_str.c_str(),
            nullptr);
      // If execl returns, it failed
      _exit(1);
    }

    // Parent process
    process_handle_ = reinterpret_cast<void *>(static_cast<intptr_t>(pid));
    BOOST_LOG(info) << "Launched subprocess (PID: " << pid << ")";
    return true;
#endif
  }

  void
  subprocess_worker_t::terminate_process() {
    if (!process_handle_) {
      return;
    }

#ifdef _WIN32
    HANDLE handle = static_cast<HANDLE>(process_handle_);

    // Wait for process to exit gracefully
    if (WaitForSingleObject(handle, 3000) == WAIT_TIMEOUT) {
      BOOST_LOG(warning) << "Subprocess did not exit gracefully, terminating";
      TerminateProcess(handle, 1);
      WaitForSingleObject(handle, 1000);
    }

    CloseHandle(handle);
#else
    pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(process_handle_));

    // Send SIGTERM and wait
    kill(pid, SIGTERM);

    int status;
    for (int i = 0; i < 30; ++i) {  // Wait up to 3 seconds
      if (waitpid(pid, &status, WNOHANG) != 0) {
        break;
      }
      std::this_thread::sleep_for(100ms);
    }

    // Force kill if still running
    if (waitpid(pid, &status, WNOHANG) == 0) {
      BOOST_LOG(warning) << "Subprocess did not exit gracefully, killing";
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
#endif

    process_handle_ = nullptr;
  }

  // =====================================================
  // subprocess_manager_t implementation
  // =====================================================

  subprocess_manager_t &
  subprocess_manager_t::instance() {
    static subprocess_manager_t instance;
    return instance;
  }

  std::shared_ptr<subprocess_worker_t>
  subprocess_manager_t::create_worker(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if worker already exists
    auto it = workers_.find(session_id);
    if (it != workers_.end()) {
      BOOST_LOG(warning) << "Worker already exists for session " << session_id;
      return it->second;
    }

    auto worker = std::make_shared<subprocess_worker_t>();
    workers_[session_id] = worker;

    BOOST_LOG(debug) << "Created worker for session " << session_id;
    return worker;
  }

  std::shared_ptr<subprocess_worker_t>
  subprocess_manager_t::get_worker(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(session_id);
    if (it != workers_.end()) {
      return it->second;
    }

    return nullptr;
  }

  void
  subprocess_manager_t::remove_worker(uint32_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(session_id);
    if (it != workers_.end()) {
      it->second->stop();
      workers_.erase(it);
      BOOST_LOG(debug) << "Removed worker for session " << session_id;
    }
  }

  void
  subprocess_manager_t::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    BOOST_LOG(info) << "Stopping all subprocess workers";
    for (auto &[id, worker] : workers_) {
      worker->stop();
    }
    workers_.clear();
  }

}  // namespace subprocess
