/**
 * @file src/stream.cpp
 * @brief Definitions for the streaming protocols.
 */
#include "process.h"

#include <future>
#include <iomanip>
#include <queue>
#include <unordered_map>

#include <fstream>
#include <openssl/err.h>

#include <boost/endian/arithmetic.hpp>

extern "C" {
// clang-format off
#include <moonlight-common-c/src/Limelight-internal.h>
#include "rswrapper.h"
// clang-format on
}

#include "config.h"
#include "display_device/session.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "stream.h"
#include "sync.h"
#include "system_tray.h"
#include "thread_safe.h"
#include "utility.h"

#include "platform/common.h"

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
#define IDX_DYNAMIC_BITRATE_CHANGE 18  // 新增：动态码率调整消息类型

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
  0x5506,  // Dynamic bitrate change (Sunshine protocol extension)
};

namespace asio = boost::asio;
namespace sys = boost::system;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace stream {

  enum class socket_e : int {
    video,  ///< Video
    audio,  ///< Audio
    microphone,  ///< Microphone
  };

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

#pragma pack(pop)

  constexpr std::size_t
  round_to_pkcs7_padded(std::size_t size) {
    return ((size + 15) / 16) * 16;
  }
  constexpr std::size_t MAX_AUDIO_PACKET_SIZE = 1400;

