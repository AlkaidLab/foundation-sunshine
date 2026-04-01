/**
 * @file src/sender/sender_main.cpp
 * @brief Main entry point for the subprocess sender (data plane).
 *
 * This subprocess handles:
 * - Screen capture (WGC on Windows)
 * - Audio capture (WASAPI loopback)
 * - Hardware encoding (NVENC/AMF/QSV)
 * - RTP packet construction and sending
 *
 * Communication with main process via named pipe IPC.
 */
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "src/subprocess/ipc_pipe.h"
#include "src/subprocess/ipc_protocol.h"

#ifdef _WIN32
  #include <windows.h>
  #include <winsock2.h>
#endif

// Use chrono_literals for time durations
using namespace std::chrono_literals;
namespace ipc = subprocess::ipc;

// Global state
static std::atomic<bool> g_running { true };
static std::atomic<bool> g_streaming { false };
static uint32_t g_session_id = 0;

// Session configuration received from main process
struct session_config_t {
  int width = 0;
  int height = 0;
  int framerate = 0;
  int bitrate_kbps = 0;
  int slices_per_frame = 1;
  int num_ref_frames = 1;
  int encoder_csc_mode = 0;
  int video_format = 0;  // 0=H264, 1=HEVC, 2=AV1
  int dynamic_range = 0;
  int chroma_sampling = 0;
  int enable_intra_refresh = 0;

  int audio_channels = 2;
  int audio_mask = 0;
  int audio_packet_duration = 5;
  bool audio_high_quality = false;
  bool audio_host_audio = false;

  int packet_size = 1024;
  int min_fec_packets = 0;
  int fec_percentage = 20;

  uint8_t encryption_flags = 0;
  uint8_t gcm_key[16] = {};
  uint8_t iv[16] = {};

  std::string display_name;
};

static session_config_t g_config;
static std::unique_ptr<ipc::pipe_client_t> g_ipc_client;

// Forward declarations
void
signal_handler(int sig);
bool
parse_args(int argc, char *argv[]);
bool
connect_to_main_process();
bool
process_ipc_message(const ipc::message_header_t &header, const std::vector<uint8_t> &payload);
void
send_status(ipc::message_type_e status, int error_code = 0, const std::string &error_msg = "");
void
start_streaming();
void
stop_streaming();

/**
 * @brief Signal handler for graceful shutdown.
 */
void
signal_handler(int sig) {
  std::cerr << "[Sender] Received signal " << sig << ", shutting down..." << std::endl;
  g_running = false;
  g_streaming = false;
}

/**
 * @brief Parse command line arguments.
 */
bool
parse_args(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--session-id" && i + 1 < argc) {
      g_session_id = static_cast<uint32_t>(std::stoul(argv[++i]));
    }
    else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " --session-id <id>" << std::endl;
      std::cout << "  --session-id <id>  Session ID for IPC connection" << std::endl;
      return false;
    }
  }

  if (g_session_id == 0) {
    std::cerr << "[Sender] Error: --session-id is required" << std::endl;
    return false;
  }

  return true;
}

/**
 * @brief Connect to the main process via IPC.
 */
bool
connect_to_main_process() {
  std::cerr << "[Sender] Connecting to main process (session " << g_session_id << ")..." << std::endl;

  g_ipc_client = std::make_unique<ipc::pipe_client_t>();

  auto result = g_ipc_client->connect(g_session_id, 10000);  // 10 second timeout
  if (result != ipc::result_e::success) {
    std::cerr << "[Sender] Failed to connect: " << ipc::result_to_string(result) << std::endl;
    return false;
  }

  std::cerr << "[Sender] Connected to main process" << std::endl;
  return true;
}

/**
 * @brief Process an IPC message from main process.
 */
