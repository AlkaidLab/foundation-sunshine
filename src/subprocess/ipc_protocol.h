/**
 * @file src/subprocess/ipc_protocol.h
 * @brief IPC protocol definitions for subprocess streaming architecture.
 *
 * This file defines the message formats and types used for communication
 * between the main process (control plane) and the subprocess (data plane).
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace subprocess {
  namespace ipc {

    /**
     * @brief Magic number to identify valid IPC messages.
     */
    constexpr uint32_t IPC_MAGIC = 0x53554E53;  // "SUNS"

    /**
     * @brief IPC protocol version for compatibility checking.
     */
    constexpr uint16_t IPC_VERSION = 1;

    /**
     * @brief Message types for IPC communication.
     */
    enum class message_type_e : uint16_t {
      // Control messages (Main -> Subprocess)
      INIT_SESSION = 0x0001,      ///< Initialize streaming session with config
      START_STREAM = 0x0002,      ///< Start streaming
      STOP_STREAM = 0x0003,       ///< Stop streaming
      REQUEST_IDR = 0x0004,       ///< Request IDR frame
      CHANGE_BITRATE = 0x0005,    ///< Change encoding bitrate
      INVALIDATE_REFS = 0x0006,   ///< Invalidate reference frames
      SHUTDOWN = 0x0007,          ///< Shutdown subprocess

      // Socket handover (Main -> Subprocess)
      SOCKET_INFO = 0x0100,       ///< Socket information for handover

      // Status messages (Subprocess -> Main)
      STATUS_READY = 0x0200,      ///< Subprocess is ready
      STATUS_STREAMING = 0x0201,  ///< Streaming started
      STATUS_STOPPED = 0x0202,    ///< Streaming stopped
      STATUS_ERROR = 0x0203,      ///< Error occurred

      // Heartbeat
      HEARTBEAT = 0x0300,         ///< Keepalive message
      HEARTBEAT_ACK = 0x0301,     ///< Heartbeat acknowledgment
    };

    /**
     * @brief Common message header for all IPC messages.
     */
#pragma pack(push, 1)
    struct message_header_t {
      uint32_t magic;             ///< Magic number (IPC_MAGIC)
      uint16_t version;           ///< Protocol version
      uint16_t type;              ///< Message type (message_type_e)
      uint32_t payload_length;    ///< Length of payload following header
      uint32_t sequence_number;   ///< Sequence number for tracking
    };

    /**
     * @brief Session initialization message payload.
     */
    struct init_session_payload_t {
      // Video configuration
      int32_t width;
      int32_t height;
      int32_t framerate;
      int32_t bitrate;           ///< Bitrate in Kbps
      int32_t slices_per_frame;
      int32_t num_ref_frames;
      int32_t encoder_csc_mode;
      int32_t video_format;      ///< 0=H264, 1=HEVC, 2=AV1
      int32_t dynamic_range;
      int32_t chroma_sampling;   ///< 0=4:2:0, 1=4:4:4
      int32_t enable_intra_refresh;

      // Audio configuration
      int32_t audio_channels;
      int32_t audio_mask;
      int32_t audio_packet_duration;
      uint8_t audio_high_quality;
      uint8_t audio_host_audio;

      // Network configuration
      int32_t packet_size;
      int32_t min_fec_packets;
      int32_t fec_percentage;

      // Encryption
      uint8_t encryption_flags;
      uint8_t gcm_key[16];
      uint8_t iv[16];

      // Display name length followed by the string
      uint16_t display_name_length;
      // char display_name[display_name_length]; // Variable length
    };

    /**
     * @brief Socket handover information (Windows-specific).
     */
    struct socket_info_payload_t {
      uint8_t socket_type;       ///< 0=video, 1=audio, 2=control
      uint16_t local_port;
      uint16_t remote_port;
      uint8_t address_family;    ///< AF_INET=2, AF_INET6=23
      uint8_t remote_addr[16];   ///< IPv4 (4 bytes) or IPv6 (16 bytes)

      // Windows WSAPROTOCOL_INFO size is ~372 bytes
      uint16_t protocol_info_length;
      // uint8_t protocol_info[protocol_info_length]; // Variable length (WSAPROTOCOL_INFO)
    };

    /**
     * @brief Bitrate change message payload.
     */
    struct change_bitrate_payload_t {
      int32_t new_bitrate_kbps;
    };

    /**
     * @brief Reference frame invalidation payload.
     */
    struct invalidate_refs_payload_t {
      int64_t first_frame;
      int64_t last_frame;
    };

    /**
     * @brief Error status payload.
     */
    struct status_error_payload_t {
      int32_t error_code;
      uint16_t message_length;
      // char message[message_length]; // Variable length
    };

#pragma pack(pop)

    /**
     * @brief Pipe name format for IPC.
     * Format: \\.\pipe\sunshine_subprocess_{session_id}
     */
    inline std::string
    get_pipe_name(uint32_t session_id) {
#ifdef _WIN32
      return R"(\\.\pipe\sunshine_subprocess_)" + std::to_string(session_id);
#else
      return "/tmp/sunshine_subprocess_" + std::to_string(session_id);
#endif
    }

    /**
     * @brief Calculate total message size from header.
     */
    inline size_t
    get_message_size(const message_header_t &header) {
      return sizeof(message_header_t) + header.payload_length;
    }

    /**
     * @brief Validate message header.
     * @return true if header is valid, false otherwise.
     */
    inline bool
    validate_header(const message_header_t &header) {
      return header.magic == IPC_MAGIC && header.version == IPC_VERSION;
    }

    /**
     * @brief Create a message header.
     */
    inline message_header_t
    make_header(message_type_e type, uint32_t payload_length, uint32_t sequence) {
      message_header_t header {};
      header.magic = IPC_MAGIC;
      header.version = IPC_VERSION;
      header.type = static_cast<uint16_t>(type);
      header.payload_length = payload_length;
      header.sequence_number = sequence;
      return header;
    }

  }  // namespace ipc
}  // namespace subprocess