  using audio_aes_t = std::array<char, round_to_pkcs7_padded(MAX_AUDIO_PACKET_SIZE)>;

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
    send(const std::string_view &payload, net::peer_t peer) {
      auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
      if (enet_peer_send(peer, 0, packet)) {
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

    udp::socket video_sock { io_context };
    udp::socket audio_sock { io_context };
    udp::socket mic_sock { io_context };

    control_server_t control_server;

    std::atomic<bool> mic_socket_enabled { false };
    std::atomic<int> mic_sessions_count { 0 };  // 需要麦克风的会话数

    std::atomic<bool> mic_encryption_enabled { false };
    std::optional<crypto::cipher::cbc_t> mic_cipher;
    crypto::aes_t mic_iv;
    std::mutex mic_cipher_mutex;

    // TODO: 未来版本应当强制启用麦克风加密，防止被窃听
    std::atomic<bool> mic_reject_plaintext { false };

    std::map<std::string, std::string> client_ip_to_name;
    std::mutex client_name_mutex;
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

    // 添加客户端名称字段
    std::string client_name;

    struct {
      std::string ping_payload;

      int lowseq;
      udp::endpoint peer;

      std::optional<crypto::cipher::gcm_t> cipher;
      std::uint64_t gcm_iv_counter;

      safe::mail_raw_t::event_t<bool> idr_events;
      safe::mail_raw_t::event_t<std::pair<int64_t, int64_t>> invalidate_ref_frames_events;
      safe::mail_raw_t::event_t<video::dynamic_param_t> dynamic_param_change_events;  // 新增：动态参数调整事件

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

      std::uint32_t connect_data;  // Used for new clients with ML_FF_SESSION_ID_V1
      std::string expected_peer_address;  // Only used for legacy clients without ML_FF_SESSION_ID_V1

      net::peer_t peer;
      std::uint32_t seq;

      platf::feedback_queue_t feedback_queue;
      safe::mail_raw_t::event_t<video::hdr_info_t> hdr_queue;
    } control;

    std::uint32_t launch_session_id;

    safe::mail_raw_t::event_t<bool> shutdown_event;
    safe::signal_t controlEnd;

    std::atomic<session::state_e> state;

    // Current total bitrate for this session (including FEC overhead) in Kbps
    // This is the user-configured bitrate, not the encoding bitrate
    std::atomic<int> current_total_bitrate { 0 };

    // 标识这是仅控制流会话（只作为输入设备，不传输视频/音频）
    bool control_only { false };
  };

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

  int
  start_broadcast(broadcast_ctx_t &ctx);
  void
  end_broadcast(broadcast_ctx_t &ctx);

  static auto broadcast_shared = safe::make_shared<broadcast_ctx_t>(start_broadcast, end_broadcast);

  session_t *
  control_server_t::get_session(const net::peer_t peer, uint32_t connect_data) {
    {
      // Fast path - look up existing session by peer
      auto lg = _peer_to_session.lock();
      auto it = _peer_to_session->find(peer);
      if (it != _peer_to_session->end()) {
        return it->second;
      }
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
      if (session_p->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) {
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

      // Use the local address from the control connection as the source address
      // for other communications to the client. This is necessary to ensure
      // proper routing on multi-homed hosts.
      auto local_address = platf::from_sockaddr((sockaddr *) &peer->localAddress.address);
      session_p->localAddress = boost::asio::ip::make_address(local_address);

      BOOST_LOG(debug) << "Control local address ["sv << local_address << ']';
      BOOST_LOG(debug) << "Control peer address ["sv << peer_addr << ':' << peer_port << ']';

      // Insert this into the map for O(1) lookups in the future
      auto ptslg = _peer_to_session.lock();
      _peer_to_session->emplace(peer, session_p);
      return session_p;
    }

    return nullptr;
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
      BOOST_LOG(error) << "Dropping unencrypted message on encrypted control stream: "sv << util::hex(type).to_string_view();
      return;
    }

    auto cb = _map_type_cb.find(type);
    if (cb == std::end(_map_type_cb)) {
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

  void
  control_server_t::iterate(std::chrono::milliseconds timeout) {
    ENetEvent event;
    auto res = enet_host_service(_host.get(), &event, timeout.count());

    if (res > 0) {
      auto session = get_session(event.peer, event.data);
      if (!session) {
        BOOST_LOG(warning) << "Rejected connection from ["sv << platf::from_sockaddr((sockaddr *) &event.peer->address.address) << "]: it's not properly set up"sv;
        enet_peer_disconnect_now(event.peer, 0);

        return;
      }

      session->pingTimeout = std::chrono::steady_clock::now() + config::stream.ping_timeout;

      switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE: {
          net::packet_t packet { event.packet };

          auto type = *(std::uint16_t *) packet->data;
          std::string_view payload { (char *) packet->data + sizeof(type), packet->dataLength - sizeof(type) };

          call(type, session, payload, false);
        } break;
        case ENET_EVENT_TYPE_CONNECT:
          BOOST_LOG(info) << "CLIENT CONNECTED"sv;
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          BOOST_LOG(info) << "CLIENT DISCONNECTED"sv;
          // No more clients to send video data to ^_^
          if (session->state == session::state_e::RUNNING) {
            session::stop(*session);
          }
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
      BOOST_LOG(warning) << "Couldn't send gamepad feedback data, still waiting for PING from Moonlight"sv;
      // Still waiting for PING from Moonlight
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
      BOOST_LOG(warning) << "Couldn't send gamepad feedback to ["sv << addr << ':' << port << ']';

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

  void
  controlBroadcastThread(control_server_t *server) {
    server->map(packetTypes[IDX_PERIODIC_PING], [](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_PERIODIC_PING]"sv;
    });

    server->map(packetTypes[IDX_START_A], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_A]"sv;
    });

    server->map(packetTypes[IDX_START_B], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_START_B]"sv;
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

    server->map(packetTypes[IDX_REQUEST_IDR_FRAME], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_REQUEST_IDR_FRAME]"sv;

      session->video.idr_events->raise(true);
    });

    server->map(packetTypes[IDX_DYNAMIC_BITRATE_CHANGE], [&](session_t *session, const std::string_view &payload) {
      BOOST_LOG(debug) << "type [IDX_DYNAMIC_BITRATE_CHANGE]"sv;

      if (payload.size() >= sizeof(int)) {
        auto new_bitrate = *(int *) payload.data();
        BOOST_LOG(info) << "Dynamic bitrate change requested: " << new_bitrate << " Kbps";

        // 验证码率范围
        if (new_bitrate > 0 && new_bitrate <= 800000) {  // 最大800Mbps
          video::dynamic_param_t param;
          param.type = video::dynamic_param_type_e::BITRATE;
          param.value.int_value = new_bitrate;
          param.valid = true;
          session->video.dynamic_param_change_events->raise(param);
        }
        else {
          BOOST_LOG(warning) << "Invalid bitrate value: " << new_bitrate << " Kbps";
        }
      }
      else {
        BOOST_LOG(warning) << "Invalid payload size for dynamic bitrate change";
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

      input::passthrough(session->input, std::move(plaintext));
    });

    server->map(packetTypes[IDX_ENCRYPTED], [server](session_t *session, const std::string_view &payload) {
      BOOST_LOG(verbose) << "type [IDX_ENCRYPTED]"sv;

      auto header = (control_encrypted_p) (payload.data() - 2);

      auto length = util::endian::little(header->length);
      auto seq = util::endian::little(header->seq);

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

        BOOST_LOG(error) << "Failed to verify tag"sv;

        session::stop(*session);
        return;
      }

      auto type = *(std::uint16_t *) plaintext.data();
      std::string_view next_payload { (char *) plaintext.data() + 4, plaintext.size() - 4 };

      if (type == packetTypes[IDX_ENCRYPTED]) {
        BOOST_LOG(error) << "Bad packet type [IDX_ENCRYPTED] found"sv;
        session::stop(*session);
        return;
      }

      // IDX_INPUT_DATA callback will attempt to decrypt unencrypted data, therefore we need pass it directly
      if (type == packetTypes[IDX_INPUT_DATA]) {
        plaintext.erase(std::begin(plaintext), std::begin(plaintext) + 4);
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
            session::stop(*session);
          }

          if (session->state.load(std::memory_order_acquire) == session::state_e::STOPPING) {
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
          }

          ++pos;
        })
      }