bool
process_ipc_message(const ipc::message_header_t &header, const std::vector<uint8_t> &payload) {
  auto msg_type = static_cast<ipc::message_type_e>(header.type);

  switch (msg_type) {
    case ipc::message_type_e::INIT_SESSION: {
      std::cerr << "[Sender] Received INIT_SESSION" << std::endl;

      if (payload.size() < sizeof(ipc::init_session_payload_t)) {
        send_status(ipc::message_type_e::STATUS_ERROR, -1, "Invalid INIT_SESSION payload size");
        return false;
      }

      auto *init = reinterpret_cast<const ipc::init_session_payload_t *>(payload.data());

      // Parse configuration
      g_config.width = init->width;
      g_config.height = init->height;
      g_config.framerate = init->framerate;
      g_config.bitrate_kbps = init->bitrate;
      g_config.slices_per_frame = init->slices_per_frame;
      g_config.num_ref_frames = init->num_ref_frames;
      g_config.encoder_csc_mode = init->encoder_csc_mode;
      g_config.video_format = init->video_format;
      g_config.dynamic_range = init->dynamic_range;
      g_config.chroma_sampling = init->chroma_sampling;
      g_config.enable_intra_refresh = init->enable_intra_refresh;

      g_config.audio_channels = init->audio_channels;
      g_config.audio_mask = init->audio_mask;
      g_config.audio_packet_duration = init->audio_packet_duration;
      g_config.audio_high_quality = init->audio_high_quality != 0;
      g_config.audio_host_audio = init->audio_host_audio != 0;

      g_config.packet_size = init->packet_size;
      g_config.min_fec_packets = init->min_fec_packets;
      g_config.fec_percentage = init->fec_percentage;

      g_config.encryption_flags = init->encryption_flags;
      std::copy(std::begin(init->gcm_key), std::end(init->gcm_key), g_config.gcm_key);
      std::copy(std::begin(init->iv), std::end(init->iv), g_config.iv);

      // Parse display name
      if (init->display_name_length > 0 &&
          payload.size() >= sizeof(ipc::init_session_payload_t) + init->display_name_length) {
        g_config.display_name = std::string(
          reinterpret_cast<const char *>(payload.data() + sizeof(ipc::init_session_payload_t)),
          init->display_name_length);
      }

      std::cerr << "[Sender] Config: " << g_config.width << "x" << g_config.height
                << "@" << g_config.framerate << "fps, " << g_config.bitrate_kbps << "Kbps"
                << ", format=" << g_config.video_format
                << ", display=" << g_config.display_name << std::endl;

      // TODO: Initialize capture and encoder here

      // Send ready status
      send_status(ipc::message_type_e::STATUS_READY);
      break;
    }

    case ipc::message_type_e::SOCKET_INFO: {
      std::cerr << "[Sender] Received SOCKET_INFO" << std::endl;

      if (payload.size() < sizeof(ipc::socket_info_payload_t)) {
        std::cerr << "[Sender] Invalid SOCKET_INFO payload size" << std::endl;
        break;
      }

      auto *info = reinterpret_cast<const ipc::socket_info_payload_t *>(payload.data());
      std::cerr << "[Sender] Socket type=" << (int) info->socket_type
                << ", remote_port=" << info->remote_port << std::endl;

#ifdef _WIN32
      // Recreate socket from WSAPROTOCOL_INFO
      if (info->protocol_info_length > 0 &&
          payload.size() >= sizeof(ipc::socket_info_payload_t) + info->protocol_info_length) {
        auto *protocol_info = reinterpret_cast<const WSAPROTOCOL_INFOW *>(
          payload.data() + sizeof(ipc::socket_info_payload_t));

        SOCKET sock = WSASocketW(AF_UNSPEC, SOCK_DGRAM, IPPROTO_UDP, 
                                  const_cast<WSAPROTOCOL_INFOW *>(protocol_info), 
                                  0, WSA_FLAG_OVERLAPPED);
        if (sock != INVALID_SOCKET) {
          std::cerr << "[Sender] Successfully received socket (type=" << (int) info->socket_type << ")" << std::endl;
          // TODO: Store socket and use for streaming
        }
        else {
          std::cerr << "[Sender] Failed to create socket from protocol info: " << WSAGetLastError() << std::endl;
        }
      }
#else
      // On Unix, we'll use different socket passing mechanism (SCM_RIGHTS)
      std::cerr << "[Sender] Socket passing not yet implemented on this platform" << std::endl;
#endif
      break;
    }

    case ipc::message_type_e::START_STREAM: {
      std::cerr << "[Sender] Received START_STREAM" << std::endl;
      start_streaming();
      break;
    }

    case ipc::message_type_e::STOP_STREAM: {
      std::cerr << "[Sender] Received STOP_STREAM" << std::endl;
      stop_streaming();
      break;
    }

    case ipc::message_type_e::REQUEST_IDR: {
      std::cerr << "[Sender] Received REQUEST_IDR" << std::endl;
      // TODO: Request IDR frame from encoder
      break;
    }

    case ipc::message_type_e::CHANGE_BITRATE: {
      if (payload.size() >= sizeof(ipc::change_bitrate_payload_t)) {
        auto *br = reinterpret_cast<const ipc::change_bitrate_payload_t *>(payload.data());
        std::cerr << "[Sender] Received CHANGE_BITRATE: " << br->new_bitrate_kbps << " Kbps" << std::endl;
        g_config.bitrate_kbps = br->new_bitrate_kbps;
        // TODO: Update encoder bitrate
      }
      break;
    }

    case ipc::message_type_e::INVALIDATE_REFS: {
      if (payload.size() >= sizeof(ipc::invalidate_refs_payload_t)) {
        auto *refs = reinterpret_cast<const ipc::invalidate_refs_payload_t *>(payload.data());
        std::cerr << "[Sender] Received INVALIDATE_REFS: frames " << refs->first_frame
                  << " to " << refs->last_frame << std::endl;
        // TODO: Invalidate reference frames in encoder
      }
      break;
    }

    case ipc::message_type_e::HEARTBEAT: {
      // Respond with heartbeat ACK
      g_ipc_client->send_message(ipc::message_type_e::HEARTBEAT_ACK);
      break;
    }

    case ipc::message_type_e::SHUTDOWN: {
      std::cerr << "[Sender] Received SHUTDOWN" << std::endl;
      g_running = false;
      return false;
    }

    default:
      std::cerr << "[Sender] Unknown message type: " << header.type << std::endl;
      break;
  }

  return true;
}

