/**
 * @file src/stream.cpp
 * @brief Definitions for the streaming protocols.
 */
#include "process.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <future>
#include <iomanip>
#include <limits>
#include <queue>
#include <sstream>
#include <set>
#include <string_view>
#include <unordered_map>

#include <fstream>
#include <openssl/err.h>

#include <boost/atomic.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/endian/arithmetic.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

#include "abr.h"

extern "C" {
// clang-format off
#include <moonlight-common-c/src/Limelight-internal.h>
#include <zako/input/zako_input.h>
#include "rswrapper.h"
// clang-format on
}

#ifndef DATA_SHARDS_MAX
  #define DATA_SHARDS_MAX 255
#endif

#include "config.h"
#include "alkaidlab_session_bridge.h"
#include "alkaidlab/sunshine_adapter/clipboard_wire_codec.h"
#include "alkaidlab/sunshine_adapter/gamestream_enet_control_transport_adapter.h"
#include "alkaidlab/sunshine_adapter/microphone_wire_codec.h"
#include "alkaidlab/sunshine_adapter/rescue_wire_codec.h"
#include "alkaidlab/sunshine_adapter/gamestream_rtsp_handshake_adapter.h"
#include "alkaidlab/control_path_health/control_path_health.h"
#include "alkaidlab/rescue_control/rescue_control.h"
#include "display_device/session.h"
#include "globals.h"
#include "rtsp.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "stream.h"
#include "stream_quality.h"
#include "sync.h"
#include "system_tray.h"
#include "thread_safe.h"
#include "utility.h"
#include "stream_quality_controller.h"

#include "platform/common.h"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include "platform/windows/clipboard.h"
#endif

#define IDX_START_A 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7
#define IDX_PERIODIC_PING 8
#define IDX_REQUEST_IDR_FRAME 9
#define IDX_ENCRYPTED 10
#define IDX_HDR_MODE 11
#define IDX_RUMBLE_TRIGGER_DATA 12
#define IDX_SET_MOTION_EVENT 13
#define IDX_SET_RGB_LED 14
#define IDX_SET_ADAPTIVE_TRIGGERS 15
#define IDX_MIC_DATA 16
#define IDX_MIC_CONFIG 17
#define IDX_DYNAMIC_PARAM_CHANGE 18  // 统一动态参数调整消息类型（支持码率、分辨率等）
#define IDX_RESOLUTION_CHANGE 19  // 分辨率变化通知
#define IDX_CLIPBOARD 20  // Clipboard sync (Sunshine protocol extension)
#define IDX_CURSOR_PLANE 21  // Cursor plane metadata (Sunshine protocol extension)
#define IDX_SESSION 22  // Session control (Foundation protocol extension)
#define IDX_RESCUE 23  // Rescue control (Alkaid protocol extension)

#ifndef LI_FF2_RESCUE_CONTROL
  #define LI_FF2_RESCUE_CONTROL (1ULL << 13)
#endif

#ifndef ML_FF2_RESCUE_CONTROL
  #define ML_FF2_RESCUE_CONTROL LI_FF2_RESCUE_CONTROL
#endif

#ifndef SS_RESCUE_PTYPE
  #define SS_RESCUE_PTYPE ALK_SUNSHINE_RESCUE_PTYPE
#endif

static const short packetTypes[] = {
  0x0305,  // Start A
  0x0307,  // Start B
  0x0301,  // Invalidate reference frames
  0x0201,  // Loss Stats
  0x0204,  // Frame Stats (unused)
  0x0206,  // Input data
  0x010b,  // Rumble data
  0x0109,  // Termination
  0x0200,  // Periodic Ping
  0x0302,  // IDR frame
  0x0001,  // fully encrypted
  0x010e,  // HDR mode
  0x5500,  // Rumble triggers (Sunshine protocol extension)
  0x5501,  // Set motion event (Sunshine protocol extension)
  0x5502,  // Set RGB LED (Sunshine protocol extension)
  0x5503,  // Set Adaptive triggers (Sunshine protocol extension)
  0x5504,  // Microphone data (Sunshine protocol extension)
  0x5505,  // Microphone config (Sunshine protocol extension)
  0x5506,  // Dynamic parameter change (Sunshine protocol extension) - 统一动态参数调整
  0x5507,  // Resolution change (Sunshine protocol extension) - 分辨率变化通知
  SS_CLIPBOARD_PTYPE,  // Clipboard sync (Sunshine protocol extension)
  SS_CURSOR_PLANE_PTYPE,  // Cursor plane metadata (Sunshine protocol extension)
  SS_SESSION_PTYPE,  // Session control (Foundation protocol extension)
  SS_RESCUE_PTYPE,  // Rescue control (Alkaid protocol extension)
};

namespace asio = boost::asio;
namespace sys = boost::system;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace stream {
  static std::atomic<std::uint64_t> next_runtime_id { 1 };

  enum class socket_e : int {
    video,  ///< Video
    audio,  ///< Audio
    microphone,  ///< Microphone
  };

  const char *
  socket_name(socket_e type) {
    switch (type) {
      case socket_e::video:
        return "video";
      case socket_e::audio:
        return "audio";
      case socket_e::microphone:
        return "microphone";
    }

    return "unknown";
  }

#pragma pack(push, 1)

  struct video_short_frame_header_t {
    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }

    std::uint8_t headerType;  // Always 0x01 for short headers

    // Sunshine extension
    // Frame processing latency, in 1/10 ms units
    //     zero when the frame is repeated or there is no backend implementation
    boost::endian::little_uint16_at frame_processing_latency;

    // Currently known values:
    // 1 = Normal P-frame
    // 2 = IDR-frame
    // 4 = P-frame with intra-refresh blocks
    // 5 = P-frame after reference frame invalidation
    std::uint8_t frameType;

    // Length of the final packet payload for codecs that cannot handle
    // zero padding, such as AV1 (Sunshine extension).
    boost::endian::little_uint16_at lastPayloadLen;

    std::uint8_t unknown[2];
  };

  static_assert(
    sizeof(video_short_frame_header_t) == 8,
    "Short frame header must be 8 bytes");

  struct video_packet_raw_t {
    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }

    RTP_PACKET rtp;
    char reserved[4];

    NV_VIDEO_PACKET packet;
  };

  struct video_packet_enc_prefix_t {
    std::uint8_t iv[12];  // 12-byte IV is ideal for AES-GCM
    std::uint32_t frameNumber;
    std::uint8_t tag[16];
  };

  struct audio_packet_t {
    RTP_PACKET rtp;
  };

  struct control_header_v2 {
    std::uint16_t type;
    std::uint16_t payloadLength;

    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }
  };

  struct control_terminate_t {
    control_header_v2 header;

    std::uint32_t ec;
  };

  struct control_rumble_t {
    control_header_v2 header;

    std::uint32_t useless;

    std::uint16_t id;
    std::uint16_t lowfreq;
    std::uint16_t highfreq;
  };

  struct control_rumble_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t left;
    std::uint16_t right;
  };

  struct control_set_motion_event_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint16_t reportrate;
    std::uint8_t type;
  };

  struct control_set_rgb_led_t {
    control_header_v2 header;

    std::uint16_t id;
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
  };

  struct control_adaptive_triggers_t {
    control_header_v2 header;

    std::uint16_t id;
    /**
     * 0x04 - Right trigger
     * 0x08 - Left trigger
     */
    std::uint8_t event_flags;
    std::uint8_t type_left;
    std::uint8_t type_right;
    std::uint8_t left[DS_EFFECT_PAYLOAD_SIZE];
    std::uint8_t right[DS_EFFECT_PAYLOAD_SIZE];
  };

  struct control_hdr_mode_t {
    control_header_v2 header;

    std::uint8_t enabled;

    // Sunshine protocol extension
    SS_HDR_METADATA metadata;
  };

  struct control_resolution_change_t {
    control_header_v2 header;

    std::uint32_t width;
    std::uint32_t height;
  };

  struct control_cursor_plane_t {
    control_header_v2 header;

    std::uint16_t version;
    std::uint16_t size;
    std::uint32_t cursorShapeId;
    std::uint32_t x;
    std::uint32_t y;
    std::uint16_t hotspotX;
    std::uint16_t hotspotY;
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t flags;
    std::uint32_t epoch;
  };

  typedef struct control_encrypted_t {
    std::uint16_t encryptedHeaderType;  // Always LE 0x0001
    std::uint16_t length;  // sizeof(seq) + 16 byte tag + secondary header and data

    // seq is accepted as an arbitrary value in Moonlight
    std::uint32_t seq;  // Monotonically increasing sequence number (used as IV for AES-GCM)

    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }
    // encrypted control_header_v2 and payload data follow
  } *control_encrypted_p;

  struct audio_fec_packet_t {
    RTP_PACKET rtp;
    AUDIO_FEC_HEADER fecHeader;
  };

  struct mic_packet_t {
    RTP_PACKET rtp;
  };

  // 扩展的RTP包结构，支持16位包类型
  struct rtp_packet_ext_t {
    std::uint8_t header;
    std::uint16_t packetType;  // 16位包类型
    std::uint16_t sequenceNumber;
    std::uint32_t timestamp;
    std::uint32_t ssrc;
  };

  struct rtp_packet_session_ext_t {
    rtp_packet_ext_t rtp;
    char sessionToken[16];
  };

#pragma pack(pop)

  constexpr std::size_t
  round_to_pkcs7_padded(std::size_t size) {
    return ((size + 15) / 16) * 16;
  }

  // Maximum payload (bytes) of a single audio RTP packet BEFORE AES-CBC
  // padding. Computed as the worst-case codec frame across all supported
  // encodings, plus a small headroom. Each value below is the per-frame
  // payload of the corresponding codec at its highest negotiated setting:
  //
  //   AC3 @640 kbps:  640e3 / 8 * (1536 / 48000)         = 2560 B  (32 ms)
  //   E-AC3 @384k:    384e3 / 8 * (1536 / 48000)         = 1536 B  (32 ms)
  //   PCM_S16 5ms 8c: 5 * 48 * 8 * sizeof(int16_t)        = 3840 B  (worst PCM)
  //   Opus (any cfg): bounded well under 1500 B in practice
  //
  // The compile-time max() ensures adding a new codec only requires bumping
  // the corresponding constant (or its formula) — no risk of forgetting a
  // magic number elsewhere. session::alloc() consumes
  // round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE) for each shard, so the
  // shard buffer width auto-tracks this constant.
  //
  // Note: large frames will exceed Ethernet MTU 1500 and be IP-fragmented at
  // the OS level — expected for raw passthrough on a LAN; receivers
  // reassemble before delivering to the RTP layer.
  namespace audio_payload {
    constexpr std::size_t kAc3MaxFrameBytes    = 2560;  // 640 kbps * 32 ms
    constexpr std::size_t kEac3MaxFrameBytes   = 1536;  // 384 kbps * 32 ms
    constexpr std::size_t kPcmS16MaxFrameBytes = 5 * 48 * 8 * sizeof(int16_t);  // 5 ms * 48 kHz * 8 ch
    constexpr std::size_t kOpusMaxFrameBytes   = 1500;  // generous upper bound
    constexpr std::size_t kHeadroomBytes       = 64;    // future-proofing margin
  }  // namespace audio_payload

  constexpr std::size_t MAX_AUDIO_PACKET_SIZE =
      std::max({
          audio_payload::kAc3MaxFrameBytes,
          audio_payload::kEac3MaxFrameBytes,
          audio_payload::kPcmS16MaxFrameBytes,
          audio_payload::kOpusMaxFrameBytes,
      }) + audio_payload::kHeadroomBytes;

  using audio_aes_t = std::array<char, round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE)>;
#ifdef _WIN32
  class host_cursor_suppression_manager_t {
  public:
    void
    acquire(std::uint64_t runtime_id) {
      if (runtime_id == 0) {
        return;
      }

      auto lg = _leases.lock();
      const auto inserted = _leases->insert(runtime_id).second;
      if (!inserted) {
        return;
      }

      if (_leases->size() == 1) {
        BOOST_LOG(info) << "Host cursor suppression acquired runtime=" << runtime_id
                        << " leases=" << _leases->size()
                        << " action=capture-only";
      }
      else {
        BOOST_LOG(info) << "Host cursor suppression acquired runtime=" << runtime_id
                        << " leases=" << _leases->size();
      }
    }

    void
    release(std::uint64_t runtime_id) {
      if (runtime_id == 0) {
        return;
      }

      auto lg = _leases.lock();
      const auto erased = _leases->erase(runtime_id);
      if (erased == 0) {
        return;
      }

      BOOST_LOG(info) << "Host cursor suppression released runtime=" << runtime_id
                      << " leases=" << _leases->size();
      if (_leases->empty()) {
        BOOST_LOG(info) << "Host cursor suppression leases ended; host cursor was never hidden";
      }
    }

    void
    restore_all(const char *reason) {
      auto lg = _leases.lock();
      if (_leases->empty()) {
        return;
      }
      const auto count = _leases->size();
      _leases->clear();
      BOOST_LOG(info) << "Host cursor suppression cleared reason=" << (reason ? reason : "unknown")
                      << " releasedLeases=" << count;
    }

  private:
    sync_util::sync_t<std::set<std::uint64_t>> _leases;
  };
#else
  class host_cursor_suppression_manager_t {
  public:
    void acquire(std::uint64_t) {}
    void release(std::uint64_t) {}
    void restore_all(const char *) {}
  };
#endif

  using mic_session_token_t = std::array<char, 16>;

  using av_session_id_t = std::variant<asio::ip::address, std::string>;  // IP address or SS-Ping-Payload from RTSP handshake
  using message_queue_t = std::shared_ptr<safe::queue_t<std::pair<udp::endpoint, std::string>>>;
  using message_queue_queue_t = std::shared_ptr<safe::queue_t<std::tuple<socket_e, av_session_id_t, message_queue_t>>>;

  // return bytes written on success
  // return -1 on error
  static inline int
  encode_audio(bool encrypted, const audio::buffer_t &plaintext, uint8_t *destination, crypto::aes_t &iv, crypto::cipher::cbc_t &cbc) {
    // If encryption isn't enabled
    if (!encrypted) {
      std::copy(std::begin(plaintext), std::end(plaintext), destination);
      return plaintext.size();
    }

    return cbc.encrypt(std::string_view { (char *) std::begin(plaintext), plaintext.size() }, destination, &iv);
  }

  static inline void
  while_starting_do_nothing(std::atomic<session::state_e> &state) {
    while (state.load(std::memory_order_acquire) == session::state_e::STARTING) {
      std::this_thread::sleep_for(1ms);
    }
  }

  class session_registry_t {
  public:
    void
    register_session(session_t *session);

    void
    unregister_session(session_t *session);

    void
    bind_control_peer(session_t *session, net::peer_t peer);

    void
    unbind_control_peer(session_t *session);

    session_t *
    find_by_runtime_id(std::uint64_t runtime_id);

    session_t *
    find_by_control_peer(net::peer_t peer);

    session_t *
    find_waiting_by_connect_data(std::uint32_t connect_data);

    std::vector<session_t *>
    find_by_client_cert_key(std::uint64_t client_cert_key);

  private:
    sync_util::sync_t<std::unordered_map<std::uint64_t, session_t *>> _runtime_to_session;
    sync_util::sync_t<std::unordered_map<std::uint32_t, session_t *>> _connect_data_to_session;
    sync_util::sync_t<std::unordered_map<net::peer_t, session_t *>> _control_peer_to_session;
    sync_util::sync_t<std::unordered_map<std::uint64_t, std::vector<session_t *>>> _cert_key_to_sessions;
  };

  class feature_lease_registry_t {
  public:
    session_runtime::owner_token_t
    acquire(session_runtime::feature_e feature, const session_t &session);

    void
    release(session_runtime::feature_e feature, const session_t &session);

    void
    release_all(const session_t &session);

    bool
    validate(session_runtime::feature_e feature, const session_t &session);

    bool
    validate(const session_runtime::owner_token_t &token);

    session_runtime::owner_token_t
    owner(session_runtime::feature_e feature);

  private:
    static std::uint8_t
    feature_key(session_runtime::feature_e feature) {
      return static_cast<std::uint8_t>(feature);
    }

    sync_util::sync_t<std::unordered_map<std::uint8_t, session_runtime::owner_token_t>> _owners;
  };

  class resource_allocator_t {
  public:
    session_runtime::display_allocation_t
    acquire_display_resource(const session_t &session);

    void
    release_display_resource(const session_t &session);

    session_runtime::display_allocation_t
    display_resource();

  private:
    sync_util::sync_t<session_runtime::display_allocation_t> _display_resource;
  };

  class control_server_t {
  public:
    int
    bind(net::af_e address_family, std::uint16_t port) {
      _host = net::host_create(address_family, _addr, port);

      return !(bool) _host;
    }

    // Get session associated with address.
    // If none are found, try to find a session not yet claimed. (It will be marked by a port of value 0
    // If none of those are found, return nullptr
    session_t *
    get_session(const net::peer_t peer, uint32_t connect_data);

    // Circular dependency:
    //   iterate refers to session
    //   session refers to broadcast_ctx_t
    //   broadcast_ctx_t refers to control_server_t
    // Therefore, iterate is implemented further down the source file
    void
    iterate(std::chrono::milliseconds timeout);

    /**
     * @brief Call the handler for a given control stream message.
     * @param type The message type.
     * @param session The session the message was received on.
     * @param payload The payload of the message.
     * @param reinjected `true` if this message is being reprocessed after decryption.
     */
    void
    call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected);

    void
    map(uint16_t type, std::function<void(session_t *, const std::string_view &)> cb) {
      _map_type_cb.emplace(type, std::move(cb));
    }

    int
    send(const std::string_view &payload,
         net::peer_t peer,
         std::uint8_t channel = CTRL_CHANNEL_GENERIC,
         enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE) {
      auto packet = enet_packet_create(payload.data(), payload.size(), flags);
      if (channel >= peer->channelCount) {
        channel = CTRL_CHANNEL_GENERIC;
      }
      if (enet_peer_send(peer, channel, packet)) {
        enet_packet_destroy(packet);

        return -1;
      }

      return 0;
    }

    void
    flush() {
      enet_host_flush(_host.get());
    }

    // Callbacks
    std::unordered_map<std::uint16_t, std::function<void(session_t *, const std::string_view &)>> _map_type_cb;

    // All active sessions (including those still waiting for a peer to connect)
    sync_util::sync_t<std::vector<session_t *>> _sessions;

    // ENet peer to session mapping for sessions with a peer connected
    sync_util::sync_t<std::unordered_map<net::peer_t, session_t *>> _peer_to_session;

    session_registry_t _registry;

    ENetAddress _addr;
    net::host_t _host;
  };

  struct broadcast_ctx_t {
    message_queue_queue_t message_queue_queue;

    std::thread recv_thread;
    std::thread video_thread;
    std::thread audio_thread;
    std::thread control_thread;
    std::thread mic_thread;

    asio::io_context io_context;
    asio::io_context mic_io_context;

    udp::socket video_sock { io_context };
    udp::socket audio_sock { io_context };
    udp::socket mic_sock { mic_io_context };

    control_server_t control_server;

    boost::atomic<bool> mic_socket_enabled { false };
    boost::atomic<int> mic_sessions_count { 0 };  // 需要麦克风的会话数

    // Per-client 麦克风加密上下文（以客户端 IP 为 key）
    struct mic_cipher_ctx_t {
      crypto::cipher::cbc_t cipher;
      crypto::aes_t iv;
      session_runtime::owner_token_t owner;

      mic_cipher_ctx_t(const crypto::aes_t &key, bool padding, std::uint32_t avRiKeyId, session_runtime::owner_token_t owner)
          : cipher(key, padding), iv(16), owner(owner) {
        // 初始化 IV：前 4 字节存储 baseIv（大端序）
        // baseIv 对应客户端的 remoteInputAesIv 的前 4 字节
        // avRiKeyId 就是 launch_session.iv 的前 4 字节（大端序），与 remoteInputAesIv 的前 4 字节相同
        *(std::uint32_t *) iv.data() = util::endian::big<std::uint32_t>(avRiKeyId);
        // 其余字节保持为 0（IV 是 16 字节，但只使用前 4 字节）
        std::memset(iv.data() + 4, 0, 12);
      }

      mic_cipher_ctx_t(mic_cipher_ctx_t &&) noexcept = default;
      mic_cipher_ctx_t &operator=(mic_cipher_ctx_t &&) noexcept = default;
    };

    // Per-session mic owner/cipher table. It is keyed by IP only as a legacy
    // transport limitation; the owner token prevents silent cross-session reuse.
    // shared_ptr 允许在锁外安全使用 cipher_ctx（即使 map 中的条目被其他线程移除，
    // 持有 shared_ptr 的线程仍可安全完成解密操作）
    // 使用 boost::container::flat_map 获得更好的缓存局部性（客户端数 N≤5，线性扫描比哈希更快）
    boost::container::flat_map<std::string, boost::shared_ptr<mic_cipher_ctx_t>> mic_ciphers;
    boost::container::flat_map<std::string, session_runtime::owner_token_t> mic_owners;
    boost::container::flat_map<mic_session_token_t, boost::shared_ptr<mic_cipher_ctx_t>> mic_ciphers_by_token;
    boost::container::flat_map<mic_session_token_t, session_runtime::owner_token_t> mic_owners_by_token;
    boost::mutex mic_cipher_mutex;

    // TODO: 未来版本应当强制启用麦克风加密，防止被窃听
    boost::atomic<bool> mic_reject_plaintext { false };

    std::map<std::string, std::string> client_ip_to_name;
    boost::mutex client_name_mutex;

    feature_lease_registry_t feature_leases;
    host_cursor_suppression_manager_t host_cursor_suppression;
    resource_allocator_t resources;
  };

  struct session_t {
    config_t config;

    safe::mail_t mail;

    std::shared_ptr<input::input_t> input;

    std::thread audioThread;
    std::thread videoThread;

    std::chrono::steady_clock::time_point pingTimeout;

    safe::shared_t<broadcast_ctx_t>::ptr_t broadcast_ref;

    boost::asio::ip::address localAddress;

    session_runtime::identity_t identity;
    session_runtime::transport_path_t active_transport_path { session_runtime::make_enet_direct_transport_path() };
    session_runtime::startup_path_evidence_t startup_path_evidence;
    session_runtime::startup_path_decision_t startup_path_decision;
    session_runtime::session_telemetry_t telemetry;
    LI_SESSION shared_session;
    AlkSessionAdapterContext alkaidlab_session_context;
    ZakoInputRuntime zako_input_runtime;

    // Legacy display/logging field. Critical routing must use identity/runtime IDs.
    std::string client_name;

    struct {
      std::string ping_payload;

      int lowseq;
      udp::endpoint peer;

      std::optional<crypto::cipher::gcm_t> cipher;
      std::uint64_t gcm_iv_counter;

      safe::mail_raw_t::event_t<bool> idr_events;
      safe::mail_raw_t::event_t<std::pair<int64_t, int64_t>> invalidate_ref_frames_events;
      video::dynamic_param_change_event_t dynamic_param_change_events;  // 新增：动态参数调整队列

      std::unique_ptr<platf::deinit_t> qos;
    } video;

    struct {
      crypto::cipher::cbc_t cipher;
      std::string ping_payload;

      std::uint16_t sequenceNumber;
      // avRiKeyId == util::endian::big(First (sizeof(avRiKeyId)) bytes of launch_session->iv)
      std::uint32_t avRiKeyId;
      std::uint32_t timestamp;
      udp::endpoint peer;

      util::buffer_t<char> shards;
      util::buffer_t<uint8_t *> shards_p;

      audio_fec_packet_t fec_packet;
      std::unique_ptr<platf::deinit_t> qos;

      bool enable_mic;
    } audio;

    struct {
      crypto::cipher::gcm_t cipher;
      crypto::aes_t legacy_input_enc_iv;  // Only used when the client doesn't support full control stream encryption
      crypto::aes_t incoming_iv;
      crypto::aes_t outgoing_iv;

      std::uint32_t connect_data;  // Used for new clients with ML_FF_SESSION_ID
      std::string expected_peer_address;  // Only used for legacy clients without ML_FF_SESSION_ID

      net::peer_t peer;
      std::uint32_t seq;
      std::chrono::steady_clock::time_point last_periodic_ping_log {};
      std::chrono::steady_clock::time_point last_rx_diag_log {};
      std::chrono::steady_clock::time_point last_decrypt_diag_log {};
      std::chrono::steady_clock::time_point last_input_diag_log {};
      std::chrono::steady_clock::time_point last_feedback_diag_log {};
      std::uint64_t rx_events { 0 };
      std::uint64_t encrypted_rx_events { 0 };
      std::uint64_t decrypted_rx_events { 0 };
      std::uint64_t input_rx_events { 0 };
      std::uint64_t decrypt_failures { 0 };
      std::uint64_t unencrypted_drops { 0 };
      std::uint64_t unknown_packets { 0 };
      std::uint64_t session_control_rx { 0 };
      std::uint64_t session_control_tx { 0 };
      std::uint64_t session_telemetry_rx { 0 };
      std::uint64_t session_telemetry_tx { 0 };
      std::uint64_t session_lease_rx { 0 };
      std::uint64_t session_lease_tx { 0 };
      bool session_control_negotiated { false };
      bool session_control_welcome_sent { false };
      std::uint64_t session_control_feature_bits { 0 };
      std::chrono::steady_clock::time_point last_session_telemetry_tx {};
      std::uint16_t last_wire_type { 0 };
      std::uint16_t last_decrypted_type { 0 };
      std::uint32_t last_encrypted_seq { 0 };
      std::size_t last_payload_size { 0 };

      platf::feedback_queue_t feedback_queue;
      safe::mail_raw_t::event_t<video::hdr_info_t> hdr_queue;
      safe::mail_raw_t::event_t<std::pair<std::uint32_t, std::uint32_t>> resolution_change_queue;  // width, height
      struct {
        std::uint32_t cursor_shape_id { 0 };
        std::uint32_t x { 0 };
        std::uint32_t y { 0 };
        std::uint16_t hotspot_x { 0 };
        std::uint16_t hotspot_y { 0 };
        std::uint16_t width { 0 };
        std::uint16_t height { 0 };
        std::uint16_t display_width { 0 };
        std::uint16_t display_height { 0 };
        std::uint16_t display_hotspot_x { 0 };
        std::uint16_t display_hotspot_y { 0 };
        std::uint16_t bitmap_width { 0 };
        std::uint16_t bitmap_height { 0 };
        std::uint32_t flags { 0 };
        std::uint32_t epoch { 0 };
        std::uint32_t session_cursor_plane_tx { 0 };
        std::chrono::steady_clock::time_point last_sent {};
        std::chrono::steady_clock::time_point last_bitmap_retry {};
        bool host_cursor_suppressed { false };
        bool bitmap_sent { false };
      } cursor_plane;
      struct {
        bool bound { false };
        bool transfer_active { false };
        uint8_t item_type { LI_CLIPBOARD_ITEM_TYPE_NONE };
        uint8_t transfer_flags { 0 };
        std::uint64_t item_id { 0 };
        std::uint64_t content_hash { 0 };
        std::uint32_t total_length { 0 };
        std::uint32_t received_length { 0 };
        std::string mime_type;
        std::string name;
        std::vector<uint8_t> data;
        std::uint32_t last_host_sequence { 0 };
        std::uint64_t last_sent_hash { 0 };
        bool suppress_next_host_echo { false };
        std::uint64_t suppressed_host_hash { 0 };
      } clipboard;
    } control;

    std::uint32_t launch_session_id;
    std::atomic_bool teardown_counted { false };

    // 保存 launch_session 的关键字段，用于动态参数更新
    bool enable_sops { false };
    bool enable_hdr { false };
    float max_nits { 1000.0f };
    float min_nits { 0.001f };
    float max_full_nits { 1000.0f };

    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::signal_t controlEnd;

    std::atomic<session::state_e> state;

    // Current total bitrate for this session (including FEC overhead) in Kbps
    // This is the user-configured bitrate, not the encoding bitrate
    std::atomic<int> current_total_bitrate { 0 };
    std::atomic<int> current_fec_percentage { 0 };
    std::atomic<int> pacing_total_bitrate { 0 };
    std::atomic<std::uint64_t> stream_quality_frame_area { 0 };
    std::atomic<std::uint64_t> stream_quality_dirty_area { 0 };
    std::atomic<bool> stream_quality_full_frame_dirty { false };
    std::atomic<std::uint32_t> stream_quality_rfi_requests { 0 };
    std::atomic<std::uint32_t> stream_quality_large_frame_fec_skipped { 0 };
    stream_quality::controller_t stream_quality_controller;
    std::chrono::steady_clock::time_point last_stream_quality_fec_feedback {};
    std::chrono::steady_clock::time_point last_stream_quality_recovery_feedback {};
    std::chrono::steady_clock::time_point stream_quality_recovery_ready_after {};
    std::chrono::steady_clock::time_point stream_quality_startup_guard_until {};
    std::chrono::steady_clock::time_point stream_quality_startup_settle_until {};
    std::chrono::steady_clock::time_point stream_quality_resync_guard_until {};
    std::chrono::steady_clock::time_point video_startup_pacing_until {};
    std::chrono::steady_clock::time_point last_adaptive_controller_off_log {};
    std::chrono::steady_clock::time_point last_client_idr_request {};
    std::chrono::steady_clock::time_point last_client_rfi_request {};
    std::chrono::steady_clock::time_point last_client_recovery_coalesce_log {};
    std::chrono::steady_clock::time_point last_stream_quality_startup_guard_log {};
    std::uint32_t coalesced_client_recovery_requests { 0 };
    std::chrono::steady_clock::time_point last_rescue_control_request {};
    std::chrono::steady_clock::time_point rescue_control_hold_until {};
    std::uint32_t rescue_control_requests { 0 };
    std::uint32_t coalesced_rescue_control_requests { 0 };
    std::chrono::steady_clock::time_point last_gamepad_feedback_wait_log {};
    std::chrono::steady_clock::time_point last_gamepad_feedback_fail_log {};
    bool adaptive_controller_enabled { false };
    const char *adaptive_controller_reason { "not-started" };
    int last_applied_stream_quality_bitrate { 0 };
    int last_applied_stream_quality_fec { -1 };
    int last_applied_stream_quality_fps { 0 };
    int last_applied_stream_quality_resolution_scale { 100 };
    int last_applied_stream_quality_chroma_sampling_type { -1 };
    int last_applied_stream_quality_dynamic_range { -1 };
    std::uint32_t last_dynamic_clarity_flags { 0 };
    int last_dynamic_clarity_qp { 0 };
    int last_dynamic_clarity_bitrate { 0 };
    std::chrono::steady_clock::time_point last_control_input_received {};
    std::chrono::steady_clock::time_point last_stream_quality_bitrate_apply {};
    std::chrono::steady_clock::time_point last_stream_quality_fec_apply {};
    std::chrono::steady_clock::time_point last_stream_quality_fps_apply {};
    std::chrono::steady_clock::time_point last_stream_quality_profile_apply {};
    struct {
      std::chrono::steady_clock::time_point window_started {};
      std::uint32_t windows { 0 };
      std::uint32_t duration_ms { 0 };
      std::uint32_t frames_seen { 0 };
      std::uint32_t complete_frames { 0 };
      std::uint32_t recovered_frames { 0 };
      std::uint32_t unrecoverable_frames { 0 };
      std::uint64_t missing_packets { 0 };
      std::uint64_t total_packets { 0 };
      std::uint64_t received_packets { 0 };
      std::uint64_t video_bytes { 0 };
      std::uint32_t audio_underruns { 0 };
      std::uint32_t audio_concealed_ms { 0 };
      std::uint32_t late_audio_drops { 0 };
      std::uint32_t audio_plc_ms { 0 };
      std::uint32_t audio_fade_ms { 0 };
      std::uint32_t max_audio_buffer_depth_ms { 0 };
      std::int32_t max_audio_drift_ppm { 0 };
      std::uint32_t late_frames { 0 };
      std::uint32_t displayed_frames { 0 };
      std::uint32_t visual_stale_frames { 0 };
      std::uint32_t duplicate_frames { 0 };
      std::uint32_t local_display_pressure { 0 };
      std::uint32_t max_decode_queue_depth { 0 };
      std::uint32_t max_render_queue_depth { 0 };
      std::uint32_t max_input_queue_depth { 0 };
      std::uint32_t max_input_latency_us { 0 };
      std::uint64_t max_frame_area { 0 };
      std::uint64_t max_dirty_area { 0 };
      std::uint32_t full_frame_dirty_windows { 0 };
      std::uint32_t rfi_requests { 0 };
      std::uint32_t large_frame_fec_skipped { 0 };
      std::uint32_t max_rtt_ms { 0 };
      std::uint32_t max_rtt_variance_ms { 0 };
      std::uint32_t healthy_actions { 0 };
      std::uint32_t constrained_actions { 0 };
      std::uint32_t crisis_actions { 0 };
      std::uint32_t recovering_actions { 0 };
      std::uint32_t bitrate_applies { 0 };
      std::uint32_t fps_applies { 0 };
      std::uint32_t fec_applies { 0 };
      std::uint32_t profile_applies { 0 };
      std::uint32_t idr_requests { 0 };
    } stream_quality_diag;

    // 标识这是仅控制流会话（只作为输入设备，不传输视频/音频）
    bool control_only { false };
  };

  static bool
  is_lan_or_pc_peer(std::string_view address) {
    if (address.empty()) {
      return false;
    }

    try {
      const auto network = net::from_address(address);
      return network == net::PC || network == net::LAN;
    }
    catch (const std::exception &) {
      return false;
    }
  }

  static session_runtime::startup_path_evidence_t
  startup_path_evidence_for_launch_session(const rtsp_stream::launch_session_t &launch_session) {
    return {
      .peer_is_lan_or_pc = is_lan_or_pc_peer(launch_session.rtsp_peer_address),
      .remote_streaming_hint = launch_session.remote_streaming_hint,
      .rtsp_route_remote_hint = launch_session.rtsp_route_remote_hint,
      .client_route_remote_hint = launch_session.client_route_remote_hint,
      .client_route_tunnel = launch_session.client_route_tunnel_hint,
      .client_vpn_active = launch_session.client_vpn_hint,
      .startup_profile = launch_session.startup_profile,
      .client_egress_kind = launch_session.client_route_egress_kind,
      .client_route_host = launch_session.client_route_host,
      .rtsp_route_host = launch_session.rtsp_route_host,
      .client_source_endpoint = launch_session.client_route_source,
      .host_observed_peer_endpoint = launch_session.rtsp_peer_address,
      .host_observed_local_endpoint = launch_session.rtsp_route_local_endpoint,
      .client_target_address_candidates = launch_session.client_target_address_candidates,
      .host_public_candidates = launch_session.host_public_candidates,
    };
  }

  static std::string
  join_path_values(const std::vector<std::string> &values) {
    std::ostringstream stream;
    bool first = true;
    for (const auto &value : values) {
      if (value.empty()) {
        continue;
      }
      if (!first) {
        stream << ',';
      }
      stream << value;
      first = false;
    }
    return stream.str();
  }

  static void
  log_startup_path_decision(std::uint64_t runtime_id,
                            std::string_view stage,
                            const session_runtime::startup_path_evidence_t &evidence,
                            const session_runtime::startup_path_decision_t &decision) {
    BOOST_LOG(info) << "[session:path] evidence runtime=" << runtime_id
                    << " stage=" << stage
                    << " clientHost=" << evidence.client_route_host
                    << " rtspHost=" << evidence.rtsp_route_host
                    << " clientSource=" << evidence.client_source_endpoint
                    << " hostPeer=" << evidence.host_observed_peer_endpoint
                    << " hostLocal=" << evidence.host_observed_local_endpoint
                    << " clientEgress=" << evidence.client_egress_kind
                    << " remoteHint=" << (evidence.remote_streaming_hint ? 1 : 0)
                    << " clientRouteRemote=" << (evidence.client_route_remote_hint ? 1 : 0)
                    << " rtspRouteRemote=" << (evidence.rtsp_route_remote_hint ? 1 : 0)
                    << " tunnel=" << (evidence.client_route_tunnel ? 1 : 0)
                    << " vpn=" << (evidence.client_vpn_active ? 1 : 0)
                    << " peerLan=" << (evidence.peer_is_lan_or_pc ? 1 : 0)
                    << " targets=" << join_path_values(evidence.client_target_address_candidates)
                    << " hostPublic=" << join_path_values(evidence.host_public_candidates);
    BOOST_LOG(info) << "[session:path] classify runtime=" << runtime_id
                    << " stage=" << stage
                    << " kind=" << session_runtime::li_path_identity_kind_name(decision.path_identity_kind)
                    << " startup=" << session_runtime::li_startup_class_name(decision.startup_class)
                    << " route=" << session_runtime::transport_route_name(decision.route)
                    << " egressKind=" << session_runtime::li_path_egress_kind_name(decision.egress_kind)
                    << " encapsulation=" << session_runtime::li_path_encapsulation_name(decision.encapsulation)
                    << " identityConfidence=" << decision.identity_confidence_ppm
                    << " reasonFlags=0x" << std::hex << decision.reason_flags
                    << " riskFlags=0x" << decision.risk_flags << std::dec
                    << " explanation=" << decision.reason
                    << " lanFast=" << (decision.allow_lan_fast_start ? 1 : 0);
  }

  static session_runtime::feature_caps_t
  session_caps_from_li_feature_bits(std::uint64_t feature_bits) {
    session_runtime::feature_caps_t caps {};

    if ((feature_bits & LI_SESSION_FEATURE_VIDEO_TELEMETRY) != 0) {
      caps.enable(session_runtime::capability_e::native_renderer_metrics)
        .enable(session_runtime::capability_e::frame_reuse_feedback);
    }
    if ((feature_bits & LI_SESSION_FEATURE_TRANSPORT_CC) != 0) {
      caps.enable(session_runtime::capability_e::transport_cc_lite)
        .enable(session_runtime::capability_e::packet_pacer_probe);
    }
    if ((feature_bits & LI_SESSION_FEATURE_NACK_RTX) != 0) {
      caps.enable(session_runtime::capability_e::nack_rtx);
    }
    if ((feature_bits & LI_SESSION_FEATURE_QUIC) != 0) {
      caps.enable((feature_bits & LI_SESSION_FEATURE_RELAY) != 0 ?
                    session_runtime::capability_e::relay_quic :
                    session_runtime::capability_e::quic_direct);
    }
    if ((feature_bits & LI_SESSION_FEATURE_TCP) != 0) {
      caps.enable(session_runtime::capability_e::relay_tcp_tls);
    }

    return caps;
  }

  static session_runtime::feature_caps_t
  session_client_caps_for_config(const config_t &config) {
    session_runtime::feature_caps_t caps {};

    if (config.mlCoreSessionVersion > 0 && config.mlCoreFeatureBits != 0) {
      return session_caps_from_li_feature_bits(config.mlCoreFeatureBits);
    }

    if (config.mlCoreSessionVersion > 0 && !config.mlCoreSupportedCaps.empty()) {
      return session_runtime::parse_capability_names(config.mlCoreSupportedCaps);
    }

    caps.enable(session_runtime::capability_e::native_renderer_metrics)
      .enable(session_runtime::capability_e::frame_reuse_feedback)
      .enable(session_runtime::capability_e::nack_rtx);

    if ((config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) != 0) {
      caps.enable(session_runtime::capability_e::transport_cc_lite);
    }
    if ((config.mlFeatureFlags2 & ML_FF2_QOS_FEEDBACK) != 0) {
      caps.enable(session_runtime::capability_e::owd_feedback);
    }
    if ((config.mlFeatureFlags2 & ML_FF2_CURSOR_PLANE) != 0) {
      caps.enable(session_runtime::capability_e::metal_renderer_metrics);
    }
    if ((config.mlFeatureFlags2 & ML_FF2_PATH_PROBE) != 0) {
      caps.enable(session_runtime::capability_e::packet_pacer_probe);
    }
    return caps;
  }

  static session_runtime::session_telemetry_report_t
  session_report_for_li_session(const session_t &session) {
    auto participant = session_runtime::make_participant(session.identity);
    const auto rtt_ms = session.active_transport_path.score.rtt_ms;
    const auto input_diag = input::diagnostics_snapshot(session.input);
    std::uint32_t cursor_state_flags = LI_SESSION_CURSOR_FLAG_SYSTEM_CURSOR_ACTIVE;
    std::uint32_t pointer_mode = LI_SESSION_POINTER_MODE_HYBRID;
    const auto cursor_flags = session.control.cursor_plane.flags;
    const bool cursor_plane_active =
      (session.config.mlFeatureFlags2 & static_cast<std::uint64_t>(ML_FF2_CURSOR_PLANE_ACTIVE)) != 0;
    const bool remote_visible = (cursor_flags & SS_CURSOR_PLANE_FLAG_VISIBLE) != 0;
    const bool remote_locked = (cursor_flags & SS_CURSOR_PLANE_FLAG_LOCKED) != 0;
    const bool remote_relative = (cursor_flags & SS_CURSOR_PLANE_FLAG_RELATIVE) != 0;

    if (cursor_plane_active || remote_visible) {
      cursor_state_flags |= LI_SESSION_CURSOR_FLAG_REMOTE_PLANE;
    }
    if (session.control.cursor_plane.host_cursor_suppressed) {
      cursor_state_flags |= LI_SESSION_CURSOR_FLAG_LOCAL_HIDDEN;
    }
    if (remote_visible) {
      cursor_state_flags |= LI_SESSION_CURSOR_FLAG_VISIBLE;
    }
    if (remote_locked || remote_relative) {
      cursor_state_flags |= LI_SESSION_CURSOR_FLAG_LOCKED |
                            LI_SESSION_CURSOR_FLAG_RELATIVE_RAW_INPUT;
      pointer_mode = LI_SESSION_POINTER_MODE_RELATIVE;
    }
    else if (cursor_plane_active || remote_visible) {
      pointer_mode = LI_SESSION_POINTER_MODE_ABSOLUTE;
    }

    auto report = session_runtime::session_telemetry_report_t {
      .participant = participant.id,
      .path_id = session.active_transport_path.path_id,
      .displayed_fps = static_cast<std::uint32_t>(std::max(session.config.monitor.framerate, 0)),
      .rtt_ms = rtt_ms,
      .loss_ppm = session.active_transport_path.score.loss_ppm,
      .renderer_backpressure = session.stream_quality_diag.max_render_queue_depth > 0,
      .decode_queue_depth = session.stream_quality_diag.max_decode_queue_depth,
      .render_queue_depth = session.stream_quality_diag.max_render_queue_depth,
      .audio_queue_depth_ms = session.stream_quality_diag.max_audio_buffer_depth_ms,
      .input_queue_depth = session.stream_quality_diag.max_input_queue_depth,
      .input_send_latency_us = session.stream_quality_diag.max_input_latency_us,
      .input_ack_latency_us = 0,
      .mouse_backlog_us = session.stream_quality_diag.max_input_latency_us,
      .pointer_mode = pointer_mode,
      .cursor_state_flags = cursor_state_flags,
    };
    const auto telemetry_snapshot = session.telemetry.snapshot();
    if (const auto *client_report = telemetry_snapshot.report_for(participant.id)) {
      report.displayed_fps = std::max(report.displayed_fps, client_report->displayed_fps);
      report.rtt_ms = client_report->rtt_ms != 0 ? client_report->rtt_ms : report.rtt_ms;
      report.loss_ppm = client_report->loss_ppm != 0 ? client_report->loss_ppm : report.loss_ppm;
      report.renderer_backpressure = report.renderer_backpressure || client_report->renderer_backpressure;
      report.decode_queue_depth = std::max(report.decode_queue_depth, client_report->decode_queue_depth);
      report.render_queue_depth = std::max(report.render_queue_depth, client_report->render_queue_depth);
      report.audio_queue_depth_ms = std::max(report.audio_queue_depth_ms, client_report->audio_queue_depth_ms);
      report.input_queue_depth = std::max(report.input_queue_depth, client_report->input_queue_depth);
      report.input_send_latency_us = std::max(report.input_send_latency_us, client_report->input_send_latency_us);
      report.input_ack_latency_us = std::max(report.input_ack_latency_us, client_report->input_ack_latency_us);
      report.mouse_backlog_us = std::max(report.mouse_backlog_us, client_report->mouse_backlog_us);
      if (client_report->pointer_mode != LI_SESSION_POINTER_MODE_UNKNOWN) {
        report.pointer_mode = client_report->pointer_mode;
      }
      report.cursor_state_flags |= client_report->cursor_state_flags;
      report.pointer_release_queue_depth = std::max(report.pointer_release_queue_depth,
                                                    client_report->pointer_release_queue_depth);
      report.pointer_release_queue_delay_us = std::max(report.pointer_release_queue_delay_us,
                                                       client_report->pointer_release_queue_delay_us);
      report.pointer_mode_switch_us = std::max(report.pointer_mode_switch_us,
                                               client_report->pointer_mode_switch_us);
      report.pointer_deltas_coalesced = std::max(report.pointer_deltas_coalesced,
                                                 client_report->pointer_deltas_coalesced);
      report.pointer_acceleration_risk_ppm = std::max(report.pointer_acceleration_risk_ppm,
                                                      client_report->pointer_acceleration_risk_ppm);
    }
    session_runtime::apply_input_smoothing_snapshot(report, {
      .queue_depth = input_diag.input_queue_depth,
      .queue_delay_us = input_diag.release_queue_delay_us,
      .deltas_coalesced = input_diag.coalesced_pointer_deltas,
      .acceleration_risk_ppm = input_diag.pointer_acceleration_risk_ppm,
      .release_smoothing_active = input_diag.release_smoothing_active,
    });
    return report;
  }

  static void
  refresh_li_session(session_t &session,
                     session::state_e state,
                     std::string_view app_id = {},
                     std::string_view app_name = {}) {
    auto report = session_report_for_li_session(session);
    const auto resolved_app_id = app_id.empty() ?
                                   std::string_view { session.shared_session.appId } :
                                   app_id;
    const auto resolved_app_name = app_name.empty() ?
                                     std::string_view { session.shared_session.appName } :
                                     app_name;
    auto li_session = session_runtime::make_li_session(session.identity,
                                                       session.active_transport_path,
                                                       session_client_caps_for_config(session.config),
                                                       session_runtime::default_rtsp_capability_manifest().supported_caps,
                                                       report,
                                                       resolved_app_id,
                                                       resolved_app_name);
    if (session.shared_session.cursorPlane.version == LI_SESSION_CURSOR_PLANE_VERSION &&
        session.shared_session.cursorPlane.epoch != 0) {
      li_session.cursorPlane = session.shared_session.cursorPlane;
    }
    if (session.shared_session.lease.version == LI_SESSION_LEASE_VERSION &&
        session.shared_session.lease.feature != LI_SESSION_RESOURCE_UNKNOWN) {
      li_session.lease = session.shared_session.lease;
    }
    switch (state) {
      case session::state_e::STOPPED:
        li_session.state = LI_SESSION_STATE_IDLE;
        break;
      case session::state_e::STOPPING:
        li_session.state = LI_SESSION_STATE_DISCONNECTING;
        break;
      case session::state_e::STARTING:
        li_session.state = LI_SESSION_STATE_PROBING;
        break;
      case session::state_e::RUNNING:
        li_session.state = session.stream_quality_controller.state() == stream_quality::state_e::recovering ?
                             LI_SESSION_STATE_RECOVERING :
                             LI_SESSION_STATE_STREAMING;
        break;
    }
    session.shared_session = li_session;
    alkaidlab_session_bridge::update_from_li_session(session.alkaidlab_session_context,
                                                     session.shared_session);
    alkaidlab_session_bridge::project_to_li_session(session.alkaidlab_session_context,
                                                    session.shared_session);
    zako_input_runtime_apply_snapshot(&session.zako_input_runtime,
                                      &session.alkaidlab_session_context.snapshot);
  }

  void
  session_registry_t::register_session(session_t *session) {
    if (!session || session->identity.runtime_id == 0) {
      return;
    }

    {
      auto lg = _runtime_to_session.lock();
      _runtime_to_session->emplace(session->identity.runtime_id, session);
    }

    if (session->identity.control_connect_data != 0) {
      auto lg = _connect_data_to_session.lock();
      _connect_data_to_session->emplace(session->identity.control_connect_data, session);
    }

    if (session->identity.client_cert_key != 0) {
      auto lg = _cert_key_to_sessions.lock();
      (*_cert_key_to_sessions)[session->identity.client_cert_key].push_back(session);
    }
  }

  void
  session_registry_t::unregister_session(session_t *session) {
    if (!session) {
      return;
    }

    {
      auto lg = _runtime_to_session.lock();
      _runtime_to_session->erase(session->identity.runtime_id);
    }

    if (session->identity.control_connect_data != 0) {
      auto lg = _connect_data_to_session.lock();
      auto it = _connect_data_to_session->find(session->identity.control_connect_data);
      if (it != _connect_data_to_session->end() && it->second == session) {
        _connect_data_to_session->erase(it);
      }
    }

    if (session->control.peer) {
      auto lg = _control_peer_to_session.lock();
      auto it = _control_peer_to_session->find(session->control.peer);
      if (it != _control_peer_to_session->end() && it->second == session) {
        _control_peer_to_session->erase(it);
      }
    }

    if (session->identity.client_cert_key != 0) {
      auto lg = _cert_key_to_sessions.lock();
      auto it = _cert_key_to_sessions->find(session->identity.client_cert_key);
      if (it != _cert_key_to_sessions->end()) {
        auto &sessions = it->second;
        sessions.erase(std::remove(sessions.begin(), sessions.end(), session), sessions.end());
        if (sessions.empty()) {
          _cert_key_to_sessions->erase(it);
        }
      }
    }
  }

  void
  session_registry_t::bind_control_peer(session_t *session, net::peer_t peer) {
    if (!session || !peer) {
      return;
    }

    auto lg = _control_peer_to_session.lock();
    (*_control_peer_to_session)[peer] = session;
  }

  void
  session_registry_t::unbind_control_peer(session_t *session) {
    if (!session || !session->control.peer) {
      return;
    }

    auto lg = _control_peer_to_session.lock();
    auto it = _control_peer_to_session->find(session->control.peer);
    if (it != _control_peer_to_session->end() && it->second == session) {
      _control_peer_to_session->erase(it);
    }
  }

  session_t *
  session_registry_t::find_by_runtime_id(std::uint64_t runtime_id) {
    if (runtime_id == 0) {
      return nullptr;
    }

    auto lg = _runtime_to_session.lock();
    auto it = _runtime_to_session->find(runtime_id);
    return it == _runtime_to_session->end() ? nullptr : it->second;
  }

  session_t *
  session_registry_t::find_by_control_peer(net::peer_t peer) {
    if (!peer) {
      return nullptr;
    }

    auto lg = _control_peer_to_session.lock();
    auto it = _control_peer_to_session->find(peer);
    return it == _control_peer_to_session->end() ? nullptr : it->second;
  }

  session_t *
  session_registry_t::find_waiting_by_connect_data(std::uint32_t connect_data) {
    if (connect_data == 0) {
      return nullptr;
    }

    auto lg = _connect_data_to_session.lock();
    auto it = _connect_data_to_session->find(connect_data);
    if (it == _connect_data_to_session->end() ||
        it->second->control.peer ||
        !(it->second->config.mlFeatureFlags & ML_FF_SESSION_ID)) {
      return nullptr;
    }

    return it->second;
  }

  std::vector<session_t *>
  session_registry_t::find_by_client_cert_key(std::uint64_t client_cert_key) {
    if (client_cert_key == 0) {
      return {};
    }

    auto lg = _cert_key_to_sessions.lock();
    auto it = _cert_key_to_sessions->find(client_cert_key);
    return it == _cert_key_to_sessions->end() ? std::vector<session_t *> {} : it->second;
  }


  static const char *
  feature_name(session_runtime::feature_e feature) {
    switch (feature) {
      case session_runtime::feature_e::clipboard:
        return "clipboard";
      case session_runtime::feature_e::microphone:
        return "microphone";
      case session_runtime::feature_e::display:
        return "display";
      case session_runtime::feature_e::dynamic_params:
        return "dynamic-params";
      case session_runtime::feature_e::input_focus:
        return "input-focus";
      case session_runtime::feature_e::transport_qos:
        return "transport-qos";
      case session_runtime::feature_e::cursor_plane:
        return "cursor-plane";
      case session_runtime::feature_e::clipboard_bulk:
        return "clipboard-bulk";
      case session_runtime::feature_e::dynamic_quality:
        return "dynamic-quality";
    }
    return "unknown";
  }

  session_runtime::owner_token_t
  feature_lease_registry_t::acquire(session_runtime::feature_e feature, const session_t &session) {
    session_runtime::owner_token_t token {
      feature,
      session.identity.runtime_id,
      session.identity.client_cert_key,
      session.identity.control_generation,
    };

    session_runtime::owner_token_t previous { feature };
    {
      auto lg = _owners.lock();
      auto it = _owners->find(feature_key(feature));
      if (it != _owners->end()) {
        previous = it->second;
      }
      (*_owners)[feature_key(feature)] = token;
    }
    BOOST_LOG(info) << "Session feature owner acquired"
                    << " feature=" << feature_name(feature)
                    << " runtime=" << token.runtime_id
                    << " previous=" << previous.runtime_id
                    << " certKey=" << token.client_cert_key
                    << " generation=" << token.control_generation;
    return token;
  }

  void
  feature_lease_registry_t::release(session_runtime::feature_e feature, const session_t &session) {
    bool released = false;
    {
      auto lg = _owners.lock();
      auto it = _owners->find(feature_key(feature));
      if (it != _owners->end() &&
          it->second.runtime_id == session.identity.runtime_id) {
        _owners->erase(it);
        released = true;
      }
    }
    if (released) {
      BOOST_LOG(info) << "Session feature owner released"
                      << " feature=" << feature_name(feature)
                      << " runtime=" << session.identity.runtime_id;
    }
  }

  void
  feature_lease_registry_t::release_all(const session_t &session) {
    std::vector<session_runtime::feature_e> released_features;
    {
      auto lg = _owners.lock();
      for (auto it = _owners->begin(); it != _owners->end();) {
        if (it->second.runtime_id == session.identity.runtime_id) {
          released_features.push_back(it->second.feature);
          it = _owners->erase(it);
        }
        else {
          ++it;
        }
      }
    }
    for (auto feature : released_features) {
      BOOST_LOG(info) << "Session feature owner released"
                      << " feature=" << feature_name(feature)
                      << " runtime=" << session.identity.runtime_id
                      << " reason=release-all";
    }
  }

  bool
  feature_lease_registry_t::validate(session_runtime::feature_e feature, const session_t &session) {
    auto lg = _owners.lock();
    auto it = _owners->find(feature_key(feature));
    return it != _owners->end() &&
           it->second.runtime_id == session.identity.runtime_id &&
           it->second.client_cert_key == session.identity.client_cert_key &&
           it->second.control_generation == session.identity.control_generation;
  }

  bool
  feature_lease_registry_t::validate(const session_runtime::owner_token_t &token) {
    if (!token) {
      return false;
    }

    auto lg = _owners.lock();
    auto it = _owners->find(feature_key(token.feature));
    return it != _owners->end() &&
           it->second.runtime_id == token.runtime_id &&
           it->second.client_cert_key == token.client_cert_key &&
           it->second.control_generation == token.control_generation;
  }

  session_runtime::owner_token_t
  feature_lease_registry_t::owner(session_runtime::feature_e feature) {
    auto lg = _owners.lock();
    auto it = _owners->find(feature_key(feature));
    return it == _owners->end() ? session_runtime::owner_token_t { feature } : it->second;
  }

  static const char *
  resource_scope_name(session_runtime::resource_scope_e scope) {
    switch (scope) {
      case session_runtime::resource_scope_e::per_session:
        return "per-session";
      case session_runtime::resource_scope_e::per_device:
        return "per-device";
      case session_runtime::resource_scope_e::global_exclusive:
        return "global-exclusive";
      case session_runtime::resource_scope_e::shared_global:
      default:
        return "shared-global";
    }
  }

  static const char *
  display_allocation_mode_name(session_runtime::display_allocation_mode_e mode) {
    switch (mode) {
      case session_runtime::display_allocation_mode_e::shared_follower:
        return "shared-follower";
      case session_runtime::display_allocation_mode_e::dedicated:
        return "dedicated";
      case session_runtime::display_allocation_mode_e::shared_owner:
      default:
        return "shared-owner";
    }
  }

  session_runtime::display_allocation_t
  resource_allocator_t::acquire_display_resource(const session_t &session) {
    auto lg = _display_resource.lock();
    if (!_display_resource->owner) {
      _display_resource->owner = session_runtime::owner_token_t {
        session_runtime::feature_e::display,
        session.identity.runtime_id,
        session.identity.client_cert_key,
        session.identity.control_generation,
      };
    }

    auto allocation = *_display_resource;
    allocation.mode = allocation.owner.runtime_id == session.identity.runtime_id ?
                        session_runtime::display_allocation_mode_e::shared_owner :
                        session_runtime::display_allocation_mode_e::shared_follower;
    return allocation;
  }

  void
  resource_allocator_t::release_display_resource(const session_t &session) {
    auto lg = _display_resource.lock();
    if (_display_resource->owner.runtime_id == session.identity.runtime_id) {
      _display_resource->owner = session_runtime::owner_token_t {
        session_runtime::feature_e::display,
      };
    }
  }

  session_runtime::display_allocation_t
  resource_allocator_t::display_resource() {
    auto lg = _display_resource.lock();
    return *_display_resource;
  }

  static std::uint16_t
  read_be16_unaligned(const void *ptr) {
    std::uint16_t value;
    std::memcpy(&value, ptr, sizeof(value));
    return util::endian::big(value);
  }

  static std::uint32_t
  read_be32_unaligned(const void *ptr) {
    std::uint32_t value;
    std::memcpy(&value, ptr, sizeof(value));
    return util::endian::big(value);
  }

  static const char *
  stream_quality_state_name(stream_quality::state_e state) {
    switch (state) {
      case stream_quality::state_e::healthy:
        return "healthy";
      case stream_quality::state_e::constrained:
        return "constrained";
      case stream_quality::state_e::crisis:
        return "crisis";
      case stream_quality::state_e::recovering:
        return "recovering";
      default:
        return "unknown";
    }
  }

  static const char *
  stream_quality_decision_reason_name(const stream_quality::action_t &action) {
    switch (action.reason) {
      case stream_quality::reason_e::startup:
        return "startup";
      case stream_quality::reason_e::recovering:
        return "recovery";
      case stream_quality::reason_e::random_loss:
      case stream_quality::reason_e::delay_congestion:
        return "network";
      case stream_quality::reason_e::render_deadline:
        return "client-render";
      case stream_quality::reason_e::motion_pressure:
        return "high-motion";
      case stream_quality::reason_e::audio_pressure:
        return "audio";
      case stream_quality::reason_e::input_pressure:
        return "input";
      case stream_quality::reason_e::healthy:
      default:
        return "healthy";
    }
  }

  static const char *
  runtime_scale_mode_name(const stream_quality::action_t &action) {
    if (action.resolution_scale_percent >= 100) {
      return "none";
    }
    return action.runtime_scale_applied ? "soft" : "none";
  }

  static int
  total_video_bitrate_from_encoding_bitrate(int encoding_bitrate_kbps, int fec_percentage) {
    if (encoding_bitrate_kbps <= 0) {
      return encoding_bitrate_kbps;
    }

    fec_percentage = std::clamp(fec_percentage, 0, 100);
    if (fec_percentage == 0) {
      return encoding_bitrate_kbps;
    }

    return static_cast<int>(std::lround(
      static_cast<double>(encoding_bitrate_kbps) *
      static_cast<double>(100 + fec_percentage) / 100.0));
  }

  static int
  encoding_bitrate_from_total_video_budget(int total_bitrate_kbps, int fec_percentage) {
    if (total_bitrate_kbps <= 0) {
      return total_bitrate_kbps;
    }

    fec_percentage = std::clamp(fec_percentage, 0, 100);
    if (fec_percentage == 0) {
      return total_bitrate_kbps;
    }

    return static_cast<int>(std::lround(
      static_cast<double>(total_bitrate_kbps) *
      100.0 / static_cast<double>(100 + fec_percentage)));
  }

  static int
  scaled_even_dimension(int dimension, int scale_percent) {
    if (dimension <= 0) {
      return 0;
    }

    auto scaled = static_cast<int>(std::lround(static_cast<double>(dimension) *
                                               static_cast<double>(scale_percent) / 100.0));
    scaled = std::max(16, scaled);
    return std::max(16, scaled - (scaled % 2));
  }

  struct runtime_profile_resolution_t {
    int width;
    int height;
  };

  runtime_profile_resolution_t
  runtime_profile_resolution_for_scale(int source_width, int source_height, int scale_percent) {
    scale_percent = std::clamp(scale_percent, 1, 100);
    return {
      .width = scaled_even_dimension(source_width, scale_percent),
      .height = scaled_even_dimension(source_height, scale_percent),
    };
  }

  bool
  runtime_profile_resolution_reconfig_enabled() {
    return session_runtime::runtime_profile_resolution_reconfig_enabled();
  }

  static stream_quality::content_type_e
  stream_quality_content_type_from_monitor(int content_type) {
    switch (content_type) {
      case 1:
        return stream_quality::content_type_e::text;
      case 2:
        return stream_quality::content_type_e::motion;
      case 3:
        return stream_quality::content_type_e::game;
      case 0:
      default:
        return stream_quality::content_type_e::desktop;
    }
  }

  static void
  update_dynamic_clarity_intent(session_t *session, int encoding_bitrate_kbps, int fps) {
    if (!session || encoding_bitrate_kbps <= 0 || fps <= 0) {
      return;
    }

    auto &monitor = session->config.monitor;
    const auto plan = stream_quality::plan_low_bitrate_clarity({
      .width = monitor.width,
      .height = monitor.height,
      .fps = fps,
      .video_bitrate_kbps = encoding_bitrate_kbps,
      .video_format = monitor.videoFormat,
      .chroma_sampling_type = monitor.chromaSamplingType,
      .content_type = stream_quality_content_type_from_monitor(monitor.contentType),
    });

    const bool changed = plan.intent_flags != session->last_dynamic_clarity_flags ||
                         plan.target_qp != session->last_dynamic_clarity_qp ||
                         std::abs(encoding_bitrate_kbps - session->last_dynamic_clarity_bitrate) >= 1000;
    monitor.lowBitrateClarityIntentFlags = plan.intent_flags;
    monitor.lowBitrateTargetQp = plan.target_qp;
    monitor.lowBitrateSharpenAlpha = plan.sharpen_alpha;
    session->last_dynamic_clarity_flags = plan.intent_flags;
    session->last_dynamic_clarity_qp = plan.target_qp;
    session->last_dynamic_clarity_bitrate = encoding_bitrate_kbps;

    if (!changed || !plan.enabled) {
      return;
    }

    BOOST_LOG(info) << "Frame interest intent generated runtime=" << session->identity.runtime_id
                    << " encoding=" << encoding_bitrate_kbps << " Kbps"
                    << " fps=" << fps
                    << " bpp=" << plan.bits_per_pixel_per_frame
                    << " qp=" << plan.target_qp
                    << " roi=" << (plan.roi_enabled ? 1 : 0)
                    << " dirtyRegion=" << (plan.dirty_region_priority ? 1 : 0)
                    << " temporalLayers=" << (plan.prefer_temporal_layers ? 1 : 0)
                    << " discardableEnhancement=" << (plan.discardable_enhancement_layer ? 1 : 0)
                    << " ltr=" << (plan.prefer_long_term_reference ? 1 : 0)
                    << " intraRefresh=" << (plan.prefer_intra_refresh ? 1 : 0)
                    << " flags=0x" << std::hex << plan.intent_flags << std::dec
                    << " sharpen=" << plan.sharpen_alpha;
  }

  bool
  should_synthesize_stream_quality_recovery_feedback(int ml_feature_flags) {
    return (ml_feature_flags & ML_FF_NETWORK_FEEDBACK) == 0;
  }

  bool
  should_apply_frame_fec_stream_quality_feedback(int ml_feature_flags) {
    return (ml_feature_flags & ML_FF_NETWORK_FEEDBACK) == 0;
  }

  bool
  stream_quality_resync_guard_allows_safety_apply(const stream_quality::action_t &action,
                                            int last_applied_bitrate_kbps,
                                            int last_applied_fec_percentage) {
    if (!action.changed) {
      return false;
    }

    const bool bitrate_downshift =
      last_applied_bitrate_kbps <= 0 ||
      (action.target_bitrate_kbps > 0 &&
       action.target_bitrate_kbps < last_applied_bitrate_kbps);
    const bool fec_downshift =
      last_applied_fec_percentage < 0 ||
      action.fec_percentage < last_applied_fec_percentage;
    const bool low_availability_safety =
      action.availability == stream_quality::availability_e::low ||
      action.state == stream_quality::state_e::crisis ||
      action.pressures.burst_loss >= 0.70 ||
      action.pressures.random_loss >= 0.70 ||
      action.pressures.render >= 0.70 ||
      action.unrecoverable_loss >= 0.03 ||
      action.packet_loss >= 0.12 ||
      action.rfi_limited ||
      action.congestion_anti_spiral;
    return action.state != stream_quality::state_e::healthy &&
           low_availability_safety &&
           (bitrate_downshift || fec_downshift);
  }

  const char *
  adaptive_controller_state_name(const session_t *session) {
    return session && session->adaptive_controller_enabled ? "auto" : "off";
  }

  const char *
  adaptive_controller_reason(const session_t *session) {
    return session && session->adaptive_controller_reason ? session->adaptive_controller_reason : "unknown";
  }

  bool
  adaptive_controller_active(session_t *session, const char *source) {
    if (session && session->adaptive_controller_enabled) {
      return true;
    }

    if (session) {
      const auto now = std::chrono::steady_clock::now();
      if (session->last_adaptive_controller_off_log.time_since_epoch().count() == 0 ||
          now - session->last_adaptive_controller_off_log >= 3000ms) {
        session->last_adaptive_controller_off_log = now;
        BOOST_LOG(info) << "Adaptive streaming controller skipped runtime="
                        << session->identity.runtime_id
                        << " adaptiveController=off"
                        << " reason=" << adaptive_controller_reason(session)
                        << " source=" << (source ? source : "unknown");
      }
    }
    return false;
  }

  template<typename T>
  static void
  atomic_store_max(std::atomic<T> &target, T value) {
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(previous,
                                         value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {}
  }

  static void
  record_frame_interest_feedback(void *channel_data, const video::frame_interest_feedback_t &feedback) {
    auto *session = static_cast<session_t *>(channel_data);
    if (!session) {
      return;
    }

    if (feedback.frame_area > 0) {
      session->stream_quality_frame_area.store(feedback.frame_area, std::memory_order_relaxed);
    }
    atomic_store_max(session->stream_quality_dirty_area, feedback.dirty_area);
    if (feedback.full_frame_dirty) {
      session->stream_quality_full_frame_dirty.store(true, std::memory_order_relaxed);
    }
  }

  static void
  annotate_feedback_with_host_motion(session_t *session, stream_quality::feedback_t &feedback) {
    if (!session) {
      return;
    }

    auto frame_area = session->stream_quality_frame_area.load(std::memory_order_relaxed);
    if (frame_area == 0 && session->config.monitor.width > 0 && session->config.monitor.height > 0) {
      frame_area = static_cast<std::uint64_t>(session->config.monitor.width) *
                   static_cast<std::uint64_t>(session->config.monitor.height);
    }
    feedback.frame_area = frame_area;
    feedback.dirty_area = session->stream_quality_dirty_area.exchange(0, std::memory_order_relaxed);
    feedback.full_frame_dirty = session->stream_quality_full_frame_dirty.exchange(false, std::memory_order_relaxed);
    feedback.rfi_requests = session->stream_quality_rfi_requests.exchange(0, std::memory_order_relaxed);
    feedback.large_frame_fec_skipped = session->stream_quality_large_frame_fec_skipped.exchange(0, std::memory_order_relaxed);
  }

  static void
  record_stream_quality_feedback_diag(session_t *session,
                                const stream_quality::feedback_t &feedback,
                                const stream_quality::action_t &action,
                                const char *source) {
    if (!session) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    auto &diag = session->stream_quality_diag;
    if (diag.window_started.time_since_epoch().count() == 0) {
      diag.window_started = now;
    }

    diag.windows++;
    diag.duration_ms += feedback.duration_ms;
    diag.frames_seen += feedback.frames_seen;
    diag.complete_frames += feedback.complete_frames;
    diag.recovered_frames += feedback.recovered_frames;
    diag.unrecoverable_frames += feedback.unrecoverable_frames;
    diag.missing_packets += feedback.missing_packets;
    diag.total_packets += feedback.total_packets;
    diag.received_packets += feedback.received_packets;
    diag.video_bytes += feedback.video_bytes;
    diag.audio_underruns += feedback.audio_underruns;
    diag.audio_concealed_ms += feedback.audio_concealed_ms;
    diag.late_audio_drops += feedback.late_audio_drops;
    diag.audio_plc_ms += feedback.audio_plc_ms;
    diag.audio_fade_ms += feedback.audio_fade_ms;
    diag.max_audio_buffer_depth_ms = std::max(diag.max_audio_buffer_depth_ms, feedback.audio_buffer_depth_ms);
    diag.max_audio_drift_ppm = std::max(diag.max_audio_drift_ppm, std::abs(feedback.audio_drift_ppm));
    diag.late_frames += feedback.late_frames;
    diag.displayed_frames += feedback.displayed_frames;
    diag.visual_stale_frames += feedback.visual_stale_frames;
    diag.duplicate_frames += feedback.duplicate_frames;
    diag.local_display_pressure = std::max(diag.local_display_pressure, feedback.local_display_pressure);
    diag.max_decode_queue_depth = std::max(diag.max_decode_queue_depth, feedback.decode_queue_depth);
    diag.max_render_queue_depth = std::max(diag.max_render_queue_depth, feedback.render_queue_depth);
    diag.max_input_queue_depth = std::max(diag.max_input_queue_depth, feedback.input_queue_depth);
    diag.max_input_latency_us = std::max(diag.max_input_latency_us,
                                          std::max(feedback.input_send_latency_us, feedback.input_ack_latency_us));
    session->active_transport_path.score.rtt_ms = feedback.rtt_ms;
    session->active_transport_path.score.jitter_ms = feedback.rtt_variance_ms;
    if (feedback.total_packets > 0) {
      session->active_transport_path.score.loss_ppm = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(1000000ULL,
                                static_cast<std::uint64_t>(feedback.missing_packets) * 1000000ULL /
                                  feedback.total_packets));
    }
    refresh_li_session(*session, session->state.load(std::memory_order_relaxed));
    diag.max_frame_area = std::max(diag.max_frame_area, feedback.frame_area);
    diag.max_dirty_area = std::max(diag.max_dirty_area, feedback.dirty_area);
    diag.full_frame_dirty_windows += feedback.full_frame_dirty ? 1U : 0U;
    diag.rfi_requests += feedback.rfi_requests;
    diag.large_frame_fec_skipped += feedback.large_frame_fec_skipped;
    diag.max_rtt_ms = std::max(diag.max_rtt_ms, feedback.rtt_ms);
    diag.max_rtt_variance_ms = std::max(diag.max_rtt_variance_ms, feedback.rtt_variance_ms);
    if (action.request_idr) {
      diag.idr_requests++;
    }

    switch (action.state) {
      case stream_quality::state_e::healthy:
        diag.healthy_actions++;
        break;
      case stream_quality::state_e::constrained:
        diag.constrained_actions++;
        break;
      case stream_quality::state_e::crisis:
        diag.crisis_actions++;
        break;
      case stream_quality::state_e::recovering:
        diag.recovering_actions++;
        break;
    }

    if (now - diag.window_started < 1000ms) {
      return;
    }

    const auto loss_percentage = diag.total_packets > 0 ?
                                   static_cast<double>(diag.missing_packets) *
                                     100.0 / static_cast<double>(diag.total_packets) :
                                   0.0;
    const auto input_latency_ms = static_cast<double>(diag.max_input_latency_us) / 1000.0;
    std::ostringstream message;
    message << "Stream-quality diag runtime=" << session->identity.runtime_id
            << " source=" << source
            << " windowMs=" << std::chrono::duration_cast<std::chrono::milliseconds>(now - diag.window_started).count()
            << " samples=" << diag.windows
            << " feedbackMs=" << diag.duration_ms
            << " frames=" << diag.frames_seen
            << " complete=" << diag.complete_frames
            << " recovered=" << diag.recovered_frames
            << " unrecoverable=" << diag.unrecoverable_frames
            << " loss=" << loss_percentage << "%"
            << " packets=" << diag.received_packets << "/" << diag.total_packets
            << " missing=" << diag.missing_packets
            << " audioUnd=" << diag.audio_underruns
            << " audioConceal=" << diag.audio_concealed_ms << "ms"
            << " lateAudioDrop=" << diag.late_audio_drops
            << " plc=" << diag.audio_plc_ms << "ms"
            << " fade=" << diag.audio_fade_ms << "ms"
            << " audioBufMax=" << diag.max_audio_buffer_depth_ms << "ms"
            << " driftMax=" << diag.max_audio_drift_ppm << "ppm"
            << " late=" << diag.late_frames
            << " displayed=" << diag.displayed_frames
            << " visualStale=" << diag.visual_stale_frames
            << " duplicate=" << diag.duplicate_frames
            << " localDisplayPressure=" << diag.local_display_pressure
            << " dqMax=" << diag.max_decode_queue_depth
            << " rqMax=" << diag.max_render_queue_depth
            << " inputQMax=" << diag.max_input_queue_depth
            << " inputLatencyMax=" << input_latency_ms << "ms"
            << " dirtyAreaMax=" << diag.max_dirty_area
            << " frameAreaMax=" << diag.max_frame_area
            << " fullFrameDirty=" << diag.full_frame_dirty_windows
            << " rfiRequests=" << diag.rfi_requests
            << " largeFrameFecSkipped=" << diag.large_frame_fec_skipped
            << " rttMax=" << diag.max_rtt_ms << "ms"
            << " jitterMax=" << diag.max_rtt_variance_ms << "ms"
            << " states(h/c/x/r)=" << diag.healthy_actions << "/"
            << diag.constrained_actions << "/" << diag.crisis_actions << "/"
            << diag.recovering_actions
            << " applies(bitrate/fps/fec)=" << diag.bitrate_applies << "/"
            << diag.fps_applies << "/" << diag.fec_applies
            << " profileApplies=" << diag.profile_applies
            << " idr=" << diag.idr_requests
            << " availability=" << stream_quality::availability_name(action.availability)
            << " tier=" << stream_quality::tier_name(action.tier)
            << " scenario=" << stream_quality::scenario_name(action.scenario)
            << " decisionReason=" << stream_quality_decision_reason_name(action)
            << " requestedScale=" << action.resolution_scale_percent << "%"
            << " actualScale=" << action.actual_scale_percent << "%"
            << " runtimeScaleMode=" << runtime_scale_mode_name(action)
            << " current(encoding/fps/fec/total/pacing)="
            << session->stream_quality_controller.current_bitrate_kbps() << "Kbps/"
            << session->stream_quality_controller.current_fps() << "fps/"
            << session->stream_quality_controller.current_fec_percentage() << "%/"
            << session->current_total_bitrate.load(std::memory_order_relaxed) << "Kbps/"
            << session->pacing_total_bitrate.load(std::memory_order_relaxed) << "Kbps";
    BOOST_LOG(info) << message.str();

    diag = {};
    diag.window_started = now;
  }

  static void
  apply_stream_quality_action(session_t *session, const stream_quality::action_t &action, const char *source) {
    if (!session || !action.changed) {
      return;
    }
    if (!adaptive_controller_active(session, source)) {
      return;
    }

    const auto target_fec_percentage = std::clamp(action.fec_percentage, 0, 100);
    const auto target_encoding_bitrate = action.target_bitrate_kbps;
    const auto target_fps = action.target_fps;

    const auto now = std::chrono::steady_clock::now();
    const bool resync_guard =
      session->stream_quality_resync_guard_until.time_since_epoch().count() != 0 &&
      now < session->stream_quality_resync_guard_until;
    const bool resync_guard_safety_apply =
      resync_guard &&
      stream_quality_resync_guard_allows_safety_apply(action,
                                                session->last_applied_stream_quality_bitrate,
                                                session->last_applied_stream_quality_fec);
    const bool dynamic_apply_blocked = resync_guard && !resync_guard_safety_apply;
    const auto bitrate_delta = std::abs(target_encoding_bitrate - session->last_applied_stream_quality_bitrate);
    const auto bitrate_threshold = std::max(250, std::max(target_encoding_bitrate, session->last_applied_stream_quality_bitrate) / 50);
    const bool startup_settle =
      session->stream_quality_startup_settle_until.time_since_epoch().count() != 0 &&
      now < session->stream_quality_startup_settle_until;
    const bool bitrate_probe_up =
      session->last_applied_stream_quality_bitrate > 0 &&
      target_encoding_bitrate > session->last_applied_stream_quality_bitrate;
    const bool bitrate_drop =
      session->last_applied_stream_quality_bitrate > 0 &&
      target_encoding_bitrate < session->last_applied_stream_quality_bitrate;
    const bool continuity_pressure =
      action.state == stream_quality::state_e::constrained ||
      action.state == stream_quality::state_e::crisis ||
      action.pressures.render >= 0.25 ||
      action.pressures.delay_congestion >= 0.25 ||
      action.pressures.burst_loss >= 0.25 ||
      action.pressures.random_loss >= 0.25;
    if (!dynamic_apply_blocked) {
      update_dynamic_clarity_intent(session, target_encoding_bitrate, target_fps);
    }
    const auto bitrate_apply_cooldown = startup_settle ?
                                          (bitrate_probe_up ? 1800ms : (bitrate_drop && continuity_pressure ? 180ms : 500ms)) :
                                          (bitrate_probe_up ? 900ms : (bitrate_drop && continuity_pressure ? 180ms : 350ms));
    const bool fec_increase =
      session->last_applied_stream_quality_fec >= 0 &&
      target_fec_percentage > session->last_applied_stream_quality_fec;
    const auto fec_apply_cooldown = fec_increase && continuity_pressure ?
                                      300ms :
                                      (startup_settle ? 1800ms : 900ms);
    const bool apply_bitrate = !dynamic_apply_blocked &&
                               target_encoding_bitrate > 0 &&
                               (session->last_applied_stream_quality_bitrate <= 0 ||
                                (bitrate_delta >= bitrate_threshold &&
                                 (session->last_stream_quality_bitrate_apply.time_since_epoch().count() == 0 ||
                                  now - session->last_stream_quality_bitrate_apply >= bitrate_apply_cooldown)));
    const bool apply_fec = !dynamic_apply_blocked &&
                           target_fec_percentage >= 0 &&
                           (session->last_applied_stream_quality_fec < 0 ||
                            (target_fec_percentage != session->last_applied_stream_quality_fec &&
                             (session->last_stream_quality_fec_apply.time_since_epoch().count() == 0 ||
                              now - session->last_stream_quality_fec_apply >= fec_apply_cooldown)));
    const auto last_fps = session->last_applied_stream_quality_fps;
    const auto fps_elapsed_ms =
      session->last_stream_quality_fps_apply.time_since_epoch().count() == 0 ?
        -1 :
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - session->last_stream_quality_fps_apply).count());
    const auto fps_decision = stream_quality::runtime_fps_apply_decision(last_fps,
                                                                   target_fps,
                                                                   fps_elapsed_ms);
    const bool fps_target_changed = fps_decision.target_changed;
    const bool startup_fps_down_allowed =
      action.pressures.random_loss >= 0.90 ||
      action.pressures.burst_loss >= 0.90 ||
      action.pressures.delay_congestion >= 0.90 ||
      action.pressures.render >= 0.95;
    const bool startup_fps_down_suppressed =
      startup_settle &&
      last_fps > 0 &&
      target_fps < last_fps &&
      !startup_fps_down_allowed;
    const bool apply_fps = !dynamic_apply_blocked && fps_decision.apply && !startup_fps_down_suppressed;
    const auto fps_cooldown_suffix = fps_target_changed ?
                                       std::string {" fpsCooldownMs="} + std::to_string(fps_decision.cooldown_ms) :
                                       std::string {};
    const bool profile_target_changed =
      action.resolution_scale_percent != session->last_applied_stream_quality_resolution_scale ||
      action.chroma_sampling_type != session->last_applied_stream_quality_chroma_sampling_type ||
      action.dynamic_range != session->last_applied_stream_quality_dynamic_range;
    const bool profile_deferred = action.profile_tier_deferred && profile_target_changed;
    const bool profile_quality_down =
      profile_target_changed &&
      ((action.resolution_scale_percent > 0 &&
        session->last_applied_stream_quality_resolution_scale > 0 &&
        action.resolution_scale_percent < session->last_applied_stream_quality_resolution_scale) ||
       (action.chroma_sampling_type >= 0 &&
        session->last_applied_stream_quality_chroma_sampling_type > action.chroma_sampling_type) ||
       (action.dynamic_range >= 0 &&
        session->last_applied_stream_quality_dynamic_range > action.dynamic_range));
    const bool profile_quality_up =
      profile_target_changed &&
      ((action.resolution_scale_percent > session->last_applied_stream_quality_resolution_scale &&
        session->last_applied_stream_quality_resolution_scale > 0) ||
       (action.chroma_sampling_type >= 0 &&
        session->last_applied_stream_quality_chroma_sampling_type >= 0 &&
        action.chroma_sampling_type > session->last_applied_stream_quality_chroma_sampling_type) ||
       (action.dynamic_range >= 0 &&
        action.dynamic_range > session->last_applied_stream_quality_dynamic_range));
    const bool profile_fast_recovery =
      profile_quality_up &&
      (action.state == stream_quality::state_e::healthy ||
       action.state == stream_quality::state_e::recovering) &&
      action.pressures.render < 0.20 &&
      action.pressures.delay_congestion < 0.20 &&
      action.pressures.burst_loss < 0.18 &&
      action.pressures.random_loss < 0.18;
    const auto profile_apply_cooldown = profile_quality_down && continuity_pressure ?
                                          250ms :
                                        profile_fast_recovery ?
                                          700ms :
                                          1500ms;
    const bool apply_profile = !dynamic_apply_blocked &&
                               action.profile_tier_supported &&
                               action.profile_tier_changed &&
                               profile_target_changed &&
                               (session->last_stream_quality_profile_apply.time_since_epoch().count() == 0 ||
                                now - session->last_stream_quality_profile_apply >= profile_apply_cooldown);
    const auto target_total_bitrate = total_video_bitrate_from_encoding_bitrate(target_encoding_bitrate,
                                                                                target_fec_percentage);
    const auto pacing_total_bitrate = std::max(action.pacing_bitrate_kbps, target_total_bitrate);
    if (!dynamic_apply_blocked) {
      session->current_total_bitrate.store(target_total_bitrate, std::memory_order_relaxed);
      session->current_fec_percentage.store(target_fec_percentage, std::memory_order_relaxed);
      session->pacing_total_bitrate.store(pacing_total_bitrate, std::memory_order_relaxed);
    }
    session->stream_quality_diag.bitrate_applies += apply_bitrate ? 1U : 0U;
    session->stream_quality_diag.fps_applies += apply_fps ? 1U : 0U;
    session->stream_quality_diag.fec_applies += apply_fec ? 1U : 0U;
    session->stream_quality_diag.profile_applies += apply_profile ? 1U : 0U;

    if (apply_fec) {
      video::dynamic_param_t fec_param;
      fec_param.type = video::dynamic_param_type_e::FEC_PERCENTAGE;
      fec_param.value.int_value = target_fec_percentage;
      fec_param.valid = true;
      session->video.dynamic_param_change_events->raise(fec_param);
      session->last_applied_stream_quality_fec = target_fec_percentage;
      session->last_stream_quality_fec_apply = now;
    }

    if (apply_bitrate) {
      video::dynamic_param_t bitrate_param;
      bitrate_param.type = video::dynamic_param_type_e::BITRATE;
      bitrate_param.value.int_value = target_encoding_bitrate;
      bitrate_param.valid = true;
      session->video.dynamic_param_change_events->raise(bitrate_param);
      session->last_applied_stream_quality_bitrate = target_encoding_bitrate;
      session->last_stream_quality_bitrate_apply = now;
    }

    if (apply_fps) {
      video::dynamic_param_t fps_param;
      fps_param.type = video::dynamic_param_type_e::FPS;
      fps_param.value.float_value = static_cast<float>(target_fps);
      fps_param.valid = true;
      session->video.dynamic_param_change_events->raise(fps_param);
      session->last_applied_stream_quality_fps = target_fps;
      session->last_stream_quality_fps_apply = now;
    }

    std::string profile_runtime_scale_suffix;
    if (apply_profile) {
      const bool resolution_scale_changed =
        action.resolution_scale_percent > 0 &&
        action.resolution_scale_percent != session->last_applied_stream_quality_resolution_scale;
      if (resolution_scale_changed) {
        const auto runtime_resolution = runtime_profile_resolution_for_scale(session->config.monitor.width,
                                                                             session->config.monitor.height,
                                                                             action.resolution_scale_percent);
        const bool resolution_reconfig_enabled = runtime_profile_resolution_reconfig_enabled();
        if (resolution_reconfig_enabled) {
          video::dynamic_param_t resolution_param;
          resolution_param.type = video::dynamic_param_type_e::RESOLUTION;
          resolution_param.value.int_array_value[0] = runtime_resolution.width;
          resolution_param.value.int_array_value[1] = runtime_resolution.height;
          resolution_param.valid = resolution_param.value.int_array_value[0] > 0 &&
                                   resolution_param.value.int_array_value[1] > 0;
          session->video.dynamic_param_change_events->raise(resolution_param);

          BOOST_LOG(info) << "Runtime profile tier encoder scale requested"
                          << " runtime=" << session->identity.runtime_id
                          << " source=" << session->config.monitor.width << "x" << session->config.monitor.height
                          << " target=" << runtime_resolution.width << "x" << runtime_resolution.height
                          << " scale=" << action.resolution_scale_percent << '%'
                          << " softOnly=1";
        }
        else {
          BOOST_LOG(info) << "Runtime profile tier encoder scale deferred"
                          << " runtime=" << session->identity.runtime_id
                          << " source=" << session->config.monitor.width << "x" << session->config.monitor.height
                          << " target=" << runtime_resolution.width << "x" << runtime_resolution.height
                          << " scale=" << action.resolution_scale_percent << '%'
                          << " reason=runtime-resolution-reconfig-disabled"
                          << " softOnly=1";
        }
        profile_runtime_scale_suffix =
          " profileRuntimeScale=" + std::to_string(runtime_resolution.width) +
          "x" + std::to_string(runtime_resolution.height) +
          " softOnly=1";
      }

      if (action.chroma_sampling_type >= 0 &&
          action.chroma_sampling_type != session->last_applied_stream_quality_chroma_sampling_type) {
        video::dynamic_param_t chroma_param;
        chroma_param.type = video::dynamic_param_type_e::CHROMA_SAMPLING;
        chroma_param.value.int_value = action.chroma_sampling_type;
        chroma_param.valid = true;
        session->video.dynamic_param_change_events->raise(chroma_param);
      }

      if (action.dynamic_range >= 0 &&
          action.dynamic_range != session->last_applied_stream_quality_dynamic_range) {
        video::dynamic_param_t dynamic_range_param;
        dynamic_range_param.type = video::dynamic_param_type_e::DYNAMIC_RANGE;
        dynamic_range_param.value.int_value = action.dynamic_range;
        dynamic_range_param.valid = true;
        session->video.dynamic_param_change_events->raise(dynamic_range_param);
      }

      session->last_applied_stream_quality_resolution_scale = action.resolution_scale_percent;
      session->last_applied_stream_quality_chroma_sampling_type = action.chroma_sampling_type;
      session->last_applied_stream_quality_dynamic_range = action.dynamic_range;
      session->last_stream_quality_profile_apply = now;
    }

    if (action.request_idr && !resync_guard) {
      session->video.idr_events->raise(true);
    }

    BOOST_LOG(info) << "Stream-quality controller [" << source << "] runtime="
                    << session->identity.runtime_id
                    << " adaptiveController=auto reason=enabled"
                    << " state=" << stream_quality_state_name(action.state)
                    << " availability=" << stream_quality::availability_name(action.availability)
                    << " reason=" << stream_quality::reason_name(action.reason)
                    << " scenario=" << stream_quality::scenario_name(action.scenario)
                    << " decisionReason=" << stream_quality_decision_reason_name(action)
                    << " requestedCeiling=" << action.requested_ceiling_kbps << " Kbps"
                    << " effectiveCeiling=" << action.effective_ceiling_kbps << " Kbps"
                    << " sustainableEstimate=" << action.sustainable_estimate_kbps << " Kbps"
                    << " encoding=" << target_encoding_bitrate << " Kbps"
                    << " encodingBudget=" << action.encoding_budget_kbps << " Kbps"
                    << " fecBudget=" << action.fec_budget_kbps << " Kbps"
                    << " total=" << target_total_bitrate << " Kbps"
                    << " fps=" << target_fps
                    << " fec=" << target_fec_percentage << "%"
                    << " pressure(random/burst/delay/motion/render/audio/input)="
                    << action.pressures.random_loss << "/"
                    << action.pressures.burst_loss << "/"
                    << action.pressures.delay_congestion << "/"
                    << action.pressures.motion << "/"
                    << action.pressures.render << "/"
                    << action.pressures.audio << "/"
                    << action.pressures.input
                    << " tier=" << stream_quality::tier_name(action.tier)
                    << " legacyTier=" << action.quality_tier
                    << " requestedScale=" << action.resolution_scale_percent << "%"
                    << " actualScale=" << action.actual_scale_percent << "%"
                    << " runtimeScaleMode=" << runtime_scale_mode_name(action)
                    << " chroma=" << action.chroma_sampling_type
                    << " dynamicRange=" << action.dynamic_range
                    << " packetLoss=" << action.packet_loss
                    << " recoveredLoss=" << action.recovered_loss
                    << " unrecoverableLoss=" << action.unrecoverable_loss
                    << " fecEfficiency=" << action.fec_efficiency
                    << " pacing=" << pacing_total_bitrate << " Kbps"
                    << " apply(bitrate=" << (apply_bitrate ? 1 : 0)
                    << ",fps=" << (apply_fps ? 1 : 0)
                    << ",fec=" << (apply_fec ? 1 : 0)
                    << ",profile=" << (apply_profile ? 1 : 0) << ")"
                    << (startup_settle ? " startupSettle=1" : "")
                    << (resync_guard ? " resyncGuard=1" : "")
                    << (resync_guard_safety_apply ? " resyncSafetyApply=1" : "")
                    << (apply_fps ? " fpsApplied=runtime-pacing" : "")
                    << (startup_fps_down_suppressed ? " fpsDeferred=startup-grace" : "")
                    << (fps_decision.deferred ? " fpsDeferred=runtime-pacing-cooldown" : "")
                    << fps_cooldown_suffix
                    << (profile_deferred ? " profileDeferred=runtime-profile-tier-backend-unavailable" : "")
                    << profile_runtime_scale_suffix
                    << (apply_profile && profile_quality_down ? " profileFastDown=1" : "")
                    << (apply_profile && profile_fast_recovery ? " profileFastRecovery=1" : "")
                    << (action.profile_tier_changed && !profile_deferred && !apply_profile ?
                          " profileStable=runtime-profile-tier-no-change" : "")
                    << (action.request_idr && !resync_guard ? " idr=1" : "")
                    << (action.request_idr && resync_guard ? " idrSuppressed=resync-guard" : "")
                    << (action.rfi_limited ? " rfiLimited=1" : "")
                    << (action.congestion_anti_spiral ? " congestionAntiSpiral=1" : "")
                    << " recoveryHold=" << action.recovery_hold_remaining
                    << " rttGradientUs=" << action.rtt_gradient_us
                    << " owdGradientUs=" << action.owd_gradient_us
                    << " owdPressure=" << action.owd_pressure;
  }

  static void
  report_stream_quality_recovery_request(session_t *session, const char *source) {
    if (!session) {
      return;
    }
    if (!adaptive_controller_active(session, source)) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (session->stream_quality_recovery_ready_after.time_since_epoch().count() != 0 &&
        now < session->stream_quality_recovery_ready_after) {
      return;
    }
    if (session->stream_quality_startup_guard_until.time_since_epoch().count() != 0 &&
        now < session->stream_quality_startup_guard_until) {
      if (session->last_stream_quality_startup_guard_log.time_since_epoch().count() == 0 ||
          now - session->last_stream_quality_startup_guard_log >= 1000ms) {
        session->last_stream_quality_startup_guard_log = now;
        BOOST_LOG(info) << "Stream-quality startup guard ignoring synthetic recovery pressure"
                        << " runtime=" << session->identity.runtime_id
                        << " source=" << source;
      }
      return;
    }
    if (!should_synthesize_stream_quality_recovery_feedback(session->config.mlFeatureFlags)) {
      return;
    }

    if (session->last_stream_quality_recovery_feedback.time_since_epoch().count() != 0 &&
        now - session->last_stream_quality_recovery_feedback < 1500ms) {
      return;
    }
    session->last_stream_quality_recovery_feedback = now;

    stream_quality::feedback_t feedback {
      .duration_ms = 250,
      .frames_seen = 1,
      .complete_frames = 0,
      .recovered_frames = 0,
      .unrecoverable_frames = 1,
      .missing_packets = 1,
      .total_packets = 1,
      .received_packets = 0,
      .rtt_variance_ms = 120,
    };
    annotate_feedback_with_host_motion(session, feedback);
    auto action = session->stream_quality_controller.on_feedback(feedback);
    apply_stream_quality_action(session, action, source);
    record_stream_quality_feedback_diag(session, feedback, action, source);
  }

  static bool
  should_forward_client_recovery_request(session_t *session,
                                         const char *source,
                                         std::chrono::steady_clock::time_point &last_request,
                                         std::chrono::milliseconds minimum_interval) {
    if (!session) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_request.time_since_epoch().count() != 0 &&
        now >= last_request &&
        now - last_request < minimum_interval) {
      session->coalesced_client_recovery_requests++;
      if (session->last_client_recovery_coalesce_log.time_since_epoch().count() == 0 ||
          now - session->last_client_recovery_coalesce_log >= 1000ms) {
        session->last_client_recovery_coalesce_log = now;
        BOOST_LOG(info) << "Coalescing client recovery request"
                        << " runtime=" << session->identity.runtime_id
                        << " source=" << source
                        << " suppressed=" << session->coalesced_client_recovery_requests;
      }
      return false;
    }

    if (session->coalesced_client_recovery_requests != 0) {
      BOOST_LOG(info) << "Forwarding client recovery request after coalescing"
                      << " runtime=" << session->identity.runtime_id
                      << " source=" << source
                      << " suppressed=" << session->coalesced_client_recovery_requests;
      session->coalesced_client_recovery_requests = 0;
    }

    last_request = now;
    const bool weak_route_recovery =
      adaptive_controller_active(session, source) &&
      (session->stream_quality_controller.state() == stream_quality::state_e::crisis ||
       session->stream_quality_controller.state() == stream_quality::state_e::constrained);
    const auto guard_until = now + (weak_route_recovery ? 700ms : 1500ms);
    if (session->stream_quality_resync_guard_until.time_since_epoch().count() == 0 ||
        session->stream_quality_resync_guard_until < guard_until) {
      session->stream_quality_resync_guard_until = guard_until;
    }
    return true;
  }

  static bool
  should_hold_startup_stream_quality_feedback(session_t *session,
                                        const stream_quality::feedback_t &feedback,
                                        const char *source) {
    if (!session) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (session->stream_quality_startup_guard_until.time_since_epoch().count() == 0 ||
        now >= session->stream_quality_startup_guard_until) {
      return false;
    }

    const auto total_packets = feedback.total_packets;
    const auto missing_packets = feedback.missing_packets;
    const auto displayed_ratio =
      feedback.frames_seen > 0 ?
        static_cast<double>(feedback.displayed_frames) / static_cast<double>(feedback.frames_seen) :
        1.0;
    const bool loss_impairs_delivery =
      feedback.rfi_requests >= 4 ||
      feedback.waiting_for_rfi_frames >= std::max(4U, feedback.frames_seen / 8U) ||
      (feedback.unrecoverable_frames >= std::max(3U, feedback.frames_seen / 16U) &&
       displayed_ratio < 0.88) ||
      (total_packets >= 120U &&
       missing_packets * 100U >= total_packets * 12U &&
       displayed_ratio < 0.88);
    const bool visual_stale =
      feedback.visual_stale_frames > 0 ||
      feedback.duplicate_frames > 0 ||
      (feedback.frames_seen >= 6 &&
       feedback.displayed_frames <= std::max(1U, feedback.frames_seen / 12U));
    const bool startup_no_display =
      feedback.duration_ms >= 250 &&
      feedback.displayed_frames == 0 &&
      (feedback.frames_seen > 0 ||
       feedback.complete_frames > 0 ||
       feedback.video_bytes > 0 ||
       feedback.input_send_latency_us > 0 ||
       feedback.input_ack_latency_us > 0);
    const bool severe_client_backpressure =
      feedback.decode_queue_depth >= 8 ||
      feedback.render_queue_depth >= 6 ||
      (feedback.frames_seen >= 12 &&
       feedback.late_frames >= std::max(6U, feedback.frames_seen / 4U));
    if (loss_impairs_delivery || visual_stale || startup_no_display || severe_client_backpressure) {
      return false;
    }

    if (session->last_stream_quality_startup_guard_log.time_since_epoch().count() == 0 ||
        now - session->last_stream_quality_startup_guard_log >= 1000ms) {
      session->last_stream_quality_startup_guard_log = now;
      BOOST_LOG(info) << "Stream-quality startup guard holding transient feedback"
                      << " runtime=" << session->identity.runtime_id
                      << " source=" << source
                      << " frames=" << feedback.frames_seen
                      << " complete=" << feedback.complete_frames
                      << " displayed=" << feedback.displayed_frames
                      << " late=" << feedback.late_frames
                      << " dq=" << feedback.decode_queue_depth
                      << " rq=" << feedback.render_queue_depth
                      << " audioUnd=" << feedback.audio_underruns
                      << " lateAudioDrop=" << feedback.late_audio_drops
                      << " rtt=" << feedback.rtt_ms << "ms"
                      << " jitter=" << feedback.rtt_variance_ms << "ms";
    }
    return true;
  }

  /**
   * First part of cipher must be struct of type control_encrypted_t
   *
   * returns empty string_view on failure
   * returns string_view pointing to payload data
   */
  template <std::size_t max_payload_size>
  static inline std::string_view
  encode_control(session_t *session, const std::string_view &plaintext, std::array<std::uint8_t, max_payload_size> &tagged_cipher) {
    static_assert(
      max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size),
      "max_payload_size >= sizeof(control_encrypted_t) + sizeof(crypto::cipher::tag_size)");

    if (session->config.controlProtocolType != 13) {
      return plaintext;
    }

    auto seq = session->control.seq++;

    auto &iv = session->control.outgoing_iv;
    if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'CH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 control stream messages
      // to be sent to each client before the IV repeats.
      iv.resize(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'C';  // Control stream
    }
    else {
      // Nvidia's old style encryption uses a 16-byte IV
      iv.resize(16);

      iv[0] = (std::uint8_t) seq;
    }

    auto packet = (control_encrypted_p) tagged_cipher.data();

    auto bytes = session->control.cipher.encrypt(plaintext, packet->payload(), &iv);
    if (bytes <= 0) {
      BOOST_LOG(error) << "Couldn't encrypt control data"sv;
      return {};
    }

    std::uint16_t packet_length = bytes + crypto::cipher::tag_size + sizeof(control_encrypted_t::seq);

    packet->encryptedHeaderType = util::endian::little(0x0001);
    packet->length = util::endian::little(packet_length);
    packet->seq = util::endian::little(seq);

    return std::string_view { (char *) tagged_cipher.data(), packet_length + sizeof(control_encrypted_t) - sizeof(control_encrypted_t::seq) };
  }

  static inline std::string_view
  encode_control(session_t *session, const std::string_view &plaintext, std::vector<std::uint8_t> &tagged_cipher) {
    if (session->config.controlProtocolType != 13) {
      return plaintext;
    }

    const auto minimum_size =
      sizeof(control_encrypted_t) +
      crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size;
    if (tagged_cipher.size() < minimum_size) {
      return {};
    }

    auto seq = session->control.seq++;

    auto &iv = session->control.outgoing_iv;
    if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
      iv.resize(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'H';
      iv[11] = 'C';
    }
    else {
      iv.resize(16);
      iv[0] = (std::uint8_t) seq;
    }

    auto packet = (control_encrypted_p) tagged_cipher.data();
    auto bytes = session->control.cipher.encrypt(plaintext, packet->payload(), &iv);
    if (bytes <= 0) {
      BOOST_LOG(error) << "Couldn't encrypt control data"sv;
      return {};
    }

    std::uint16_t packet_length = bytes + crypto::cipher::tag_size + sizeof(control_encrypted_t::seq);
    packet->encryptedHeaderType = util::endian::little(0x0001);
    packet->length = util::endian::little(packet_length);
    packet->seq = util::endian::little(seq);

    return std::string_view {
      (char *) tagged_cipher.data(),
      packet_length + sizeof(control_encrypted_t) - sizeof(control_encrypted_t::seq),
    };
  }

  namespace clipboard_payload {
    std::vector<uint8_t>
    build_item_start(uint8_t transfer_flags,
                     uint8_t item_type,
                     std::uint64_t item_id,
                     std::uint64_t content_hash,
                     std::uint32_t total_length,
                     const std::string_view &mime_type,
                     const std::string_view &name) {
      const auto payload_size = alk_sunshine_clipboard_item_start_size(mime_type.size(), name.size());
      std::vector<uint8_t> payload(payload_size);
      std::size_t payload_length = 0;
      if (!alk_sunshine_clipboard_build_item_start(transfer_flags,
                                                   item_type,
                                                   item_id,
                                                   content_hash,
                                                   total_length,
                                                   reinterpret_cast<const std::uint8_t *>(mime_type.data()),
                                                   mime_type.size(),
                                                   reinterpret_cast<const std::uint8_t *>(name.data()),
                                                   name.size(),
                                                   payload.data(),
                                                   payload.size(),
                                                   &payload_length)) {
        return {};
      }
      payload.resize(payload_length);
      return payload;
    }

    std::vector<uint8_t>
    build_item_chunk(std::uint64_t item_id,
                     std::uint32_t chunk_offset,
                     const std::string_view &chunk) {
      std::vector<uint8_t> payload(16u + chunk.size());
      std::size_t payload_length = 0;
      if (!alk_sunshine_clipboard_build_item_chunk(item_id,
                                                   chunk_offset,
                                                   reinterpret_cast<const std::uint8_t *>(chunk.data()),
                                                   chunk.size(),
                                                   payload.data(),
                                                   payload.size(),
                                                   &payload_length)) {
        return {};
      }
      payload.resize(payload_length);
      return payload;
    }

    std::array<std::uint8_t, 1 + sizeof(std::uint64_t)>
    build_item_end(std::uint64_t item_id) {
      std::array<std::uint8_t, 1 + sizeof(std::uint64_t)> payload {};
      std::size_t payload_length = 0;
      (void) alk_sunshine_clipboard_build_item_end(item_id,
                                                   payload.data(),
                                                   payload.size(),
                                                   &payload_length);
      return payload;
    }
  }  // namespace clipboard_payload

  constexpr std::uint32_t max_clipboard_text_size = 1U * 1024U * 1024U;

  bool
  clipboard_transfer_length_valid(uint8_t item_type, std::uint32_t total_length) {
    switch (item_type) {
      case LI_CLIPBOARD_ITEM_TYPE_NONE:
        return total_length == 0;
      case LI_CLIPBOARD_ITEM_TYPE_IMAGE:
        return total_length <= LI_CLIPBOARD_MAX_IMAGE_SIZE;
      case LI_CLIPBOARD_ITEM_TYPE_TEXT:
        return total_length <= max_clipboard_text_size;
      default:
        return false;
    }
  }

  bool
  clipboard_transfer_chunk_next_length(std::uint32_t received_length,
                                       std::uint32_t total_length,
                                       std::uint32_t chunk_offset,
                                       std::uint16_t chunk_length,
                                       std::uint32_t &next_received_length) {
    if (chunk_offset != received_length ||
        chunk_offset > total_length ||
        chunk_length > total_length - chunk_offset) {
      return false;
    }

    next_received_length = received_length + static_cast<std::uint32_t>(chunk_length);
    return true;
  }

  /**
   * @brief 确保麦克风 socket 处于打开状态。
   * 如果 socket 已关闭（上次会话结束时被关闭），则重新 open + bind。
   * @param ctx broadcast 上下文
   * @return true 如果 socket 已打开或成功重新打开
   */
  bool
  ensure_mic_sock_open(broadcast_ctx_t &ctx) {
    if (ctx.mic_sock.is_open()) {
      return true;
    }

    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    auto protocol = address_family == net::IPV4 ? udp::v4() : udp::v6();
    auto mic_port = net::map_port(MIC_STREAM_PORT);
    boost::system::error_code ec;

    ctx.mic_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't re-open Microphone socket: "sv << ec.message();
      return false;
    }

    ctx.mic_sock.bind(udp::endpoint(protocol, mic_port), ec);
    if (ec) {
      BOOST_LOG(error) << "Couldn't re-bind Microphone socket to port ["sv << mic_port << "]: "sv << ec.message();
      ctx.mic_sock.close();
      return false;
    }

    BOOST_LOG(info) << "Microphone socket re-opened on port " << mic_port;
    return true;
  }

  /**
   * @brief 重置麦克风加密状态（清除所有客户端的加密上下文）。
   * 在所有麦克风会话结束或 broadcast 结束时调用。
   */
  void
  reset_mic_encryption(broadcast_ctx_t &ctx) {
    boost::lock_guard<boost::mutex> lg(ctx.mic_cipher_mutex);
    ctx.mic_ciphers.clear();
    ctx.mic_owners.clear();
    ctx.mic_ciphers_by_token.clear();
    ctx.mic_owners_by_token.clear();
  }

  void
  remove_mic_encryption_for_session(broadcast_ctx_t &ctx, const session_t &session) {
    const auto client_ip = session.audio.peer.address().to_string();
    boost::lock_guard<boost::mutex> lg(ctx.mic_cipher_mutex);
    ctx.mic_ciphers.erase(client_ip);
    ctx.mic_owners.erase(client_ip);

    for (auto it = ctx.mic_ciphers_by_token.begin(); it != ctx.mic_ciphers_by_token.end();) {
      if (it->second && it->second->owner.runtime_id == session.identity.runtime_id) {
        it = ctx.mic_ciphers_by_token.erase(it);
      }
      else {
        ++it;
      }
    }

    for (auto it = ctx.mic_owners_by_token.begin(); it != ctx.mic_owners_by_token.end();) {
      if (it->second.runtime_id == session.identity.runtime_id) {
        it = ctx.mic_owners_by_token.erase(it);
      }
      else {
        ++it;
      }
    }
  }

  std::optional<mic_session_token_t>
  mic_session_token_for_session(const session_t &session) {
    if (session.audio.ping_payload.size() != std::tuple_size<mic_session_token_t>::value) {
      return std::nullopt;
    }

    mic_session_token_t token {};
    std::copy_n(session.audio.ping_payload.data(), token.size(), token.begin());
    return token;
  }

  bool
  activate_mic_owner_for_session(session_t &session) {
    if (!session.audio.enable_mic || !session.broadcast_ref) {
      return false;
    }

    auto &ctx = *session.broadcast_ref.get();
    auto owner = ctx.feature_leases.acquire(session_runtime::feature_e::microphone, session);
    const auto client_ip = session.audio.peer.address().to_string();
    const bool should_enable_mic_encryption = (session.config.encryptionFlagsEnabled & SS_ENC_MIC) != 0;

    {
      boost::lock_guard<boost::mutex> lg(ctx.client_name_mutex);
      ctx.client_ip_to_name[client_ip] = session.client_name;
    }

    boost::lock_guard<boost::mutex> lg(ctx.mic_cipher_mutex);
    ctx.mic_ciphers.clear();
    ctx.mic_owners.clear();
    ctx.mic_ciphers_by_token.clear();
    ctx.mic_owners_by_token.clear();

    boost::shared_ptr<broadcast_ctx_t::mic_cipher_ctx_t> cipher_ctx;
    if (should_enable_mic_encryption) {
      cipher_ctx = boost::make_shared<broadcast_ctx_t::mic_cipher_ctx_t>(
        session.audio.cipher.key, session.audio.cipher.padding, session.audio.avRiKeyId, owner);
      ctx.mic_ciphers[client_ip] = cipher_ctx;
    }
    ctx.mic_owners[client_ip] = owner;
    if (auto token = mic_session_token_for_session(session)) {
      ctx.mic_owners_by_token[*token] = owner;
      if (cipher_ctx) {
        ctx.mic_ciphers_by_token[*token] = cipher_ctx;
      }
    }

    BOOST_LOG(info) << "Client " << session.client_name << ": Microphone owner is runtime session "
                    << session.identity.runtime_id << " for " << client_ip
                    << (should_enable_mic_encryption ? " with encryption" : " without encryption");
    return true;
  }

  bool
  promote_mic_owner_if_needed(broadcast_ctx_t &ctx, const session_t *ended_session) {
    auto owner = ctx.feature_leases.owner(session_runtime::feature_e::microphone);
    if (owner &&
        (!ended_session || owner.runtime_id != ended_session->identity.runtime_id) &&
        ctx.feature_leases.validate(owner)) {
      return true;
    }

    auto sessions_lock = ctx.control_server._sessions.lock();
    for (auto *candidate : *ctx.control_server._sessions) {
      if (!candidate ||
          candidate == ended_session ||
          candidate->control_only ||
          !candidate->audio.enable_mic ||
          candidate->state.load(std::memory_order_acquire) != session::state_e::RUNNING) {
        continue;
      }

      return activate_mic_owner_for_session(*candidate);
    }

    if (ended_session) {
      ctx.feature_leases.release(session_runtime::feature_e::microphone, *ended_session);
    }
    reset_mic_encryption(ctx);
    return false;
  }

  /**
   * @brief 为会话设置麦克风接收。
   * 统一处理 mic_sessions_count 递增、socket 打开、加密上下文注册。
   * 如果 socket 打开失败，回滚计数并跳过麦克风启用。
   * @param session 当前会话
   * @return true 如果麦克风设置成功
   */
  bool
  setup_mic_for_session(session_t &session) {
    auto &ctx = *session.broadcast_ref.get();

    const auto mic_count_after_add = ctx.mic_sessions_count.fetch_add(1) + 1;
    BOOST_LOG(info) << "Client " << session.client_name
                    << ": Microphone session requested runtime=" << session.identity.runtime_id
                    << " launchSession=" << session.launch_session_id
                    << " micSessions=" << mic_count_after_add
                    << " peer=" << session.audio.peer.address().to_string();

    // 确保 mic socket 处于打开状态（上次会话结束时可能已关闭）
    if (!ensure_mic_sock_open(ctx)) {
      BOOST_LOG(error) << "Failed to ensure mic socket is open, microphone will be unavailable for " << session.client_name;
      // 回滚计数 — socket 未打开不应算有效 mic 会话
      ctx.mic_sessions_count.fetch_sub(1);
      return false;
    }

    ctx.mic_socket_enabled.store(true);
    BOOST_LOG(info) << "Client " << session.client_name
                    << ": Microphone socket enabled runtime=" << session.identity.runtime_id
                    << " micSessions=" << ctx.mic_sessions_count.load()
                    << " socketOpen=" << ctx.mic_sock.is_open();

    return activate_mic_owner_for_session(session);
  }

  void
  refresh_mic_owner_for_session(session_t &session) {
    activate_mic_owner_for_session(session);
  }

  int
  start_broadcast(broadcast_ctx_t &ctx);
  void
  end_broadcast(broadcast_ctx_t &ctx);

  static auto broadcast_shared = safe::make_shared<broadcast_ctx_t>(start_broadcast, end_broadcast);

  session_t *
  control_server_t::get_session(const net::peer_t peer, uint32_t connect_data) {
    {
      // Fast path - look up existing session by peer
      auto session = _registry.find_by_control_peer(peer);
      if (session) {
        return session;
      }
    }

    if (auto session = _registry.find_waiting_by_connect_data(connect_data)) {
      TUPLE_2D(peer_port, peer_addr, platf::from_sockaddr_ex((sockaddr *) &peer->address.address));

      rtsp_stream::launch_session_clear(session->launch_session_id);

      session->control.peer = peer;
      session->identity.control_generation++;

      auto local_address = platf::from_sockaddr((sockaddr *) &peer->localAddress.address);
      session->localAddress = boost::asio::ip::make_address(local_address);

      BOOST_LOG(debug) << "Initialized new control stream session by connect data match [registry]"
                       << " runtime_id=" << session->identity.runtime_id
                       << " generation=" << session->identity.control_generation;
      BOOST_LOG(debug) << "Control local address ["sv << local_address << ']';
      BOOST_LOG(debug) << "Control peer address ["sv << peer_addr << ':' << peer_port << ']';
      BOOST_LOG(info) << "Alkaid adapter boundary: " << ALK_SUNSHINE_GAMESTREAM_ENET_CONTROL_TRANSPORT_ADAPTER_ID << " active"
                      << " runtime=" << session->identity.runtime_id
                      << " launchSession=" << session->launch_session_id
                      << " peer=" << peer_addr << ':' << peer_port
	                      << " rtspLaunchAdapter=" << ALK_SUNSHINE_GAMESTREAM_RTSP_HANDSHAKE_ADAPTER_ID << "-detached"
	                      << " clipboardCodec=gamestream-clipboard-payload-codec"
	                      << " microphoneCodec=gamestream-microphone-payload-codec"
	                      << " rescueCodec=gamestream-rescue-payload-codec"
	                      << " leaseCodec=session-control-codec"
	                      << " cursorPlaneCodec=session-control-codec";

      _registry.bind_control_peer(session, peer);
      refresh_mic_owner_for_session(*session);
      auto ptslg = _peer_to_session.lock();
      _peer_to_session->emplace(peer, session);
      return session;
    }

    // Slow path - process new session
    TUPLE_2D(peer_port, peer_addr, platf::from_sockaddr_ex((sockaddr *) &peer->address.address));
    auto lg = _sessions.lock();
    for (auto pos = std::begin(*_sessions); pos != std::end(*_sessions); ++pos) {
      auto session_p = *pos;

      // Skip sessions that are already established
      if (session_p->control.peer) {
        continue;
      }

      // Identify the connection by the unique connect data if the client supports it.
      // Only fall back to IP address matching for clients without session ID support.
      if (session_p->config.mlFeatureFlags & ML_FF_SESSION_ID) {
        if (session_p->control.connect_data != connect_data) {
          continue;
        }
        else {
          BOOST_LOG(debug) << "Initialized new control stream session by connect data match [v2]"sv;
        }
      }
      else {
        if (session_p->control.expected_peer_address != peer_addr) {
          continue;
        }
        else {
          BOOST_LOG(debug) << "Initialized new control stream session by IP address match [v1]"sv;
        }
      }

      // Once the control stream connection is established, RTSP session state can be torn down
      rtsp_stream::launch_session_clear(session_p->launch_session_id);

      session_p->control.peer = peer;
      session_p->identity.control_generation++;

      // Use the local address from the control connection as the source address
      // for other communications to the client. This is necessary to ensure
      // proper routing on multi-homed hosts.
      auto local_address = platf::from_sockaddr((sockaddr *) &peer->localAddress.address);
      session_p->localAddress = boost::asio::ip::make_address(local_address);

      BOOST_LOG(debug) << "Control local address ["sv << local_address << ']';
      BOOST_LOG(debug) << "Control peer address ["sv << peer_addr << ':' << peer_port << ']';
      BOOST_LOG(info) << "Alkaid adapter boundary: " << ALK_SUNSHINE_GAMESTREAM_ENET_CONTROL_TRANSPORT_ADAPTER_ID << " active"
                      << " runtime=" << session_p->identity.runtime_id
                      << " launchSession=" << session_p->launch_session_id
                      << " peer=" << peer_addr << ':' << peer_port
                      << " rtspLaunchAdapter=" << ALK_SUNSHINE_GAMESTREAM_RTSP_HANDSHAKE_ADAPTER_ID << "-detached"
                      << " clipboardCodec=gamestream-clipboard-payload-codec"
                      << " microphoneCodec=gamestream-microphone-payload-codec"
                      << " leaseCodec=session-control-codec"
                      << " cursorPlaneCodec=session-control-codec";

      // Insert this into the map for O(1) lookups in the future
      _registry.bind_control_peer(session_p, peer);
      refresh_mic_owner_for_session(*session_p);
      auto ptslg = _peer_to_session.lock();
      _peer_to_session->emplace(peer, session_p);
      return session_p;
    }

    return nullptr;
  }

  static void
  log_control_peer_diag(session_t *session, net::peer_t peer, const char *reason) {
    if (!session || !peer) {
      BOOST_LOG(info) << "Control peer diag [" << reason << "] unavailable";
      return;
    }

    const auto service_time = peer->host ? peer->host->serviceTime : 0;
    const auto elapsed_since = [service_time](enet_uint32 then) -> enet_uint32 {
      return then != 0 && service_time >= then ? service_time - then : 0;
    };
    const auto until = [service_time](enet_uint32 future) -> enet_uint32 {
      return future != 0 && future >= service_time ? future - service_time : 0;
    };
    const auto last_recv_age = elapsed_since(peer->lastReceiveTime);
    const auto last_send_age = elapsed_since(peer->lastSendTime);
    const auto next_timeout_in = until(peer->nextTimeout);
    const auto outbound_queue_depth = static_cast<std::uint32_t>(enet_list_size(&peer->outgoingCommands));
    const auto send_reliable_queue_depth = static_cast<std::uint32_t>(enet_list_size(&peer->outgoingSendReliableCommands));
    const auto sent_reliable_queue_depth = static_cast<std::uint32_t>(enet_list_size(&peer->sentReliableCommands));
    const auto packet_loss_ppm = static_cast<std::uint32_t>(
      std::min<double>(
        1000000.0,
        static_cast<double>(peer->packetLoss) /
          static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE) * 1000000.0));

    AlkControlPathHealthSample health_sample;
    alk_control_path_health_sample_init(&health_sample);
    health_sample.rtt_ms = peer->roundTripTime;
    health_sample.rtt_variance_ms = peer->roundTripTimeVariance;
    health_sample.packet_loss_ppm = packet_loss_ppm;
    health_sample.last_recv_age_ms = last_recv_age;
    health_sample.next_timeout_ms = next_timeout_in;
    health_sample.outbound_queue_depth = outbound_queue_depth;
    health_sample.send_reliable_queue_depth = send_reliable_queue_depth;
    health_sample.sent_reliable_queue_depth = sent_reliable_queue_depth;
    health_sample.waiting_queue_depth = static_cast<std::uint32_t>(
      std::min<std::size_t>(peer->totalWaitingData, std::numeric_limits<std::uint32_t>::max()));
    health_sample.reliable_in_transit = peer->reliableDataInTransit;
    health_sample.unexpected_disconnect = reason && std::string_view { reason }.find("disconnect") != std::string_view::npos;
    health_sample.reconnecting = session->state.load(std::memory_order_relaxed) == session::state_e::STARTING;

    AlkControlPathHealthDecision health_decision;
    alk_control_path_health_decision_init(&health_decision);
    alk_control_path_health_evaluate(&health_sample, &health_decision);

    BOOST_LOG(info) << "Control peer diag [" << reason << "] runtime=" << session->identity.runtime_id
                    << " state=" << peer->state
                    << " health=" << alk_control_path_health_state_name(health_decision.state)
                    << " healthReason=0x" << std::hex << health_decision.reason_flags << std::dec
                    << " healthPpm=" << health_decision.health_ppm
                    << " rescue=" << (health_decision.rescue_recommended ? 1 : 0)
                    << " reconnect=" << (health_decision.reconnect_recommended ? 1 : 0)
                    << " rtt=" << peer->roundTripTime << "ms"
                    << " loss=" << (static_cast<double>(peer->packetLoss) /
                                    static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE) * 100.0)
                    << "%"
                    << " lastRecvAge=" << last_recv_age << "ms"
                    << " lastSendAge=" << last_send_age << "ms"
                    << " nextTimeoutIn=" << next_timeout_in << "ms"
                    << " timeout(limit/min/max)=" << peer->timeoutLimit << "/"
                    << peer->timeoutMinimum << "/" << peer->timeoutMaximum
                    << " queues(out/sendRel/sentRel/wait/reliableTransit)="
                    << outbound_queue_depth << "/"
                    << send_reliable_queue_depth << "/"
                    << sent_reliable_queue_depth << "/"
                    << peer->totalWaitingData << "/"
                    << peer->reliableDataInTransit
                    << " rx=" << session->control.rx_events
                    << " encRx=" << session->control.encrypted_rx_events
                    << " decRx=" << session->control.decrypted_rx_events
                    << " decFail=" << session->control.decrypt_failures
                    << " unencryptedDrop=" << session->control.unencrypted_drops
                    << " unknown=" << session->control.unknown_packets
                    << " lastWire=0x" << std::hex << session->control.last_wire_type
                    << " lastDec=0x" << session->control.last_decrypted_type
                    << std::dec
                    << " lastSeq=" << session->control.last_encrypted_seq
                    << " lastPayload=" << session->control.last_payload_size;
  }

  /**
   * @brief Call the handler for a given control stream message.
   * @param type The message type.
   * @param session The session the message was received on.
   * @param payload The payload of the message.
   * @param reinjected `true` if this message is being reprocessed after decryption.
   */
  void
  control_server_t::call(std::uint16_t type, session_t *session, const std::string_view &payload, bool reinjected) {
    // If we are using the encrypted control stream protocol, drop any messages that come off the wire unencrypted
    if (session->config.controlProtocolType == 13 && !reinjected && type != packetTypes[IDX_ENCRYPTED]) {
      session->control.unencrypted_drops++;
      BOOST_LOG(error) << "Dropping unencrypted message on encrypted control stream runtime="sv
                       << session->identity.runtime_id
                       << " type="sv << util::hex(type).to_string_view()
                       << " drops="sv << session->control.unencrypted_drops;
      return;
    }

    auto cb = _map_type_cb.find(type);
    if (cb == std::end(_map_type_cb)) {
      session->control.unknown_packets++;
      BOOST_LOG(debug)
        << "type [Unknown] { "sv << util::hex(type).to_string_view() << " }"sv << std::endl
        << "---data---"sv << std::endl
        << util::hex_vec(payload) << std::endl
        << "---end data---"sv;
    }
    else {
      cb->second(session, payload);
    }
  }

  static void
  record_control_input_received(session_t *session, std::size_t payload_size, const char *path) {
    if (!session) {
      return;
    }

    session->control.input_rx_events++;
    const auto now = std::chrono::steady_clock::now();
    session->last_control_input_received = now;
    if (session->control.last_input_diag_log.time_since_epoch().count() == 0 ||
        now - session->control.last_input_diag_log >= 1000ms) {
      session->control.last_input_diag_log = now;
      BOOST_LOG(info) << "Control input packet received runtime="sv
                      << session->identity.runtime_id
                      << " path="sv << path
                      << " payloadBytes="sv << payload_size
                      << " inputRx="sv << session->control.input_rx_events
                      << " controlRx="sv << session->control.rx_events
                      << " decrypted="sv << session->control.decrypted_rx_events;
    }
  }

  static bool
  is_control_input_recent(void *channel_data) {
    auto *session = static_cast<session_t *>(channel_data);
    if (!session || session->last_control_input_received.time_since_epoch().count() == 0) {
      return false;
    }

    return std::chrono::steady_clock::now() - session->last_control_input_received <= 350ms;
  }

  static std::chrono::duration<double, std::milli>
  startup_video_pacing_interval(void *channel_data,
                                int target_fps,
                                std::chrono::duration<double, std::milli> target_interval) {
    auto *session = static_cast<session_t *>(channel_data);
    if (!session ||
        session->video_startup_pacing_until.time_since_epoch().count() == 0 ||
        std::chrono::steady_clock::now() >= session->video_startup_pacing_until) {
      return target_interval;
    }

    if (session->stream_quality_controller.state() == stream_quality::state_e::healthy) {
      return target_interval;
    }

    const auto startup_fps = std::clamp(std::min(target_fps, 90), 30, std::max(target_fps, 30));
    return std::chrono::duration<double, std::milli> { 1000.0 / static_cast<double>(startup_fps) };
  }

  void
  control_server_t::iterate(std::chrono::milliseconds timeout) {
    constexpr int max_events_per_iter = 256;

    for (int events_processed = 0; events_processed < max_events_per_iter; ++events_processed) {
      ENetEvent event;
      auto res = enet_host_service(_host.get(), &event, events_processed == 0 ? timeout.count() : 0);

      if (res <= 0) {
        return;
      }

      auto session = get_session(event.peer, event.data);
      if (!session) {
        BOOST_LOG(warning) << "Rejected connection from ["sv << platf::from_sockaddr((sockaddr *) &event.peer->address.address) << "]: it's not properly set up"sv;
        enet_peer_disconnect_now(event.peer, 0);

        continue;
      }

      session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          net::packet_t packet { event.packet };

          auto type = *(std::uint16_t *) packet->data;
          std::string_view payload { (char *) packet->data + sizeof(type), packet->dataLength - sizeof(type) };
          session->control.rx_events++;
          session->control.last_wire_type = type;
          session->control.last_payload_size = payload.size();

          const auto now = std::chrono::steady_clock::now();
          if (session->control.last_rx_diag_log.time_since_epoch().count() == 0 ||
              now - session->control.last_rx_diag_log >= 1000ms) {
            session->control.last_rx_diag_log = now;
            log_control_peer_diag(session, event.peer, "rx");
          }

          call(type, session, payload, false);
          enet_host_flush(_host.get());
        } break;
        case ENET_EVENT_TYPE_CONNECT:
          enet_peer_timeout(event.peer, 2, 10000, 10000);
          enet_peer_ping_interval(event.peer, 500);
          BOOST_LOG(info) << "CLIENT CONNECTED runtime="sv << session->identity.runtime_id
                          << " peer="sv << platf::from_sockaddr((sockaddr *) &event.peer->address.address)
                          << " channels="sv << event.peer->channelCount
                          << " timeout(limit/min/max)="sv << event.peer->timeoutLimit << "/"
                          << event.peer->timeoutMinimum << "/" << event.peer->timeoutMaximum
                          << " pingInterval="sv << event.peer->pingInterval << "ms";
          log_control_peer_diag(session, event.peer, "connect");
          enet_host_flush(_host.get());
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          {
            log_control_peer_diag(session, event.peer, "disconnect-event");
            const auto service_time = _host && _host.get() ? _host.get()->serviceTime : 0;
            const auto last_recv_age = event.peer->lastReceiveTime != 0 ?
                                         service_time - event.peer->lastReceiveTime :
                                         0;
            const auto last_send_age = event.peer->lastSendTime != 0 ?
                                         service_time - event.peer->lastSendTime :
                                         0;
            BOOST_LOG(info) << "CLIENT DISCONNECTED runtime="sv << session->identity.runtime_id
                            << " eventData="sv << event.data
                            << " peerState="sv << event.peer->state
                            << " roundTrip="sv << event.peer->roundTripTime << "ms"
                            << " packetLoss="sv
                            << (static_cast<double>(event.peer->packetLoss) /
                                static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE) * 100.0)
                            << "%"
                            << " lastRecvAge="sv << last_recv_age << "ms"
                            << " lastSendAge="sv << last_send_age << "ms";
          }
          // No more clients to send video data to ^_^
          if (session->state == session::state_e::RUNNING) {
            session::stop(*session);
          }
          enet_host_flush(_host.get());
          break;
        case ENET_EVENT_TYPE_NONE:
          break;
      }
    }
  }

  namespace fec {
    using rs_t = util::safe_ptr<reed_solomon, [](reed_solomon *rs) { reed_solomon_release(rs); }>;

    struct fec_t {
      size_t data_shards;
      size_t nr_shards;
      size_t percentage;

      size_t blocksize;
      size_t prefixsize;
      util::buffer_t<char> shards;
      util::buffer_t<char> headers;
      util::buffer_t<uint8_t *> shards_p;

      std::vector<platf::buffer_descriptor_t> payload_buffers;

      char *
      data(size_t el) {
        return (char *) shards_p[el];
      }

      char *
      prefix(size_t el) {
        return prefixsize ? &headers[el * prefixsize] : nullptr;
      }

      size_t
      size() const {
        return nr_shards;
      }
    };

    static fec_t
    encode(const std::string_view &payload, size_t blocksize, size_t fecpercentage, size_t minparityshards, size_t prefixsize) {
      auto payload_size = payload.size();

      auto pad = payload_size % blocksize != 0;

      auto aligned_data_shards = payload_size / blocksize;
      auto data_shards = aligned_data_shards + (pad ? 1 : 0);
      auto parity_shards = (data_shards * fecpercentage + 99) / 100;

      // increase the FEC percentage for this frame if the parity shard minimum is not met
      if (parity_shards < minparityshards && fecpercentage != 0) {
        parity_shards = minparityshards;
        fecpercentage = (100 * parity_shards) / data_shards;

        BOOST_LOG(verbose) << "Increasing FEC percentage to "sv << fecpercentage << " to meet parity shard minimum"sv << std::endl;
      }

      auto nr_shards = data_shards + parity_shards;

      // If we need to store a zero-padded data shard, allocate that first to
      // to keep the shards in order and reduce buffer fragmentation
      auto parity_shard_offset = pad ? 1 : 0;
      util::buffer_t<char> shards { (parity_shard_offset + parity_shards) * blocksize };
      util::buffer_t<uint8_t *> shards_p { nr_shards };
      std::vector<platf::buffer_descriptor_t> payload_buffers;
      payload_buffers.reserve(2);

      // Point into the payload buffer for all except the final padded data shard
      auto next = std::begin(payload);
      for (auto x = 0; x < aligned_data_shards; ++x) {
        shards_p[x] = (uint8_t *) next;
        next += blocksize;
      }
      payload_buffers.emplace_back(std::begin(payload), aligned_data_shards * blocksize);

      // If the last data shard needs to be zero-padded, we must use the shards buffer
      if (pad) {
        shards_p[aligned_data_shards] = (uint8_t *) &shards[0];

        // GCC doesn't figure out that std::copy_n() can be replaced with memcpy() here
        // and ends up compiling a horribly slow element-by-element copy loop, so we
        // help it by using memcpy()/memset() directly.
        auto copy_len = std::min<size_t>(blocksize, std::end(payload) - next);
        std::memcpy(shards_p[aligned_data_shards], next, copy_len);
        if (copy_len < blocksize) {
          // Zero any additional space after the end of the payload
          std::memset(shards_p[aligned_data_shards] + copy_len, 0, blocksize - copy_len);
        }
      }

      // Add a payload buffer describing the shard buffer
      payload_buffers.emplace_back(std::begin(shards), shards.size());

      if (fecpercentage != 0) {
        // Point into our allocated buffer for the parity shards
        for (auto x = 0; x < parity_shards; ++x) {
          shards_p[data_shards + x] = (uint8_t *) &shards[(parity_shard_offset + x) * blocksize];
        }

        // packets = parity_shards + data_shards
        rs_t rs { reed_solomon_new(data_shards, parity_shards) };

        reed_solomon_encode(rs.get(), shards_p.begin(), nr_shards, blocksize);
      }

      return {
        data_shards,
        nr_shards,
        fecpercentage,
        blocksize,
        prefixsize,
        std::move(shards),
        util::buffer_t<char> { nr_shards * prefixsize },
        std::move(shards_p),
        std::move(payload_buffers),
      };
    }
  }  // namespace fec

  /**
   * @brief Combines two buffers and inserts new buffers at each slice boundary of the result.
   * @param insert_size The number of bytes to insert.
   * @param slice_size The number of bytes between insertions.
   * @param data1 The first data buffer.
   * @param data2 The second data buffer.
   */
  std::vector<uint8_t>
  concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    auto data_size = data1.size() + data2.size();
    auto pad = data_size % slice_size != 0;
    auto elements = data_size / slice_size + (pad ? 1 : 0);

    std::vector<uint8_t> result;
    result.resize(elements * insert_size + data_size);

    auto next = std::begin(data1);
    auto end = std::end(data1);
    for (auto x = 0; x < elements; ++x) {
      void *p = &result[x * (insert_size + slice_size)];

      // For the last iteration, only copy to the end of the data
      if (x == elements - 1) {
        slice_size = data_size - (x * slice_size);
      }

      // Test if this slice will extend into the next buffer
      if (next + slice_size > end) {
        // Copy the first portion from the first buffer
        auto copy_len = end - next;
        std::copy(next, end, (char *) p + insert_size);

        // Copy the remaining portion from the second buffer
        next = std::begin(data2);
        end = std::end(data2);
        std::copy(next, next + (slice_size - copy_len), (char *) p + copy_len + insert_size);
        next += slice_size - copy_len;
      }
      else {
        std::copy(next, next + slice_size, (char *) p + insert_size);
        next += slice_size;
      }
    }

    return result;
  }

  std::vector<uint8_t>
  replace(const std::string_view &original, const std::string_view &old, const std::string_view &_new) {
    std::vector<uint8_t> replaced;
    replaced.reserve(original.size() + _new.size() - old.size());

    auto begin = std::begin(original);
    auto end = std::end(original);
    auto next = std::search(begin, end, std::begin(old), std::end(old));

    std::copy(begin, next, std::back_inserter(replaced));
    if (next != end) {
      std::copy(std::begin(_new), std::end(_new), std::back_inserter(replaced));
      std::copy(next + old.size(), end, std::back_inserter(replaced));
    }

    return replaced;
  }

  /**
   * @brief Pass gamepad feedback data back to the client.
   * @param session The session object.
   * @param msg The message to pass.
   * @return 0 on success.
   */
  int
  send_feedback_msg(session_t *session, platf::gamepad_feedback_msg_t &msg) {
    if (!session->control.peer) {
      const auto now = std::chrono::steady_clock::now();
      const bool is_motion_feedback = msg.type == platf::gamepad_feedback_e::set_motion_event_state;
      if (session->last_gamepad_feedback_wait_log.time_since_epoch().count() == 0 ||
          now - session->last_gamepad_feedback_wait_log >= 2s) {
        session->last_gamepad_feedback_wait_log = now;
        if (is_motion_feedback) {
          BOOST_LOG(debug) << "Gamepad feedback skipped: runtime=" << session->identity.runtime_id
                           << " reason=waiting-control-peer type=motion";
        }
        else {
          BOOST_LOG(warning) << "Gamepad feedback skipped: runtime=" << session->identity.runtime_id
                             << " reason=waiting-control-peer type=" << static_cast<int>(msg.type);
        }
      }
      return -1;
    }

    std::string payload;
    if (msg.type == platf::gamepad_feedback_e::rumble) {
      control_rumble_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble;

      plaintext.useless = 0xC0FFEE;
      plaintext.id = util::endian::little(msg.id);
      plaintext.lowfreq = util::endian::little(data.lowfreq);
      plaintext.highfreq = util::endian::little(data.highfreq);

      BOOST_LOG(verbose) << "Rumble: "sv << msg.id << " :: "sv << util::hex(data.lowfreq).to_string_view() << " :: "sv << util::hex(data.highfreq).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::rumble_triggers) {
      control_rumble_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_RUMBLE_TRIGGER_DATA];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rumble_triggers;

      plaintext.id = util::endian::little(msg.id);
      plaintext.left = util::endian::little(data.left_trigger);
      plaintext.right = util::endian::little(data.right_trigger);

      BOOST_LOG(verbose) << "Rumble triggers: "sv << msg.id << " :: "sv << util::hex(data.left_trigger).to_string_view() << " :: "sv << util::hex(data.right_trigger).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::set_motion_event_state) {
      control_set_motion_event_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_MOTION_EVENT];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.motion_event_state;

      plaintext.id = util::endian::little(msg.id);
      plaintext.reportrate = util::endian::little(data.report_rate);
      plaintext.type = data.motion_type;

      BOOST_LOG(verbose) << "Motion event state: "sv << msg.id << " :: "sv << util::hex(data.report_rate).to_string_view() << " :: "sv << util::hex(data.motion_type).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::set_rgb_led) {
      control_set_rgb_led_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_RGB_LED];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      auto &data = msg.data.rgb_led;

      plaintext.id = util::endian::little(msg.id);
      plaintext.r = data.r;
      plaintext.g = data.g;
      plaintext.b = data.b;

      BOOST_LOG(verbose) << "RGB: "sv << msg.id << " :: "sv << util::hex(data.r).to_string_view() << util::hex(data.g).to_string_view() << util::hex(data.b).to_string_view();
      std::array<std::uint8_t,
        sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else if (msg.type == platf::gamepad_feedback_e::set_adaptive_triggers) {
      control_adaptive_triggers_t plaintext;
      plaintext.header.type = packetTypes[IDX_SET_ADAPTIVE_TRIGGERS];
      plaintext.header.payloadLength = sizeof(plaintext) - sizeof(control_header_v2);

      plaintext.id = util::endian::little(msg.id);
      plaintext.event_flags = msg.data.adaptive_triggers.event_flags;
      plaintext.type_left = msg.data.adaptive_triggers.type_left;
      std::ranges::copy(msg.data.adaptive_triggers.left, plaintext.left);
      plaintext.type_right = msg.data.adaptive_triggers.type_right;
      std::ranges::copy(msg.data.adaptive_triggers.right, plaintext.right);

      std::array<std::uint8_t, sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
        encrypted_payload;

      payload = encode_control(session, util::view(plaintext), encrypted_payload);
    }
    else {
      BOOST_LOG(error) << "Unknown gamepad feedback message type"sv;
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      const auto now = std::chrono::steady_clock::now();
      if (session->last_gamepad_feedback_fail_log.time_since_epoch().count() == 0 ||
          now - session->last_gamepad_feedback_fail_log >= 2s) {
        session->last_gamepad_feedback_fail_log = now;
        BOOST_LOG(warning) << "Gamepad feedback send failed: runtime=" << session->identity.runtime_id
                           << " peer=["sv << addr << ':' << port << ']';
      }

      return -1;
    }

    return 0;
  }

  int
  send_hdr_mode(session_t *session, video::hdr_info_t hdr_info) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send HDR mode, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    control_hdr_mode_t plaintext {};
    plaintext.header.type = packetTypes[IDX_HDR_MODE];
    plaintext.header.payloadLength = sizeof(control_hdr_mode_t) - sizeof(control_header_v2);

    plaintext.enabled = hdr_info->enabled;
    plaintext.metadata = hdr_info->metadata;

    std::array<std::uint8_t,
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send HDR mode to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(debug) << "Sent HDR mode: " << hdr_info->enabled;
    return 0;
  }

  int
  send_resolution_change(session_t *session, std::uint32_t width, std::uint32_t height) {
    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send resolution change, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
      return -1;
    }

    control_resolution_change_t plaintext {};
    plaintext.header.type = packetTypes[IDX_RESOLUTION_CHANGE];
    plaintext.header.payloadLength = sizeof(control_resolution_change_t) - sizeof(control_header_v2);

    plaintext.width = util::endian::little(width);
    plaintext.height = util::endian::little(height);

    std::array<std::uint8_t,
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto payload = encode_control(session, util::view(plaintext), encrypted_payload);
    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send resolution change to ["sv << addr << ':' << port << ']';

      return -1;
    }

    BOOST_LOG(debug) << "Sent resolution change: " << width << "x" << height;
    return 0;
  }

  bool
  client_supports_cursor_plane(session_t *session) {
    return session != nullptr &&
           (session->config.mlFeatureFlags2 & static_cast<std::uint64_t>(ML_FF2_CURSOR_PLANE)) != 0;
  }

  bool
  client_supports_rescue_control(session_t *session) {
    return session != nullptr &&
           (session->config.mlFeatureFlags2 & static_cast<std::uint64_t>(ML_FF2_RESCUE_CONTROL)) != 0;
  }

  struct cursor_plane_sample_t {
    bool available { false };
    std::uint32_t cursor_shape_id { 1 };
    std::uint32_t x { 0 };
    std::uint32_t y { 0 };
    std::uint16_t hotspot_x { 0 };
    std::uint16_t hotspot_y { 0 };
    std::uint16_t width { 16 };
    std::uint16_t height { 16 };
    std::uint16_t display_width { 16 };
    std::uint16_t display_height { 16 };
    std::uint16_t display_hotspot_x { 0 };
    std::uint16_t display_hotspot_y { 0 };
    std::uint16_t bitmap_width { 0 };
    std::uint16_t bitmap_height { 0 };
    std::uint32_t flags { 0 };
    std::uint32_t host_dpi_scale_ppm { 1000000 };
    std::uint32_t size_source { LI_SESSION_CURSOR_SIZE_SOURCE_UNKNOWN };
    std::uint32_t confidence_ppm { 0 };
    std::uint16_t bitmap_format { 0 };
    std::uint16_t bitmap_stride { 0 };
    std::vector<std::uint8_t> bitmap_bgra;
  };

  cursor_plane_sample_t
  sample_host_cursor_plane(session_t *session) {
    cursor_plane_sample_t sample {};

#ifdef _WIN32
    auto semantic_cursor_shape_id = [](HCURSOR cursor) -> std::uint32_t {
      if (cursor == nullptr) {
        return SS_CURSOR_PLANE_SHAPE_UNKNOWN;
      }
      struct known_cursor_t {
        LPCSTR name;
        std::uint32_t shape_id;
      };
      static const known_cursor_t known[] {
        { IDC_ARROW, SS_CURSOR_PLANE_SHAPE_ARROW },
        { IDC_HAND, SS_CURSOR_PLANE_SHAPE_HAND },
        { IDC_IBEAM, SS_CURSOR_PLANE_SHAPE_IBEAM },
        { IDC_WAIT, SS_CURSOR_PLANE_SHAPE_WAIT },
        { IDC_APPSTARTING, SS_CURSOR_PLANE_SHAPE_WAIT },
        { IDC_CROSS, SS_CURSOR_PLANE_SHAPE_CROSS },
        { IDC_SIZEWE, SS_CURSOR_PLANE_SHAPE_SIZE_WE },
        { IDC_SIZENS, SS_CURSOR_PLANE_SHAPE_SIZE_NS },
        { IDC_SIZENWSE, SS_CURSOR_PLANE_SHAPE_SIZE_NWSE },
        { IDC_SIZENESW, SS_CURSOR_PLANE_SHAPE_SIZE_NESW },
        { IDC_SIZEALL, SS_CURSOR_PLANE_SHAPE_SIZE_ALL },
        { IDC_NO, SS_CURSOR_PLANE_SHAPE_NO },
      };

      for (const auto &candidate : known) {
        if (cursor == LoadCursorA(nullptr, candidate.name)) {
          return candidate.shape_id;
        }
      }

      const auto cursor_handle = reinterpret_cast<std::uintptr_t>(cursor);
      auto hashed = static_cast<std::uint32_t>((cursor_handle >> 4U) ^ (cursor_handle >> 32U) ^ cursor_handle);
      if ((hashed & ~SS_CURSOR_PLANE_SHAPE_CUSTOM_FLAG) == 0) {
        hashed = 1;
      }
      return SS_CURSOR_PLANE_SHAPE_CUSTOM_FLAG | (hashed & ~SS_CURSOR_PLANE_SHAPE_CUSTOM_FLAG);
    };

    CURSORINFO cursor_info {};
    cursor_info.cbSize = sizeof(cursor_info);
    if (!GetCursorInfo(&cursor_info)) {
      return sample;
    }

    POINT point = cursor_info.ptScreenPos;
    if (point.x == 0 && point.y == 0) {
      GetCursorPos(&point);
    }

    const int origin_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int origin_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int fallback_width = std::max(1, GetSystemMetrics(SM_CXCURSOR));
    const int fallback_height = std::max(1, GetSystemMetrics(SM_CYCURSOR));
    const int stream_width = session ? std::max(1, session->config.monitor.width) : std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int stream_height = session ? std::max(1, session->config.monitor.height) : std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    int dpi_x = 96;
    int dpi_y = 96;
    HDC dpi_dc = GetDC(nullptr);
    if (dpi_dc != nullptr) {
      dpi_x = std::max(1, GetDeviceCaps(dpi_dc, LOGPIXELSX));
      dpi_y = std::max(1, GetDeviceCaps(dpi_dc, LOGPIXELSY));
      ReleaseDC(nullptr, dpi_dc);
    }
    const int dpi_scaled_width =
      (dpi_x > 110 && fallback_width <= 40) ?
        std::max(1, (fallback_width * dpi_x + 48) / 96) :
        fallback_width;
    const int dpi_scaled_height =
      (dpi_y > 110 && fallback_height <= 40) ?
        std::max(1, (fallback_height * dpi_y + 48) / 96) :
        fallback_height;

    const bool cursor_visible =
      (cursor_info.flags & CURSOR_SHOWING) != 0 &&
#ifdef CURSOR_SUPPRESSED
      (cursor_info.flags & CURSOR_SUPPRESSED) == 0 &&
#endif
      cursor_info.hCursor != nullptr;
    RECT virtual_rect {
      origin_x,
      origin_y,
      origin_x + std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
      origin_y + std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)),
    };
    RECT clip_rect {};
    const bool clip_available = GetClipCursor(&clip_rect) != FALSE;
    const bool cursor_clipped =
      clip_available &&
      (clip_rect.left > virtual_rect.left + 1 ||
       clip_rect.top > virtual_rect.top + 1 ||
       clip_rect.right < virtual_rect.right - 1 ||
       clip_rect.bottom < virtual_rect.bottom - 1);
    const bool cursor_center_locked =
      cursor_clipped &&
      (clip_rect.right - clip_rect.left <= 4 ||
       clip_rect.bottom - clip_rect.top <= 4);

    sample.available = true;
    sample.flags = cursor_visible ? SS_CURSOR_PLANE_FLAG_VISIBLE : 0;
    if (!cursor_visible) {
      // Hidden host cursor is the common signal for raw/relative game camera
      // input. Tell clients not to draw a local cursor plane and to prefer
      // relative mouse movement while the host remains in this state.
      sample.flags |= SS_CURSOR_PLANE_FLAG_RELATIVE;
    }
    if (!cursor_visible || cursor_center_locked) {
      sample.flags |= SS_CURSOR_PLANE_FLAG_LOCKED;
    }
    const int stream_x = static_cast<int>(point.x - origin_x);
    const int stream_y = static_cast<int>(point.y - origin_y);
    sample.x = static_cast<std::uint32_t>(std::clamp(stream_x, 0, stream_width - 1));
    sample.y = static_cast<std::uint32_t>(std::clamp(stream_y, 0, stream_height - 1));
    sample.width = static_cast<std::uint16_t>(std::clamp(fallback_width, 1, 512));
    sample.height = static_cast<std::uint16_t>(std::clamp(fallback_height, 1, 512));
    sample.display_width = static_cast<std::uint16_t>(std::clamp(std::max(fallback_width, dpi_scaled_width), 1, 512));
    sample.display_height = static_cast<std::uint16_t>(std::clamp(std::max(fallback_height, dpi_scaled_height), 1, 512));
    sample.bitmap_width = sample.width;
    sample.bitmap_height = sample.height;
    sample.host_dpi_scale_ppm = static_cast<std::uint32_t>(
      std::clamp<long long>((static_cast<long long>(std::max(dpi_x, dpi_y)) * 1000000LL + 48LL) / 96LL,
                            250000LL,
                            8000000LL));
    sample.size_source = LI_SESSION_CURSOR_SIZE_SOURCE_SYSTEM_METRIC;
    sample.confidence_ppm = 750000;
    sample.cursor_shape_id = cursor_visible ?
                               semantic_cursor_shape_id(cursor_info.hCursor) :
                               SS_CURSOR_PLANE_SHAPE_UNKNOWN;

    ICONINFO icon_info {};
    if (cursor_visible && GetIconInfo(cursor_info.hCursor, &icon_info)) {
      sample.hotspot_x = static_cast<std::uint16_t>(std::clamp<DWORD>(icon_info.xHotspot, 0, 512));
      sample.hotspot_y = static_cast<std::uint16_t>(std::clamp<DWORD>(icon_info.yHotspot, 0, 512));

      BITMAP bitmap {};
      if (icon_info.hbmColor != nullptr &&
          GetObject(icon_info.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
        sample.width = static_cast<std::uint16_t>(std::clamp<LONG>(bitmap.bmWidth, 1, 512));
        sample.height = static_cast<std::uint16_t>(std::clamp<LONG>(bitmap.bmHeight, 1, 512));
        sample.bitmap_width = sample.width;
        sample.bitmap_height = sample.height;

        // Always prefer the host-provided cursor bitmap when Windows exposes
        // one. Standard Windows cursors are not visually identical to macOS
        // NSCursor fallbacks, and game/special cursors must preserve their real
        // appearance. Semantic shape ids are only a fallback/cache key.
        {
          const int bitmap_width = std::clamp<LONG>(bitmap.bmWidth, 1, 512);
          const int bitmap_height = std::clamp<LONG>(bitmap.bmHeight, 1, 512);
          const int stride = bitmap_width * 4;
          std::vector<std::uint8_t> bgra(static_cast<std::size_t>(stride) *
                                         static_cast<std::size_t>(bitmap_height));

          BITMAPINFO bitmap_info {};
          bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
          bitmap_info.bmiHeader.biWidth = bitmap_width;
          bitmap_info.bmiHeader.biHeight = -bitmap_height;  // top-down DIB
          bitmap_info.bmiHeader.biPlanes = 1;
          bitmap_info.bmiHeader.biBitCount = 32;
          bitmap_info.bmiHeader.biCompression = BI_RGB;

          HDC hdc = GetDC(nullptr);
          const int copied_rows = hdc != nullptr ?
                                    GetDIBits(hdc,
                                              icon_info.hbmColor,
                                              0,
                                              bitmap_height,
                                              bgra.data(),
                                              &bitmap_info,
                                              DIB_RGB_COLORS) :
                                    0;
          if (hdc != nullptr) {
            ReleaseDC(nullptr, hdc);
          }

          if (copied_rows == bitmap_height) {
            bool has_alpha = false;
            for (std::size_t i = 3; i < bgra.size(); i += 4) {
              if (bgra[i] != 0) {
                has_alpha = true;
                break;
              }
            }

            // Some Windows cursor bitmaps use an all-zero alpha channel and
            // rely on the mask bitmap. Preserve transparency for black empty
            // pixels, but make colored pixels visible so custom/game cursors do
            // not disappear on the client.
            if (!has_alpha) {
              for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
                const bool has_color = bgra[i] != 0 || bgra[i + 1] != 0 || bgra[i + 2] != 0;
                bgra[i + 3] = has_color ? 0xFF : 0x00;
              }
            }

            sample.bitmap_format = SS_CURSOR_PLANE_BITMAP_FORMAT_BGRA;
            sample.bitmap_stride = static_cast<std::uint16_t>(stride);
            sample.bitmap_bgra = std::move(bgra);
            sample.flags |= SS_CURSOR_PLANE_FLAG_SHAPE_BITMAP;
            sample.size_source = LI_SESSION_CURSOR_SIZE_SOURCE_ICON_BITMAP;
            sample.confidence_ppm = 900000;
          }
        }
      }
      else if (icon_info.hbmMask != nullptr &&
               GetObject(icon_info.hbmMask, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
        sample.width = static_cast<std::uint16_t>(std::clamp<LONG>(bitmap.bmWidth, 1, 512));
        sample.height = static_cast<std::uint16_t>(std::clamp<LONG>(std::max<LONG>(1, bitmap.bmHeight / 2), 1, 512));
        sample.bitmap_width = sample.width;
        sample.bitmap_height = sample.height;
        sample.size_source = LI_SESSION_CURSOR_SIZE_SOURCE_ICON_BITMAP;
        sample.confidence_ppm = 800000;
      }

      if (icon_info.hbmColor != nullptr) {
        DeleteObject(icon_info.hbmColor);
      }
      if (icon_info.hbmMask != nullptr) {
        DeleteObject(icon_info.hbmMask);
      }
    }
    sample.display_width = static_cast<std::uint16_t>(
      std::clamp<int>(std::max({ static_cast<int>(sample.display_width),
                                 static_cast<int>(sample.bitmap_width),
                                 dpi_scaled_width }),
                      1,
                      512));
    sample.display_height = static_cast<std::uint16_t>(
      std::clamp<int>(std::max({ static_cast<int>(sample.display_height),
                                 static_cast<int>(sample.bitmap_height),
                                 dpi_scaled_height }),
                      1,
                      512));
    sample.display_hotspot_x = sample.hotspot_x;
    sample.display_hotspot_y = sample.hotspot_y;
    if (sample.bitmap_width > 0 && sample.bitmap_height > 0 &&
        (sample.display_width != sample.bitmap_width ||
         sample.display_height != sample.bitmap_height)) {
      sample.display_hotspot_x = static_cast<std::uint16_t>(
        std::clamp<std::uint32_t>(
          (static_cast<std::uint32_t>(sample.hotspot_x) * sample.display_width + sample.bitmap_width / 2U) /
            std::max<std::uint16_t>(1, sample.bitmap_width),
          0,
          sample.display_width));
      sample.display_hotspot_y = static_cast<std::uint16_t>(
        std::clamp<std::uint32_t>(
          (static_cast<std::uint32_t>(sample.hotspot_y) * sample.display_height + sample.bitmap_height / 2U) /
            std::max<std::uint16_t>(1, sample.bitmap_height),
          0,
          sample.display_height));
    }
#else
    (void) session;
#endif

    return sample;
  }

  int
  send_session_control_cursor_plane(session_t *session, const cursor_plane_sample_t &sample);

  int
  send_cursor_plane_update(session_t *session,
                           std::uint32_t cursor_shape_id,
                           std::uint32_t x,
                           std::uint32_t y,
                           std::uint16_t hotspot_x,
                           std::uint16_t hotspot_y,
                           std::uint16_t width,
                           std::uint16_t height,
                           std::uint32_t flags,
                           std::uint16_t bitmap_format = 0,
                           std::uint16_t bitmap_stride = 0,
                           const std::vector<std::uint8_t> *bitmap_bgra = nullptr) {
    if (!session || !client_supports_cursor_plane(session)) {
      return -1;
    }
    if (!session->control.peer) {
      return -1;
    }

    constexpr std::size_t max_cursor_bitmap_bytes = 512U * 512U * 4U;
    const bool include_bitmap = bitmap_bgra != nullptr &&
                                !bitmap_bgra->empty() &&
                                bitmap_bgra->size() <= max_cursor_bitmap_bytes &&
                                bitmap_format == SS_CURSOR_PLANE_BITMAP_FORMAT_BGRA &&
                                bitmap_stride > 0;
    const std::size_t cursor_payload_size =
      sizeof(SS_CURSOR_PLANE_V1) +
      (include_bitmap ? sizeof(SS_CURSOR_PLANE_BITMAP_V1) + bitmap_bgra->size() : 0);
    if (cursor_payload_size > 0xFFFFu) {
      BOOST_LOG(warning) << "Skipping cursor bitmap payload: too large bytes=" << cursor_payload_size;
      return send_cursor_plane_update(session,
                                      cursor_shape_id,
                                      x,
                                      y,
                                      hotspot_x,
                                      hotspot_y,
                                      width,
                                      height,
                                      flags & ~SS_CURSOR_PLANE_FLAG_SHAPE_BITMAP);
    }

    std::vector<std::uint8_t> plaintext(sizeof(control_header_v2) + cursor_payload_size);
    auto *header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_CURSOR_PLANE];
    header->payloadLength = static_cast<std::uint16_t>(cursor_payload_size);

    auto *cursor = reinterpret_cast<SS_CURSOR_PLANE_V1 *>(plaintext.data() + sizeof(control_header_v2));
    cursor->version = util::endian::little<std::uint16_t>(SS_CURSOR_PLANE_VERSION);
    cursor->size = util::endian::little<std::uint16_t>(static_cast<std::uint16_t>(cursor_payload_size));
    cursor->cursorShapeId = util::endian::little<std::uint32_t>(cursor_shape_id);
    cursor->x = util::endian::little<std::uint32_t>(x);
    cursor->y = util::endian::little<std::uint32_t>(y);
    cursor->hotspotX = util::endian::little<std::uint16_t>(hotspot_x);
    cursor->hotspotY = util::endian::little<std::uint16_t>(hotspot_y);
    cursor->width = util::endian::little<std::uint16_t>(width);
    cursor->height = util::endian::little<std::uint16_t>(height);
    cursor->flags = util::endian::little<std::uint32_t>(
      include_bitmap ? (flags | SS_CURSOR_PLANE_FLAG_SHAPE_BITMAP) :
                       (flags & ~SS_CURSOR_PLANE_FLAG_SHAPE_BITMAP));
    cursor->epoch = util::endian::little<std::uint32_t>(++session->control.cursor_plane.epoch);

    if (include_bitmap) {
      auto *bitmap_header = reinterpret_cast<SS_CURSOR_PLANE_BITMAP_V1 *>(
        plaintext.data() + sizeof(control_header_v2) + sizeof(SS_CURSOR_PLANE_V1));
      bitmap_header->format = util::endian::little<std::uint16_t>(bitmap_format);
      bitmap_header->stride = util::endian::little<std::uint16_t>(bitmap_stride);
      bitmap_header->bitmapBytes = util::endian::little<std::uint32_t>(
        static_cast<std::uint32_t>(bitmap_bgra->size()));
      std::memcpy(plaintext.data() + sizeof(control_header_v2) +
                    sizeof(SS_CURSOR_PLANE_V1) + sizeof(SS_CURSOR_PLANE_BITMAP_V1),
                  bitmap_bgra->data(),
                  bitmap_bgra->size());
    }

    std::vector<std::uint8_t> encrypted_payload(
      sizeof(control_encrypted_t) +
      crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size);

    auto payload = encode_control(session, std::string_view {
                                             reinterpret_cast<const char *>(plaintext.data()),
                                             plaintext.size(),
                                           },
                                  encrypted_payload);
    if (payload.empty()) {
      BOOST_LOG(error) << "Couldn't encode cursor plane control payload";
      return -1;
    }

    const auto packet_flags = include_bitmap ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    if (session->broadcast_ref->control_server.send(payload,
                                                    session->control.peer,
                                                    CTRL_CHANNEL_GENERIC,
                                                    packet_flags)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send cursor plane update to ["sv << addr << ':' << port << ']';
      return -1;
    }

    return 0;
  }

  void
  update_host_cursor_suppression_for_session(session_t *session, bool active, const char *reason) {
    if (!session || !session->broadcast_ref) {
      return;
    }

    const bool should_suppress = active &&
                                 session->config.monitor.preferCursorPlane &&
                                 client_supports_cursor_plane(session);
    auto &state = session->control.cursor_plane.host_cursor_suppressed;
    if (should_suppress == state) {
      return;
    }

    if (should_suppress) {
      session->broadcast_ref->host_cursor_suppression.acquire(session->identity.runtime_id);
      state = true;
      BOOST_LOG(info) << "Cursor plane host cursor suppression active runtime="
                      << session->identity.runtime_id
                      << " reason=" << (reason ? reason : "unknown");
    }
    else {
      session->broadcast_ref->host_cursor_suppression.release(session->identity.runtime_id);
      state = false;
      BOOST_LOG(info) << "Cursor plane host cursor suppression inactive runtime="
                      << session->identity.runtime_id
                      << " reason=" << (reason ? reason : "unknown");
    }
  }

  void
  maybe_send_cursor_plane_update(session_t *session, std::chrono::steady_clock::time_point now) {
    if (!session || !session->control.peer || !client_supports_cursor_plane(session)) {
      update_host_cursor_suppression_for_session(session, false, "cursor-plane-unavailable");
      return;
    }

    update_host_cursor_suppression_for_session(session, true, "cursor-plane-control-loop");

    auto sample = sample_host_cursor_plane(session);
    if (!sample.available) {
      return;
    }

    auto &last = session->control.cursor_plane;
    const bool sample_visible = (sample.flags & SS_CURSOR_PLANE_FLAG_VISIBLE) != 0;
    const bool position_changed = sample_visible &&
                                  (last.x != sample.x ||
                                   last.y != sample.y);
    const bool geometry_changed = sample_visible &&
                                  (last.hotspot_x != sample.hotspot_x ||
                                   last.hotspot_y != sample.hotspot_y ||
                                   last.width != sample.width ||
                                   last.height != sample.height ||
                                   last.display_width != sample.display_width ||
                                   last.display_height != sample.display_height ||
                                   last.display_hotspot_x != sample.display_hotspot_x ||
                                   last.display_hotspot_y != sample.display_hotspot_y ||
                                   last.bitmap_width != sample.bitmap_width ||
                                   last.bitmap_height != sample.bitmap_height);
    const bool changed = last.cursor_shape_id != sample.cursor_shape_id ||
                         position_changed ||
                         geometry_changed ||
                         last.flags != sample.flags;
    const bool never_sent = last.last_sent.time_since_epoch().count() == 0;
    if (!never_sent && now - last.last_sent < 16ms) {
      return;
    }
    if (!changed && !never_sent && now - last.last_sent < 250ms) {
      return;
    }

    const bool first_bitmap_missing = never_sent || !last.bitmap_sent;
    const bool bitmap_retry_due =
      first_bitmap_missing &&
      last.last_bitmap_retry.time_since_epoch().count() != 0 &&
      now - last.last_bitmap_retry >= 250ms;
    const bool should_send_bitmap =
      !sample.bitmap_bgra.empty() &&
      (first_bitmap_missing ||
       bitmap_retry_due ||
       last.cursor_shape_id != sample.cursor_shape_id ||
       last.width != sample.width ||
       last.height != sample.height ||
       last.hotspot_x != sample.hotspot_x ||
       last.hotspot_y != sample.hotspot_y);

    const bool shape_changed = last.cursor_shape_id != sample.cursor_shape_id;
    (void) send_session_control_cursor_plane(session, sample);
    if (send_cursor_plane_update(session,
                                 sample.cursor_shape_id,
                                 sample.x,
                                 sample.y,
                                 sample.display_hotspot_x,
                                 sample.display_hotspot_y,
                                 sample.display_width,
                                 sample.display_height,
                                 should_send_bitmap ? (sample.flags | SS_CURSOR_PLANE_FLAG_SHAPE_BITMAP) :
                                                      (sample.flags & ~SS_CURSOR_PLANE_FLAG_SHAPE_BITMAP),
                                 should_send_bitmap ? sample.bitmap_format : 0,
                                 should_send_bitmap ? sample.bitmap_stride : 0,
                                 should_send_bitmap ? &sample.bitmap_bgra : nullptr) == 0) {
      last.cursor_shape_id = sample.cursor_shape_id;
      last.x = sample.x;
      last.y = sample.y;
      last.hotspot_x = sample.hotspot_x;
      last.hotspot_y = sample.hotspot_y;
      last.width = sample.width;
      last.height = sample.height;
      last.display_width = sample.display_width;
      last.display_height = sample.display_height;
      last.display_hotspot_x = sample.display_hotspot_x;
      last.display_hotspot_y = sample.display_hotspot_y;
      last.bitmap_width = sample.bitmap_width;
      last.bitmap_height = sample.bitmap_height;
      last.flags = sample.flags;
      last.last_sent = now;
      if (should_send_bitmap) {
        last.bitmap_sent = true;
        last.last_bitmap_retry = now;
      }
      else if (!last.bitmap_sent && !sample.bitmap_bgra.empty()) {
        last.last_bitmap_retry = now;
      }

      if (never_sent || shape_changed || should_send_bitmap) {
        BOOST_LOG(info) << "Cursor plane update sent runtime=" << session->identity.runtime_id
                        << " shape=" << sample.cursor_shape_id
                        << " flags=0x" << util::hex(sample.flags).to_string_view()
                        << " pos=" << sample.x << "," << sample.y
                        << " hotspot=" << sample.display_hotspot_x << "," << sample.display_hotspot_y
                        << " display=" << sample.display_width << "x" << sample.display_height
                        << " bitmapSize=" << sample.bitmap_width << "x" << sample.bitmap_height
                        << " legacySize=" << sample.width << "x" << sample.height
                        << " dpiPpm=" << sample.host_dpi_scale_ppm
                        << " source=" << sample.size_source
                        << " bitmap=" << (should_send_bitmap ? sample.bitmap_bgra.size() : 0);
      }
    }
  }

  int
  send_clipboard_payload(session_t *session, const std::string_view &clipboard_payload) {
    constexpr std::size_t max_clipboard_control_payload = 0xFFFFu;

    if (!session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send clipboard payload, still waiting for PING from Moonlight"sv;
      return -1;
    }
    if (clipboard_payload.size() > max_clipboard_control_payload) {
      BOOST_LOG(error) << "Clipboard control payload too large: " << clipboard_payload.size();
      return -1;
    }

    std::vector<std::uint8_t> plaintext(sizeof(control_header_v2) + clipboard_payload.size());
    auto *header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_CLIPBOARD];
    header->payloadLength = static_cast<std::uint16_t>(clipboard_payload.size());
    if (!clipboard_payload.empty()) {
      std::memcpy(header->payload(), clipboard_payload.data(), clipboard_payload.size());
    }

    std::vector<std::uint8_t> encrypted_payload(
      sizeof(control_encrypted_t) +
      crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size);

    auto payload = encode_control(session,
                                  std::string_view {
                                    reinterpret_cast<char *>(plaintext.data()),
                                    plaintext.size(),
                                  },
                                  encrypted_payload);
    if (payload.empty()) {
      BOOST_LOG(error) << "Couldn't encode clipboard control payload";
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload, session->control.peer)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send clipboard payload to ["sv << addr << ':' << port << ']';
      return -1;
    }

    return 0;
  }

  int
  send_session_control_payload(session_t *session,
                               const void *session_payload,
                               std::size_t session_payload_size,
                               std::string_view label,
                               enet_uint32 packet_flags = ENET_PACKET_FLAG_RELIABLE) {
    if (!session || !session->control.peer || !session_payload || session_payload_size == 0) {
      BOOST_LOG(warning) << "Couldn't send Session control " << label
                         << ", control peer is unavailable";
      return -1;
    }
    if (session_payload_size > LI_SESSION_CONTROL_MAX_MESSAGE_SIZE ||
        session_payload_size > std::numeric_limits<std::uint16_t>::max()) {
      BOOST_LOG(error) << "Session control " << label
                       << " payload is too large: " << session_payload_size;
      return -1;
    }

    std::vector<std::uint8_t> plaintext(sizeof(control_header_v2) + session_payload_size);
    auto *header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_SESSION];
    header->payloadLength = static_cast<std::uint16_t>(session_payload_size);
    std::memcpy(header->payload(), session_payload, session_payload_size);

    std::vector<std::uint8_t> encrypted_payload(
      sizeof(control_encrypted_t) +
      crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size);

    auto payload = encode_control(session,
                                  std::string_view {
                                    reinterpret_cast<char *>(plaintext.data()),
                                    plaintext.size(),
                                  },
                                  encrypted_payload);
    if (payload.empty()) {
      BOOST_LOG(error) << "Couldn't encode Session control " << label;
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload,
                                                    session->control.peer,
                                                    CTRL_CHANNEL_SESSION,
                                                    packet_flags)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send Session control " << label
                         << " to ["sv << addr << ':' << port << ']';
      return -1;
    }

    session->control.session_control_tx++;
    return 0;
  }

  int
  send_rescue_control_ack(session_t *session,
                          const AlkSunshineRescueWireAck &ack,
                          enet_uint32 packet_flags = ENET_PACKET_FLAG_RELIABLE) {
    if (!session || !session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send rescue-control ack, control peer is unavailable";
      return -1;
    }

    ALK_SUNSHINE_RESCUE_ACK ack_payload;
    if (!alk_sunshine_rescue_build_ack(&ack, &ack_payload)) {
      BOOST_LOG(warning) << "Couldn't build rescue-control ack payload";
      return -1;
    }

    std::vector<std::uint8_t> plaintext(sizeof(control_header_v2) + sizeof(ack_payload));
    auto *header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_RESCUE];
    header->payloadLength = static_cast<std::uint16_t>(sizeof(ack_payload));
    std::memcpy(header->payload(), &ack_payload, sizeof(ack_payload));

    std::vector<std::uint8_t> encrypted_payload(
      sizeof(control_encrypted_t) +
      crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size);

    auto payload = encode_control(session,
                                  std::string_view {
                                    reinterpret_cast<char *>(plaintext.data()),
                                    plaintext.size(),
                                  },
                                  encrypted_payload);
    if (payload.empty()) {
      BOOST_LOG(error) << "Couldn't encode rescue-control ack";
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload,
                                                    session->control.peer,
                                                    CTRL_CHANNEL_URGENT,
                                                    packet_flags)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send rescue-control ack to ["sv << addr << ':' << port << ']';
      return -1;
    }

    return 0;
  }

  static bool
  apply_rescue_control_request(session_t *session, const AlkRescueControlRequest &request) {
    if (!session || request.version != ALK_RESCUE_CONTROL_VERSION) {
      return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (session->last_rescue_control_request.time_since_epoch().count() != 0 &&
        now - session->last_rescue_control_request < 250ms) {
      session->coalesced_rescue_control_requests++;
      BOOST_LOG(info) << "Alkaid rescue-control coalesced"
                      << " runtime=" << session->identity.runtime_id
                      << " requestId=" << request.request_id
                      << " suppressed=" << session->coalesced_rescue_control_requests;
      return false;
    }
    const auto coalesced = session->coalesced_rescue_control_requests;
    session->coalesced_rescue_control_requests = 0;
    session->last_rescue_control_request = now;
    session->rescue_control_requests++;

    const int current_bitrate = session->last_applied_stream_quality_bitrate > 0 ?
                                  session->last_applied_stream_quality_bitrate :
                                  std::max(1, session->config.monitor.bitrate);
    const int requested_bitrate = request.max_bitrate_kbps > 0 ?
                                    static_cast<int>(request.max_bitrate_kbps) :
                                    std::max(1200, current_bitrate * 2 / 3);
    const int target_bitrate = std::clamp(requested_bitrate, 800, std::max(800, current_bitrate));
    const int target_fec = std::clamp(
      std::max<int>(session->last_applied_stream_quality_fec, static_cast<int>(request.fec_percent)),
      0,
      45);
    const int target_scale = std::clamp<int>(
      request.target_scale_percent == 0 ? 75 : static_cast<int>(request.target_scale_percent),
      25,
      100);

    video::dynamic_param_t bitrate_param;
    bitrate_param.type = video::dynamic_param_type_e::BITRATE;
    bitrate_param.value.int_value = target_bitrate;
    bitrate_param.valid = true;
    session->video.dynamic_param_change_events->raise(bitrate_param);
    session->last_applied_stream_quality_bitrate = target_bitrate;
    session->last_stream_quality_bitrate_apply = now;

    video::dynamic_param_t fec_param;
    fec_param.type = video::dynamic_param_type_e::FEC_PERCENTAGE;
    fec_param.value.int_value = target_fec;
    fec_param.valid = true;
    session->video.dynamic_param_change_events->raise(fec_param);
    session->last_applied_stream_quality_fec = target_fec;
    session->last_stream_quality_fec_apply = now;

    bool runtime_scale_requested = false;
    runtime_profile_resolution_t runtime_resolution {
      session->config.monitor.width,
      session->config.monitor.height,
    };
    if (target_scale < 100 && target_scale != session->last_applied_stream_quality_resolution_scale) {
      runtime_resolution = runtime_profile_resolution_for_scale(session->config.monitor.width,
                                                                session->config.monitor.height,
                                                                target_scale);
      if (runtime_profile_resolution_reconfig_enabled()) {
        video::dynamic_param_t resolution_param;
        resolution_param.type = video::dynamic_param_type_e::RESOLUTION;
        resolution_param.value.int_array_value[0] = runtime_resolution.width;
        resolution_param.value.int_array_value[1] = runtime_resolution.height;
        resolution_param.valid = runtime_resolution.width > 0 && runtime_resolution.height > 0;
        session->video.dynamic_param_change_events->raise(resolution_param);
        session->last_applied_stream_quality_resolution_scale = target_scale;
        session->last_stream_quality_profile_apply = now;
        runtime_scale_requested = true;
      }
    }

    const auto hold_ms = std::clamp<std::uint32_t>(request.hold_ms == 0 ? 1000u : request.hold_ms,
                                                   300u,
                                                   3000u);
    session->rescue_control_hold_until = now + std::chrono::milliseconds(hold_ms);
    session->current_total_bitrate.store(total_video_bitrate_from_encoding_bitrate(target_bitrate, target_fec),
                                         std::memory_order_relaxed);
    session->current_fec_percentage.store(target_fec, std::memory_order_relaxed);
    session->pacing_total_bitrate.store(total_video_bitrate_from_encoding_bitrate(target_bitrate, target_fec),
                                        std::memory_order_relaxed);
    session->stream_quality_resync_guard_until = std::max(session->stream_quality_resync_guard_until,
                                                          now + 650ms);

    if (request.request_idr) {
      session->video.idr_events->raise(true);
    }

    BOOST_LOG(info) << "Alkaid rescue-control applied"
                    << " runtime=" << session->identity.runtime_id
                    << " requestId=" << request.request_id
                    << " triggers=0x" << std::hex << request.trigger_flags << std::dec
                    << " streamMode=" << request.stream_mode
                    << " scale=" << target_scale << "%"
                    << " target=" << runtime_resolution.width << "x" << runtime_resolution.height
                    << " runtimeScale=" << (runtime_scale_requested ? 1 : 0)
                    << " runtimeScaleMode=" << (runtime_scale_requested ? "soft" : "none")
                    << " bitrate=" << target_bitrate << " Kbps"
                    << " fec=" << target_fec << "%"
                    << " holdMs=" << hold_ms
                    << " blurry=" << (request.enable_blurry_upscale ? 1 : 0)
                    << " idr=" << (request.request_idr ? 1 : 0)
                    << " coalesced=" << coalesced
                    << (target_scale < 100 && !runtime_scale_requested ?
                          " rescueScaleDeferred=runtime-scale-unavailable-or-unchanged" : "");
    return true;
  }

  static std::uint32_t
  session_cursor_flags_from_sample(std::uint32_t ss_flags) {
    std::uint32_t flags = LI_SESSION_CURSOR_FLAG_REMOTE_PLANE;
    if ((ss_flags & SS_CURSOR_PLANE_FLAG_VISIBLE) != 0) {
      flags |= LI_SESSION_CURSOR_FLAG_VISIBLE;
    }
    if ((ss_flags & SS_CURSOR_PLANE_FLAG_LOCKED) != 0) {
      flags |= LI_SESSION_CURSOR_FLAG_LOCKED;
    }
    if ((ss_flags & SS_CURSOR_PLANE_FLAG_RELATIVE) != 0) {
      flags |= LI_SESSION_CURSOR_FLAG_RELATIVE_RAW_INPUT;
    }
    return flags;
  }

  int
  send_session_control_cursor_plane(session_t *session, const cursor_plane_sample_t &sample) {
    if (!session || !session->control.peer) {
      return -1;
    }
    if ((session->control.session_control_feature_bits & LI_SESSION_FEATURE_CURSOR_PLANE_V2) == 0) {
      return 0;
    }

    refresh_li_session(*session, session::state(*session));
    LI_SESSION_CURSOR_PLANE cursor_plane {};
    LiInitializeSessionCursorPlane(&cursor_plane);
    cursor_plane.flags = session_cursor_flags_from_sample(sample.flags);
    cursor_plane.cursorShapeId = sample.cursor_shape_id;
    cursor_plane.renderPolicy = LI_SESSION_CURSOR_RENDER_POLICY_CONTENT_SCALED;
    cursor_plane.sizeSource = sample.size_source != LI_SESSION_CURSOR_SIZE_SOURCE_UNKNOWN ?
                                sample.size_source :
                                LI_SESSION_CURSOR_SIZE_SOURCE_FALLBACK;
    cursor_plane.confidencePpm = sample.confidence_ppm != 0 ? sample.confidence_ppm : 500000;
    cursor_plane.streamWidth = static_cast<std::uint32_t>(std::max(1, session->config.monitor.width));
    cursor_plane.streamHeight = static_cast<std::uint32_t>(std::max(1, session->config.monitor.height));
    cursor_plane.positionX = sample.x;
    cursor_plane.positionY = sample.y;
    cursor_plane.displayWidth = sample.display_width;
    cursor_plane.displayHeight = sample.display_height;
    cursor_plane.hotspotX = sample.display_hotspot_x;
    cursor_plane.hotspotY = sample.display_hotspot_y;
    cursor_plane.bitmapWidth = sample.bitmap_width != 0 ? sample.bitmap_width : sample.width;
    cursor_plane.bitmapHeight = sample.bitmap_height != 0 ? sample.bitmap_height : sample.height;
    cursor_plane.bitmapStride = sample.bitmap_stride;
    cursor_plane.bitmapFormat = sample.bitmap_format == SS_CURSOR_PLANE_BITMAP_FORMAT_BGRA ?
                                  LI_SESSION_CURSOR_BITMAP_FORMAT_BGRA :
                                  LI_SESSION_CURSOR_BITMAP_FORMAT_NONE;
    cursor_plane.hostDpiScalePpm = sample.host_dpi_scale_ppm;
    cursor_plane.userScalePpm = 1000000;
    cursor_plane.minClientPointSize = 8;
    cursor_plane.maxClientPointSize = 128;
    cursor_plane.epoch = session->control.cursor_plane.epoch + 1U;

    alkaidlab_session_bridge::update_cursor_plane(session->alkaidlab_session_context,
                                                  cursor_plane);
    alkaidlab_session_bridge::project_to_li_session(session->alkaidlab_session_context,
                                                    session->shared_session);
    const auto packet = session_runtime::make_session_control_cursor_plane(
      session->shared_session,
      ++session->control.cursor_plane.session_cursor_plane_tx);
    static_assert(sizeof(packet) <= LI_SESSION_CONTROL_MAX_MESSAGE_SIZE);

    const auto result = send_session_control_payload(session,
                                                     &packet,
                                                     sizeof(packet),
                                                     "cursor-plane"sv,
                                                     ENET_PACKET_FLAG_UNSEQUENCED);
    if (result == 0) {
      BOOST_LOG(debug) << "Session cursor plane sent runtime="sv
                       << session->identity.runtime_id
                       << " shape="sv << cursor_plane.cursorShapeId
                       << " display="sv << cursor_plane.displayWidth << 'x' << cursor_plane.displayHeight
                       << " bitmap="sv << cursor_plane.bitmapWidth << 'x' << cursor_plane.bitmapHeight
                       << " hotspot="sv << cursor_plane.hotspotX << ',' << cursor_plane.hotspotY
                       << " dpiPpm="sv << cursor_plane.hostDpiScalePpm
                       << " source="sv << cursor_plane.sizeSource
                       << " epoch="sv << cursor_plane.epoch;
    }
    return result;
  }

  int
  send_session_control_welcome(session_t *session) {
    if (!session || !session->control.peer) {
      BOOST_LOG(warning) << "Couldn't send Session control welcome, control peer is unavailable"sv;
      return -1;
    }

    refresh_li_session(*session, session::state(*session));
    const auto welcome = session_runtime::make_session_control_welcome(session->shared_session);
    static_assert(sizeof(welcome) <= LI_SESSION_CONTROL_MAX_MESSAGE_SIZE);

    std::vector<std::uint8_t> plaintext(sizeof(control_header_v2) + sizeof(welcome));
    auto *header = reinterpret_cast<control_header_v2 *>(plaintext.data());
    header->type = packetTypes[IDX_SESSION];
    header->payloadLength = static_cast<std::uint16_t>(sizeof(welcome));
    std::memcpy(header->payload(), &welcome, sizeof(welcome));

    std::vector<std::uint8_t> encrypted_payload(
      sizeof(control_encrypted_t) +
      crypto::cipher::round_to_pkcs7_padded(plaintext.size()) +
      crypto::cipher::tag_size);

    auto payload = encode_control(session,
                                  std::string_view {
                                    reinterpret_cast<char *>(plaintext.data()),
                                    plaintext.size(),
                                  },
                                  encrypted_payload);
    if (payload.empty()) {
      BOOST_LOG(error) << "Couldn't encode Session control welcome";
      return -1;
    }

    if (session->broadcast_ref->control_server.send(payload,
                                                    session->control.peer,
                                                    CTRL_CHANNEL_SESSION)) {
      TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
      BOOST_LOG(warning) << "Couldn't send Session control welcome to ["sv << addr << ':' << port << ']';
      return -1;
    }

    session->control.session_control_tx++;
    session->control.session_control_welcome_sent = true;
    BOOST_LOG(info) << "Session control welcome sent runtime="sv
                    << session->identity.runtime_id
                    << " logical=0x"sv << util::hex(welcome.logicalSessionKey).to_string_view()
                    << " features=0x"sv << util::hex(welcome.negotiatedFeatureBits).to_string_view()
                    << " channel="sv << static_cast<int>(CTRL_CHANNEL_SESSION);
    return 0;
  }

  int
  send_session_control_telemetry(session_t *session) {
    if (!session) {
      return -1;
    }
    if ((session->control.session_control_feature_bits & LI_SESSION_FEATURE_SESSION_TELEMETRY) == 0) {
      return 0;
    }

    refresh_li_session(*session, session::state(*session));
    const auto telemetry = session_runtime::make_session_control_telemetry(
      session->shared_session,
      static_cast<std::uint32_t>(++session->control.session_telemetry_tx));
    static_assert(sizeof(telemetry) <= LI_SESSION_CONTROL_MAX_MESSAGE_SIZE);

    const auto result = send_session_control_payload(session,
                                                     &telemetry,
                                                     sizeof(telemetry),
                                                     "telemetry"sv,
                                                     ENET_PACKET_FLAG_UNSEQUENCED);
    if (result == 0) {
      BOOST_LOG(debug) << "Session telemetry sent runtime="sv
                       << session->identity.runtime_id
                       << " path=0x"sv << util::hex(telemetry.pathId).to_string_view()
                       << " rttUs="sv << telemetry.telemetry.rttUs
                       << " lossPpm="sv << telemetry.telemetry.packetLossPpm;
    }
    return result;
  }

  static session_runtime::feature_e
  session_feature_from_li_resource(std::uint32_t resource) {
    switch (resource) {
      case LI_SESSION_RESOURCE_INPUT_FOCUS:
        return session_runtime::feature_e::input_focus;
      case LI_SESSION_RESOURCE_MICROPHONE:
        return session_runtime::feature_e::microphone;
      case LI_SESSION_RESOURCE_CLIPBOARD:
        return session_runtime::feature_e::clipboard;
      case LI_SESSION_RESOURCE_DISPLAY:
        return session_runtime::feature_e::display;
      case LI_SESSION_RESOURCE_DYNAMIC_QUALITY:
        return session_runtime::feature_e::dynamic_quality;
      case LI_SESSION_RESOURCE_TRANSPORT_QOS:
        return session_runtime::feature_e::transport_qos;
      case LI_SESSION_RESOURCE_CURSOR_PLANE:
        return session_runtime::feature_e::cursor_plane;
      default:
        return session_runtime::feature_e::dynamic_params;
    }
  }

  static bool
  session_resource_is_known(std::uint32_t resource) {
    return resource >= LI_SESSION_RESOURCE_INPUT_FOCUS &&
           resource <= LI_SESSION_RESOURCE_CURSOR_PLANE;
  }

  int
  send_session_control_lease(session_t *session,
                             const LI_SESSION_LEASE &granted_lease,
                             std::uint32_t operation,
                             std::uint32_t status) {
    if (!session) {
      return -1;
    }
    if ((session->control.session_control_feature_bits & LI_SESSION_FEATURE_LEASE_CONTROL) == 0) {
      return 0;
    }

    refresh_li_session(*session, session::state(*session));
    const auto lease = session_runtime::make_session_control_lease_ack(session->shared_session,
                                                                       granted_lease,
                                                                       operation,
                                                                       status);
    static_assert(sizeof(lease) <= LI_SESSION_CONTROL_MAX_MESSAGE_SIZE);
    const auto result = send_session_control_payload(session,
                                                     &lease,
                                                     sizeof(lease),
                                                     "lease"sv);
    if (result == 0) {
      session->control.session_lease_tx++;
      BOOST_LOG(info) << "Session lease sent runtime="sv
                      << session->identity.runtime_id
                      << " resource="sv << session_runtime::lease_feature_name(lease.resource)
                      << " op="sv << lease.operation
                      << " status="sv << lease.status
                      << " ownerRuntime="sv << lease.lease.ownerRuntimeId;
    }
    return result;
  }

  int
  send_small_clipboard_payload(session_t *session, const std::string_view &clipboard_payload) {
    constexpr std::size_t max_clipboard_plaintext_payload = 128;
    if (clipboard_payload.size() > max_clipboard_plaintext_payload) {
      BOOST_LOG(error) << "Clipboard payload too large for small-payload helper: " << clipboard_payload.size();
      return -1;
    }
    return send_clipboard_payload(session, clipboard_payload);
  }

  int
  send_clipboard_item(session_t *session,
                      uint8_t transfer_flags,
                      uint8_t item_type,
                      const std::string_view &mime_type,
                      const std::string_view &name,
                      const std::vector<std::uint8_t> &data,
                      std::uint64_t content_hash) {
    const auto item_id = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());

    const auto start_payload = clipboard_payload::build_item_start(transfer_flags,
                                                                   item_type,
                                                                   item_id,
                                                                   content_hash,
                                                                   static_cast<std::uint32_t>(data.size()),
                                                                   mime_type,
                                                                   name);
    if (start_payload.empty()) {
      BOOST_LOG(error) << "Clipboard item start payload failed adapter encode";
      return -1;
    }
    if (send_clipboard_payload(session,
                               std::string_view {
                                 reinterpret_cast<const char *>(start_payload.data()),
                                 start_payload.size(),
                               }) != 0) {
      return -1;
    }

    for (std::size_t offset = 0; offset < data.size(); offset += LI_CLIPBOARD_MAX_CHUNK_SIZE) {
      const auto chunk_length = std::min<std::size_t>(LI_CLIPBOARD_MAX_CHUNK_SIZE, data.size() - offset);
      const auto chunk_payload = clipboard_payload::build_item_chunk(item_id,
                                                                     static_cast<std::uint32_t>(offset),
                                                                     std::string_view {
                                                                       reinterpret_cast<const char *>(data.data() + offset),
                                                                       chunk_length,
                                                                     });
      if (chunk_payload.empty() && chunk_length != 0) {
        BOOST_LOG(error) << "Clipboard item chunk payload failed adapter encode";
        return -1;
      }
      if (send_clipboard_payload(session,
                                 std::string_view {
                                   reinterpret_cast<const char *>(chunk_payload.data()),
                                   chunk_payload.size(),
                                 }) != 0) {
        return -1;
      }
    }

    const auto end_payload = clipboard_payload::build_item_end(item_id);
    return send_small_clipboard_payload(session,
                                        std::string_view {
                                          reinterpret_cast<const char *>(end_payload.data()),
                                          end_payload.size(),
                                        });
  }

  int
  send_empty_clipboard_snapshot(session_t *session) {
    return send_clipboard_item(session,
                               LI_CLIPBOARD_TRANSFER_FLAG_SNAPSHOT,
                               LI_CLIPBOARD_ITEM_TYPE_NONE,
                               {},
                               {},
                               {},
                               0);
  }

  void
  controlBroadcastThread(control_server_t *server) {
    auto reset_clipboard_transfer = [](session_t *session) {
      session->control.clipboard.transfer_active = false;
      session->control.clipboard.item_type = LI_CLIPBOARD_ITEM_TYPE_NONE;
      session->control.clipboard.transfer_flags = 0;
      session->control.clipboard.item_id = 0;
      session->control.clipboard.content_hash = 0;
      session->control.clipboard.total_length = 0;
      session->control.clipboard.received_length = 0;
      session->control.clipboard.mime_type.clear();
      session->control.clipboard.name.clear();
      std::vector<std::uint8_t>().swap(session->control.clipboard.data);
    };

    auto clear_clipboard_binding = [reset_clipboard_transfer](session_t *session) {
      session->control.clipboard.bound = false;
      session->control.clipboard.last_host_sequence = 0;
      session->control.clipboard.last_sent_hash = 0;
      session->control.clipboard.suppress_next_host_echo = false;
      session->control.clipboard.suppressed_host_hash = 0;
      reset_clipboard_transfer(session);
    };

    auto is_clipboard_owner = [](session_t *session) {
      return session &&
             session->control.clipboard.bound &&
             session->broadcast_ref &&
             session->broadcast_ref->feature_leases.validate(session_runtime::feature_e::clipboard, *session);
    };

    auto client_supports_clipboard = [](session_t *session) {
      return session &&
             (session->config.mlFeatureFlags & (ML_FF_CLIPBOARD_TEXT | ML_FF_CLIPBOARD_IMAGE)) != 0;
    };

    auto client_supports_clipboard_item = [](session_t *session, uint8_t item_type) {
      if (!session) {
        return false;
      }

      switch (item_type) {
        case LI_CLIPBOARD_ITEM_TYPE_NONE:
          return true;
        case LI_CLIPBOARD_ITEM_TYPE_TEXT:
          return (session->config.mlFeatureFlags & ML_FF_CLIPBOARD_TEXT) != 0;
        case LI_CLIPBOARD_ITEM_TYPE_IMAGE:
          return (session->config.mlFeatureFlags & ML_FF_CLIPBOARD_IMAGE) != 0;
        default:
          return false;
      }
    };

#ifdef _WIN32
    auto send_host_clipboard_snapshot = [client_supports_clipboard_item](session_t *session,
                                                                         uint8_t transfer_flags,
                                                                         bool update_sequence_tracking) {
      platf::clipboard::item_t item;
      std::string reason;
      const auto sequence = platf::clipboard::current_sequence_number();

      if (!platf::clipboard::read_current_item(item, &reason)) {
        BOOST_LOG(warning) << "Failed to read Windows clipboard: " << reason;
        return -1;
      }

      if (item.type == LI_CLIPBOARD_ITEM_TYPE_NONE) {
        if (update_sequence_tracking) {
          session->control.clipboard.last_host_sequence = sequence;
          session->control.clipboard.last_sent_hash = 0;
        }

        if ((transfer_flags & LI_CLIPBOARD_TRANSFER_FLAG_SNAPSHOT) != 0) {
          return send_empty_clipboard_snapshot(session);
        }

        if (reason.find("size limit") != std::string::npos) {
          BOOST_LOG(info) << "Skipping clipboard update: " << reason;
        }
        else {
          BOOST_LOG(debug) << "Skipping empty/unsupported clipboard update: " << reason;
        }
        return 0;
      }

      if (!client_supports_clipboard_item(session, item.type)) {
        BOOST_LOG(debug) << "Skipping host clipboard item type " << static_cast<int>(item.type)
                         << " because client did not negotiate this clipboard capability";
        return 0;
      }

      if (session->control.clipboard.suppress_next_host_echo &&
          session->control.clipboard.suppressed_host_hash == item.content_hash) {
        session->control.clipboard.suppress_next_host_echo = false;
        session->control.clipboard.suppressed_host_hash = 0;
        if (update_sequence_tracking) {
          session->control.clipboard.last_host_sequence = sequence;
          session->control.clipboard.last_sent_hash = item.content_hash;
        }
        BOOST_LOG(debug) << "Suppressed echoed host clipboard item hash=" << item.content_hash;
        return 0;
      }

      if (send_clipboard_item(session,
                              transfer_flags,
                              item.type,
                              item.mime_type,
                              item.name,
                              item.data,
                              item.content_hash) != 0) {
        BOOST_LOG(warning) << "Failed to send host clipboard item to client " << session->client_name;
        return -1;
      }

      if (update_sequence_tracking) {
        session->control.clipboard.last_host_sequence = sequence;
        session->control.clipboard.last_sent_hash = item.content_hash;
      }

      BOOST_LOG(info) << "Sent host clipboard item to client " << session->client_name
                      << " type=" << static_cast<int>(item.type)
                      << " length=" << item.data.size()
                      << " flags=0x" << std::hex << static_cast<int>(transfer_flags) << std::dec;
      return 0;
    };

    auto maybe_send_host_clipboard_update = [send_host_clipboard_snapshot, is_clipboard_owner, client_supports_clipboard](session_t *session) {
      if (!config::input.clipboard_sync ||
          !client_supports_clipboard(session) ||
          !is_clipboard_owner(session) ||
          !platf::clipboard::is_backend_available()) {
        return;
      }

      const auto host_sequence = platf::clipboard::current_sequence_number();
      if (host_sequence != 0 &&
          host_sequence != session->control.clipboard.last_host_sequence) {
        if (send_host_clipboard_snapshot(session, 0, true) != 0) {
          BOOST_LOG(warning) << "Failed to send clipboard change update to client " << session->client_name;
        }
      }
    };
#else
    auto maybe_send_host_clipboard_update = [](session_t *) {};
#endif

    struct control_mic_stats_t {
      std::uint64_t packets {};
      std::uint64_t bytes {};
      std::uint64_t token_mismatch {};
      std::uint64_t lease_rejected {};
      std::uint64_t write_ok {};
      std::uint64_t write_failed {};
      std::uint64_t invalid {};
    };
    std::unordered_map<std::uint64_t, control_mic_stats_t> control_mic_stats;
    constexpr std::uint32_t control_mic_magic = ALK_SUNSHINE_MICROPHONE_CONTROL_DATA_MAGIC;
    constexpr std::uint16_t control_mic_version = ALK_SUNSHINE_MICROPHONE_CONTROL_VERSION;
    constexpr std::uint16_t control_mic_flag_session_token = ALK_SUNSHINE_MICROPHONE_CONTROL_FLAG_SESSION_TOKEN;
    constexpr std::size_t control_mic_token_size = ALK_SUNSHINE_MICROPHONE_SESSION_TOKEN_LENGTH;
    auto log_control_mic_stats = [&](session_t *session,
                                     control_mic_stats_t &stats,
                                     const char *event) {
      const std::string_view event_view = event ? std::string_view { event } : std::string_view {};
      const bool important = stats.packets == 1 ||
                             stats.write_ok == 1 ||
                             (stats.packets % 250) == 0 ||
                             (event_view == "control-token-mismatch" && stats.token_mismatch <= 3) ||
                             (event_view == "control-lease-rejected" && stats.lease_rejected <= 3) ||
                             (event_view == "control-invalid" && stats.invalid <= 3) ||
                             (event_view == "control-write-failed" && (stats.write_failed == 1 || (stats.write_failed % 25) == 0));
      if (!important) {
        return;
      }
      BOOST_LOG(info) << "Microphone receive stats"
                      << " event=" << event
                      << " runtime=" << (session ? session->identity.runtime_id : 0)
                      << " packets=" << stats.packets
                      << " bytes=" << stats.bytes
                      << " tokenMismatch=" << stats.token_mismatch
                      << " leaseReject=" << stats.lease_rejected
                      << " writeOk=" << stats.write_ok
                      << " writeFail=" << stats.write_failed
                      << " invalid=" << stats.invalid;
    };

    server->map(packetTypes[IDX_PERIODIC_PING], [](session_t *session, const std::string_view &payload) {
      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_periodic_ping_log.time_since_epoch().count() == 0 ||
          now - session->control.last_periodic_ping_log >= 2s) {
        const auto timeout_left = session->pingTimeout > now ?
                                    std::chrono::duration_cast<std::chrono::milliseconds>(session->pingTimeout - now).count() :
                                    0;
        BOOST_LOG(info) << "Control periodic ping received runtime="sv
                        << session->identity.runtime_id
                        << " payloadBytes="sv << payload.size()
                        << " timeoutLeft="sv << timeout_left << "ms";
        session->control.last_periodic_ping_log = now;
      }
    });

    server->map(packetTypes[IDX_START_A], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_A]"sv;
    });

    server->map(packetTypes[IDX_START_B], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_B]"sv;
    });

    server->map(packetTypes[IDX_MIC_DATA], [&](session_t *session, const std::string_view &payload) {
      if (!session || !session->broadcast_ref) {
        return;
      }

      auto &stats = control_mic_stats[session->identity.runtime_id];
      stats.packets++;
      stats.bytes += payload.size();

      AlkSunshineMicrophoneControlPacket mic_packet;
      if (!alk_sunshine_microphone_parse_control_packet(payload.data(), payload.size(), &mic_packet) ||
          mic_packet.payload.length == 0 ||
          mic_packet.magic != control_mic_magic ||
          mic_packet.control_version != control_mic_version) {
        stats.invalid++;
        log_control_mic_stats(session, stats, "control-invalid");
        return;
      }

      const auto sequence_number = mic_packet.sequence_number;
      const auto flags = mic_packet.flags;
      const auto *token = reinterpret_cast<const char *>(mic_packet.session_token.data);
      const auto *opus = reinterpret_cast<const char *>(mic_packet.payload.data);
      const auto opus_size = mic_packet.payload.length;

      if ((flags & control_mic_flag_session_token) != 0 &&
          session->audio.ping_payload.size() == control_mic_token_size &&
          std::memcmp(token, session->audio.ping_payload.data(), control_mic_token_size) != 0) {
        stats.token_mismatch++;
        log_control_mic_stats(session, stats, "control-token-mismatch");
        return;
      }

      if (!session->broadcast_ref->feature_leases.validate(session_runtime::feature_e::microphone, *session)) {
        stats.lease_rejected++;
        log_control_mic_stats(session, stats, "control-lease-rejected");
        return;
      }

      const int write_result = audio::write_mic_data(reinterpret_cast<const std::uint8_t *>(opus),
                                                     opus_size,
                                                     sequence_number);
      if (write_result >= 0) {
        stats.write_ok++;
        log_control_mic_stats(session, stats, "control-write-ok");
      }
      else {
        stats.write_failed++;
        log_control_mic_stats(session, stats, "control-write-failed");
      }
    });

    server->map(packetTypes[IDX_SESSION], [&](session_t *session_p, const std::string_view &payload) {
      if (!session_p || !session_p->broadcast_ref) {
        return;
      }

      if (session_p->config.mlCoreSessionVersion <= 0 ||
          session_p->config.mlCoreFeatureBits == 0) {
        BOOST_LOG(debug) << "Ignoring Session control packet from client without RTSP Session feature gate"
                         << " runtime=" << session_p->identity.runtime_id
                         << " payloadBytes=" << payload.size();
        return;
      }

      if (payload.size() < sizeof(LI_SESSION_CONTROL_HEADER)) {
        BOOST_LOG(warning) << "Invalid Session control packet"
                           << " runtime=" << session_p->identity.runtime_id
                           << " payloadBytes=" << payload.size();
        return;
      }

      const auto *session_header = reinterpret_cast<const LI_SESSION_CONTROL_HEADER *>(payload.data());
      switch (session_header->messageType) {
        case LI_SESSION_CONTROL_MSG_HELLO: {
          const auto hello = session_runtime::parse_session_control_hello(payload);
          if (!hello.has_value()) {
            BOOST_LOG(warning) << "Invalid Session control hello"
                               << " runtime=" << session_p->identity.runtime_id
                               << " payloadBytes=" << payload.size();
            return;
          }

          session_p->control.session_control_rx++;
          session_p->control.session_control_negotiated = true;
          session_p->config.mlCoreSessionVersion = LI_SESSION_VERSION;
          if (hello->clientFeatureBits != 0) {
            session_p->config.mlCoreFeatureBits = hello->clientFeatureBits;
          }
          if (hello->logicalSessionKey != 0) {
            session_p->identity.logical_session_key = hello->logicalSessionKey;
          }
          if (hello->participantKey != 0) {
            session_p->identity.participant_key = hello->participantKey;
          }
          if (hello->clientKey != 0) {
            session_p->identity.client_key = hello->clientKey;
          }
          if (hello->deviceKey != 0) {
            session_p->identity.device_key = hello->deviceKey;
          }
          if (hello->controlGeneration != 0) {
            session_p->identity.control_generation = hello->controlGeneration;
          }
          if (hello->sessionId[0] != '\0') {
            session_p->identity.logical_session_id = hello->sessionId;
          }
          if (hello->participantId[0] != '\0') {
            session_p->identity.participant_id = hello->participantId;
          }
          if (hello->deviceName[0] != '\0') {
            session_p->identity.client_unique_id = hello->deviceName;
          }
          if (hello->displayName[0] != '\0') {
            session_p->identity.client_name = hello->displayName;
            session_p->client_name = hello->displayName;
          }

          refresh_li_session(*session_p, session::state(*session_p));
          session_p->control.session_control_feature_bits =
            session_p->shared_session.featureCaps.negotiated;

          BOOST_LOG(info) << "Session control hello received runtime="sv
                          << session_p->identity.runtime_id
                          << " logical=0x"sv << util::hex(hello->logicalSessionKey).to_string_view()
                          << " participant=0x"sv << util::hex(hello->participantKey).to_string_view()
                          << " features=0x"sv << util::hex(session_p->control.session_control_feature_bits).to_string_view()
                          << " client="sv << session_p->client_name;

          if (send_session_control_welcome(session_p) != 0) {
            BOOST_LOG(warning) << "Failed to send Session control welcome"
                               << " runtime=" << session_p->identity.runtime_id;
          }
          break;
        }

        case LI_SESSION_CONTROL_MSG_TELEMETRY: {
          const auto telemetry = session_runtime::parse_session_control_telemetry(payload);
          if (!telemetry.has_value()) {
            BOOST_LOG(warning) << "Invalid Session telemetry"
                               << " runtime=" << session_p->identity.runtime_id
                               << " payloadBytes=" << payload.size();
            return;
          }

          session_p->control.session_telemetry_rx++;
          const session_runtime::session_telemetry_report_t report {
            .participant = {
              .participant_key = telemetry->participantKey,
              .runtime_id = telemetry->runtimeId,
              .device_key = session_p->identity.device_key,
            },
            .path_id = telemetry->pathId,
            .displayed_fps = telemetry->telemetry.currentFramerate,
            .rtt_ms = telemetry->telemetry.rttUs / 1000U,
            .loss_ppm = telemetry->telemetry.packetLossPpm,
            .renderer_backpressure = telemetry->telemetry.videoRenderQueueDepth > 0,
            .decode_queue_depth = telemetry->telemetry.videoDecodeQueueDepth,
            .render_queue_depth = telemetry->telemetry.videoRenderQueueDepth,
            .audio_queue_depth_ms = telemetry->telemetry.audioQueueDepthMs,
            .input_queue_depth = telemetry->telemetry.inputQueueDepth,
            .input_send_latency_us = telemetry->telemetry.inputSendLatencyUs,
            .input_ack_latency_us = telemetry->telemetry.inputAckLatencyUs,
            .mouse_backlog_us = telemetry->telemetry.mouseBacklogUs,
            .pointer_mode = telemetry->telemetry.pointerMode,
            .cursor_state_flags = telemetry->telemetry.cursorStateFlags,
            .pointer_release_queue_depth = telemetry->telemetry.pointerReleaseQueueDepth,
            .pointer_release_queue_delay_us = telemetry->telemetry.pointerReleaseQueueDelayUs,
            .pointer_mode_switch_us = telemetry->telemetry.pointerModeSwitchUs,
            .pointer_deltas_coalesced = telemetry->telemetry.pointerDeltasCoalesced,
            .pointer_acceleration_risk_ppm = telemetry->telemetry.pointerAccelerationRiskPpm,
          };
          session_p->telemetry.submit(report);
          session_p->active_transport_path.score.rtt_ms = report.rtt_ms;
          session_p->active_transport_path.score.loss_ppm = report.loss_ppm;
          session_p->active_transport_path.score.jitter_ms = telemetry->telemetry.jitterUs / 1000U;
          refresh_li_session(*session_p, session::state(*session_p));

          const auto now = std::chrono::steady_clock::now();
          if (session_p->control.last_session_telemetry_tx.time_since_epoch().count() == 0 ||
              now - session_p->control.last_session_telemetry_tx >= 1000ms) {
            session_p->control.last_session_telemetry_tx = now;
            (void) send_session_control_telemetry(session_p);
          }
          BOOST_LOG(debug) << "Session telemetry received runtime="sv
                           << session_p->identity.runtime_id
                           << " path=0x"sv << util::hex(telemetry->pathId).to_string_view()
                           << " rttUs="sv << telemetry->telemetry.rttUs
                           << " lossPpm="sv << telemetry->telemetry.packetLossPpm
                           << " inputQ="sv << telemetry->telemetry.inputQueueDepth;
          break;
        }

        case LI_SESSION_CONTROL_MSG_LEASE: {
          const auto lease = session_runtime::parse_session_control_lease(payload);
          if (!lease.has_value()) {
            BOOST_LOG(warning) << "Invalid Session lease"
                               << " runtime=" << session_p->identity.runtime_id
                               << " payloadBytes=" << payload.size();
            return;
          }

          session_p->control.session_lease_rx++;
          LI_SESSION_LEASE response_lease = lease->lease;
          response_lease.version = LI_SESSION_LEASE_VERSION;
          response_lease.feature = lease->resource;
          response_lease.mode = lease->mode;
          response_lease.ownerRuntimeId = session_p->identity.runtime_id;
          response_lease.ownerParticipantKey = session_p->identity.participant_key;
          response_lease.renewable = true;

          std::uint32_t status = LI_SESSION_CONTROL_LEASE_STATUS_DENIED;
          std::uint32_t response_op = LI_SESSION_CONTROL_LEASE_OP_ACK;
          if (session_resource_is_known(lease->resource)) {
            const auto feature = session_feature_from_li_resource(lease->resource);
            if (lease->operation == LI_SESSION_CONTROL_LEASE_OP_RELEASE) {
              session_p->broadcast_ref->feature_leases.release(feature, *session_p);
              response_lease.valid = false;
              status = LI_SESSION_CONTROL_LEASE_STATUS_RELEASED;
            }
            else if (lease->operation == LI_SESSION_CONTROL_LEASE_OP_REQUEST ||
                     lease->operation == LI_SESSION_CONTROL_LEASE_OP_RENEW) {
              const auto current_owner = session_p->broadcast_ref->feature_leases.owner(feature);
              if (current_owner &&
                  current_owner.runtime_id != session_p->identity.runtime_id &&
                  lease->mode == LI_SESSION_LEASE_MODE_EXCLUSIVE_OWNER) {
                response_lease.ownerRuntimeId = current_owner.runtime_id;
                response_lease.ownerParticipantKey = 0;
                response_lease.valid = true;
                status = LI_SESSION_CONTROL_LEASE_STATUS_CONFLICT;
              }
              else {
                session_p->broadcast_ref->feature_leases.acquire(feature, *session_p);
                response_lease.valid = true;
                response_lease.ttlMs = lease->lease.ttlMs != 0 ? lease->lease.ttlMs : 3000;
                status = LI_SESSION_CONTROL_LEASE_STATUS_GRANTED;
              }
            }
          }
          else {
            response_op = LI_SESSION_CONTROL_LEASE_OP_REJECT;
          }

          alkaidlab_session_bridge::update_lease(session_p->alkaidlab_session_context,
                                                 response_lease,
                                                 status);
          alkaidlab_session_bridge::project_to_li_session(session_p->alkaidlab_session_context,
                                                          session_p->shared_session);
          zako_input_runtime_apply_snapshot(&session_p->zako_input_runtime,
                                            &session_p->alkaidlab_session_context.snapshot);
          (void) send_session_control_lease(session_p, response_lease, response_op, status);
          BOOST_LOG(info) << "Session lease received runtime="sv
                          << session_p->identity.runtime_id
                          << " resource="sv << session_runtime::lease_feature_name(lease->resource)
                          << " op="sv << lease->operation
                          << " status="sv << status;
          break;
        }

        default:
          BOOST_LOG(debug) << "Ignoring unsupported Session control packet"
                           << " runtime=" << session_p->identity.runtime_id
                           << " type=" << session_header->messageType
                           << " payloadBytes=" << payload.size();
          break;
      }
    });

    server->map(packetTypes[IDX_CLIPBOARD], [&, reset_clipboard_transfer, clear_clipboard_binding, is_clipboard_owner, client_supports_clipboard, client_supports_clipboard_item](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_CLIPBOARD]"sv;

#ifdef _WIN32
      if (!config::input.clipboard_sync || !platf::clipboard::is_backend_available()) {
        session->broadcast_ref->feature_leases.release(session_runtime::feature_e::clipboard, *session);
        clear_clipboard_binding(session);
        BOOST_LOG(debug) << "Ignoring clipboard control packet because clipboard sync is disabled"sv;
        return;
      }
#endif

      AlkSunshineClipboardWireEvent clipboard_event;
      if (!alk_sunshine_clipboard_parse(payload.data(), payload.size(), &clipboard_event)) {
        BOOST_LOG(warning) << "Clipboard payload failed adapter parse";
        return;
      }

      switch (clipboard_event.kind) {
        case LI_CLIPBOARD_MSG_BIND: {
          if (!client_supports_clipboard(session)) {
            BOOST_LOG(warning) << "Ignoring clipboard bind from client without negotiated clipboard capability " << session->client_name;
            break;
          }
          auto sessions_lock = server->_sessions.lock();
          for (auto *other: *server->_sessions) {
            if (other != session) {
              clear_clipboard_binding(other);
            }
          }
          reset_clipboard_transfer(session);
          session->broadcast_ref->feature_leases.acquire(session_runtime::feature_e::clipboard, *session);
          session->control.clipboard.bound = true;
          session->control.clipboard.last_host_sequence = 0;
          session->control.clipboard.last_sent_hash = 0;
          session->control.clipboard.suppress_next_host_echo = false;
          session->control.clipboard.suppressed_host_hash = 0;
          BOOST_LOG(info) << "Clipboard session bound for client " << session->client_name;
          break;
        }
        case LI_CLIPBOARD_MSG_UNBIND:
          session->broadcast_ref->feature_leases.release(session_runtime::feature_e::clipboard, *session);
          clear_clipboard_binding(session);
          BOOST_LOG(info) << "Clipboard session unbound for client " << session->client_name;
          break;
        case LI_CLIPBOARD_MSG_SNAPSHOT_REQUEST:
          if (!is_clipboard_owner(session)) {
            BOOST_LOG(warning) << "Ignoring clipboard snapshot request from non-owner client " << session->client_name;
            break;
          }
          if (!client_supports_clipboard(session)) {
            BOOST_LOG(warning) << "Ignoring clipboard snapshot request from client without negotiated clipboard capability " << session->client_name;
            break;
          }
          BOOST_LOG(info) << "Clipboard snapshot requested by client " << session->client_name;
#ifdef _WIN32
          if (platf::clipboard::is_backend_available()) {
            if (send_host_clipboard_snapshot(session, LI_CLIPBOARD_TRANSFER_FLAG_SNAPSHOT, true) != 0) {
              BOOST_LOG(warning) << "Failed to send clipboard snapshot to client " << session->client_name;
            }
            break;
          }
#endif
          if (send_empty_clipboard_snapshot(session) != 0) {
            BOOST_LOG(warning) << "Failed to send empty clipboard snapshot to client " << session->client_name;
          }
          break;
        case LI_CLIPBOARD_MSG_ITEM_START: {
          const auto transfer_flags = clipboard_event.item_start.transfer_flags;
          const auto item_type = clipboard_event.item_start.item_type;
          if (!is_clipboard_owner(session)) {
            reset_clipboard_transfer(session);
            BOOST_LOG(warning) << "Ignoring clipboard item start from non-owner client " << session->client_name;
            return;
          }
          if (!client_supports_clipboard_item(session, item_type)) {
            reset_clipboard_transfer(session);
            BOOST_LOG(warning) << "Ignoring clipboard item type " << static_cast<int>(item_type)
                               << " from client without negotiated type capability " << session->client_name;
            return;
          }
          const auto item_id = clipboard_event.item_start.item_id;
          const auto content_hash = clipboard_event.item_start.content_hash;
          const auto total_length = clipboard_event.item_start.total_length;

          if (!clipboard_transfer_length_valid(item_type, total_length)) {
            BOOST_LOG(warning) << "Clipboard ITEM_START exceeded size limits for client "
                               << session->client_name
                               << " type=" << static_cast<int>(item_type)
                               << " length=" << total_length;
            return;
          }

          reset_clipboard_transfer(session);
          session->control.clipboard.transfer_active = true;
          session->control.clipboard.transfer_flags = transfer_flags;
          session->control.clipboard.item_type = item_type;
          session->control.clipboard.item_id = item_id;
          session->control.clipboard.content_hash = content_hash;
          session->control.clipboard.total_length = total_length;
          session->control.clipboard.received_length = 0;
          session->control.clipboard.mime_type.assign(reinterpret_cast<const char *>(clipboard_event.item_start.mime_type.data),
                                                      clipboard_event.item_start.mime_type.length);
          session->control.clipboard.name.assign(reinterpret_cast<const char *>(clipboard_event.item_start.name.data),
                                                 clipboard_event.item_start.name.length);
          session->control.clipboard.data.assign(total_length, 0);
          BOOST_LOG(info) << "Clipboard ITEM_START from client " << session->client_name
                          << " type=" << static_cast<int>(item_type)
                          << " length=" << total_length;
          break;
        }
        case LI_CLIPBOARD_MSG_ITEM_CHUNK: {
          const auto chunk_length = clipboard_event.item_chunk.chunk_length;
          const auto item_id = clipboard_event.item_chunk.item_id;
          const auto chunk_offset = clipboard_event.item_chunk.chunk_offset;
          if (!is_clipboard_owner(session)) {
            reset_clipboard_transfer(session);
            BOOST_LOG(warning) << "Ignoring clipboard item chunk from non-owner client " << session->client_name;
            return;
          }

          if (!session->control.clipboard.transfer_active ||
              session->control.clipboard.item_id != item_id) {
            BOOST_LOG(warning) << "Clipboard ITEM_CHUNK had no active transfer";
            return;
          }

          std::uint32_t next_received_length = 0;
          if (!clipboard_transfer_chunk_next_length(session->control.clipboard.received_length,
                                                    session->control.clipboard.total_length,
                                                    chunk_offset,
                                                    chunk_length,
                                                    next_received_length)) {
            BOOST_LOG(warning) << "Clipboard ITEM_CHUNK was out of order or invalid";
            reset_clipboard_transfer(session);
            return;
          }

          if (chunk_length != 0) {
            std::memcpy(session->control.clipboard.data.data() + chunk_offset,
                        clipboard_event.item_chunk.chunk.data,
                        chunk_length);
          }
          session->control.clipboard.received_length = next_received_length;
          break;
        }
        case LI_CLIPBOARD_MSG_ITEM_END: {
          const auto item_id = clipboard_event.item_id;
          if (!is_clipboard_owner(session)) {
            reset_clipboard_transfer(session);
            BOOST_LOG(warning) << "Ignoring clipboard item end from non-owner client " << session->client_name;
            return;
          }
          if (!session->control.clipboard.transfer_active ||
              session->control.clipboard.item_id != item_id) {
            BOOST_LOG(warning) << "Clipboard ITEM_END had no matching transfer";
            return;
          }

          if (session->control.clipboard.received_length !=
              session->control.clipboard.total_length) {
            BOOST_LOG(warning) << "Clipboard ITEM_END before transfer completion";
            reset_clipboard_transfer(session);
            return;
          }

          BOOST_LOG(info) << "Clipboard item fully received from client "
                          << session->client_name
                          << " type=" << static_cast<int>(session->control.clipboard.item_type)
                          << " length=" << session->control.clipboard.total_length
                          << " mime=" << session->control.clipboard.mime_type
                          << " name=" << session->control.clipboard.name;
#ifdef _WIN32
          if (platf::clipboard::is_backend_available()) {
            platf::clipboard::item_t item;
            item.type = session->control.clipboard.item_type;
            item.data = std::move(session->control.clipboard.data);
            item.mime_type = std::move(session->control.clipboard.mime_type);
            item.name = std::move(session->control.clipboard.name);
            item.content_hash = session->control.clipboard.content_hash;

            std::string reason;
            if (platf::clipboard::write_item(item, &reason)) {
              session->control.clipboard.suppress_next_host_echo = item.content_hash != 0;
              session->control.clipboard.suppressed_host_hash = item.content_hash;
              BOOST_LOG(info) << "Applied client clipboard item to Windows clipboard"
                              << " type=" << static_cast<int>(item.type)
                              << " length=" << item.data.size();
            }
            else {
              BOOST_LOG(warning) << "Failed to apply client clipboard item to Windows clipboard: " << reason;
            }
          }
#endif
          reset_clipboard_transfer(session);
          break;
        }
        case LI_CLIPBOARD_MSG_ITEM_CANCEL: {
          const auto item_id = clipboard_event.item_id;
          if (!is_clipboard_owner(session)) {
            reset_clipboard_transfer(session);
            BOOST_LOG(warning) << "Ignoring clipboard item cancel from non-owner client " << session->client_name;
            return;
          }
          if (item_id == 0 || session->control.clipboard.item_id == item_id) {
            reset_clipboard_transfer(session);
          }
          break;
        }
        default:
          BOOST_LOG(warning) << "Unknown clipboard control message kind: " << static_cast<int>(clipboard_event.kind);
          break;
      }
    });

    server->map(packetTypes[IDX_RESCUE], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_RESCUE]"sv;

      if (!client_supports_rescue_control(session)) {
        BOOST_LOG(warning) << "Ignoring rescue-control packet from client without negotiated capability"
                           << " runtime=" << (session ? session->identity.runtime_id : 0);
        return;
      }

      AlkRescueControlRequest request;
      if (!alk_sunshine_rescue_parse_request(payload.data(), payload.size(), &request)) {
        BOOST_LOG(warning) << "Ignoring malformed rescue-control payload"
                           << " runtime=" << (session ? session->identity.runtime_id : 0)
                           << " payloadBytes=" << payload.size();
        return;
      }

      BOOST_LOG(info) << "Alkaid rescue-control received"
                      << " runtime=" << session->identity.runtime_id
                      << " requestId=" << request.request_id
                      << " triggers=0x" << std::hex << request.trigger_flags << std::dec
                      << " streamMode=" << request.stream_mode
                      << " urgency=" << request.urgency_ppm
                      << " targetScale=" << request.target_scale_percent << "%"
                      << " maxBitrate=" << request.max_bitrate_kbps << " Kbps"
                      << " fec=" << request.fec_percent << "%"
                      << " holdMs=" << request.hold_ms
                      << " blurry=" << (request.enable_blurry_upscale ? 1 : 0)
                      << " idr=" << (request.request_idr ? 1 : 0)
                      << " codec=gamestream-rescue-payload-codec";

      const bool accepted = apply_rescue_control_request(session, request);

      AlkSunshineRescueWireAck ack;
      alk_sunshine_rescue_ack_init(&ack);
      ack.request_id = request.request_id;
      ack.accepted = accepted;
      ack.applied_scale_percent = std::clamp<std::uint32_t>(
        request.target_scale_percent == 0 ? 100u : request.target_scale_percent,
        25u,
        100u);
      ack.applied_bitrate_kbps = session->last_applied_stream_quality_bitrate > 0 ?
                                   static_cast<std::uint32_t>(session->last_applied_stream_quality_bitrate) :
                                   0u;
      ack.applied_fec_percent = session->last_applied_stream_quality_fec >= 0 ?
                                  static_cast<std::uint32_t>(session->last_applied_stream_quality_fec) :
                                  0u;
      if (send_rescue_control_ack(session, ack, ENET_PACKET_FLAG_RELIABLE) == 0) {
        BOOST_LOG(info) << "Alkaid rescue-control ack sent"
                        << " runtime=" << session->identity.runtime_id
                        << " requestId=" << ack.request_id
                        << " accepted=" << (ack.accepted ? 1 : 0)
                        << " scale=" << ack.applied_scale_percent << "%"
                        << " bitrate=" << ack.applied_bitrate_kbps << " Kbps"
                        << " fec=" << ack.applied_fec_percent << "%";
      }
    });

    server->map(packetTypes[IDX_LOSS_STATS], [&](session_t *session, const std::string_view &payload) {
      int32_t *stats = (int32_t *) payload.data();
      auto count = stats[0];
      std::chrono::milliseconds t { stats[1] };

      auto lastGoodFrame = stats[3];

      BOOST_LOG(verbose)
        << "type [IDX_LOSS_STATS]"sv << std::endl
        << "---begin stats---" << std::endl
        << "loss count since last report [" << count << ']' << std::endl
        << "time in milli since last report [" << t.count() << ']' << std::endl
        << "last good frame [" << lastGoodFrame << ']' << std::endl
        << "---end stats---";
    });

    server->map(SS_FRAME_FEC_PTYPE, [&](session_t *session, const std::string_view &payload) {
      if (!should_apply_frame_fec_stream_quality_feedback(session->config.mlFeatureFlags)) {
        return;
      }
      if (!adaptive_controller_active(session, "fec")) {
        return;
      }
      if (payload.size() < sizeof(SS_FRAME_FEC_STATUS)) {
        BOOST_LOG(warning) << "Ignoring truncated frame FEC status payload: " << payload.size();
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      if (session->last_stream_quality_fec_feedback.time_since_epoch().count() != 0 &&
          now - session->last_stream_quality_fec_feedback < 250ms) {
        return;
      }
      session->last_stream_quality_fec_feedback = now;

      const auto *status = reinterpret_cast<const SS_FRAME_FEC_STATUS *>(payload.data());
      const auto total_data = read_be16_unaligned(&status->totalDataPackets);
      const auto total_parity = read_be16_unaligned(&status->totalParityPackets);
      const auto received_data = read_be16_unaligned(&status->receivedDataPackets);
      const auto received_parity = read_be16_unaligned(&status->receivedParityPackets);
      const auto missing = read_be16_unaligned(&status->missingPacketsBeforeHighestReceived);
      const auto total_packets = static_cast<std::uint32_t>(total_data + total_parity);
      const auto received_packets = static_cast<std::uint32_t>(received_data + received_parity);
      const auto unrecoverable = received_packets < total_data ? 1U : 0U;
      const auto recovered = !unrecoverable && received_data < total_data ? 1U : 0U;

      stream_quality::feedback_t feedback {
        .duration_ms = 100,
        .frames_seen = 1,
        .complete_frames = unrecoverable ? 0U : 1U,
        .recovered_frames = recovered,
        .unrecoverable_frames = unrecoverable,
        .missing_packets = missing,
        .total_packets = total_packets,
        .received_packets = received_packets,
      };
      annotate_feedback_with_host_motion(session, feedback);
      auto action = session->stream_quality_controller.on_feedback(feedback);
      apply_stream_quality_action(session, action, "fec");
      record_stream_quality_feedback_diag(session, feedback, action, "fec");
    });

    server->map(SS_NETWORK_FEEDBACK_PTYPE, [&](session_t *session, const std::string_view &payload) {
      if (!(session->config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK)) {
        BOOST_LOG(debug) << "Ignoring network feedback from client without negotiated support";
        return;
      }
      if (!adaptive_controller_active(session, "feedback")) {
        return;
      }
      if (payload.size() < sizeof(SS_NETWORK_FEEDBACK_V1)) {
        BOOST_LOG(warning) << "Ignoring truncated network feedback payload: " << payload.size();
        return;
      }

      const auto *feedback = reinterpret_cast<const SS_NETWORK_FEEDBACK_V1 *>(payload.data());
      const auto version = read_be16_unaligned(&feedback->version);
      const auto size = read_be16_unaligned(&feedback->size);
      if (version != SS_NETWORK_FEEDBACK_VERSION || size < sizeof(SS_NETWORK_FEEDBACK_V1)) {
        BOOST_LOG(warning) << "Ignoring unsupported network feedback version=" << version
                           << " size=" << size;
        return;
      }

      const auto total_data = read_be32_unaligned(&feedback->totalDataPackets);
      const auto total_parity = read_be32_unaligned(&feedback->totalParityPackets);
      const auto received_data = read_be32_unaligned(&feedback->receivedDataPackets);
      const auto received_parity = read_be32_unaligned(&feedback->receivedParityPackets);

      stream_quality::feedback_t network_feedback {
        .duration_ms = read_be32_unaligned(&feedback->durationMs),
        .frames_seen = read_be32_unaligned(&feedback->framesSeen),
        .complete_frames = read_be32_unaligned(&feedback->completeFrames),
        .recovered_frames = read_be32_unaligned(&feedback->recoveredFrames),
        .unrecoverable_frames = read_be32_unaligned(&feedback->unrecoverableFrames),
        .missing_packets = read_be32_unaligned(&feedback->missingPackets),
        .total_packets = total_data + total_parity,
        .received_packets = received_data + received_parity,
        .video_bytes = read_be32_unaligned(&feedback->videoBytes),
        .rtt_ms = read_be32_unaligned(&feedback->rttMs),
        .rtt_variance_ms = read_be32_unaligned(&feedback->rttVarianceMs),
        .audio_underruns = read_be32_unaligned(&feedback->audioUnderruns),
      };
      network_feedback.local_display_pressure = stream_quality::infer_local_display_pressure(network_feedback);
      annotate_feedback_with_host_motion(session, network_feedback);
      if (should_hold_startup_stream_quality_feedback(session, network_feedback, "feedback")) {
        return;
      }
      auto action = session->stream_quality_controller.on_feedback(network_feedback);
      apply_stream_quality_action(session, action, "feedback");
      record_stream_quality_feedback_diag(session, network_feedback, action, "feedback");
      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_feedback_diag_log.time_since_epoch().count() == 0 ||
          now - session->control.last_feedback_diag_log >= 1000ms) {
        session->control.last_feedback_diag_log = now;
        BOOST_LOG(info) << "Control feedback received runtime=" << session->identity.runtime_id
                        << " duration=" << network_feedback.duration_ms << "ms"
                        << " frames=" << network_feedback.frames_seen
                        << " videoBytes=" << network_feedback.video_bytes
                        << " rtt=" << network_feedback.rtt_ms << "ms";
      }
    });

    server->map(SS_NETWORK_FEEDBACK_V2_PTYPE, [&](session_t *session, const std::string_view &payload) {
      if (!(session->config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_QOS_FEEDBACK)) {
        BOOST_LOG(debug) << "Ignoring network feedback from client without negotiated support";
        return;
      }
      if (!adaptive_controller_active(session, "feedback")) {
        return;
      }
      if (payload.size() < sizeof(SS_NETWORK_FEEDBACK_V2)) {
        BOOST_LOG(warning) << "Ignoring truncated network feedback payload: " << payload.size();
        return;
      }

      const auto *feedback = reinterpret_cast<const SS_NETWORK_FEEDBACK_V2 *>(payload.data());
      const auto version = read_be16_unaligned(&feedback->version);
      const auto size = read_be16_unaligned(&feedback->size);
      if (version != SS_NETWORK_FEEDBACK_V2_VERSION || size < sizeof(SS_NETWORK_FEEDBACK_V2)) {
        BOOST_LOG(warning) << "Ignoring unsupported network feedback version=" << version
                           << " size=" << size;
        return;
      }

      const auto total_data = read_be32_unaligned(&feedback->totalDataPackets);
      const auto total_parity = read_be32_unaligned(&feedback->totalParityPackets);
      const auto received_data = read_be32_unaligned(&feedback->receivedDataPackets);
      const auto received_parity = read_be32_unaligned(&feedback->receivedParityPackets);

      stream_quality::feedback_t network_feedback {
        .duration_ms = read_be32_unaligned(&feedback->durationMs),
        .frames_seen = read_be32_unaligned(&feedback->framesSeen),
        .complete_frames = read_be32_unaligned(&feedback->completeFrames),
        .recovered_frames = read_be32_unaligned(&feedback->recoveredFrames),
        .unrecoverable_frames = read_be32_unaligned(&feedback->unrecoverableFrames),
        .missing_packets = read_be32_unaligned(&feedback->missingPackets),
        .total_packets = total_data + total_parity,
        .received_packets = received_data + received_parity,
        .video_bytes = read_be32_unaligned(&feedback->videoBytes),
        .rtt_ms = read_be32_unaligned(&feedback->rttMs),
        .rtt_variance_ms = read_be32_unaligned(&feedback->rttVarianceMs),
        .audio_underruns = read_be32_unaligned(&feedback->audioUnderruns),
        .decode_queue_depth = read_be32_unaligned(&feedback->decodeQueueDepth),
        .render_queue_depth = read_be32_unaligned(&feedback->renderQueueDepth),
        .late_frames = read_be32_unaligned(&feedback->lateFrames),
        .displayed_frames = read_be32_unaligned(&feedback->displayedFrames),
        .input_queue_depth = read_be32_unaligned(&feedback->inputQueueDepth),
        .input_send_latency_us = read_be32_unaligned(&feedback->inputSendLatencyUs),
        .input_ack_latency_us = read_be32_unaligned(&feedback->inputAckLatencyUs),
      };
      network_feedback.local_display_pressure = stream_quality::infer_local_display_pressure(network_feedback);
      annotate_feedback_with_host_motion(session, network_feedback);
      if (should_hold_startup_stream_quality_feedback(session, network_feedback, "feedback")) {
        return;
      }
      auto action = session->stream_quality_controller.on_feedback(network_feedback);
      apply_stream_quality_action(session, action, "feedback");
      record_stream_quality_feedback_diag(session, network_feedback, action, "feedback");
      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_feedback_diag_log.time_since_epoch().count() == 0 ||
          now - session->control.last_feedback_diag_log >= 1000ms) {
        session->control.last_feedback_diag_log = now;
        BOOST_LOG(info) << "Control feedback received runtime=" << session->identity.runtime_id
                        << " duration=" << network_feedback.duration_ms << "ms"
                        << " frames=" << network_feedback.frames_seen
                        << " videoBytes=" << network_feedback.video_bytes
                        << " rtt=" << network_feedback.rtt_ms << "ms"
                        << " late=" << network_feedback.late_frames
                        << " renderQ=" << network_feedback.render_queue_depth;
      }
    });

    server->map(SS_NETWORK_FEEDBACK_V3_PTYPE, [&](session_t *session, const std::string_view &payload) {
      if (!(session->config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_QOS_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_AUDIO_CONTINUITY)) {
        BOOST_LOG(debug) << "Ignoring network feedback from client without negotiated support";
        return;
      }
      if (!adaptive_controller_active(session, "feedback")) {
        return;
      }
      if (payload.size() < sizeof(SS_NETWORK_FEEDBACK_V3)) {
        BOOST_LOG(warning) << "Ignoring truncated network feedback payload: " << payload.size();
        return;
      }

      const auto *feedback = reinterpret_cast<const SS_NETWORK_FEEDBACK_V3 *>(payload.data());
      const auto version = read_be16_unaligned(&feedback->version);
      const auto size = read_be16_unaligned(&feedback->size);
      if (version != SS_NETWORK_FEEDBACK_V3_VERSION || size < sizeof(SS_NETWORK_FEEDBACK_V3)) {
        BOOST_LOG(warning) << "Ignoring unsupported network feedback version=" << version
                           << " size=" << size;
        return;
      }

      const auto total_data = read_be32_unaligned(&feedback->totalDataPackets);
      const auto total_parity = read_be32_unaligned(&feedback->totalParityPackets);
      const auto received_data = read_be32_unaligned(&feedback->receivedDataPackets);
      const auto received_parity = read_be32_unaligned(&feedback->receivedParityPackets);

      stream_quality::feedback_t network_feedback {
        .duration_ms = read_be32_unaligned(&feedback->durationMs),
        .frames_seen = read_be32_unaligned(&feedback->framesSeen),
        .complete_frames = read_be32_unaligned(&feedback->completeFrames),
        .recovered_frames = read_be32_unaligned(&feedback->recoveredFrames),
        .unrecoverable_frames = read_be32_unaligned(&feedback->unrecoverableFrames),
        .missing_packets = read_be32_unaligned(&feedback->missingPackets),
        .total_packets = total_data + total_parity,
        .received_packets = received_data + received_parity,
        .video_bytes = read_be32_unaligned(&feedback->videoBytes),
        .rtt_ms = read_be32_unaligned(&feedback->rttMs),
        .rtt_variance_ms = read_be32_unaligned(&feedback->rttVarianceMs),
        .audio_underruns = read_be32_unaligned(&feedback->audioUnderruns),
        .decode_queue_depth = read_be32_unaligned(&feedback->decodeQueueDepth),
        .render_queue_depth = read_be32_unaligned(&feedback->renderQueueDepth),
        .late_frames = read_be32_unaligned(&feedback->lateFrames),
        .displayed_frames = read_be32_unaligned(&feedback->displayedFrames),
        .input_queue_depth = read_be32_unaligned(&feedback->inputQueueDepth),
        .input_send_latency_us = read_be32_unaligned(&feedback->inputSendLatencyUs),
        .input_ack_latency_us = read_be32_unaligned(&feedback->inputAckLatencyUs),
        .audio_concealed_ms = read_be32_unaligned(&feedback->audioConcealedMs),
        .late_audio_drops = read_be32_unaligned(&feedback->lateAudioDrops),
        .audio_plc_ms = read_be32_unaligned(&feedback->audioPlcMs),
        .audio_fade_ms = read_be32_unaligned(&feedback->audioFadeMs),
        .audio_buffer_depth_ms = read_be32_unaligned(&feedback->audioBufferDepthMs),
        .audio_drift_ppm = static_cast<std::int32_t>(read_be32_unaligned(&feedback->audioDriftPpm)),
      };
      network_feedback.local_display_pressure = stream_quality::infer_local_display_pressure(network_feedback);
      annotate_feedback_with_host_motion(session, network_feedback);
      if (should_hold_startup_stream_quality_feedback(session, network_feedback, "feedback")) {
        return;
      }
      auto action = session->stream_quality_controller.on_feedback(network_feedback);
      apply_stream_quality_action(session, action, "feedback");
      record_stream_quality_feedback_diag(session, network_feedback, action, "feedback");
      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_feedback_diag_log.time_since_epoch().count() == 0 ||
          now - session->control.last_feedback_diag_log >= 1000ms) {
        session->control.last_feedback_diag_log = now;
        BOOST_LOG(info) << "Control feedback received runtime=" << session->identity.runtime_id
                        << " duration=" << network_feedback.duration_ms << "ms"
                        << " frames=" << network_feedback.frames_seen
                        << " videoBytes=" << network_feedback.video_bytes
                        << " rtt=" << network_feedback.rtt_ms << "ms"
                        << " late=" << network_feedback.late_frames
                        << " renderQ=" << network_feedback.render_queue_depth
                        << " audioUnd=" << network_feedback.audio_underruns
                        << " audioConceal=" << network_feedback.audio_concealed_ms << "ms";
      }
    });

    server->map(SS_NETWORK_FEEDBACK_V4_PTYPE, [&](session_t *session, const std::string_view &payload) {
      if (!(session->config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_QOS_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_AUDIO_CONTINUITY) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_VISUAL_FRESHNESS)) {
        BOOST_LOG(debug) << "Ignoring network feedback from client without negotiated support";
        return;
      }
      if (!adaptive_controller_active(session, "feedback")) {
        return;
      }
      if (payload.size() < sizeof(SS_NETWORK_FEEDBACK_V4)) {
        BOOST_LOG(warning) << "Ignoring truncated network feedback payload: " << payload.size();
        return;
      }

      const auto *feedback = reinterpret_cast<const SS_NETWORK_FEEDBACK_V4 *>(payload.data());
      const auto version = read_be16_unaligned(&feedback->version);
      const auto size = read_be16_unaligned(&feedback->size);
      if (version != SS_NETWORK_FEEDBACK_V4_VERSION || size < sizeof(SS_NETWORK_FEEDBACK_V4)) {
        BOOST_LOG(warning) << "Ignoring unsupported network feedback version=" << version
                           << " size=" << size;
        return;
      }

      const auto total_data = read_be32_unaligned(&feedback->totalDataPackets);
      const auto total_parity = read_be32_unaligned(&feedback->totalParityPackets);
      const auto received_data = read_be32_unaligned(&feedback->receivedDataPackets);
      const auto received_parity = read_be32_unaligned(&feedback->receivedParityPackets);

      stream_quality::feedback_t network_feedback {
        .duration_ms = read_be32_unaligned(&feedback->durationMs),
        .frames_seen = read_be32_unaligned(&feedback->framesSeen),
        .complete_frames = read_be32_unaligned(&feedback->completeFrames),
        .recovered_frames = read_be32_unaligned(&feedback->recoveredFrames),
        .unrecoverable_frames = read_be32_unaligned(&feedback->unrecoverableFrames),
        .missing_packets = read_be32_unaligned(&feedback->missingPackets),
        .total_packets = total_data + total_parity,
        .received_packets = received_data + received_parity,
        .video_bytes = read_be32_unaligned(&feedback->videoBytes),
        .rtt_ms = read_be32_unaligned(&feedback->rttMs),
        .rtt_variance_ms = read_be32_unaligned(&feedback->rttVarianceMs),
        .audio_underruns = read_be32_unaligned(&feedback->audioUnderruns),
        .decode_queue_depth = read_be32_unaligned(&feedback->decodeQueueDepth),
        .render_queue_depth = read_be32_unaligned(&feedback->renderQueueDepth),
        .late_frames = read_be32_unaligned(&feedback->lateFrames),
        .displayed_frames = read_be32_unaligned(&feedback->displayedFrames),
        .visual_stale_frames = read_be32_unaligned(&feedback->visualStaleFrames),
        .duplicate_frames = read_be32_unaligned(&feedback->duplicateFrames),
        .input_queue_depth = read_be32_unaligned(&feedback->inputQueueDepth),
        .input_send_latency_us = read_be32_unaligned(&feedback->inputSendLatencyUs),
        .input_ack_latency_us = read_be32_unaligned(&feedback->inputAckLatencyUs),
        .audio_concealed_ms = read_be32_unaligned(&feedback->audioConcealedMs),
        .late_audio_drops = read_be32_unaligned(&feedback->lateAudioDrops),
        .audio_plc_ms = read_be32_unaligned(&feedback->audioPlcMs),
        .audio_fade_ms = read_be32_unaligned(&feedback->audioFadeMs),
        .audio_buffer_depth_ms = read_be32_unaligned(&feedback->audioBufferDepthMs),
        .audio_drift_ppm = static_cast<std::int32_t>(read_be32_unaligned(&feedback->audioDriftPpm)),
      };
      network_feedback.local_display_pressure = stream_quality::infer_local_display_pressure(network_feedback);
      annotate_feedback_with_host_motion(session, network_feedback);
      if (should_hold_startup_stream_quality_feedback(session, network_feedback, "feedback")) {
        return;
      }
      auto action = session->stream_quality_controller.on_feedback(network_feedback);
      apply_stream_quality_action(session, action, "feedback");
      record_stream_quality_feedback_diag(session, network_feedback, action, "feedback");
      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_feedback_diag_log.time_since_epoch().count() == 0 ||
          now - session->control.last_feedback_diag_log >= 1000ms) {
        session->control.last_feedback_diag_log = now;
        BOOST_LOG(info) << "Control feedback received runtime=" << session->identity.runtime_id
                        << " duration=" << network_feedback.duration_ms << "ms"
                        << " frames=" << network_feedback.frames_seen
                        << " displayed=" << network_feedback.displayed_frames
                        << " visualStale=" << network_feedback.visual_stale_frames
                        << " duplicate=" << network_feedback.duplicate_frames
                        << " videoBytes=" << network_feedback.video_bytes
                        << " rtt=" << network_feedback.rtt_ms << "ms"
                        << " late=" << network_feedback.late_frames
                        << " renderQ=" << network_feedback.render_queue_depth
                        << " audioUnd=" << network_feedback.audio_underruns
                        << " audioConceal=" << network_feedback.audio_concealed_ms << "ms";
      }
    });

    // Extended network feedback — same base payload plus client-side OWD
    // gradient telemetry for early congestion detection. The wire type and
    // payload version remain explicit contract fields; runtime policy source
    // labels intentionally stay versionless.
    server->map(SS_NETWORK_FEEDBACK_V5_PTYPE, [&](session_t *session, const std::string_view &payload) {
      if (!(session->config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_QOS_FEEDBACK) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_AUDIO_CONTINUITY) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_VISUAL_FRESHNESS) ||
          !(session->config.mlFeatureFlags2 & ML_FF2_DELAY_GRADIENT)) {
        BOOST_LOG(debug) << "Ignoring network feedback from client without negotiated support";
        return;
      }
      if (!adaptive_controller_active(session, "feedback")) {
        return;
      }
      if (payload.size() < sizeof(SS_NETWORK_FEEDBACK_V5)) {
        BOOST_LOG(warning) << "Ignoring truncated network feedback payload: " << payload.size();
        return;
      }

      const auto *feedback = reinterpret_cast<const SS_NETWORK_FEEDBACK_V5 *>(payload.data());
      const auto version = read_be16_unaligned(&feedback->version);
      const auto size = read_be16_unaligned(&feedback->size);
      if (version != SS_NETWORK_FEEDBACK_V5_VERSION || size < sizeof(SS_NETWORK_FEEDBACK_V5)) {
        BOOST_LOG(warning) << "Ignoring unsupported network feedback version=" << version
                           << " size=" << size;
        return;
      }

      const auto total_data = read_be32_unaligned(&feedback->totalDataPackets);
      const auto total_parity = read_be32_unaligned(&feedback->totalParityPackets);
      const auto received_data = read_be32_unaligned(&feedback->receivedDataPackets);
      const auto received_parity = read_be32_unaligned(&feedback->receivedParityPackets);
      const auto delay_samples = read_be32_unaligned(&feedback->delaySamples);
      const auto delay_gradient_raw = read_be32_unaligned(&feedback->delayGradientUs);
      const auto interarrival_jitter_us = read_be32_unaligned(&feedback->interarrivalJitterUs);

      stream_quality::feedback_t network_feedback {
        .duration_ms = read_be32_unaligned(&feedback->durationMs),
        .frames_seen = read_be32_unaligned(&feedback->framesSeen),
        .complete_frames = read_be32_unaligned(&feedback->completeFrames),
        .recovered_frames = read_be32_unaligned(&feedback->recoveredFrames),
        .unrecoverable_frames = read_be32_unaligned(&feedback->unrecoverableFrames),
        .missing_packets = read_be32_unaligned(&feedback->missingPackets),
        .total_packets = total_data + total_parity,
        .received_packets = received_data + received_parity,
        .video_bytes = read_be32_unaligned(&feedback->videoBytes),
        .rtt_ms = read_be32_unaligned(&feedback->rttMs),
        .rtt_variance_ms = read_be32_unaligned(&feedback->rttVarianceMs),
        .audio_underruns = read_be32_unaligned(&feedback->audioUnderruns),
        .decode_queue_depth = read_be32_unaligned(&feedback->decodeQueueDepth),
        .render_queue_depth = read_be32_unaligned(&feedback->renderQueueDepth),
        .late_frames = read_be32_unaligned(&feedback->lateFrames),
        .displayed_frames = read_be32_unaligned(&feedback->displayedFrames),
        .visual_stale_frames = read_be32_unaligned(&feedback->visualStaleFrames),
        .duplicate_frames = read_be32_unaligned(&feedback->duplicateFrames),
        .input_queue_depth = read_be32_unaligned(&feedback->inputQueueDepth),
        .input_send_latency_us = read_be32_unaligned(&feedback->inputSendLatencyUs),
        .input_ack_latency_us = read_be32_unaligned(&feedback->inputAckLatencyUs),
        .audio_concealed_ms = read_be32_unaligned(&feedback->audioConcealedMs),
        .late_audio_drops = read_be32_unaligned(&feedback->lateAudioDrops),
        .audio_plc_ms = read_be32_unaligned(&feedback->audioPlcMs),
        .audio_fade_ms = read_be32_unaligned(&feedback->audioFadeMs),
        .audio_buffer_depth_ms = read_be32_unaligned(&feedback->audioBufferDepthMs),
        .audio_drift_ppm = static_cast<std::int32_t>(read_be32_unaligned(&feedback->audioDriftPpm)),
        .delay_gradient_us = static_cast<std::int32_t>(delay_gradient_raw),
        .interarrival_jitter_us = interarrival_jitter_us,
        .delay_samples = delay_samples,
        .delay_gradient_valid = (delay_samples > 0),
      };
      network_feedback.local_display_pressure = stream_quality::infer_local_display_pressure(network_feedback);
      annotate_feedback_with_host_motion(session, network_feedback);
      if (should_hold_startup_stream_quality_feedback(session, network_feedback, "feedback")) {
        return;
      }
      auto action = session->stream_quality_controller.on_feedback(network_feedback);
      apply_stream_quality_action(session, action, "feedback");
      record_stream_quality_feedback_diag(session, network_feedback, action, "feedback");
      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_feedback_diag_log.time_since_epoch().count() == 0 ||
          now - session->control.last_feedback_diag_log >= 1000ms) {
        session->control.last_feedback_diag_log = now;
        BOOST_LOG(info) << "Control feedback received runtime=" << session->identity.runtime_id
                        << " duration=" << network_feedback.duration_ms << "ms"
                        << " frames=" << network_feedback.frames_seen
                        << " displayed=" << network_feedback.displayed_frames
                        << " videoBytes=" << network_feedback.video_bytes
                        << " rtt=" << network_feedback.rtt_ms << "ms"
                        << " owdGradient=" << network_feedback.delay_gradient_us << "us"
                        << " owdSamples=" << network_feedback.delay_samples
                        << " interJitter=" << network_feedback.interarrival_jitter_us << "us";
      }
    });

    server->map(packetTypes[IDX_REQUEST_IDR_FRAME], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_REQUEST_IDR_FRAME]"sv;

      session->stream_quality_rfi_requests.fetch_add(1, std::memory_order_relaxed);
      if (!should_forward_client_recovery_request(session,
                                                  "idr",
                                                  session->last_client_idr_request,
                                                  450ms)) {
        return;
      }
      report_stream_quality_recovery_request(session, "idr");
      session->video.idr_events->raise(true);
    });

    // 辅助函数：处理分辨率变更
    auto handle_resolution_change = [](session_t *session, int new_width, int new_height) {
      int old_width = session->config.monitor.width;
      int old_height = session->config.monitor.height;
      
      BOOST_LOG(info) << "Dynamic resolution change requested: " << old_width << "x" << old_height 
                      << " -> " << new_width << "x" << new_height;

      // 验证分辨率范围
      constexpr int MAX_RESOLUTION = 16384;
      if (new_width <= 0 || new_width > MAX_RESOLUTION || new_height <= 0 || new_height > MAX_RESOLUTION) {
        BOOST_LOG(warning) << "Invalid resolution value: " << new_width << "x" << new_height;
        return;
      }

      // 检查分辨率是否真的改变了
      if (old_width == new_width && old_height == new_height) {
        BOOST_LOG(debug) << "Resolution unchanged, ignoring request";
        return;
      }

      // 检测是否是旋转导致的宽高互换（例如：1920x1080 -> 1080x1920）
      bool is_rotation = (old_width == new_height && old_height == new_width);
      if (is_rotation) {
        BOOST_LOG(info) << "Detected display rotation: width and height swapped";
      }

      // 更新会话配置
      session->config.monitor.width = new_width;
      session->config.monitor.height = new_height;

      // 创建临时的 launch_session_t 来更新显示设备配置
      // 注意：必须按照结构体声明顺序初始化字段
      rtsp_stream::launch_session_t temp_launch_session {};
      temp_launch_session.id = session->launch_session_id;
      temp_launch_session.client_name = session->client_name;
      temp_launch_session.width = new_width;
      temp_launch_session.height = new_height;
      temp_launch_session.fps = session->config.monitor.framerate;
      temp_launch_session.enable_hdr = session->enable_hdr;
      temp_launch_session.enable_sops = session->enable_sops;
      temp_launch_session.max_nits = session->max_nits;
      temp_launch_session.min_nits = session->min_nits;
      temp_launch_session.max_full_nits = session->max_full_nits;

      // 更新显示设备配置（重新配置模式）
      // 注意：这也会触发捕获端和编码器的重新初始化，以适配新的分辨率
      if (is_rotation) {
        BOOST_LOG(info) << "Reconfiguring display device for rotation: " << old_width << "x" << old_height 
                        << " -> " << new_width << "x" << new_height;
      }
      else {
        BOOST_LOG(info) << "Reconfiguring display device for new resolution: " << old_width << "x" << old_height 
                        << " -> " << new_width << "x" << new_height;
      }
      
      display_device::session_t::get().configure_display(config::video, temp_launch_session, true);

      // 请求 IDR 帧以确保客户端能正确显示新分辨率
      // 这对于旋转场景特别重要，因为宽高互换需要新的关键帧
      session->video.idr_events->raise(true);

      // 注意：编码器和触摸端口的更新会在捕获端重新初始化时自动处理
      // - 编码器会在重新初始化时使用新的宽高（通过 config.monitor.width/height）
      // - 触摸端口会在视频捕获循环中通过 make_port() 自动更新
      BOOST_LOG(info) << "Resolution change completed: " << new_width << "x" << new_height 
                      << (is_rotation ? " (rotation detected)" : "");
    };

    // 统一动态参数更新协议 (IDX_DYNAMIC_PARAM_CHANGE)
    // Payload 格式：
    // - 参数类型 (int, 4字节): 0=分辨率, 1=FPS, 2=码率, 3=QP, 4=FEC, 5=预设, 6=自适应量化, 7=多遍编码, 8=VBV缓冲区, 9=色度采样, 10=动态范围
    // - 参数值：
    //   * 分辨率 (类型0): 2个int (8字节, width和height)
    //   * FPS (类型1): 1个float (4字节)
    //   * 其他单值参数（码率、QP等）: 1个int (4字节)
    server->map(packetTypes[IDX_DYNAMIC_PARAM_CHANGE], [&, handle_resolution_change](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_DYNAMIC_PARAM_CHANGE]"sv;

      constexpr size_t MIN_PAYLOAD_SIZE = sizeof(int);
      if (payload.size() < MIN_PAYLOAD_SIZE) {
        BOOST_LOG(warning) << "Invalid payload size for dynamic param change. Expected at least " 
                           << MIN_PAYLOAD_SIZE << " bytes, got " << payload.size();
        return;
      }

      const int param_type = *reinterpret_cast<const int *>(payload.data());
      
      if (param_type < 0 || param_type >= static_cast<int>(video::dynamic_param_type_e::MAX_PARAM_TYPE)) {
        BOOST_LOG(warning) << "Invalid parameter type: " << param_type;
        return;
      }

      const auto param_type_enum = static_cast<video::dynamic_param_type_e>(param_type);
      
      // 处理分辨率变更（需要两个int值）
      if (param_type_enum == video::dynamic_param_type_e::RESOLUTION) {
        constexpr size_t RESOLUTION_PAYLOAD_SIZE = sizeof(int) * 3;  // 类型 + width + height
        if (payload.size() < RESOLUTION_PAYLOAD_SIZE) {
          BOOST_LOG(warning) << "Invalid payload size for resolution change. Expected " 
                             << RESOLUTION_PAYLOAD_SIZE << " bytes, got " << payload.size();
          return;
        }

        const auto *resolution_data = reinterpret_cast<const int *>(payload.data());
        handle_resolution_change(session, resolution_data[1], resolution_data[2]);
        return;
      }

      // 处理FPS变更（需要float值）
      if (param_type_enum == video::dynamic_param_type_e::FPS) {
        constexpr size_t FPS_PAYLOAD_SIZE = sizeof(int) + sizeof(float);
        if (payload.size() < FPS_PAYLOAD_SIZE) {
          BOOST_LOG(warning) << "Invalid payload size for FPS change. Expected " 
                             << FPS_PAYLOAD_SIZE << " bytes, got " << payload.size();
          return;
        }

        const float new_fps = *reinterpret_cast<const float *>(payload.data() + sizeof(int));
        
        if (new_fps <= 0.0f || new_fps > 1000.0f) {
          BOOST_LOG(warning) << "Invalid FPS value: " << new_fps;
          return;
        }

        session->config.monitor.framerate = static_cast<int>(new_fps);
        
        video::dynamic_param_t param;
        param.type = video::dynamic_param_type_e::FPS;
        param.value.float_value = new_fps;
        param.valid = true;
        session->video.dynamic_param_change_events->raise(param);
        
        BOOST_LOG(info) << "Dynamic FPS change: " << new_fps << " fps";
        return;
      }

      // 处理其他单值参数（码率、QP等，使用int值）
      constexpr size_t INT_PARAM_PAYLOAD_SIZE = sizeof(int) * 2;
      if (payload.size() < INT_PARAM_PAYLOAD_SIZE) {
        BOOST_LOG(warning) << "Invalid payload size for dynamic param change. Expected at least " 
                           << INT_PARAM_PAYLOAD_SIZE << " bytes, got " << payload.size();
        return;
      }

      const int param_value = reinterpret_cast<const int *>(payload.data())[1];

      video::dynamic_param_t param;
      param.type = param_type_enum;
      param.valid = true;

      // 参数验证和处理的辅助lambda
      auto validate_and_raise = [&](bool valid, auto value, const char *name, const char *unit = "") {
        if (valid) {
          if constexpr (std::is_same_v<decltype(value), bool>) {
            param.value.bool_value = value;
          } else {
            param.value.int_value = value;
          }
          session->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Dynamic " << name << " change: " << value << unit;
          return true;
        }
        BOOST_LOG(warning) << "Invalid " << name << " value: " << param_value;
        return false;
      };

      switch (param_type_enum) {
        case video::dynamic_param_type_e::BITRATE:
          if (validate_and_raise(param_value > 0 && param_value <= 800000, param_value, "bitrate", " Kbps")) {
            session->current_total_bitrate = param_value;
          }
          break;
        case video::dynamic_param_type_e::QP:
          validate_and_raise(param_value >= 0 && param_value <= 51, param_value, "QP");
          break;
        case video::dynamic_param_type_e::FEC_PERCENTAGE:
          if (validate_and_raise(param_value >= 0 && param_value <= 100, param_value, "FEC percentage", "%")) {
            session->current_fec_percentage = param_value;
          }
          break;
        case video::dynamic_param_type_e::ADAPTIVE_QUANTIZATION: {
          bool enabled = (param_value != 0);
          param.value.bool_value = enabled;
          session->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Dynamic adaptive quantization change: " << (enabled ? "enabled" : "disabled");
          break;
        }
        case video::dynamic_param_type_e::MULTI_PASS:
          validate_and_raise(param_value >= 0 && param_value <= 2, param_value, "multi-pass");
          break;
        case video::dynamic_param_type_e::VBV_BUFFER_SIZE:
          validate_and_raise(param_value > 0, param_value, "VBV buffer size", " Kbps");
          break;
        case video::dynamic_param_type_e::CHROMA_SAMPLING:
          validate_and_raise(param_value == 0 || param_value == 1, param_value, "chroma sampling");
          break;
        case video::dynamic_param_type_e::DYNAMIC_RANGE:
          validate_and_raise(param_value >= 0 && param_value <= 2, param_value, "dynamic range");
          break;
        case video::dynamic_param_type_e::PRESET:
          param.value.int_value = param_value;
          session->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Dynamic preset change: " << param_value;
          break;
        default:
          BOOST_LOG(warning) << "Unsupported parameter type: " << param_type;
          break;
      }
    });

    server->map(packetTypes[IDX_INVALIDATE_REF_FRAMES], [&](session_t *session, const std::string_view &payload) {
      auto frames = (std::int64_t *) payload.data();
      auto firstFrame = frames[0];
      auto lastFrame = frames[1];

      BOOST_LOG(debug)
        << "type [IDX_INVALIDATE_REF_FRAMES]"sv << std::endl
        << "firstFrame [" << firstFrame << ']' << std::endl
        << "lastFrame [" << lastFrame << ']';

      session->stream_quality_rfi_requests.fetch_add(1, std::memory_order_relaxed);
      if (!should_forward_client_recovery_request(session,
                                                  "rfi",
                                                  session->last_client_rfi_request,
                                                  250ms)) {
        return;
      }
      report_stream_quality_recovery_request(session, "rfi");
      session->video.invalidate_ref_frames_events->raise(std::make_pair(firstFrame, lastFrame));
    });

    server->map(packetTypes[IDX_INPUT_DATA], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_INPUT_DATA]"sv;

      auto tagged_cipher_length = util::endian::big(*(int32_t *) payload.data());
      std::string_view tagged_cipher { payload.data() + sizeof(tagged_cipher_length), (size_t) tagged_cipher_length };

      std::vector<uint8_t> plaintext;

      auto &cipher = session->control.cipher;
      auto &iv = session->control.legacy_input_enc_iv;
      if (cipher.decrypt(tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session);
        return;
      }

      if (tagged_cipher_length >= 16 + iv.size()) {
        std::copy(payload.end() - 16, payload.end(), std::begin(iv));
      }

      record_control_input_received(session, plaintext.size(), "legacy");
      input::passthrough(session->input, std::move(plaintext));
    });

    server->map(packetTypes[IDX_ENCRYPTED], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_ENCRYPTED]"sv;
      session->control.encrypted_rx_events++;

      auto header = (control_encrypted_p) (payload.data() - 2);

      auto length = util::endian::little(header->length);
      auto seq = util::endian::little(header->seq);
      session->control.last_encrypted_seq = seq;

      if (length < (16 + 4 + 4)) {
        BOOST_LOG(warning) << "Control: Runt packet"sv;
        return;
      }

      auto tagged_cipher_length = length - 4;
      std::string_view tagged_cipher { (char *) header->payload(), (size_t) tagged_cipher_length };

      auto &cipher = session->control.cipher;
      auto &iv = session->control.incoming_iv;
      if (session->config.encryptionFlagsEnabled & SS_ENC_CONTROL_V2) {
        // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
        // Section 8.2.1. The sequence number is our "invocation" field and the 'CC' in the
        // high bytes is the "fixed" field. Because each client provides their own unique
        // key, our values in the fixed field need only uniquely identify each independent
        // use of the client's key with AES-GCM in our code.
        //
        // The sequence number is 32 bits long which allows for 2^32 control stream messages
        // to be received from each client before the IV repeats.
        iv.resize(12);
        std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
        iv[10] = 'C';  // Client originated
        iv[11] = 'C';  // Control stream
      }
      else {
        // Nvidia's old style encryption uses a 16-byte IV
        iv.resize(16);

        iv[0] = (std::uint8_t) seq;
      }

      std::vector<uint8_t> plaintext;
      if (cipher.decrypt(tagged_cipher, plaintext, &iv)) {
        // something went wrong :(

        session->control.decrypt_failures++;
        BOOST_LOG(error) << "Failed to verify control tag runtime="sv
                         << session->identity.runtime_id
                         << " seq="sv << seq
                         << " payloadBytes="sv << payload.size()
                         << " failures="sv << session->control.decrypt_failures;

        session::stop(*session);
        return;
      }

      auto type = *(std::uint16_t *) plaintext.data();
      std::string_view next_payload { (char *) plaintext.data() + 4, plaintext.size() - 4 };
      session->control.decrypted_rx_events++;
      session->control.last_decrypted_type = type;
      session->control.last_payload_size = next_payload.size();

      const auto now = std::chrono::steady_clock::now();
      if (session->control.last_decrypt_diag_log.time_since_epoch().count() == 0 ||
          now - session->control.last_decrypt_diag_log >= 1000ms) {
        session->control.last_decrypt_diag_log = now;
        BOOST_LOG(info) << "Control encrypted packet decrypted runtime="sv
                        << session->identity.runtime_id
                        << " seq="sv << seq
                        << " type="sv << util::hex(type).to_string_view()
                        << " payloadBytes="sv << next_payload.size()
                        << " decrypted="sv << session->control.decrypted_rx_events;
        log_control_peer_diag(session, session->control.peer, "decrypt");
      }

      if (type == packetTypes[IDX_ENCRYPTED]) {
        BOOST_LOG(error) << "Bad packet type [IDX_ENCRYPTED] found"sv;
        session::stop(*session);
        return;
      }

      // IDX_INPUT_DATA callback will attempt to decrypt unencrypted data, therefore we need pass it directly
      if (type == packetTypes[IDX_INPUT_DATA]) {
        plaintext.erase(std::begin(plaintext), std::begin(plaintext) + 4);
        record_control_input_received(session, plaintext.size(), "encrypted");
        input::passthrough(session->input, std::move(plaintext));
      }
      else {
        server->call(type, session, next_payload, true);
      }
    });

    // This thread handles latency-sensitive control messages
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    // Check for both the full shutdown event and the shutdown event for this
    // broadcast to ensure we can inform connected clients of our graceful
    // termination when we shut down.
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    while (!shutdown_event->peek() && !broadcast_shutdown_event->peek()) {
      bool has_session_awaiting_peer = false;

      {
        auto lg = server->_sessions.lock();

        auto now = std::chrono::steady_clock::now();

        KITTY_WHILE_LOOP(auto pos = std::begin(*server->_sessions), pos != std::end(*server->_sessions), {
          // Don't perform additional session processing if we're shutting down
          if (shutdown_event->peek() || broadcast_shutdown_event->peek()) {
            break;
          }

          auto session = *pos;

          if (now > session->pingTimeout) {
            auto address = session->control.peer ? platf::from_sockaddr((sockaddr *) &session->control.peer->address.address) : session->control.expected_peer_address;
            BOOST_LOG(info) << address << ": Ping Timeout"sv;
            log_control_peer_diag(session, session->control.peer, "ping-timeout");
            session::stop(*session);
          }

          if (session->state.load(std::memory_order_acquire) == session::state_e::STOPPING) {
            update_host_cursor_suppression_for_session(session, false, "session-stopping");
            session->broadcast_ref->feature_leases.release_all(*session);
            server->_registry.unregister_session(session);
            pos = server->_sessions->erase(pos);


            if (session->control.peer) {
              {
                auto ptslg = server->_peer_to_session.lock();
                server->_peer_to_session->erase(session->control.peer);
              }

              enet_peer_disconnect_now(session->control.peer, 0);
            }

            session->controlEnd.raise(true);
            continue;
          }

          // Remember if we have a session that's waiting for a peer to connect to the
          // control stream. This ensures the clients are properly notified even when
          // the app terminates before they finish connecting.
          if (!session->control.peer) {
            has_session_awaiting_peer = true;
          }
          else {
            auto &feedback_queue = session->control.feedback_queue;
            while (feedback_queue->peek()) {
              auto feedback_msg = feedback_queue->pop();

              send_feedback_msg(session, *feedback_msg);
            }

            auto &hdr_queue = session->control.hdr_queue;
            while (session->control.peer && hdr_queue->peek()) {
              auto hdr_info = hdr_queue->pop();

              send_hdr_mode(session, std::move(hdr_info));
            }

            auto &resolution_change_queue = session->control.resolution_change_queue;
            while (session->control.peer && resolution_change_queue->peek()) {
              auto resolution = resolution_change_queue->pop();
              
              if (resolution) {
                send_resolution_change(session, resolution->first, resolution->second);
              }
            }

            maybe_send_cursor_plane_update(session, now);
            maybe_send_host_clipboard_update(session);
          }

          ++pos;
        })
      }

      // Don't break until any pending sessions either expire or connect
      if (proc::proc.running() == 0 && !has_session_awaiting_peer) {
        BOOST_LOG(info) << "Process terminated"sv;
        break;
      }

      server->iterate(20ms);
    }

    {
      auto lg = server->_sessions.lock();
      for (auto *session : *server->_sessions) {
        update_host_cursor_suppression_for_session(session, false, "control-thread-exit");
      }
    }

    // Let all remaining connections know the server is shutting down
    // reason: graceful termination
    std::uint32_t reason = 0x80030023;

    control_terminate_t plaintext;
    plaintext.header.type = packetTypes[IDX_TERMINATION];
    plaintext.header.payloadLength = sizeof(plaintext.ec);
    plaintext.ec = util::endian::big<uint32_t>(reason);

    std::array<std::uint8_t,
      sizeof(control_encrypted_t) + crypto::cipher::round_to_pkcs7_padded(sizeof(plaintext)) + crypto::cipher::tag_size>
      encrypted_payload;

    auto lg = server->_sessions.lock();
    for (auto pos = std::begin(*server->_sessions); pos != std::end(*server->_sessions); ++pos) {
      auto session = *pos;

      // We may not have gotten far enough to have an ENet connection yet
      if (session->control.peer) {
        auto payload = encode_control(session, util::view(plaintext), encrypted_payload);

        if (server->send(payload, session->control.peer)) {
          TUPLE_2D(port, addr, platf::from_sockaddr_ex((sockaddr *) &session->control.peer->address.address));
          BOOST_LOG(warning) << "Couldn't send termination code to ["sv << addr << ':' << port << ']';
        }
      }

      session->shutdown_event->raise(true);
      session->controlEnd.raise(true);
    }

    server->flush();
  }

  void
  micRecvThread(broadcast_ctx_t &ctx) {
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto &mic_io = ctx.mic_io_context;

    udp::endpoint peer;
    std::array<char, 2048> mic_recv_buffer;
    bool mic_device_initialized = false;

    // 麦克风统计结构体（按客户端地址分组）
    struct MicStats {
      uint64_t total_packets = 0;
      uint64_t decrypt_success = 0;
      uint64_t decrypt_failed = 0;
      uint64_t invalid_data = 0;
      uint64_t no_owner = 0;
      uint64_t lease_rejected = 0;
      uint64_t write_success = 0;
      uint64_t write_failed = 0;
    };
    std::map<std::string, MicStats> client_stats;

    // // SSRC验证辅助函数
    // auto validate_mic_ssrc = [](uint32_t ssrc, const std::string &client_id) -> bool {
    //   if (ssrc != MIC_PACKET_MAGIC) {
    //     BOOST_LOG(warning) << "Client " << client_id << " received invalid microphone packet type (SSRC: 0x" 
    //                       << std::hex << ssrc << std::dec << ")";
    //     return false;
    //   }
    //   return true;
    // };

    auto process_audio_data = [&](const uint8_t *audio_data,
                                  size_t data_size,
                                  uint16_t sequence_number,
                                  const std::string &peer_addr,
                                  const std::string &client_ip,
                                  const mic_session_token_t *session_token) {
      if (!ctx.mic_socket_enabled.load()) {
        return;
      }

      // 更新统计
      auto &stats = client_stats[peer_addr];
      stats.total_packets++;
      auto maybe_log_mic_stats = [&](const char *event) {
        const std::string_view event_view = event ? std::string_view { event } : std::string_view {};
        const bool important = stats.total_packets == 1 ||
                               stats.write_success == 1 ||
                               (stats.total_packets % 250) == 0 ||
                               (event_view == "no-owner" && (stats.no_owner == 1 || (stats.no_owner % 100) == 0)) ||
                               (event_view == "lease-rejected" && (stats.lease_rejected == 1 || (stats.lease_rejected % 100) == 0)) ||
                               (event_view == "decrypt-failed" && (stats.decrypt_failed == 1 || (stats.decrypt_failed % 100) == 0)) ||
                               (event_view == "write-failed-encrypted" && (stats.write_failed == 1 || (stats.write_failed % 25) == 0)) ||
                               (event_view == "write-failed-plaintext" && (stats.write_failed == 1 || (stats.write_failed % 25) == 0));
        if (!important) {
          return;
        }
        BOOST_LOG(info) << "Microphone receive stats"
                        << " event=" << event
                        << " peer=" << peer_addr
                        << " clientIp=" << client_ip
                        << " packets=" << stats.total_packets
                        << " decryptOk=" << stats.decrypt_success
                        << " decryptFail=" << stats.decrypt_failed
                        << " invalid=" << stats.invalid_data
                        << " noOwner=" << stats.no_owner
                        << " leaseReject=" << stats.lease_rejected
                        << " writeOk=" << stats.write_success
                        << " writeFail=" << stats.write_failed
                        << " micSocket=" << ctx.mic_socket_enabled.load()
                        << " micSessions=" << ctx.mic_sessions_count.load();
      };

      // 查找该客户端的 per-client 加密上下文
      // 仅在锁内拷贝 shared_ptr，解密和写入在锁外进行
      // 避免在持有 mutex 期间调用可能阻塞的 write_mic_data（含 WASAPI Sleep）
      boost::shared_ptr<broadcast_ctx_t::mic_cipher_ctx_t> cipher_ctx;
      session_runtime::owner_token_t mic_owner {
        session_runtime::feature_e::microphone,
      };
      {
        boost::lock_guard<boost::mutex> lg(ctx.mic_cipher_mutex);
        if (session_token != nullptr) {
          auto owner_it = ctx.mic_owners_by_token.find(*session_token);
          if (owner_it != ctx.mic_owners_by_token.end()) {
            mic_owner = owner_it->second;
          }
          auto cipher_it = ctx.mic_ciphers_by_token.find(*session_token);
          if (cipher_it != ctx.mic_ciphers_by_token.end()) {
            cipher_ctx = cipher_it->second;
          }
        }

        if (!mic_owner) {
          auto owner_it = ctx.mic_owners.find(client_ip);
          if (owner_it != ctx.mic_owners.end()) {
            mic_owner = owner_it->second;
          }
        }
        if (!cipher_ctx) {
          auto it = ctx.mic_ciphers.find(client_ip);
          if (it != ctx.mic_ciphers.end()) {
            cipher_ctx = it->second;
          }
        }
      }

      if (!mic_owner) {
        stats.no_owner++;
        maybe_log_mic_stats("no-owner");
        return;
      }
      if (!ctx.feature_leases.validate(mic_owner)) {
        stats.lease_rejected++;
        maybe_log_mic_stats("lease-rejected");
        return;
      }

      if (cipher_ctx) {
          // 根据 sequenceNumber 更新 IV
          // 客户端使用: baseIv[0:4] (Big Endian) + (sequenceNumber - 1) & 0xFFFF
          // 这与音频加密不同，音频加密使用: avRiKeyId + sequenceNumber
          // cipher_ctx->iv 的前 4 字节存储的是 baseIv（大端序），对应客户端的 remoteInputAesIv
          crypto::aes_t current_iv(16);  // 确保是 16 字节
          uint32_t baseIvVal = util::endian::big<std::uint32_t>(*(std::uint32_t *) cipher_ctx->iv.data());
          // 服务端收到的 sequence_number 就是包里的实际值，直接使用即可（不需要减1），客户端减1是因为它的 sequenceNumber 变量在写入包后就递增了
          uint32_t ivSeq = baseIvVal + (sequence_number & 0xFFFF);
          *(std::uint32_t *) current_iv.data() = util::endian::big<std::uint32_t>(ivSeq);
          // 确保后 12 字节为 0（客户端构建 IV 时后 12 字节也是 0）
          std::memset(current_iv.data() + 4, 0, 12);
          std::vector<std::uint8_t> plaintext;
          std::string_view cipher_view((const char *) audio_data, data_size);
          if (cipher_ctx->cipher.decrypt(cipher_view, plaintext, &current_iv) != 0) {
            // 解密失败：可能是网络损坏包、IV不匹配、或密钥错误
            stats.decrypt_failed++;
            maybe_log_mic_stats("decrypt-failed");
            return;  // 丢弃数据包
          }

          stats.decrypt_success++;

          if (plaintext.size() > 0) {
            // 简单的有效性检查：Opus 数据不应该全是 0 或全是 0xFF
            bool looks_valid = true;
            if (plaintext.size() >= 4) {
              uint8_t first_byte = plaintext[0];
              uint8_t second_byte = plaintext[1];
              uint8_t third_byte = plaintext[2];
              uint8_t fourth_byte = plaintext[3];
              bool all_zero = (first_byte == 0 && second_byte == 0 && third_byte == 0 && fourth_byte == 0);
              bool all_ff = (first_byte == 0xFF && second_byte == 0xFF && third_byte == 0xFF && fourth_byte == 0xFF);
              if (all_zero || all_ff) {
                looks_valid = false;
                stats.invalid_data++;
              }
            }
            // 注意：如果 plaintext.size() < 4，无法验证，假设有效并继续处理

            if (!looks_valid) {
              return;  // 丢弃数据包
            }
          }

          // 解密成功且数据看起来有效
          const int write_result = audio::write_mic_data(plaintext.data(), plaintext.size(), sequence_number);
          if (write_result >= 0) {
            stats.write_success++;
            maybe_log_mic_stats("write-ok-encrypted");
          }
          else {
            stats.write_failed++;
            maybe_log_mic_stats("write-failed-encrypted");
          }
          return;
      }

      // 该客户端没有注册加密上下文 — 视为明文数据

      // 安全模式：拒绝明文数据
      if (ctx.mic_reject_plaintext.load()) {
        BOOST_LOG(warning) << "Rejected plaintext microphone data (mic_reject_plaintext enabled)";
        stats.decrypt_failed++;
        maybe_log_mic_stats("plaintext-rejected");
        return;
      }

      // 未加密数据或加密未启用，直接处理
      // 也要统计未加密数据
      stats.decrypt_success++;  // 明文数据算作"成功"
      const int write_result = audio::write_mic_data(audio_data, data_size, sequence_number);
      if (write_result >= 0) {
        stats.write_success++;
        maybe_log_mic_stats("write-ok-plaintext");
      }
      else {
        stats.write_failed++;
        maybe_log_mic_stats("write-failed-plaintext");
      }
    };

    std::function<void(const boost::system::error_code, size_t)> mic_recv_func;
    mic_recv_func = [&](const boost::system::error_code &ec, size_t received_bytes) {
      if (!ctx.mic_socket_enabled.load()) {
        return;
      }

      // 致命错误（socket 已关闭/无效）：不重新注册接收，让 mic_io.run() 自然退出
      if (ec) {
        if (ec == boost::asio::error::operation_aborted ||
            ec == boost::asio::error::bad_descriptor ||
            ec == boost::system::errc::bad_file_descriptor ||
            ec == boost::system::errc::not_a_socket) {
          BOOST_LOG(debug) << "Mic socket closed: "sv << ec.message();
          return;
        }
      }

      // fail_guard：在此之后的任何 return 都会重新注册 async_receive_from
      // 包括瞬态错误（connection_refused/reset）和数据处理
      auto fg = util::fail_guard([&]() {
        if (ctx.mic_socket_enabled.load()) {
          ctx.mic_sock.async_receive_from(asio::buffer(mic_recv_buffer), peer, 0, mic_recv_func);
        }
      });

      // 瞬态错误（connection_refused/reset）：记录但继续接收
      // 这些通常是 ICMP 错误（客户端断开、端口不可达等），不应停止整个接收
      if (ec) {
        if (ec == boost::system::errc::connection_refused ||
            ec == boost::system::errc::connection_reset) {
          BOOST_LOG(debug) << "Mic socket transient error (ignored): "sv << ec.message();
        }
        else {
          BOOST_LOG(error) << "Mic socket error: "sv << ec.message();
        }
        return;  // fail_guard 会重新注册接收
      }

      if (received_bytes < sizeof(RTP_PACKET)) {
        return;
      }

      // 获取客户端标识：设备名拼接IP地址
      std::string client_ip = peer.address().to_string();
      std::string client_id;
      {
        boost::lock_guard<boost::mutex> lg(ctx.client_name_mutex);
        auto it = ctx.client_ip_to_name.find(client_ip);
        if (it != ctx.client_ip_to_name.end()) {
          client_id = it->second + "@" + client_ip;  // 设备名@IP
        } else {
          client_id = "@" + client_ip;  // 回退到IP（未知设备名时）
        }
      }

      auto read_mic_u16 = [&](std::size_t offset) -> std::uint16_t {
        std::uint16_t value = 0;
        if (offset + sizeof(value) <= received_bytes) {
          std::memcpy(&value, mic_recv_buffer.data() + offset, sizeof(value));
        }
        return util::endian::little(value);
      };

      auto try_session_mic_header = [&](std::size_t packet_type_offset,
                                          std::size_t sequence_offset,
                                          std::size_t token_offset,
                                          std::size_t header_size,
                                          const char *layout) -> bool {
        if (received_bytes < header_size) {
          return false;
        }
        if (read_mic_u16(packet_type_offset) != packetTypes[IDX_MIC_DATA]) {
          return false;
        }
        if (received_bytes > header_size) {
          const auto sequence_number = read_mic_u16(sequence_offset);
          mic_session_token_t session_token {};
          std::copy_n(mic_recv_buffer.data() + token_offset, session_token.size(), session_token.begin());
          BOOST_LOG(debug) << "Microphone packet session header layout=" << layout
                           << " bytes=" << received_bytes;
          process_audio_data(reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size,
                             received_bytes - header_size,
                             sequence_number,
                             client_id,
                             client_ip,
                             &session_token);
        }
        return true;
      };

      // 尝试16位扩展包类型。当前 Enhanced Moonlight common-c 曾经以未 pack 的
      // C struct 发送 session header（flags 后有 padding，header 为 32 字节），
      // 而 Sunshine 这里使用 pack(1) 结构（header 为 29 字节）。两者都接受，
      // 避免麦克风数据到达但因 header 对齐差异被静默丢弃。
      if (try_session_mic_header(offsetof(rtp_packet_session_ext_t, rtp.packetType),
                                 offsetof(rtp_packet_session_ext_t, rtp.sequenceNumber),
                                 offsetof(rtp_packet_session_ext_t, sessionToken),
                                 sizeof(rtp_packet_session_ext_t),
                                 "packed")) {
        return;
      }
      constexpr std::size_t padded_session_packet_type_offset = 2;
      constexpr std::size_t padded_session_sequence_offset = 4;
      constexpr std::size_t padded_session_token_offset = 16;
      constexpr std::size_t padded_session_header_size = 32;
      if (try_session_mic_header(padded_session_packet_type_offset,
                                 padded_session_sequence_offset,
                                 padded_session_token_offset,
                                 padded_session_header_size,
                                 "padded")) {
        return;
      }

      if (received_bytes >= sizeof(rtp_packet_ext_t)) {
        auto *header_ext = (rtp_packet_ext_t *) mic_recv_buffer.data();
        if (util::endian::little(header_ext->packetType) == packetTypes[IDX_MIC_DATA]) {
          size_t header_size = sizeof(rtp_packet_ext_t);
          if (received_bytes > header_size) {
            uint16_t sequence_number = util::endian::little(header_ext->sequenceNumber);
            // uint32_t ssrc = util::endian::little(header_ext->ssrc);  // 小端序
            // if (!validate_mic_ssrc(ssrc, client_id)) {
            //   return;
            // }
            process_audio_data(reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size,
                               received_bytes - header_size,
                               sequence_number,
                               client_id,
                               client_ip,
                               nullptr);
          }
          return;
        }
      }

      // 8位包类型
      auto *header = (mic_packet_t *) mic_recv_buffer.data();
      if (header->rtp.packetType == MIC_PACKET_TYPE_OPUS) {
        size_t header_size = sizeof(mic_packet_t);
        if (received_bytes > header_size) {
          // 客户端按小端序发送序列号（MicrophoneStream.java 使用 LITTLE_ENDIAN）
          // 服务端必须按小端序读取，否则会读错（比如 1 会读成 256）
          uint16_t sequence_number = util::endian::little(header->rtp.sequenceNumber);
          // uint32_t ssrc = util::endian::little(header->rtp.ssrc);  // 小端序
          // if (!validate_mic_ssrc(ssrc, client_id)) {
          //   return;
          // }
          size_t data_size = received_bytes - header_size;
          
          // BOOST_LOG(verbose) << "Received MIC packet: total=" << received_bytes 
          //                 << " bytes, header=" << header_size 
          //                 << " bytes, data=" << data_size 
          //                 << " bytes, sequenceNumber=" << sequence_number << " (little-endian)"
          //                 << " from " << client_id;
          process_audio_data(reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size,
                             data_size,
                             sequence_number,
                             client_id,
                             client_ip,
                             nullptr);
        }
      }
      else {
        auto &stats = client_stats[client_id];
        stats.total_packets++;
        stats.invalid_data++;
        if (stats.invalid_data == 1 || (stats.invalid_data % 250) == 0) {
          BOOST_LOG(info) << "Microphone receive stats"
                          << " event=unrecognized-header"
                          << " peer=" << client_id
                          << " bytes=" << received_bytes
                          << " first=" << static_cast<int>(static_cast<unsigned char>(mic_recv_buffer[0]))
                          << " second=" << (received_bytes > 1 ? static_cast<int>(static_cast<unsigned char>(mic_recv_buffer[1])) : -1)
                          << " invalid=" << stats.invalid_data
                          << " micSocket=" << ctx.mic_socket_enabled.load()
                          << " micSessions=" << ctx.mic_sessions_count.load();
        }
      }
    };

    BOOST_LOG(debug) << "Starting microphone receive thread";

    auto retry_delay = 300ms;  // 初始重试延迟，指数退避到最大5秒

    while (!broadcast_shutdown_event->peek()) {
      if (!ctx.mic_socket_enabled.load()) {
        retry_delay = 300ms;  // 会话结束时重置延迟

        // 重置设备初始化标志，下次会话重新初始化麦克风设备
        // （处理音频设备在运行中被卸载/重装的情况）
        if (mic_device_initialized) {
          audio::release_mic_redirect_device();
          mic_device_initialized = false;
          BOOST_LOG(debug) << "Microphone device released, will re-initialize on next session";
        }

        std::this_thread::sleep_for(100ms);
        continue;
      }

      // 延迟初始化麦克风设备
      if (!mic_device_initialized) {
        if (audio::init_mic_redirect_device() != 0) {
          std::this_thread::sleep_for(retry_delay);
          retry_delay = std::min(retry_delay * 2, 5000ms);  // 指数退避，最大5秒
          continue;
        }
        mic_device_initialized = true;
      }

      ctx.mic_sock.async_receive_from(asio::buffer(mic_recv_buffer), peer, 0, mic_recv_func);

      while (ctx.mic_socket_enabled.load() && !broadcast_shutdown_event->peek()) {
        mic_io.run();
      }
      mic_io.restart();  // 重置 io_context，以便下次会话可以重新进入 mic_io.run()
    }

    if (mic_device_initialized) {
      audio::release_mic_redirect_device();
    }

    // 打印所有客户端的麦克风解密统计
    if (!client_stats.empty()) {
      BOOST_LOG(info) << "=== Microphone Decryption Stats Summary ===";
      for (const auto &[client, stats] : client_stats) {
        if (stats.total_packets > 0) {
          double success_rate = (double)stats.decrypt_success / stats.total_packets * 100.0;
          BOOST_LOG(info) << "Client " << client << ": "
                         << "total=" << stats.total_packets
                         << ", success=" << stats.decrypt_success << " (" << std::fixed << std::setprecision(1) << success_rate << "%)"
                         << ", failed=" << stats.decrypt_failed
                         << ", invalid=" << stats.invalid_data
                         << ", noOwner=" << stats.no_owner
                         << ", leaseReject=" << stats.lease_rejected
                         << ", writeOk=" << stats.write_success
                         << ", writeFail=" << stats.write_failed;
        }
      }
    }

    BOOST_LOG(debug) << "Microphone receive thread ended";
  }

  void
  recvThread(broadcast_ctx_t &ctx) {
    std::unordered_map<av_session_id_t, message_queue_t> peer_to_video_session;
    std::unordered_map<av_session_id_t, message_queue_t> peer_to_audio_session;

    auto &video_sock = ctx.video_sock;
    auto &audio_sock = ctx.audio_sock;
    auto &message_queue_queue = ctx.message_queue_queue;
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto &io = ctx.io_context;

    udp::endpoint peer;
    std::array<std::array<char, 2048>, 2> buffers;
    std::array<std::function<void(const boost::system::error_code, size_t)>, 2> recv_funcs;

    // 统一处理PING包逻辑
    auto handle_ping = [](auto &session_map, auto &peer, auto &buf, size_t bytes, std::string_view type_str) {
      try {
        if (bytes == 4) {
          if (auto it = session_map.find(peer.address()); it != std::end(session_map)) {
            BOOST_LOG(debug) << "RAISE: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;
            it->second->raise(peer, std::string { buf.data(), bytes });
          }
        }
        else if (bytes >= sizeof(SS_PING)) {
          auto ping = (PSS_PING) buf.data();
          if (auto it = session_map.find(std::string { ping->payload, sizeof(ping->payload) }); it != std::end(session_map)) {
            BOOST_LOG(debug) << "RAISE: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;
            it->second->raise(peer, std::string { buf.data(), bytes });
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Error processing packet: " << e.what();
      }
    };

    // 更新会话映射
    auto update_session_map = [](auto &message_queue_queue, auto &video_map, auto &audio_map) {
      while (message_queue_queue->peek()) {
        if (auto message_queue_opt = message_queue_queue->pop()) {
          auto [socket_type, session_id, message_queue] = *message_queue_opt;
          auto &target_map = socket_type == socket_e::video ? video_map :
                                                              (socket_type == socket_e::audio ? audio_map : throw std::runtime_error("Unknown socket type"));

          if (message_queue) {
            target_map.emplace(session_id, message_queue);
          }
          else {
            target_map.erase(session_id);
          }
        }
      }
    };

    // 初始化接收函数
    auto init_recv_func = [&](auto &sock, size_t buf_idx, auto &session_map, std::string_view type_str) {
      recv_funcs[buf_idx] = [&, buf_idx, type_str](const boost::system::error_code &ec, size_t bytes) {
        // 静默处理正常关闭错误
        if (ec == boost::asio::error::operation_aborted ||
            ec == boost::asio::error::bad_descriptor) {
          return;  // Socket已关闭，不重新调度
        }

        // 静默处理网络连接错误
        if (ec == boost::system::errc::connection_refused ||
            ec == boost::system::errc::connection_reset) {
          return;  // 连接错误，不重新调度
        }

        // 如果有其他错误，记录并返回
        if (ec) {
          BOOST_LOG(error) << type_str << " receive error: "sv << ec.message();
          return;  // 有错误，不重新调度
        }

        BOOST_LOG(verbose) << "Recv: "sv << peer.address().to_string() << ':' << peer.port() << " :: " << type_str;

        update_session_map(message_queue_queue, peer_to_video_session, peer_to_audio_session);
        if (bytes == 0) {
          BOOST_LOG(warning) << "Received empty packet";
          // 即使是空包，也继续接收
        }
        else {
          handle_ping(session_map, peer, buffers[buf_idx], bytes, type_str);
        }

        // 只有在成功接收数据后才重新调度
        try {
          sock.async_receive_from(asio::buffer(buffers[buf_idx]), peer, 0, recv_funcs[buf_idx]);
        }
        catch (const std::exception &e) {
          BOOST_LOG(error) << "Failed to restart async receive: " << e.what();
        }
      };
    };

    try {
      init_recv_func(video_sock, 0, peer_to_video_session, "VIDEO");
      init_recv_func(audio_sock, 1, peer_to_audio_session, "AUDIO");

      video_sock.async_receive_from(asio::buffer(buffers[0]), peer, 0, recv_funcs[0]);
      audio_sock.async_receive_from(asio::buffer(buffers[1]), peer, 0, recv_funcs[1]);

      while (!broadcast_shutdown_event->peek()) {
        io.run();
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(fatal) << "recvThread exception: " << e.what();
    }
  }

  void
  videoBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto video_epoch = std::chrono::steady_clock::now();

    // Video traffic is sent on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    logging::min_max_avg_periodic_logger<double> frame_processing_latency_logger(debug, "Frame processing latency", "ms");

    logging::time_delta_periodic_logger frame_send_batch_latency_logger(debug, "Network: each send_batch() latency");
    logging::time_delta_periodic_logger frame_fec_latency_logger(debug, "Network: each FEC block latency");
    logging::time_delta_periodic_logger frame_network_latency_logger(debug, "Network: frame's overall network latency");

    crypto::aes_t iv(12);

    auto timer = platf::create_high_precision_timer();
    if (!timer || !*timer) {
      BOOST_LOG(error) << "Failed to create timer, aborting video broadcast thread";
      return;
    }

    auto ratecontrol_next_frame_start = std::chrono::steady_clock::now();
    auto last_video_send_summary = std::chrono::steady_clock::now();
    std::uint64_t sent_summary_frames = 0;
    std::uint64_t sent_summary_dupes = 0;
    std::uint64_t sent_summary_shards = 0;
    std::uint64_t sent_summary_bytes = 0;

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      frame_network_latency_logger.first_point_now();

      auto session = (session_t *) packet->channel_data;
      auto lowseq = session->video.lowseq;

      std::string_view payload { (char *) packet->data(), packet->data_size() };
      std::vector<uint8_t> payload_with_replacements;

      // Apply replacements on the packet payload before performing any other operations.
      // We need to know the final frame size to calculate the last packet size, and we
      // must avoid matching replacements against the frame header or any other non-video
      // part of the payload.
      if (packet->is_idr() && packet->replacements) {
        for (auto &replacement : *packet->replacements) {
          auto frame_old = replacement.old;
          auto frame_new = replacement._new;

          payload_with_replacements = replace(payload, frame_old, frame_new);
          payload = { (char *) payload_with_replacements.data(), payload_with_replacements.size() };
        }
      }

      video_short_frame_header_t frame_header = {};
      frame_header.headerType = 0x01;  // Short header type
      frame_header.frameType = packet->is_idr()                     ? 2 :
                               packet->after_ref_frame_invalidation ? 5 :
                                                                      1;
      frame_header.lastPayloadLen = (payload.size() + sizeof(frame_header)) % (session->config.packetsize - sizeof(NV_VIDEO_PACKET));
      if (frame_header.lastPayloadLen == 0) {
        frame_header.lastPayloadLen = session->config.packetsize - sizeof(NV_VIDEO_PACKET);
      }

      if (packet->frame_timestamp) {
        auto duration_to_latency = [](const std::chrono::steady_clock::duration &duration) {
          const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
          return (uint16_t) std::clamp<decltype(duration_us)>((duration_us + 50) / 100, 0, std::numeric_limits<uint16_t>::max());
        };

        uint16_t latency = duration_to_latency(std::chrono::steady_clock::now() - *packet->frame_timestamp);
        frame_header.frame_processing_latency = latency;
        frame_processing_latency_logger.collect_and_log(latency / 10.);
      }
      else {
        frame_header.frame_processing_latency = 0;
      }

      auto fecPercentage = std::clamp(session->current_fec_percentage.load(std::memory_order_relaxed), 0, 100);

      // Insert space for packet headers
      auto blocksize = session->config.packetsize + MAX_RTP_HEADER_SIZE;
      auto payload_blocksize = blocksize - sizeof(video_packet_raw_t);
      auto payload_new = concat_and_insert(sizeof(video_packet_raw_t), payload_blocksize,
        std::string_view { (char *) &frame_header, sizeof(frame_header) }, payload);

      payload = std::string_view { (char *) payload_new.data(), payload_new.size() };

      // There are 2 bits for FEC block count for a maximum of 4 FEC blocks
      constexpr auto MAX_FEC_BLOCKS = 4;

      // The max number of data shards per block is found by solving this system of equations for D:
      // D = 255 - P
      // P = D * F
      // which results in the solution:
      // D = 255 / (1 + F)
      // multiplied by 100 since F is the percentage as an integer:
      // D = (255 * 100) / (100 + F)
      auto max_data_shards_per_fec_block = (DATA_SHARDS_MAX * 100) / (100 + fecPercentage);

      // Compute the number of FEC blocks needed for this frame using the block size and max shards
      auto max_data_per_fec_block = max_data_shards_per_fec_block * blocksize;
      auto fec_blocks_needed = (payload.size() + (max_data_per_fec_block - 1)) / max_data_per_fec_block;

      // If the number of FEC blocks needed exceeds the protocol limit, turn off FEC for this frame.
      // For normal FEC percentages, this should only happen for enormous frames (over 800 packets at 20%).
      if (fec_blocks_needed > MAX_FEC_BLOCKS) {
        BOOST_LOG(warning) << "Skipping FEC for abnormally large encoded frame (needed "sv << fec_blocks_needed << " FEC blocks)"sv;
        session->stream_quality_large_frame_fec_skipped.fetch_add(1, std::memory_order_relaxed);
        fecPercentage = 0;
        fec_blocks_needed = MAX_FEC_BLOCKS;
      }

      std::array<std::string_view, MAX_FEC_BLOCKS> fec_blocks;
      decltype(fec_blocks)::iterator
        fec_blocks_begin = std::begin(fec_blocks),
        fec_blocks_end = std::begin(fec_blocks) + fec_blocks_needed;

      BOOST_LOG(verbose) << "Generating "sv << fec_blocks_needed << " FEC blocks"sv;
      const bool packet_started_without_timestamp = !packet->frame_timestamp;
      std::uint64_t frame_sent_shards = 0;
      std::uint64_t frame_sent_bytes = 0;

      // Align individual FEC blocks to blocksize
      auto unaligned_size = payload.size() / fec_blocks_needed;
      auto aligned_size = ((unaligned_size + (blocksize - 1)) / blocksize) * blocksize;

      // If we exceed the 10-bit FEC packet index (which means our frame exceeded 4096 packets),
      // the frame will be unrecoverable. Log an error for this case.
      if (aligned_size / blocksize >= 1024) {
        BOOST_LOG(error) << "Encoder produced a frame too large to send! Is the encoder broken? (needed "sv << (aligned_size / blocksize) << " packets)"sv;
      }

      // Split the data into aligned FEC blocks
      for (int x = 0; x < fec_blocks_needed; ++x) {
        if (x == fec_blocks_needed - 1) {
          // The last block must extend to the end of the payload
          fec_blocks[x] = payload.substr(x * aligned_size);
        }
        else {
          // Earlier blocks just extend to the next block offset
          fec_blocks[x] = payload.substr(x * aligned_size, aligned_size);
        }
      }

      try {
        auto pacing_total_kbps = session->pacing_total_bitrate.load(std::memory_order_relaxed);
        if (pacing_total_kbps <= 0) {
          pacing_total_kbps = std::max(session->current_total_bitrate.load(std::memory_order_relaxed), session->config.monitor.bitrate);
        }
        const auto pacing_bytes_per_second = std::max<std::uint64_t>(
          125000,
          static_cast<std::uint64_t>(pacing_total_kbps) * 1000 / 8);
        auto ratecontrol_packet_interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::duration<double>(static_cast<double>(blocksize) / static_cast<double>(pacing_bytes_per_second)));

        // Send less than 64K in a single batch.
        // On Windows, batches above 64K seem to bypass SO_SNDBUF regardless of its size,
        // appear in "Other I/O" and begin waiting for interrupts.
        // This gives inconsistent performance so we'd rather avoid it.
        size_t burst_bytes = std::clamp<size_t>(
          static_cast<size_t>(pacing_bytes_per_second / 200),
          blocksize,
          64 * 1024);
        size_t send_batch_size = std::max<size_t>(1, burst_bytes / blocksize);
        // Also don't exceed 64 packets, which can happen when Moonlight requests
        // unusually small packet size.
        // Generic Segmentation Offload on Linux can't do more than 64.
        send_batch_size = std::min<size_t>(64, send_batch_size);

        // Don't ignore the last ratecontrol group of the previous frame
        auto ratecontrol_frame_start = std::max(ratecontrol_next_frame_start, std::chrono::steady_clock::now());

        size_t ratecontrol_frame_packets_sent = 0;

        auto blockIndex = 0;
        std::for_each(fec_blocks_begin, fec_blocks_end, [&](std::string_view &current_payload) {
          auto packets = (current_payload.size() + (blocksize - 1)) / blocksize;

          for (int x = 0; x < packets; ++x) {
            auto *inspect = (video_packet_raw_t *) &current_payload[x * blocksize];

            inspect->packet.frameIndex = packet->frame_index();
            inspect->packet.streamPacketIndex = ((uint32_t) lowseq + x) << 8;

            // Match multiFecFlags with Moonlight
            inspect->packet.multiFecFlags = 0x10;
            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);

            inspect->packet.flags = FLAG_CONTAINS_PIC_DATA;
            if (x == 0) {
              inspect->packet.flags |= FLAG_SOF;
            }
            if (x == packets - 1) {
              inspect->packet.flags |= FLAG_EOF;
            }
          }

          frame_fec_latency_logger.first_point_now();
          // If video encryption is enabled, we allocate space for the encryption header before each shard
          auto shards = fec::encode(current_payload, blocksize, fecPercentage, session->config.minRequiredFecPackets,
            session->video.cipher ? sizeof(video_packet_enc_prefix_t) : 0);
          frame_fec_latency_logger.second_point_now_and_log();
          frame_sent_shards += shards.size();
          frame_sent_bytes += static_cast<std::uint64_t>(shards.size()) *
                              static_cast<std::uint64_t>(shards.blocksize + shards.prefixsize);

          auto peer_address = session->video.peer.address();
          auto batch_info = platf::batched_send_info_t {
            shards.headers.begin(),
            shards.prefixsize,
            shards.payload_buffers,
            shards.blocksize,
            0,
            0,
            (uintptr_t) sock.native_handle(),
            peer_address,
            session->video.peer.port(),
            session->localAddress,
          };

          size_t next_shard_to_send = 0;

          // RTP video timestamps use a 90 KHz clock and the frame_timestamp from when the frame was captured
          // When a timestamp isn't available (duplicate frames), the timestamp from rate control is used instead.
          bool frame_is_dupe = false;
          if (!packet->frame_timestamp) {
            packet->frame_timestamp = ratecontrol_next_frame_start;
            frame_is_dupe = true;
          }
          using rtp_tick = std::chrono::duration<uint32_t, std::ratio<1, 90000>>;
          uint32_t timestamp = std::chrono::round<rtp_tick>(*packet->frame_timestamp - video_epoch).count();

          // set FEC info now that we know for sure what our percentage will be for this frame
          for (auto x = 0; x < shards.size(); ++x) {
            auto *inspect = (video_packet_raw_t *) shards.data(x);

            inspect->packet.fecInfo =
              (x << 12 |
                shards.data_shards << 22 |
                shards.percentage << 4);

            inspect->rtp.header = 0x80 | FLAG_EXTENSION;
            inspect->rtp.sequenceNumber = util::endian::big<uint16_t>(lowseq + x);
            inspect->rtp.timestamp = util::endian::big<uint32_t>(timestamp);

            inspect->packet.multiFecBlocks = (blockIndex << 4) | ((fec_blocks_needed - 1) << 6);
            inspect->packet.frameIndex = packet->frame_index();

            // Encrypt this shard if video encryption is enabled
            if (session->video.cipher) {
              // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
              // Section 8.2.1. The sequence number is our "invocation" field and the 'V' in the
              // high bytes is the "fixed" field. Because each client provides their own unique
              // key, our values in the fixed field need only uniquely identify each independent
              // use of the client's key with AES-GCM in our code.
              //
              // The IV counter is 64 bits long which allows for 2^64 encrypted video packets
              // to be sent to each client before the IV repeats.
              std::copy_n((uint8_t *) &session->video.gcm_iv_counter, sizeof(session->video.gcm_iv_counter), std::begin(iv));
              iv[11] = 'V';  // Video stream
              session->video.gcm_iv_counter++;

              // Encrypt the target buffer in place
              auto *prefix = (video_packet_enc_prefix_t *) shards.prefix(x);
              prefix->frameNumber = packet->frame_index();
              std::copy(std::begin(iv), std::end(iv), prefix->iv);
              session->video.cipher->encrypt(std::string_view { (char *) inspect, (size_t) blocksize },
                prefix->tag, (uint8_t *) inspect, &iv);
            }

            if (x - next_shard_to_send + 1 >= send_batch_size ||
                x + 1 == shards.size()) {
              auto due = ratecontrol_frame_start + ratecontrol_packet_interval * ratecontrol_frame_packets_sent;
              auto now = std::chrono::steady_clock::now();
              if (now < due) {
                timer->sleep_for(due - now);
              }

              size_t current_batch_size = x - next_shard_to_send + 1;
              batch_info.block_offset = next_shard_to_send;
              batch_info.block_count = current_batch_size;

              frame_send_batch_latency_logger.first_point_now();
              // Use a batched send if it's supported on this platform
              if (!platf::send_batch(batch_info)) {
                // Batched send is not available, so send each packet individually
                BOOST_LOG(verbose) << "Falling back to unbatched send"sv;
                for (auto y = 0; y < current_batch_size; y++) {
                  auto send_info = platf::send_info_t {
                    shards.prefix(next_shard_to_send + y),
                    shards.prefixsize,
                    shards.data(next_shard_to_send + y),
                    shards.blocksize,
                    (uintptr_t) sock.native_handle(),
                    peer_address,
                    session->video.peer.port(),
                    session->localAddress,
                  };

                  platf::send(send_info);
                }
              }
              frame_send_batch_latency_logger.second_point_now_and_log();

              ratecontrol_frame_packets_sent += current_batch_size;
              next_shard_to_send = x + 1;
            }
          }

          // remember this in case the next frame comes immediately
          ratecontrol_next_frame_start = ratecontrol_frame_start +
                                         ratecontrol_packet_interval * ratecontrol_frame_packets_sent;

          frame_network_latency_logger.second_point_now_and_log();

          BOOST_LOG(verbose) << "Sent Frame seq ["sv << packet->frame_index() << "] pts ["sv << timestamp
                             << "] shards ["sv << shards.size() << "/"sv << shards.percentage << "%]"sv
                             << (frame_is_dupe ? " Dupe" : "")
                             << (packet->is_idr() ? " Key" : "")
                             << (packet->after_ref_frame_invalidation ? " RFI" : "");

          ++blockIndex;
          lowseq += shards.size();
        });

        session->video.lowseq = lowseq;
        ++sent_summary_frames;
        sent_summary_dupes += packet_started_without_timestamp ? 1U : 0U;
        sent_summary_shards += frame_sent_shards;
        sent_summary_bytes += frame_sent_bytes;
        const auto summary_now = std::chrono::steady_clock::now();
        if (summary_now - last_video_send_summary >= 1000ms) {
          BOOST_LOG(info) << "Video send summary runtime=" << session->identity.runtime_id
                          << " frames=" << sent_summary_frames
                          << " dupes=" << sent_summary_dupes
                          << " shards=" << sent_summary_shards
                          << " bytes=" << sent_summary_bytes
                          << " fec=" << fecPercentage << "%"
                          << " pacing=" << session->pacing_total_bitrate.load(std::memory_order_relaxed)
                          << " Kbps"
                          << " peer=" << session->video.peer.address()
                          << ':' << session->video.peer.port();
          sent_summary_frames = 0;
          sent_summary_dupes = 0;
          sent_summary_shards = 0;
          sent_summary_bytes = 0;
          last_video_send_summary = summary_now;
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast video failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  void
  audioBroadcastThread(udp::socket &sock) {
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    audio_packet_t audio_packet;
    fec::rs_t rs { reed_solomon_new(RTPA_DATA_SHARDS, RTPA_FEC_SHARDS) };
    crypto::aes_t iv(16);

    // For unknown reasons, the RS parity matrix computed by our RS implementation
    // doesn't match the one Nvidia uses for audio data. I'm not exactly sure why,
    // but we can simply replace it with the matrix generated by OpenFEC which
    // works correctly. This is possible because the data and FEC shard count is
    // constant and known in advance.
    const unsigned char parity[] = { 0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c };
    memcpy(rs.get()->p, parity, sizeof(parity));

    audio_packet.rtp.header = 0x80;
    audio_packet.rtp.packetType = 97;
    audio_packet.rtp.ssrc = 0;

    // Audio traffic is sent on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }

      TUPLE_2D_REF(channel_data, packet_data, *packet);
      auto session = (session_t *) channel_data;

      auto sequenceNumber = session->audio.sequenceNumber;
      auto timestamp = session->audio.timestamp;

      *(std::uint32_t *) iv.data() = util::endian::big<std::uint32_t>(session->audio.avRiKeyId + sequenceNumber);

      auto &shards_p = session->audio.shards_p;

      // 检查客户端是否启用了音频加密
      bool audio_encryption_enabled = (session->config.encryptionFlagsEnabled & SS_ENC_AUDIO) != 0;
      if (sequenceNumber == 0) {
        // 只在第一个包时记录一次，避免日志过多
        BOOST_LOG(info) << "Audio encryption status: encryptionFlagsEnabled=0x" 
                        << std::hex << session->config.encryptionFlagsEnabled << std::dec
                        << ", SS_ENC_AUDIO (0x04) check: " << (audio_encryption_enabled ? "enabled" : "disabled")
                        << ", will " << (audio_encryption_enabled ? "ENCRYPT" : "NOT encrypt") << " audio data";
      }

      size_t plaintext_size = packet_data.size();
      
      // 验证 cipher 是否已初始化
      if (sequenceNumber == 0) {
        bool cipher_initialized = (session->audio.cipher.key.size() > 0);
        BOOST_LOG(info) << "Audio cipher status: initialized=" << (cipher_initialized ? "yes" : "no")
                        << ", key_size=" << session->audio.cipher.key.size();
      }
      
      auto bytes = encode_audio(audio_encryption_enabled, packet_data,
        shards_p[sequenceNumber % RTPA_DATA_SHARDS], iv, session->audio.cipher);
      
      if (sequenceNumber == 0) {
        // 验证加密是否真的执行了
        if (audio_encryption_enabled) {
          // 加密后的大小应该大于等于明文（因为 PKCS5 填充）
          bool encryption_applied = (bytes >= plaintext_size && bytes % 16 == 0);
          BOOST_LOG(info) << "Audio packet encryption: plaintext_size=" << plaintext_size 
                          << ", encrypted_size=" << bytes
                          << ", encryption " << (encryption_applied ? "SUCCESS (data is encrypted)" : "FAILED (data may not be encrypted)");
        } else {
          BOOST_LOG(info) << "Audio packet: plaintext_size=" << plaintext_size 
                          << ", output_size=" << bytes
                          << " (NOT encrypted, sent as plaintext)";
        }
      }
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio packet"sv;
        break;
      }

      BOOST_LOG(verbose) << "Audio [seq "sv << sequenceNumber << ", pts "sv << timestamp << "] ::  send..."sv;

      audio_packet.rtp.sequenceNumber = util::endian::big(sequenceNumber);
      audio_packet.rtp.timestamp = util::endian::big(timestamp);

      session->audio.sequenceNumber++;
      session->audio.timestamp += session->config.audio.packetDuration;

      auto peer_address = session->audio.peer.address();
      try {
        auto send_info = platf::send_info_t {
          (const char *) &audio_packet,
          sizeof(audio_packet),
          (const char *) shards_p[sequenceNumber % RTPA_DATA_SHARDS],
          (size_t) bytes,
          (uintptr_t) sock.native_handle(),
          peer_address,
          session->audio.peer.port(),
          session->localAddress,
        };
        platf::send(send_info);

        auto &fec_packet = session->audio.fec_packet;
        // initialize the FEC header at the beginning of the FEC block
        if (sequenceNumber % RTPA_DATA_SHARDS == 0) {
          fec_packet.fecHeader.baseSequenceNumber = util::endian::big(sequenceNumber);
          fec_packet.fecHeader.baseTimestamp = util::endian::big(timestamp);
        }

        // generate parity shards at the end of the FEC block
        if ((sequenceNumber + 1) % RTPA_DATA_SHARDS == 0) {
          reed_solomon_encode(rs.get(), shards_p.begin(), RTPA_TOTAL_SHARDS, bytes);

          for (auto x = 0; x < RTPA_FEC_SHARDS; ++x) {
            fec_packet.rtp.sequenceNumber = util::endian::big<std::uint16_t>(sequenceNumber + x + 1);
            fec_packet.fecHeader.fecShardIndex = x;

            auto send_info = platf::send_info_t {
              (const char *) &fec_packet,
              sizeof(fec_packet),
              (const char *) shards_p[RTPA_DATA_SHARDS + x],
              (size_t) bytes,
              (uintptr_t) sock.native_handle(),
              peer_address,
              session->audio.peer.port(),
              session->localAddress,
            };
            platf::send(send_info);
            BOOST_LOG(verbose) << "Audio FEC ["sv << (sequenceNumber & ~(RTPA_DATA_SHARDS - 1)) << ' ' << x << "] ::  send..."sv;
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Broadcast audio failed "sv << e.what();
        std::this_thread::sleep_for(100ms);
      }
    }

    shutdown_event->raise(true);
  }

  int
  start_broadcast(broadcast_ctx_t &ctx) {
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    auto protocol = address_family == net::IPV4 ? udp::v4() : udp::v6();
    auto control_port = net::map_port(CONTROL_PORT);
    auto video_port = net::map_port(VIDEO_STREAM_PORT);
    auto audio_port = net::map_port(AUDIO_STREAM_PORT);
    auto mic_port = net::map_port(MIC_STREAM_PORT);

    if (ctx.control_server.bind(address_family, control_port)) {
      BOOST_LOG(error) << "Couldn't bind Control server to port ["sv << control_port << "], likely another process already bound to the port"sv;

      return -1;
    }

    boost::system::error_code ec;
    ctx.video_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Video server: "sv << ec.message();

      return -1;
    }

    // Set video socket send buffer size (SO_SENDBUF) to 1MB
    try {
      ctx.video_sock.set_option(boost::asio::socket_base::send_buffer_size(1024 * 1024));
    }
    catch (...) {
      BOOST_LOG(error) << "Failed to set video socket send buffer size (SO_SENDBUF)";
    }

    auto bind_addr_str = net::get_bind_address(address_family);
    const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
      return -1;
    }

    ctx.video_sock.bind(udp::endpoint(bind_addr, video_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Video server to port ["sv << video_port << "]: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.open(protocol, ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't open socket for Audio server: "sv << ec.message();

      return -1;
    }

    ctx.audio_sock.bind(udp::endpoint(bind_addr, audio_port), ec);
    if (ec) {
      BOOST_LOG(fatal) << "Couldn't bind Audio server to port ["sv << audio_port << "]: "sv << ec.message();

      return -1;
    }

    // 仅在启用麦克风串流时启动麦克风socket
    if (config::audio.stream_mic) {
      ctx.mic_sock.open(protocol, ec);
      if (ec) {
        BOOST_LOG(fatal) << "Couldn't open socket for Microphone server: "sv << ec.message();
        return -1;
      }
      ctx.mic_sock.bind(udp::endpoint(protocol, mic_port), ec);
      if (ec) {
        BOOST_LOG(fatal) << "Couldn't bind Microphone server to port ["sv << mic_port << "]: "sv << ec.message();
        return -1;
      }
      ctx.mic_socket_enabled.store(true);
      BOOST_LOG(info) << "Microphone socket started on port " << mic_port;
    } else {
      BOOST_LOG(info) << "Microphone streaming disabled by config";
    }

    ctx.message_queue_queue = std::make_shared<message_queue_queue_t::element_type>(30);

    ctx.video_thread = std::thread { videoBroadcastThread, std::ref(ctx.video_sock) };
    ctx.audio_thread = std::thread { audioBroadcastThread, std::ref(ctx.audio_sock) };
    ctx.control_thread = std::thread { controlBroadcastThread, &ctx.control_server };

    ctx.recv_thread = std::thread { recvThread, std::ref(ctx) };
    ctx.mic_thread = std::thread { micRecvThread, std::ref(ctx) };

    return 0;
  }

  void
  end_broadcast(broadcast_ctx_t &ctx) {
    auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

    broadcast_shutdown_event->raise(true);

    auto video_packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mail::man->queue<audio::packet_t>(mail::audio_packets);

    // Minimize delay stopping video/audio threads
    video_packets->stop();
    audio_packets->stop();

    ctx.message_queue_queue->stop();
    ctx.io_context.stop();
    ctx.mic_io_context.stop();

    ctx.video_sock.close();
    ctx.audio_sock.close();

    if (ctx.mic_socket_enabled.load()) {
      ctx.mic_socket_enabled.store(false);
      ctx.mic_sock.close();
      ctx.mic_sessions_count.store(0);
      
      reset_mic_encryption(ctx);
      
      BOOST_LOG(debug) << "Microphone socket closed and encryption context securely cleared";
    }

    video_packets.reset();
    audio_packets.reset();

    BOOST_LOG(debug) << "Waiting for main listening thread to end..."sv;
    ctx.recv_thread.join();
    BOOST_LOG(debug) << "Waiting for main video thread to end..."sv;
    ctx.video_thread.join();
    BOOST_LOG(debug) << "Waiting for main audio thread to end..."sv;
    ctx.audio_thread.join();
    BOOST_LOG(debug) << "Waiting for main control thread to end..."sv;
    ctx.control_thread.join();
    BOOST_LOG(debug) << "Waiting for microphone thread to end..."sv;
    ctx.mic_thread.join();
    BOOST_LOG(debug) << "All broadcasting threads ended"sv;

    broadcast_shutdown_event->reset();
  }

  int
  recv_ping(session_t *session, decltype(broadcast_shared)::ptr_t ref, socket_e type, std::string_view expected_payload, udp::endpoint &peer, std::chrono::milliseconds timeout) {
    auto messages = std::make_shared<message_queue_t::element_type>(30);
    av_session_id_t session_id = std::string { expected_payload };
    const auto type_name = socket_name(type);

    // Only allow matches on the peer address for legacy clients
    if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID)) {
      ref->message_queue_queue->raise(type, peer.address(), messages);
    }
    ref->message_queue_queue->raise(type, session_id, messages);

    auto fg = util::fail_guard([&]() {
      messages->stop();

      // remove message queue from session
      if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID)) {
        ref->message_queue_queue->raise(type, peer.address(), nullptr);
      }
      ref->message_queue_queue->raise(type, session_id, nullptr);
    });

    auto start_time = std::chrono::steady_clock::now();
    auto current_time = start_time;
    auto deadline = start_time + timeout;
    std::size_t non_ping_count = 0;
    std::size_t last_non_ping_size = 0;
    udp::endpoint last_non_ping_peer;

    while (current_time < deadline) {
      if (session->shutdown_event->peek()) {
        BOOST_LOG(info) << "Initial "sv << type_name << " ping wait aborted by session shutdown runtime="sv
                        << session->identity.runtime_id
                        << " waitedMs="sv << std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time).count()
                        << " nonPing="sv << non_ping_count;
        return -1;
      }

      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - current_time);
      const auto wait_time = std::min(remaining, 100ms);
      auto msg_opt = messages->pop(wait_time);
      if (!msg_opt) {
        current_time = std::chrono::steady_clock::now();
        continue;
      }

      TUPLE_2D_REF(recv_peer, msg, *msg_opt);
      if (msg.find(expected_payload) != std::string::npos) {
        // Match the new PING payload format
        BOOST_LOG(debug) << "Received "sv << type_name << " ping [v2] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      }
      else if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID) && msg == "PING"sv) {
        // Match the legacy fixed PING payload only if the new type is not supported
        BOOST_LOG(debug) << "Received "sv << type_name << " ping [v1] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      }
      else {
        ++non_ping_count;
        last_non_ping_size = msg.size();
        last_non_ping_peer = recv_peer;
        BOOST_LOG(debug) << "Received non-"sv << type_name << "-ping from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
        current_time = std::chrono::steady_clock::now();
        continue;
      }

      // Update connection details.
      peer = recv_peer;
      BOOST_LOG(info) << "Initial "sv << type_name << " ping received from "sv << peer.address() << ':' << peer.port()
                      << " runtime="sv << session->identity.runtime_id;
      return 0;
    }

    BOOST_LOG(error) << "Initial "sv << type_name << " ping timeout runtime="sv << session->identity.runtime_id
                     << " timeoutMs="sv << timeout.count()
                     << " expectedPayload="sv << util::hex_vec(expected_payload)
                     << " nonPing="sv << non_ping_count
                     << " lastNonPingBytes="sv << last_non_ping_size
                     << " lastNonPingPeer="sv << last_non_ping_peer.address() << ':' << last_non_ping_peer.port();
    return -1;
  }

  void
  videoThread(session_t *session) {
    auto fg = util::fail_guard([&]() {
      session::stop(*session);
    });

    while_starting_do_nothing(session->state);

    auto ref = broadcast_shared.ref();
    auto error = recv_ping(session, ref, socket_e::video, session->video.ping_payload, session->video.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on video traffic if requested by the client
    auto address = session->video.peer.address();
    session->video.qos = platf::enable_socket_qos(ref->video_sock.native_handle(), address,
      session->video.peer.port(), platf::qos_data_type_e::video, session->config.videoQosType != 0);

    BOOST_LOG(info) << "Start capturing Video cursorPlane="
                    << (session->config.monitor.preferCursorPlane ? 1 : 0)
                    << " mlFeatureFlags2=0x" << std::hex << session->config.mlFeatureFlags2 << std::dec
                    << " runtime=" << session->identity.runtime_id;
    if (session->config.monitor.preferCursorPlane) {
      update_host_cursor_suppression_for_session(session, true, "video-thread-start");
    }
    // Debug: Log the display_name before calling video::capture
    BOOST_LOG(debug) << "stream.cpp: session->config.monitor.display_name = [" << (session->config.monitor.display_name.empty() ? "<empty>" : session->config.monitor.display_name) << "]";
    video::capture(session->mail,
                   session->config.monitor,
                   session,
                   session->video.dynamic_param_change_events,
                   record_frame_interest_feedback,
                   is_control_input_recent,
                   startup_video_pacing_interval);
  }

  void
  audioThread(session_t *session) {
    while_starting_do_nothing(session->state);

    auto ref = broadcast_shared.ref();
    auto error = recv_ping(session, ref, socket_e::audio, session->audio.ping_payload, session->audio.peer, config::stream.ping_timeout);
    if (error < 0) {
      BOOST_LOG(warning) << "Audio stream did not receive its initial ping; continuing video/control session without host audio runtime="sv
                         << session->identity.runtime_id;
      return;
    }

    // Enable local prioritization and QoS tagging on audio traffic if requested by the client
    auto address = session->audio.peer.address();
    session->audio.qos = platf::enable_socket_qos(ref->audio_sock.native_handle(), address,
      session->audio.peer.port(), platf::qos_data_type_e::audio, session->config.audioQosType != 0);

    BOOST_LOG(debug) << "Start capturing Audio"sv;
    audio::capture(session->mail, session->config.audio, session);
    if (session->state.load(std::memory_order_acquire) == session::state_e::RUNNING) {
      BOOST_LOG(warning) << "Audio capture ended while session is still running; keeping video/control session alive runtime="sv
                         << session->identity.runtime_id;
    }
  }

  namespace session {
    std::atomic_uint running_sessions;
    std::atomic_uint running_non_control_only_sessions;  // 跟踪非仅控制流会话的数量
    std::atomic_uint teardown_sessions;

    state_e
    state(session_t &session) {
      return session.state.load(std::memory_order_relaxed);
    }

    std::uint32_t
    teardown_count() {
      return teardown_sessions.load(std::memory_order_relaxed);
    }

    static std::string
    session_peer_address(const session_t &session) {
      if (session.control.peer) {
        try {
          return platf::from_sockaddr((sockaddr *) &session.control.peer->address.address);
        }
        catch (...) {}
      }
      return session.control.expected_peer_address;
    }

    static bool
    session_matches_client(const session_t &session,
                           const std::string &client_cert_uuid,
                           const std::string &client_address) {
      if (!client_cert_uuid.empty()) {
        if (!session.identity.client_cert_uuid.empty() &&
            session.identity.client_cert_uuid == client_cert_uuid) {
          return true;
        }

        // Fallback to address matching for older runtimes that have not yet
        // populated a cert UUID, but are still clearly the same client.
        return !client_address.empty() &&
               session_peer_address(session) == client_address;
      }

      return !client_address.empty() &&
             session_peer_address(session) == client_address;
    }

    stop_sessions_result_t
    stop_sessions_for_client(const std::string &client_cert_uuid,
                             const std::string &client_address,
                             std::string_view reason) {
      stop_sessions_result_t result {};

      if (!broadcast_shared.has_ref()) {
        return result;
      }

      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        return result;
      }

      auto sessions_lock = broadcast_ref->control_server._sessions.lock();
      for (auto *session_p : *broadcast_ref->control_server._sessions) {
        if (!session_p || !session_matches_client(*session_p, client_cert_uuid, client_address)) {
          continue;
        }

        ++result.matched;
        const auto state = session::state(*session_p);
        if (state == state_e::RUNNING) {
          BOOST_LOG(info) << "Stopping same-client active session before new launch"
                          << " reason=" << reason
                          << " runtime=" << session_p->identity.runtime_id
                          << " client=" << session_p->client_name
                          << " certUuid=" << session_p->identity.client_cert_uuid
                          << " peer=" << session_peer_address(*session_p);
          session::stop(*session_p);
          ++result.stopped;
        }
        else if (state == state_e::STARTING) {
          ++result.starting;
          BOOST_LOG(warning) << "Same-client session still starting"
                             << " reason=" << reason
                             << " runtime=" << session_p->identity.runtime_id
                             << " client=" << session_p->client_name
                             << " certUuid=" << session_p->identity.client_cert_uuid
                             << " peer=" << session_peer_address(*session_p);
        }
        else if (state == state_e::STOPPING) {
          ++result.already_stopping;
        }
      }

      if (result.matched > 0) {
        BOOST_LOG(info) << "Same-client session replacement requested"
                        << " reason=" << reason
                        << " clientCertUuid=" << client_cert_uuid
                        << " clientAddress=" << client_address
                        << " matched=" << result.matched
                        << " stopped=" << result.stopped
                        << " starting=" << result.starting
                        << " stopping=" << result.already_stopping;
      }

      return result;
    }

    void
    stop(session_t &session) {
      while_starting_do_nothing(session.state);
      auto expected = state_e::RUNNING;
      auto already_stopping = !session.state.compare_exchange_strong(expected, state_e::STOPPING);
      if (already_stopping) {
        return;
      }

      bool expected_counted = false;
      if (session.teardown_counted.compare_exchange_strong(expected_counted, true, std::memory_order_acq_rel)) {
        ++teardown_sessions;
      }
      refresh_li_session(session, state_e::STOPPING);
      session.shutdown_event->raise(true);
    }

    void
    join(session_t &session) {
      // Current Nvidia drivers have a bug where NVENC can deadlock the encoder thread with hardware-accelerated
      // GPU scheduling enabled. If this happens, we will terminate ourselves and the service can restart.
      // The alternative is that Sunshine can never start another session until it's manually restarted.
      auto task = []() {
        BOOST_LOG(fatal) << "Hang detected! Session failed to terminate in 10 seconds."sv;
        logging::log_flush();
        lifetime::debug_trap();
      };
      auto force_kill = task_pool.pushDelayed(task, 10s).task_id;
      auto fg = util::fail_guard([&force_kill]() {
        // Cancel the kill task if we manage to return from this function
        task_pool.cancel(force_kill);
      });
      auto teardown_fg = util::fail_guard([&session]() {
        if (session.teardown_counted.exchange(false, std::memory_order_acq_rel)) {
          --teardown_sessions;
        }
      });

      // 仅控制流会话没有视频/音频线程
      if (!session.control_only) {
        BOOST_LOG(info) << "Session teardown waiting for video runtime="sv << session.identity.runtime_id;
        session.videoThread.join();
        BOOST_LOG(info) << "Session teardown video joined runtime="sv << session.identity.runtime_id;
        BOOST_LOG(info) << "Session teardown waiting for audio runtime="sv << session.identity.runtime_id;
        session.audioThread.join();
        BOOST_LOG(info) << "Session teardown audio joined runtime="sv << session.identity.runtime_id;
      }
      else {
        BOOST_LOG(debug) << "Control-only session: skipping video/audio thread join"sv;
      }
      BOOST_LOG(info) << "Session teardown waiting for control runtime="sv << session.identity.runtime_id;
      session.controlEnd.view();
      BOOST_LOG(info) << "Session teardown control ended runtime="sv << session.identity.runtime_id;
      // Reset input on session stop to avoid stuck repeated keys
      BOOST_LOG(debug) << "Resetting Input..."sv;
      input::reset(session.input);
      session.broadcast_ref->feature_leases.release_all(session);

      // 对于仅控制流会话，只减少总会话计数，不调用 streaming_will_stop
      // 只有当所有非控制流会话都结束时才调用 streaming_will_stop
      if (session.control_only) {
        --running_sessions;
        BOOST_LOG(debug) << "Control-only session ended (remaining sessions: "sv << running_sessions.load() << ")"sv;
      }
      else {
        // 非仅控制流会话：减少两个计数器
        --running_sessions;
        // If this is the last non-control-only session, invoke the platform callbacks
        if (--running_non_control_only_sessions == 0) {
          // 最后一个会话结束时，确保麦克风socket已关闭
          if (session.broadcast_ref->mic_socket_enabled.load()) {
            session.broadcast_ref->mic_socket_enabled.store(false);
            session.broadcast_ref->mic_sessions_count.store(0);
            session.broadcast_ref->mic_sock.close();
            reset_mic_encryption(*session.broadcast_ref.get());
            BOOST_LOG(debug) << "Microphone socket closed (last session ended)";
          }

          bool restore_display_state { true };
          if (proc::proc.running()) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
            system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif

            // TODO: make this configurable per app
            restore_display_state = false;
          }

          if (restore_display_state) {
            display_device::session_t::get().restore_state();
          }

          session.broadcast_ref->resources.release_display_resource(session);
          platf::streaming_will_stop();
        }
        else {
          auto was_display_owner = session.broadcast_ref->resources.display_resource().owner.runtime_id == session.identity.runtime_id;
          // 非最后一个会话：如果当前会话启用了麦克风，减少计数
          if (session.audio.enable_mic) {
            int remaining_count = session.broadcast_ref->mic_sessions_count.fetch_sub(1) - 1;
            if (remaining_count == 0) {
              // 没有会话需要麦克风了，关闭socket并清除所有加密上下文
              session.broadcast_ref->mic_socket_enabled.store(false);
              session.broadcast_ref->mic_sock.close();
              reset_mic_encryption(*session.broadcast_ref.get());
              BOOST_LOG(debug) << "Microphone socket closed (no sessions require it)";
            }
            else {
              // 只移除当前客户端的加密上下文，保留其他客户端的
              auto client_ip = session.audio.peer.address().to_string();
              remove_mic_encryption_for_session(*session.broadcast_ref.get(), session);
              promote_mic_owner_if_needed(*session.broadcast_ref.get(), &session);
              BOOST_LOG(debug) << "Microphone sessions remaining: " << remaining_count << " (removed cipher for " << client_ip << ")";
            }
          }
          session.broadcast_ref->resources.release_display_resource(session);
          if (was_display_owner) {
            auto sessions_lock = session.broadcast_ref->control_server._sessions.lock();
            for (auto *candidate : *session.broadcast_ref->control_server._sessions) {
              if (candidate != &session &&
                  !candidate->control_only &&
                  candidate->state.load(std::memory_order_acquire) == state_e::RUNNING) {
                auto display_resource = session.broadcast_ref->resources.acquire_display_resource(*candidate);
                session.broadcast_ref->feature_leases.acquire(session_runtime::feature_e::input_focus, *candidate);
                session.broadcast_ref->feature_leases.acquire(session_runtime::feature_e::dynamic_quality, *candidate);
                session.broadcast_ref->feature_leases.acquire(session_runtime::feature_e::transport_qos, *candidate);
                refresh_li_session(*candidate, state_e::RUNNING);
                BOOST_LOG(debug) << "Promoted runtime session " << display_resource.owner.runtime_id
                                 << " to shared display/input/dynamic-quality owner after runtime session "
                                 << session.identity.runtime_id << " ended";
                break;
              }
            }
          }
        }
      }

      abr::cleanup_for_runtime(session.identity.runtime_id);
      abr::cleanup(session.client_name);
      session.state.store(state_e::STOPPED, std::memory_order_relaxed);
      refresh_li_session(session, state_e::STOPPED);

      BOOST_LOG(debug) << "Session ended"sv;
    }

    int
    start(session_t &session, const std::string &addr_string) {
      session.startup_path_evidence.peer_is_lan_or_pc = is_lan_or_pc_peer(addr_string);
      session.startup_path_evidence.host_observed_peer_endpoint = addr_string;
      session.startup_path_decision = session_runtime::classify_startup_path(session.startup_path_evidence);
      session.active_transport_path = session_runtime::make_transport_path(session.startup_path_decision,
                                                                           session.startup_path_evidence);
      session.active_transport_path.state = session_runtime::transport_path_state_e::active;
      log_startup_path_decision(session.identity.runtime_id,
                                "control-start",
                                session.startup_path_evidence,
                                session.startup_path_decision);
      session.state.store(state_e::STARTING, std::memory_order_release);
      refresh_li_session(session, state_e::STARTING);
      session.input = input::alloc(session.mail);

      session.broadcast_ref = broadcast_shared.ref();
      if (!session.broadcast_ref) {
        session.state.store(state_e::STOPPED, std::memory_order_release);
        refresh_li_session(session, state_e::STOPPED);
        return -1;
      }

      session.control.expected_peer_address = addr_string;
      if (session.control_only) {
        BOOST_LOG(info) << "Starting control-only session from ["sv << addr_string << "] - will only handle input control"sv;
      }
      else {
        BOOST_LOG(debug) << "Expecting incoming session connections from "sv << addr_string;
      }

      // Insert this session into the session list
      {
        auto lg = session.broadcast_ref->control_server._sessions.lock();
        session.broadcast_ref->control_server._sessions->push_back(&session);
        session.broadcast_ref->control_server._registry.register_session(&session);
      }

      auto addr = boost::asio::ip::make_address(addr_string);
      session.video.peer.address(addr);
      session.video.peer.port(0);

      session.audio.peer.address(addr);
      session.audio.peer.port(0);

      session.pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      // 仅控制流会话不启动视频/音频线程
      if (!session.control_only) {
        session.audioThread = std::thread { audioThread, &session };
        session.videoThread = std::thread { videoThread, &session };
      }
      else {
        BOOST_LOG(debug) << "Control-only session: skipping video and audio thread creation"sv;
      }

      session.state.store(state_e::RUNNING, std::memory_order_relaxed);
      const auto stream_quality_started_at = std::chrono::steady_clock::now();
      const bool adaptive_controller_enabled = config::stream.adaptive_streaming_optimization &&
                                               (session.config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) != 0;
      const bool lan_fast_start = adaptive_controller_enabled &&
                                  session.startup_path_decision.allow_lan_fast_start;
      session.adaptive_controller_enabled = adaptive_controller_enabled;
      session.adaptive_controller_reason = adaptive_controller_enabled ?
                                           "enabled" :
                                           (config::stream.adaptive_streaming_optimization ?
                                              "client-feedback-unsupported" :
                                              "config-disabled");
      session.stream_quality_startup_guard_until = adaptive_controller_enabled && !lan_fast_start ?
                                               stream_quality_started_at + 6500ms :
                                               std::chrono::steady_clock::time_point {};
      session.stream_quality_startup_settle_until = adaptive_controller_enabled && !lan_fast_start ?
                                                stream_quality_started_at + 10000ms :
                                                std::chrono::steady_clock::time_point {};
      session.video_startup_pacing_until = adaptive_controller_enabled && !lan_fast_start ?
                                             stream_quality_started_at + 2500ms :
                                             std::chrono::steady_clock::time_point {};
      session.last_stream_quality_startup_guard_log = {};
      if (lan_fast_start) {
        BOOST_LOG(info) << "Stream-quality startup guard skipped runtime="
                        << session.identity.runtime_id
                        << " peer=" << addr_string
                        << " kind=" << session_runtime::li_path_identity_kind_name(session.startup_path_decision.path_identity_kind)
                        << " startup=" << session_runtime::li_startup_class_name(session.startup_path_decision.startup_class)
                        << " pathReason=" << session.startup_path_decision.reason;
      }
      else if (adaptive_controller_enabled) {
        BOOST_LOG(info) << "Stream-quality startup guard enabled runtime="
                        << session.identity.runtime_id
                        << " peer=" << addr_string
                        << " kind=" << session_runtime::li_path_identity_kind_name(session.startup_path_decision.path_identity_kind)
                        << " startup=" << session_runtime::li_startup_class_name(session.startup_path_decision.startup_class)
                        << " pathReason=" << session.startup_path_decision.reason
                        << " route=" << session_runtime::transport_route_name(session.startup_path_decision.route)
                        << " egressKind=" << session_runtime::li_path_egress_kind_name(session.startup_path_decision.egress_kind)
                        << " encapsulation=" << session_runtime::li_path_encapsulation_name(session.startup_path_decision.encapsulation);
      }

      // 仅控制流会话不触发 streaming_will_start 回调，因为它们不传输视频/音频
      // 但它们仍然需要被计入 running_sessions，以便正确管理会话
      if (session.control_only) {
        // 仅控制流会话：只增加总会话计数，不调用平台回调
        ++running_sessions;
        BOOST_LOG(debug) << "Control-only session started (total sessions: "sv << running_sessions.load() << ")"sv;
      }
      else {
        auto display_resource = session.broadcast_ref->resources.acquire_display_resource(session);
        if (display_resource.owner.runtime_id == session.identity.runtime_id) {
          BOOST_LOG(debug) << "Runtime session " << session.identity.runtime_id
                           << " owns the shared display resource"
                           << " (scope=" << resource_scope_name(display_resource.scope)
                           << ", mode=" << display_allocation_mode_name(display_resource.mode) << ")";
        }
        else {
          BOOST_LOG(debug) << "Runtime session " << session.identity.runtime_id
                           << " is sharing display resource owned by runtime session "
                           << display_resource.owner.runtime_id
                           << " (scope=" << resource_scope_name(display_resource.scope)
                           << ", mode=" << display_allocation_mode_name(display_resource.mode) << ")";
        }
        session.broadcast_ref->feature_leases.acquire(session_runtime::feature_e::input_focus, session);
        session.broadcast_ref->feature_leases.acquire(session_runtime::feature_e::dynamic_quality, session);
        session.broadcast_ref->feature_leases.acquire(session_runtime::feature_e::transport_qos, session);
        BOOST_LOG(info) << "Runtime session " << session.identity.runtime_id
                        << " active transport path="
                        << session_runtime::transport_route_name(session.active_transport_path.route)
                        << " pathId=" << session.active_transport_path.path_id;

        // 非仅控制流会话：增加两个计数器
        ++running_sessions;
        // If this is the first non-control-only session, invoke the platform callbacks
        if (++running_non_control_only_sessions == 1) {
          // 根据会话的麦克风启用标志管理麦克风socket
          if (session.audio.enable_mic) {
            setup_mic_for_session(session);
          }
          else {
            // 如果第一个会话不需要麦克风，关闭麦克风socket
            session.broadcast_ref->mic_socket_enabled.store(false);
            session.broadcast_ref->mic_sock.close();
            BOOST_LOG(info) << "Client " << session.client_name << ": Microphone socket closed (session doesn't require it)";
          }

          platf::streaming_will_start();
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_playing(proc::proc.get_last_run_app_name());
#endif
        }
        else {
          // 非第一个会话：如果启用麦克风
          if (session.audio.enable_mic) {
            setup_mic_for_session(session);
          }
        }
      }

      refresh_li_session(session, state_e::RUNNING);
      return 0;
    }

    std::shared_ptr<session_t>
    alloc(config_t &config, rtsp_stream::launch_session_t &launch_session) {
      auto session = std::make_shared<session_t>();

      auto mail = std::make_shared<safe::mail_raw_t>();

      session->shutdown_event = mail->event<bool>(mail::shutdown);
      session->launch_session_id = launch_session.id;
      session->identity = launch_session.identity;
      session->identity.runtime_id = next_runtime_id.fetch_add(1, std::memory_order_relaxed);
      if (session->identity.launch_session_id == 0) {
        session->identity.launch_session_id = launch_session.id;
      }
      session->identity.control_connect_data = launch_session.control_connect_data;
      session->identity.av_ping_payload = launch_session.av_ping_payload;
      session->startup_path_evidence = startup_path_evidence_for_launch_session(launch_session);
      session->startup_path_decision = session_runtime::classify_startup_path(session->startup_path_evidence);
      session->active_transport_path = session_runtime::make_transport_path(session->startup_path_decision,
                                                                            session->startup_path_evidence);
      session->active_transport_path.state = session_runtime::transport_path_state_e::active;
      log_startup_path_decision(session->identity.runtime_id,
                                "launch",
                                session->startup_path_evidence,
                                session->startup_path_decision);

      // 设置客户端名称
      session->client_name = session->identity.client_name.empty() ?
                               launch_session.client_name :
                               session->identity.client_name;

      // 保存 launch_session 的关键字段，用于后续动态参数更新
      session->enable_sops = launch_session.enable_sops;
      session->enable_hdr = launch_session.enable_hdr;
      session->max_nits = launch_session.max_nits;
      session->min_nits = launch_session.min_nits;
      session->max_full_nits = launch_session.max_full_nits;

      session->config = config;
      LiInitializeSession(&session->shared_session);
      zako_input_runtime_init(&session->zako_input_runtime);
      const auto app_id = launch_session.appid == 0 ? std::string {} : std::to_string(launch_session.appid);
      std::string app_name;
      try {
        app_name = launch_session.appid > 0 ? proc::proc.get_app_name(launch_session.appid) : std::string {};
      }
      catch (...) {
        app_name.clear();
      }
      session_runtime::copy_li_string(session->shared_session.appId,
                                      sizeof(session->shared_session.appId),
                                      app_id);
      session_runtime::copy_li_string(session->shared_session.appName,
                                      sizeof(session->shared_session.appName),
                                      app_name);
      session->config.monitor.cursorProbeRuntimeId = session->identity.runtime_id;
      session->adaptive_controller_enabled = config::stream.adaptive_streaming_optimization &&
                                             (config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) != 0;
      session->adaptive_controller_reason = session->adaptive_controller_enabled ?
                                            "enabled" :
                                            (config::stream.adaptive_streaming_optimization ?
                                               "client-feedback-unsupported" :
                                               "config-disabled");

      // Initialize encoding and pacing budgets separately. The stream-quality
      // controller adjusts encoder bitrate, while pacing reserves one FEC
      // overhead pass for the actual send budget.
      int encoding_bitrate = config.monitor.bitrate;
      int ceiling_fps = config.monitor.qualityCeilingFramerate > 0 ?
                          std::max(config.monitor.qualityCeilingFramerate, config.monitor.framerate) :
                          config.monitor.framerate;
      const int user_quality_bitrate = config.monitor.qualityCeilingBitrate > 0 ?
                                         std::max(config.monitor.qualityCeilingBitrate, 1) :
                                         std::max(encoding_bitrate, 1);
      const auto pixels_per_second = static_cast<double>(std::max(config.monitor.width, 1)) *
                                     static_cast<double>(std::max(config.monitor.height, 1)) *
                                     static_cast<double>(std::max(ceiling_fps, 1));
      const double fps_protect_bpp = config.monitor.chromaSamplingType == 1 ? 0.024 : 0.020;
      const double ideal_bpp = config.monitor.chromaSamplingType == 1 ? 0.140 : 0.117;
      const int fps_needed_bitrate = std::max(
        user_quality_bitrate,
        static_cast<int>(std::lround(pixels_per_second * fps_protect_bpp / 1000.0)));
      const int ideal_demand_bitrate = std::max(
        fps_needed_bitrate,
        static_cast<int>(std::lround(pixels_per_second * ideal_bpp / 1000.0)));
      int ceiling_encoding_bitrate = std::max(user_quality_bitrate, ideal_demand_bitrate);
      int max_fec_percentage = rtsp_stream::adaptive_stream_max_fec_percentage_for_client(config::stream.fec_percentage,
                                                                                          config.mlFeatureFlags,
                                                                                          session->adaptive_controller_enabled);
      int fec_percentage = rtsp_stream::effective_stream_fec_percentage_for_client(config::stream.fec_percentage,
                                                                                   config.mlFeatureFlags,
                                                                                   session->adaptive_controller_enabled);
      fec_percentage = std::clamp(fec_percentage, 0, max_fec_percentage);
      const bool enhanced_feedback_client = session->adaptive_controller_enabled;
      const bool strong_lan_fast_start = enhanced_feedback_client &&
                                         session->startup_path_decision.allow_lan_fast_start;
      if (strong_lan_fast_start && fec_percentage > 0) {
        fec_percentage = std::min(fec_percentage, 2);
      }
      if (!enhanced_feedback_client) {
        ceiling_fps = config.monitor.framerate;
        ceiling_encoding_bitrate = std::max(encoding_bitrate, 1);
      }
      const int requested_total_ceiling = total_video_bitrate_from_encoding_bitrate(ceiling_encoding_bitrate,
                                                                                    fec_percentage);
      int ceiling_total_bitrate = std::max(requested_total_ceiling, 1);
      const auto startup_quality_stream = stream_quality::stream_description_t {
        .width = config.monitor.width,
        .height = config.monitor.height,
        .fps = ceiling_fps,
        .video_bitrate_kbps = ceiling_encoding_bitrate,
        .video_format = config.monitor.videoFormat,
        .chroma_sampling_type = config.monitor.chromaSamplingType,
        .content_type = stream_quality_content_type_from_monitor(config.monitor.contentType),
      };
      if (enhanced_feedback_client && !strong_lan_fast_start) {
        const int startup_encoding_limit = encoding_bitrate_from_total_video_budget(ceiling_total_bitrate,
                                                                                    fec_percentage);
        const int rtsp_seeded_bitrate = encoding_bitrate;
        const int computed_startup_bitrate = stream_quality::startup_bitrate_for_ceiling(startup_quality_stream);
        const int startup_encoding_bitrate =
          stream_quality::startup_bitrate_preserving_seed(startup_quality_stream, rtsp_seeded_bitrate);
        encoding_bitrate = std::clamp(startup_encoding_bitrate,
                                      1,
                                      std::max(1, std::min(ceiling_encoding_bitrate, startup_encoding_limit)));
        session->config.monitor.framerate = config.monitor.framerate;
        session->config.monitor.frameRateNum = config.monitor.frameRateNum;
        session->config.monitor.frameRateDen = config.monitor.frameRateDen;
        if (rtsp_seeded_bitrate > 0 && startup_encoding_bitrate < computed_startup_bitrate) {
          BOOST_LOG(info) << "Remote-safe startup seed preserved runtime=" << session->identity.runtime_id
                          << " rtspSeed=" << rtsp_seeded_bitrate << " Kbps"
                          << " startup=" << encoding_bitrate << " Kbps"
                          << " fps=" << config.monitor.framerate
                          << " ceilingEncoding=" << ceiling_encoding_bitrate << " Kbps";
        }
      }
      session->current_total_bitrate = total_video_bitrate_from_encoding_bitrate(encoding_bitrate, fec_percentage);
      session->current_fec_percentage = fec_percentage;
      session->pacing_total_bitrate = session->current_total_bitrate.load(std::memory_order_relaxed);
      if (enhanced_feedback_client) {
        session->stream_quality_controller.configure({
          .baseline_bitrate_kbps = ceiling_encoding_bitrate,
          .baseline_fec_percentage = fec_percentage,
          .max_fec_percentage = max_fec_percentage,
          .startup_bitrate_kbps = encoding_bitrate,
          .ceiling_total_bitrate_kbps = ceiling_total_bitrate,
          .baseline_fps = ceiling_fps,
          .startup_fps = config.monitor.framerate,
          .frame_width = config.monitor.width,
          .frame_height = config.monitor.height,
          .chroma_sampling_type = config.monitor.chromaSamplingType,
          .dynamic_range = config.monitor.dynamicRange,
          .runtime_profile_tier_supported = runtime_profile_resolution_reconfig_enabled(),
          .user_quality_kbps = user_quality_bitrate,
          .ideal_demand_kbps = ideal_demand_bitrate,
          .fps_needed_kbps = fps_needed_bitrate,
        });
      }
      session->last_applied_stream_quality_bitrate = encoding_bitrate;
      session->last_applied_stream_quality_fec = fec_percentage;
      session->last_applied_stream_quality_fps = config.monitor.framerate;
      session->last_applied_stream_quality_resolution_scale = 100;
      session->last_applied_stream_quality_chroma_sampling_type = config.monitor.chromaSamplingType;
      session->last_applied_stream_quality_dynamic_range = config.monitor.dynamicRange;
      session->last_dynamic_clarity_flags = config.monitor.lowBitrateClarityIntentFlags;
      session->last_dynamic_clarity_qp = config.monitor.lowBitrateTargetQp;
      session->last_dynamic_clarity_bitrate = encoding_bitrate;
      session->stream_quality_recovery_ready_after = enhanced_feedback_client ?
                                                std::chrono::steady_clock::now() + 1500ms :
                                                std::chrono::steady_clock::time_point {};
      BOOST_LOG(info) << "Stream-quality startup baseline runtime=" << session->identity.runtime_id
                      << " adaptiveController=" << adaptive_controller_state_name(session.get())
                      << " reason=" << adaptive_controller_reason(session.get())
                      << " controllerSource=alkaid-sdk"
                      << " weakNetContract=" << ALK_STREAM_QUALITY_CONTROL_VERSION
                      << " encoding=" << encoding_bitrate << " Kbps"
                      << " total=" << session->current_total_bitrate.load(std::memory_order_relaxed) << " Kbps"
                      << " ceilingEncoding=" << ceiling_encoding_bitrate << " Kbps"
                      << " userQuality=" << user_quality_bitrate << " Kbps"
                      << " idealDemand=" << ideal_demand_bitrate << " Kbps"
                      << " fpsNeeded=" << fps_needed_bitrate << " Kbps"
	                      << " ceilingTotal=" << ceiling_total_bitrate << " Kbps"
	                      << " fps=" << config.monitor.framerate
	                      << " ceilingFps=" << ceiling_fps
	                      << " fec=" << fec_percentage << "%"
	                      << " maxFec=" << max_fec_percentage << "%"
	                      << " kind=" << session_runtime::li_path_identity_kind_name(session->startup_path_decision.path_identity_kind)
	                      << " startup=" << session_runtime::li_startup_class_name(session->startup_path_decision.startup_class)
	                      << " pathReason=" << session->startup_path_decision.reason
	                      << " route=" << session_runtime::transport_route_name(session->startup_path_decision.route)
	                      << " egressKind=" << session_runtime::li_path_egress_kind_name(session->startup_path_decision.egress_kind)
	                      << " encapsulation=" << session_runtime::li_path_encapsulation_name(session->startup_path_decision.encapsulation)
	                      << (strong_lan_fast_start ? " fastStart=lan" : " fastStart=remote-safe")
	                      << ((config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) ? " feedback=1" : " feedback=0");
      if (config.monitor.lowBitrateClarityIntentFlags != 0) {
        BOOST_LOG(info) << "Frame interest intent generated runtime=" << session->identity.runtime_id
                        << " flags=0x" << std::hex << config.monitor.lowBitrateClarityIntentFlags << std::dec
                        << " qp=" << config.monitor.lowBitrateTargetQp
                        << " sharpen=" << config.monitor.lowBitrateSharpenAlpha;
      }

      session->control.connect_data = launch_session.control_connect_data;
      session->control.feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
      session->control.hdr_queue = mail->event<video::hdr_info_t>(mail::hdr);
      session->control.resolution_change_queue = mail->event<std::pair<std::uint32_t, std::uint32_t>>(mail::resolution_change);
      session->control.legacy_input_enc_iv = launch_session.iv;
      session->control.cipher = crypto::cipher::gcm_t {
        launch_session.gcm_key, false
      };

      session->video.idr_events = mail->event<bool>(mail::idr);
      session->video.invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
      session->video.dynamic_param_change_events = mail->queue<video::dynamic_param_t>(mail::dynamic_param_change);
      session->video.lowseq = 0;
      session->video.ping_payload = launch_session.av_ping_payload;
      if (config.encryptionFlagsEnabled & SS_ENC_VIDEO) {
        BOOST_LOG(info) << "Video encryption enabled"sv;
        session->video.cipher = crypto::cipher::gcm_t {
          launch_session.gcm_key, false
        };
        session->video.gcm_iv_counter = 0;
      }

      // Per-shard capacity must match MAX_AUDIO_PACKET_SIZE (defined near
      // the top of this file). The previous hardcoded 2048 here would
      // overflow with AC3 frames (~2560 B encrypted) and corrupt neighbour
      // shards / RS parity for E-AC3 (~1552 B), causing client-side decode
      // failures even when the encoder reported success.
      constexpr auto max_block_size = crypto::cipher::round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE);

      util::buffer_t<char> shards { RTPA_TOTAL_SHARDS * max_block_size };
      util::buffer_t<uint8_t *> shards_p { RTPA_TOTAL_SHARDS };

      for (auto x = 0; x < RTPA_TOTAL_SHARDS; ++x) {
        shards_p[x] = (uint8_t *) &shards[x * max_block_size];
      }

      // Audio FEC spans multiple audio packets,
      // therefore its session specific
      session->audio.shards = std::move(shards);
      session->audio.shards_p = std::move(shards_p);

      session->audio.fec_packet.rtp.header = 0x80;
      session->audio.fec_packet.rtp.packetType = 127;
      session->audio.fec_packet.rtp.timestamp = 0;
      session->audio.fec_packet.rtp.ssrc = 0;

      session->audio.fec_packet.fecHeader.payloadType = 97;
      session->audio.fec_packet.fecHeader.ssrc = 0;

      session->audio.cipher = crypto::cipher::cbc_t {
        launch_session.gcm_key, true
      };

      session->audio.ping_payload = launch_session.av_ping_payload;
      session->audio.avRiKeyId = util::endian::big(*(std::uint32_t *) launch_session.iv.data());
      session->audio.sequenceNumber = 0;
      session->audio.timestamp = 0;

      session->audio.enable_mic = launch_session.enable_mic;

      session->control_only = launch_session.control_only;

      session->control.peer = nullptr;
      session->state.store(state_e::STOPPED, std::memory_order_relaxed);
      refresh_li_session(*session,
                         state_e::STOPPED,
                         std::to_string(launch_session.appid),
                         launch_session.client_name);

      session->mail = std::move(mail);

      return session;
    }

    static bool
    apply_dynamic_param_to_session(session_t *session_p, const video::dynamic_param_t &param, const std::string &target_label) {
      if (!session_p ||
          session_p->state.load(std::memory_order_relaxed) != state_e::RUNNING) {
        return false;
      }

      // Update session's current send budget if this is an encoder bitrate change
      if (param.type == video::dynamic_param_type_e::BITRATE && param.valid) {
        const auto current_fec_percentage = session_p->current_fec_percentage.load(std::memory_order_relaxed);
        const auto total_bitrate = total_video_bitrate_from_encoding_bitrate(param.value.int_value,
                                                                             current_fec_percentage);
        session_p->current_total_bitrate = total_bitrate;
        session_p->pacing_total_bitrate = std::max(session_p->pacing_total_bitrate.load(std::memory_order_relaxed),
                                                   total_bitrate);
        BOOST_LOG(info) << "Updated session bitrate for " << target_label
                        << ": encoding=" << param.value.int_value
                        << " Kbps total=" << total_bitrate
                        << " Kbps fec=" << current_fec_percentage << "%";
      }

      session_p->video.dynamic_param_change_events->raise(param);
      BOOST_LOG(info) << "Sent dynamic parameter change event to " << target_label
                      << ": type=" << (int) param.type;
      return true;
    }

    bool
    change_dynamic_param_for_runtime(std::uint64_t runtime_id, const video::dynamic_param_t &param) {
      // 先检查是否有活动的广播引用，避免在无活跃session时
      // 触发start_broadcast/end_broadcast循环（"僵尸广播"），
      // 这可能阻塞HTTPS服务器线程导致客户端显示主机离线
      if (!broadcast_shared.has_ref()) {
        return false;
      }

      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        BOOST_LOG(warning) << "No broadcast context available when changing dynamic parameter for client";
        return false;
      }

      auto session_p = broadcast_ref->control_server._registry.find_by_runtime_id(runtime_id);
      if (apply_dynamic_param_to_session(session_p, param, "runtime session '" + std::to_string(runtime_id) + "'")) {
        return true;
      }

      BOOST_LOG(warning) << "No active session found for runtime_id: " << runtime_id;
      return false;
    }

    bool
    change_dynamic_param_for_client(const std::string &client_name, const video::dynamic_param_t &param) {
      // 先检查是否有活动的广播引用，避免在无活跃session时
      // 触发start_broadcast/end_broadcast循环（"僵尸广播"），
      // 这可能阻塞HTTPS服务器线程导致客户端显示主机离线
      if (!broadcast_shared.has_ref()) {
        return false;
      }

      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        BOOST_LOG(warning) << "No broadcast context available when changing dynamic parameter for client";
        return false;
      }

      auto lg = broadcast_ref->control_server._sessions.lock();
      for (auto session_p : *broadcast_ref->control_server._sessions) {
        if (session_p->client_name == client_name &&
            apply_dynamic_param_to_session(session_p, param, "legacy client '" + client_name + "'")) {
          return true;
        }
      }

      BOOST_LOG(warning) << "No active session found for client: " << client_name;
      return false;
    }

    std::vector<session_info_t>
    get_all_sessions_info() {
      std::vector<session_info_t> sessions_info;

      // 关键修复：先检查是否有活动的引用，避免触发 start_broadcast
      // 如果没有活动的引用，说明没有活动的 session，直接返回空列表
      if (!broadcast_shared.has_ref()) {
        return sessions_info;
      }

      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        BOOST_LOG(warning) << "No broadcast context when getting all sessions info";
        return sessions_info;
      }

      // 在持有锁的情况下，快速复制会话的基本信息
      // 由于存储的是原始指针，我们需要在持有锁时快速访问
      auto lg = broadcast_ref->control_server._sessions.lock();
      for (auto session_p : *broadcast_ref->control_server._sessions) {
        // 双重检查：确保会话指针仍然有效
        if (!session_p) {
          continue;
        }

        try {
          session_info_t info;
          auto state = session_p->state.load(std::memory_order_relaxed);
          refresh_li_session(*session_p, state);
          const auto &li_session = session_p->shared_session;

          info.runtime_id = li_session.runtimeId;
          info.launch_session_id = li_session.launchSessionId;
          info.control_generation = li_session.controlGeneration;
          info.logical_session_key = li_session.logicalSessionKey;
          info.client_cert_uuid = session_p->identity.client_cert_uuid;
          info.client_unique_id = session_p->identity.client_unique_id;
          info.client_name = li_session.client.displayName[0] != '\0' ?
                               std::string { li_session.client.displayName } :
                               session_p->client_name;
          info.session_id = li_session.launchSessionId;
          info.trusted_client_identity = session_p->identity.has_trusted_client_identity();

          // Get client address
          if (session_p->control.peer) {
            try {
              info.client_address = platf::from_sockaddr((sockaddr *) &session_p->control.peer->address.address);
            }
            catch (...) {
              info.client_address = session_p->control.expected_peer_address;
            }
          }
          else {
            info.client_address = session_p->control.expected_peer_address;
          }

          // Get session state
          switch (state) {
            case state_e::STOPPED:
              info.state = "STOPPED";
              break;
            case state_e::STOPPING:
              info.state = "STOPPING";
              break;
            case state_e::STARTING:
              info.state = "STARTING";
              break;
            case state_e::RUNNING:
              info.state = "RUNNING";
              break;
            default:
              info.state = "UNKNOWN";
              break;
          }
          info.canonical_state = li_session.state;

          // Get video configuration
          info.width = session_p->config.monitor.width;
          info.height = session_p->config.monitor.height;
          info.fps = session_p->config.monitor.framerate;

          // Get current total bitrate (including FEC) from session-specific field
          // This is the user-configured bitrate, which may have been changed dynamically
          info.bitrate = session_p->current_total_bitrate.load(std::memory_order_relaxed);

          // Get audio and other settings
          info.host_audio = session_p->config.audio.flags[audio::config_t::HOST_AUDIO];
          info.enable_hdr = session_p->config.monitor.dynamicRange > 0;
          info.enable_mic = session_p->audio.enable_mic;
          auto input_owner = broadcast_ref->feature_leases.owner(session_runtime::feature_e::input_focus);
          info.input_owner = input_owner.runtime_id == session_p->identity.runtime_id;
          info.input_owner_runtime_id = input_owner.runtime_id;
          auto mic_owner = broadcast_ref->feature_leases.owner(session_runtime::feature_e::microphone);
          info.mic_owner = mic_owner.runtime_id == session_p->identity.runtime_id;
          info.mic_owner_runtime_id = mic_owner.runtime_id;
          auto clipboard_owner = broadcast_ref->feature_leases.owner(session_runtime::feature_e::clipboard);
          info.clipboard_owner = clipboard_owner.runtime_id == session_p->identity.runtime_id;
          info.clipboard_owner_runtime_id = clipboard_owner.runtime_id;
          auto dynamic_quality_owner = broadcast_ref->feature_leases.owner(session_runtime::feature_e::dynamic_quality);
          info.dynamic_quality_owner = dynamic_quality_owner.runtime_id == session_p->identity.runtime_id;
          info.dynamic_quality_owner_runtime_id = dynamic_quality_owner.runtime_id;
          auto display_resource = broadcast_ref->resources.display_resource();
          info.display_owner = display_resource.owner.runtime_id == session_p->identity.runtime_id;
          info.display_owner_runtime_id = display_resource.owner.runtime_id;
          info.display_resource_scope = resource_scope_name(display_resource.scope);
          info.display_allocation_mode = info.display_owner ?
                                           display_allocation_mode_name(display_resource.mode) :
                                           display_allocation_mode_name(session_runtime::display_allocation_mode_e::shared_follower);
          info.display_resource_slot = display_resource.resource_slot;
          info.dedicated_display = info.display_allocation_mode == display_allocation_mode_name(session_runtime::display_allocation_mode_e::dedicated);
          info.transport_path_id = li_session.transportPath.pathId;
          info.transport_kind = li_session.transportPath.kind;
          info.transport_protocol = li_session.transportPath.protocol;
          info.transport_flags = li_session.transportPath.flags;
          info.transport_rtt_us = li_session.transportPath.rttUs;
          info.transport_jitter_us = li_session.transportPath.jitterUs;
          info.transport_packet_loss_ppm = li_session.transportPath.packetLossPpm;
          info.transport_route_id = li_session.transportPath.routeId;
          info.pointer_mode = li_session.telemetry.pointerMode;
          info.cursor_state_flags = li_session.telemetry.cursorStateFlags;
          info.pointer_release_queue_depth = li_session.telemetry.pointerReleaseQueueDepth;
          info.pointer_release_queue_delay_us = li_session.telemetry.pointerReleaseQueueDelayUs;
          info.pointer_mode_switch_us = li_session.telemetry.pointerModeSwitchUs;
          info.pointer_deltas_coalesced = li_session.telemetry.pointerDeltasCoalesced;
          info.pointer_acceleration_risk_ppm = li_session.telemetry.pointerAccelerationRiskPpm;

          // Get app information
          try {
            info.app_id = proc::proc.running();
            info.app_name = (info.app_id > 0) ? proc::proc.get_last_run_app_name() : "None";
          }
          catch (...) {
            info.app_id = 0;
            info.app_name = "None";
          }

          sessions_info.push_back(info);
        }
        catch (const std::exception &e) {
          BOOST_LOG(warning) << "Error processing session: " << e.what() << " when getting all sessions info";
          continue;
        }
        catch (...) {
          BOOST_LOG(warning) << "Unknown error processing session when getting all sessions info";
          continue;
        }
      }

      return sessions_info;
    }
  }  // namespace session
}  // namespace stream