      // Don't break until any pending sessions either expire or connect
      if (proc::proc.running() == 0 && !has_session_awaiting_peer) {
        BOOST_LOG(info) << "Process terminated"sv;
        break;
      }

      server->iterate(150ms);
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
    auto &io = ctx.io_context;

    udp::endpoint peer;
    std::array<char, 2048> mic_recv_buffer;
    bool mic_device_initialized = false;

    // 麦克风统计结构体（按客户端地址分组）
    struct MicStats {
      uint64_t total_packets = 0;
      uint64_t decrypt_success = 0;
      uint64_t decrypt_failed = 0;
      uint64_t invalid_data = 0;
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

    auto process_audio_data = [&](const uint8_t *audio_data, size_t data_size, uint16_t sequence_number, const std::string &peer_addr) {
      if (!ctx.mic_socket_enabled.load()) {
        return;
      }

      // 根据 RTSP 协商的配置判断是否启用加密
      // 不依赖数据长度猜测，直接使用会话配置作为权威来源
      bool is_encrypted = ctx.mic_encryption_enabled.load();

      // 更新统计
      auto &stats = client_stats[peer_addr];
      stats.total_packets++;

      if (is_encrypted && ctx.mic_cipher.has_value()) {
        std::lock_guard<std::mutex> lg(ctx.mic_cipher_mutex);
        // 根据 sequenceNumber 更新 IV
        // 客户端使用: baseIv[0:4] (Big Endian) + (sequenceNumber - 1) & 0xFFFF
        // 这与音频加密不同，音频加密使用: avRiKeyId + sequenceNumber
        // ctx.mic_iv 的前 4 字节存储的是 baseIv（大端序），对应客户端的 remoteInputAesIv
        crypto::aes_t current_iv(16);  // 确保是 16 字节
        uint32_t baseIvVal = util::endian::big<std::uint32_t>(*(std::uint32_t *) ctx.mic_iv.data());
        // 服务端收到的 sequence_number 就是包里的实际值，直接使用即可（不需要减1），客户端减1是因为它的 sequenceNumber 变量在写入包后就递增了
        uint32_t ivSeq = baseIvVal + (sequence_number & 0xFFFF);
        *(std::uint32_t *) current_iv.data() = util::endian::big<std::uint32_t>(ivSeq);
        // 确保后 12 字节为 0（客户端构建 IV 时后 12 字节也是 0）
        std::memset(current_iv.data() + 4, 0, 12);
        std::vector<std::uint8_t> plaintext;
        std::string_view cipher_view((const char *) audio_data, data_size);
        if (ctx.mic_cipher->decrypt(cipher_view, plaintext, &current_iv) != 0) {
          // 解密失败：可能是网络损坏包、IV不匹配、或密钥错误
          stats.decrypt_failed++;
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
        audio::write_mic_data(plaintext.data(), plaintext.size(), sequence_number);
        return;
      }

      // 安全模式：拒绝明文数据
      if (ctx.mic_reject_plaintext.load()) {
        BOOST_LOG(warning) << "Rejected plaintext microphone data (mic_reject_plaintext enabled)";
        stats.decrypt_failed++;
        return;
      }

      // 未加密数据或加密未启用，直接处理
      // 也要统计未加密数据
      stats.decrypt_success++;  // 明文数据算作"成功"
      audio::write_mic_data(audio_data, data_size, sequence_number);
    };

    std::function<void(const boost::system::error_code, size_t)> mic_recv_func;
    mic_recv_func = [&](const boost::system::error_code &ec, size_t received_bytes) {
      if (!ctx.mic_socket_enabled.load()) {
        return;
      }

      auto fg = util::fail_guard([&]() {
        if (ctx.mic_socket_enabled.load()) {
          ctx.mic_sock.async_receive_from(asio::buffer(mic_recv_buffer), peer, 0, mic_recv_func);
        }
      });

      if (ec) {
        if (ec == boost::asio::error::operation_aborted) {
          BOOST_LOG(info) << "Mic socket normally closed";
          return;
        }
        if (ec != boost::system::errc::connection_refused &&
            ec != boost::system::errc::connection_reset) {
          BOOST_LOG(error) << "Mic socket error: "sv << ec.message();
        }
        return;
      }

      if (received_bytes < sizeof(RTP_PACKET)) {
        return;
      }

      // 获取客户端标识：设备名拼接IP地址
      std::string client_ip = peer.address().to_string();
      std::string client_id;
      {
        std::lock_guard<std::mutex> lg(ctx.client_name_mutex);
        auto it = ctx.client_ip_to_name.find(client_ip);
        if (it != ctx.client_ip_to_name.end()) {
          client_id = it->second + "@" + client_ip;  // 设备名@IP
        } else {
          client_id = "@" + client_ip;  // 回退到IP（未知设备名时）
        }
      }

      // 尝试16位扩展包类型
      if (received_bytes >= sizeof(rtp_packet_ext_t)) {
        auto *header_ext = (rtp_packet_ext_t *) mic_recv_buffer.data();
        if (header_ext->packetType == packetTypes[IDX_MIC_DATA]) {
          size_t header_size = sizeof(rtp_packet_ext_t);
          if (received_bytes > header_size) {
            uint16_t sequence_number = util::endian::little(header_ext->sequenceNumber);
            // uint32_t ssrc = util::endian::little(header_ext->ssrc);  // 小端序
            // if (!validate_mic_ssrc(ssrc, client_id)) {
            //   return;
            // }
            process_audio_data(reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size, received_bytes - header_size, sequence_number, client_id);
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
          process_audio_data(reinterpret_cast<const uint8_t *>(mic_recv_buffer.data()) + header_size, data_size, sequence_number, client_id);
        }
      }
    };

    BOOST_LOG(debug) << "Starting microphone receive thread";

    auto retry_delay = 300ms;  // 初始重试延迟，指数退避到最大5秒

    while (!broadcast_shutdown_event->peek()) {
      if (!ctx.mic_socket_enabled.load()) {
        retry_delay = 300ms;  // 会话结束时重置延迟
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
        io.run();
      }
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
                         << ", invalid=" << stats.invalid_data;
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

      auto fecPercentage = config::stream.fec_percentage;

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
        fecPercentage = 0;
        fec_blocks_needed = MAX_FEC_BLOCKS;
      }

      std::array<std::string_view, MAX_FEC_BLOCKS> fec_blocks;
      decltype(fec_blocks)::iterator
        fec_blocks_begin = std::begin(fec_blocks),
        fec_blocks_end = std::begin(fec_blocks) + fec_blocks_needed;

      BOOST_LOG(verbose) << "Generating "sv << fec_blocks_needed << " FEC blocks"sv;

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
        // Use around 80% of 1Gbps          1Gbps            percent    ms     packet      byte
        size_t ratecontrol_packets_in_1ms = std::giga::num * 80 / 100 / 1000 / blocksize / 8;

        // Send less than 64K in a single batch.
        // On Windows, batches above 64K seem to bypass SO_SNDBUF regardless of its size,
        // appear in "Other I/O" and begin waiting for interrupts.
        // This gives inconsistent performance so we'd rather avoid it.
        size_t send_batch_size = 64 * 1024 / blocksize;
        // Also don't exceed 64 packets, which can happen when Moonlight requests
        // unusually small packet size.
        // Generic Segmentation Offload on Linux can't do more than 64.
        send_batch_size = std::min<size_t>(64, send_batch_size);

        // Don't ignore the last ratecontrol group of the previous frame
        auto ratecontrol_frame_start = std::max(ratecontrol_next_frame_start, std::chrono::steady_clock::now());

        size_t ratecontrol_frame_packets_sent = 0;
        size_t ratecontrol_group_packets_sent = 0;

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
              // Do pacing within the frame.
              // Also trigger pacing before the first send_batch() of the frame
              // to account for the last send_batch() of the previous frame.
              if (ratecontrol_group_packets_sent >= ratecontrol_packets_in_1ms ||
                  ratecontrol_frame_packets_sent == 0) {
                auto due = ratecontrol_frame_start +
                           std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                             ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;

                auto now = std::chrono::steady_clock::now();
                if (now < due) {
                  timer->sleep_for(due - now);
                }

                ratecontrol_group_packets_sent = 0;
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

              ratecontrol_group_packets_sent += current_batch_size;
              ratecontrol_frame_packets_sent += current_batch_size;
              next_shard_to_send = x + 1;
            }
          }

          // remember this in case the next frame comes immediately
          ratecontrol_next_frame_start = ratecontrol_frame_start +
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) *
                                           ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;

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