/**
 * @brief Send status message to main process.
 */
void
send_status(ipc::message_type_e status, int error_code, const std::string &error_msg) {
  if (!g_ipc_client || !g_ipc_client->is_connected()) {
    return;
  }

  if (status == ipc::message_type_e::STATUS_ERROR) {
    std::vector<uint8_t> payload(sizeof(ipc::status_error_payload_t) + error_msg.size());
    auto *err = reinterpret_cast<ipc::status_error_payload_t *>(payload.data());
    err->error_code = error_code;
    err->message_length = static_cast<uint16_t>(error_msg.size());
    if (!error_msg.empty()) {
      std::memcpy(payload.data() + sizeof(ipc::status_error_payload_t), error_msg.data(), error_msg.size());
    }
    g_ipc_client->send_message(status, payload.data(), payload.size());
  }
  else {
    g_ipc_client->send_message(status);
  }
}

/**
 * @brief Start streaming (capture + encode + send).
 */
void
start_streaming() {
  if (g_streaming) {
    return;
  }

  std::cerr << "[Sender] Starting streaming..." << std::endl;
  g_streaming = true;
  send_status(ipc::message_type_e::STATUS_STREAMING);

  // TODO: Implement actual streaming loop
  // 1. Initialize display capture (WGC)
  // 2. Initialize hardware encoder (NVENC/AMF/QSV)
  // 3. Initialize audio capture (WASAPI)
  // 4. Main loop: capture -> encode -> RTP packetize -> send
}

/**
 * @brief Stop streaming.
 */
void
stop_streaming() {
  if (!g_streaming) {
    return;
  }

  std::cerr << "[Sender] Stopping streaming..." << std::endl;
  g_streaming = false;

  // TODO: Cleanup capture and encoder

  send_status(ipc::message_type_e::STATUS_STOPPED);
}

/**
 * @brief Main entry point.
 */
int
main(int argc, char *argv[]) {
  std::cerr << "[Sender] Subprocess sender starting..." << std::endl;

#ifdef _WIN32
  // Initialize Winsock
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    std::cerr << "[Sender] WSAStartup failed" << std::endl;
    return 1;
  }
#endif

  // Setup signal handlers
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Parse command line arguments
  if (!parse_args(argc, argv)) {
    return 1;
  }

  // Connect to main process
  if (!connect_to_main_process()) {
    return 1;
  }

  // Main IPC message loop
  while (g_running) {
    ipc::message_header_t header;
    std::vector<uint8_t> payload;

    auto result = g_ipc_client->receive_message(header, payload, 1000);
    if (result == ipc::result_e::error_timeout) {
      continue;
    }
    else if (result == ipc::result_e::error_disconnected) {
      std::cerr << "[Sender] Disconnected from main process" << std::endl;
      break;
    }
    else if (result != ipc::result_e::success) {
      std::cerr << "[Sender] IPC error: " << ipc::result_to_string(result) << std::endl;
      continue;
    }

    if (!process_ipc_message(header, payload)) {
      break;
    }
  }

  // Cleanup
  stop_streaming();
  g_ipc_client.reset();

#ifdef _WIN32
  WSACleanup();
#endif

  std::cerr << "[Sender] Subprocess sender exiting" << std::endl;
  return 0;
}