    ctx.video_sock.close();
    ctx.audio_sock.close();

    // 确保麦克风socket已关闭并清理敏感数据
    if (ctx.mic_socket_enabled.load()) {
      ctx.mic_socket_enabled.store(false);
      ctx.mic_sock.close();
      ctx.mic_sessions_count.store(0);
      
      // 安全清理：清零所有麦克风加密相关的敏感数据
      {
        std::lock_guard<std::mutex> lg(ctx.mic_cipher_mutex);
        if (ctx.mic_cipher.has_value()) {
          ctx.mic_cipher.reset();  // 销毁cipher对象并释放密钥
        }
        std::memset(ctx.mic_iv.data(), 0, ctx.mic_iv.size());  // 清零IV
        ctx.mic_encryption_enabled.store(false);
      }
      
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

    // Only allow matches on the peer address for legacy clients
    if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
      ref->message_queue_queue->raise(type, peer.address(), messages);
    }
    ref->message_queue_queue->raise(type, session_id, messages);

    auto fg = util::fail_guard([&]() {
      messages->stop();

      // remove message queue from session
      if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1)) {
        ref->message_queue_queue->raise(type, peer.address(), nullptr);
      }
      ref->message_queue_queue->raise(type, session_id, nullptr);
    });

    auto start_time = std::chrono::steady_clock::now();
    auto current_time = start_time;

    while (current_time - start_time < config::stream.ping_timeout) {
      auto delta_time = current_time - start_time;

      auto msg_opt = messages->pop(config::stream.ping_timeout - delta_time);
      if (!msg_opt) {
        break;
      }

      TUPLE_2D_REF(recv_peer, msg, *msg_opt);
      if (msg.find(expected_payload) != std::string::npos) {
        // Match the new PING payload format
        BOOST_LOG(debug) << "Received ping [v2] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      }
      else if (!(session->config.mlFeatureFlags & ML_FF_SESSION_ID_V1) && msg == "PING"sv) {
        // Match the legacy fixed PING payload only if the new type is not supported
        BOOST_LOG(debug) << "Received ping [v1] from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
      }
      else {
        BOOST_LOG(debug) << "Received non-ping from "sv << recv_peer.address() << ':' << recv_peer.port() << " ["sv << util::hex_vec(msg) << ']';
        current_time = std::chrono::steady_clock::now();
        continue;
      }

      // Update connection details.
      peer = recv_peer;
      return 0;
    }

    BOOST_LOG(error) << "Initial Ping Timeout"sv;
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

    BOOST_LOG(debug) << "Start capturing Video"sv;
    // Debug: Log the display_name before calling video::capture
    BOOST_LOG(debug) << "stream.cpp: session->config.monitor.display_name = [" << (session->config.monitor.display_name.empty() ? "<empty>" : session->config.monitor.display_name) << "]";
    video::capture(session->mail, session->config.monitor, session, session->video.dynamic_param_change_events);
  }

  void
  audioThread(session_t *session) {
    auto fg = util::fail_guard([&]() {
      session::stop(*session);
    });

    while_starting_do_nothing(session->state);

    auto ref = broadcast_shared.ref();
    auto error = recv_ping(session, ref, socket_e::audio, session->audio.ping_payload, session->audio.peer, config::stream.ping_timeout);
    if (error < 0) {
      return;
    }

    // Enable local prioritization and QoS tagging on audio traffic if requested by the client
    auto address = session->audio.peer.address();
    session->audio.qos = platf::enable_socket_qos(ref->audio_sock.native_handle(), address,
      session->audio.peer.port(), platf::qos_data_type_e::audio, session->config.audioQosType != 0);

    BOOST_LOG(debug) << "Start capturing Audio"sv;
    audio::capture(session->mail, session->config.audio, session);
  }

  namespace session {
    std::atomic_uint running_sessions;
    std::atomic_uint running_non_control_only_sessions;  // 跟踪非仅控制流会话的数量

    state_e
    state(session_t &session) {
      return session.state.load(std::memory_order_relaxed);
    }

    void
    stop(session_t &session) {
      while_starting_do_nothing(session.state);
      auto expected = state_e::RUNNING;
      auto already_stopping = !session.state.compare_exchange_strong(expected, state_e::STOPPING);
      if (already_stopping) {
        return;
      }

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

      // 仅控制流会话没有视频/音频线程
      if (!session.control_only) {
        BOOST_LOG(debug) << "Waiting for video to end..."sv;
        session.videoThread.join();
        BOOST_LOG(debug) << "Waiting for audio to end..."sv;
        session.audioThread.join();
      }
      else {
        BOOST_LOG(debug) << "Control-only session: skipping video/audio thread join"sv;
      }
      BOOST_LOG(debug) << "Waiting for control to end..."sv;
      session.controlEnd.view();
      // Reset input on session stop to avoid stuck repeated keys
      BOOST_LOG(debug) << "Resetting Input..."sv;
      input::reset(session.input);

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
            session.broadcast_ref->mic_sock.close();
            session.broadcast_ref->mic_socket_enabled.store(false);
            session.broadcast_ref->mic_sessions_count.store(0);
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

          platf::streaming_will_stop();
        }
        else {
          // 非最后一个会话：如果当前会话启用了麦克风，减少计数
          if (session.audio.enable_mic) {
            int remaining_count = session.broadcast_ref->mic_sessions_count.fetch_sub(1) - 1;
            if (remaining_count == 0) {
              // 没有会话需要麦克风了，关闭socket
              session.broadcast_ref->mic_sock.close();
              session.broadcast_ref->mic_socket_enabled.store(false);
              BOOST_LOG(debug) << "Microphone socket closed (no sessions require it)";
            }
            else {
              BOOST_LOG(debug) << "Microphone sessions remaining: " << remaining_count;
            }
          }
        }
      }

      BOOST_LOG(debug) << "Session ended"sv;
    }

    int
    start(session_t &session, const std::string &addr_string) {
      session.input = input::alloc(session.mail);

      session.broadcast_ref = broadcast_shared.ref();
      if (!session.broadcast_ref) {
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

      // 仅控制流会话不触发 streaming_will_start 回调，因为它们不传输视频/音频
      // 但它们仍然需要被计入 running_sessions，以便正确管理会话
      if (session.control_only) {
        // 仅控制流会话：只增加总会话计数，不调用平台回调
        ++running_sessions;
        BOOST_LOG(debug) << "Control-only session started (total sessions: "sv << running_sessions.load() << ")"sv;
      }
      else {
        // 非仅控制流会话：增加两个计数器
        ++running_sessions;
        // If this is the first non-control-only session, invoke the platform callbacks
        if (++running_non_control_only_sessions == 1) {
          // 根据会话的麦克风启用标志管理麦克风socket
          if (session.audio.enable_mic) {
            session.broadcast_ref->mic_sessions_count.fetch_add(1);
            session.broadcast_ref->mic_socket_enabled.store(true);
            
            // 注册客户端IP到名称的映射（用于麦克风统计日志）
            std::string client_ip = session.audio.peer.address().to_string();
            {
              std::lock_guard<std::mutex> lg(session.broadcast_ref->client_name_mutex);
              session.broadcast_ref->client_ip_to_name[client_ip] = session.client_name;
              BOOST_LOG(debug) << "Mic Registered client mapping: " << client_ip << " -> " << session.client_name;
            }
            
            // 检查是否需要启用 MIC 加密
            bool should_enable_mic_encryption = (session.config.encryptionFlagsEnabled & SS_ENC_MIC) != 0;
            if (should_enable_mic_encryption) {
              std::lock_guard<std::mutex> lg(session.broadcast_ref->mic_cipher_mutex);
              if (!session.broadcast_ref->mic_cipher) {
                session.broadcast_ref->mic_cipher.emplace(session.audio.cipher.key, session.audio.cipher.padding);
                session.broadcast_ref->mic_iv.resize(16);
                // 初始化 IV：前 4 字节存储 baseIv（大端序）
                // baseIv 对应客户端的 remoteInputAesIv 的前 4 字节
                // avRiKeyId 就是 launch_session.iv 的前 4 字节（大端序），与 remoteInputAesIv 的前 4 字节相同
                *(std::uint32_t *) session.broadcast_ref->mic_iv.data() = util::endian::big<std::uint32_t>(session.audio.avRiKeyId);
                // 其余字节保持为 0（IV 是 16 字节，但只使用前 4 字节）
                std::memset(session.broadcast_ref->mic_iv.data() + 4, 0, 12);
                session.broadcast_ref->mic_encryption_enabled.store(true);
              }
              BOOST_LOG(info) << "Client " << session.client_name << ": Microphone encryption ENABLED";
            } else {
              BOOST_LOG(info) << "Client " << session.client_name << ": Microphone encryption DISABLED";
            }
          }
          else {
            // 如果第一个会话不需要麦克风，关闭麦克风socket
            session.broadcast_ref->mic_sock.close();
            session.broadcast_ref->mic_socket_enabled.store(false);
            BOOST_LOG(info) << "Client " << session.client_name << ": Microphone socket closed (session doesn't require it)";
          }

          platf::streaming_will_start();
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_playing(proc::proc.get_last_run_app_name());
#endif
        }
        else {
          // 非第一个会话：如果启用麦克风，增加计数
          if (session.audio.enable_mic) {
            session.broadcast_ref->mic_sessions_count.fetch_add(1);
            session.broadcast_ref->mic_socket_enabled.store(true);
            
            // 注册客户端IP到名称的映射
            std::string client_ip = session.audio.peer.address().to_string();
            {
              std::lock_guard<std::mutex> lg(session.broadcast_ref->client_name_mutex);
              session.broadcast_ref->client_ip_to_name[client_ip] = session.client_name;
              BOOST_LOG(debug) << "Registered client mapping: " << client_ip << " -> " << session.client_name;
            }
            
            BOOST_LOG(debug) << "Microphone socket enabled for additional session";
          }
        }
      }

      return 0;
    }

    std::shared_ptr<session_t>
    alloc(config_t &config, rtsp_stream::launch_session_t &launch_session) {
      auto session = std::make_shared<session_t>();

      auto mail = std::make_shared<safe::mail_raw_t>();

      session->shutdown_event = mail->event<bool>(mail::shutdown);
      session->launch_session_id = launch_session.id;

      // 设置客户端名称
      session->client_name = launch_session.client_name;

      session->config = config;

      // Initialize current total bitrate (including FEC) from config
      // config.monitor.bitrate is the encoding bitrate (excluding FEC)
      // We need to convert it to total bitrate (including FEC)
      int encoding_bitrate = config.monitor.bitrate;
      int fec_percentage = config::stream.fec_percentage;
      if (fec_percentage > 0 && fec_percentage <= 80) {
        // Convert encoding bitrate to total bitrate: total = encoding * 100 / (100 - fec_percentage)
        session->current_total_bitrate = (int) (encoding_bitrate * 100.f / (100 - fec_percentage));
      }
      else {
        // If FEC percentage is 0 or > 80%, encoding bitrate equals total bitrate
        session->current_total_bitrate = encoding_bitrate;
      }

      session->control.connect_data = launch_session.control_connect_data;
      session->control.feedback_queue = mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback);
      session->control.hdr_queue = mail->event<video::hdr_info_t>(mail::hdr);
      session->control.legacy_input_enc_iv = launch_session.iv;
      session->control.cipher = crypto::cipher::gcm_t {
        launch_session.gcm_key, false
      };

      session->video.idr_events = mail->event<bool>(mail::idr);
      session->video.invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);
      session->video.dynamic_param_change_events = mail->event<video::dynamic_param_t>(mail::dynamic_param_change);
      session->video.lowseq = 0;
      session->video.ping_payload = launch_session.av_ping_payload;
      if (config.encryptionFlagsEnabled & SS_ENC_VIDEO) {
        BOOST_LOG(info) << "Video encryption enabled"sv;
        session->video.cipher = crypto::cipher::gcm_t {
          launch_session.gcm_key, false
        };
        session->video.gcm_iv_counter = 0;
      }

      constexpr auto max_block_size = crypto::cipher::round_to_pkcs7_padded(2048);

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

      session->mail = std::move(mail);

      return session;
    }

    bool
    change_dynamic_param_for_client(const std::string &client_name, const video::dynamic_param_t &param) {
      auto broadcast_ref = broadcast_shared.ref();
      if (!broadcast_ref) {
        BOOST_LOG(warning) << "No broadcast context available when changing dynamic parameter for client";
        return false;
      }

      auto lg = broadcast_ref->control_server._sessions.lock();
      for (auto session_p : *broadcast_ref->control_server._sessions) {
        if (session_p->client_name == client_name &&
            session_p->state.load(std::memory_order_relaxed) == state_e::RUNNING) {
          // Update session's current total bitrate if this is a bitrate change
          if (param.type == video::dynamic_param_type_e::BITRATE && param.valid) {
            // The param.value.int_value is the total bitrate (user-configured, including FEC)
            session_p->current_total_bitrate = param.value.int_value;
            BOOST_LOG(info) << "Updated session total bitrate for client '" << client_name
                            << "': " << param.value.int_value << " Kbps (including FEC)";
          }

          session_p->video.dynamic_param_change_events->raise(param);
          BOOST_LOG(info) << "Sent dynamic parameter change event to client '" << client_name
                          << "': type=" << (int) param.type;
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

          info.client_name = session_p->client_name;
          info.session_id = session_p->launch_session_id;

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
          auto state = session_p->state.load(std::memory_order_relaxed);
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
