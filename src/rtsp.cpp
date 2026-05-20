/**
 * @file src/rtsp.cpp
 * @brief Definitions for RTSP streaming.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include <moonlight-common-c/src/Limelight-internal.h>
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
#include <libavcodec/avcodec.h>
}

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#include "rtsp.h"
#include "stream.h"
#include "stream_bitrate.h"
#include "stream_quality.h"
#include "sync.h"
#include "video.h"

namespace asio = boost::asio;

using asio::ip::tcp;
using asio::ip::udp;

using namespace std::literals;

namespace rtsp_stream {
  namespace {
    constexpr int safe_max_stream_fec_percentage = 100;
    constexpr int enhanced_feedback_startup_fec_percentage = 10;
    constexpr int enhanced_feedback_adaptive_fec_percentage = 35;

    std::string
    rtsp_launch_state(const launch_session_t &session) {
      std::ostringstream ss;
      ss << "launchSession=" << session.unique_id
         << " id=" << session.id
         << " appid=" << session.appid
         << " peer=" << session.rtsp_peer_address
         << " mode=" << session.width << "x" << session.height << '@' << session.fps
         << " mic=" << (session.enable_mic ? 1 : 0)
         << " setup(video/audio/control/mic)="
         << (session.setup_video ? 1 : 0) << '/'
         << (session.setup_audio ? 1 : 0) << '/'
         << (session.setup_control ? 1 : 0) << '/'
         << (session.setup_mic ? 1 : 0);
      return ss.str();
    }

    bool
    same_known_launch_client(const launch_session_t &left, const launch_session_t &right) {
      return !left.identity.client_cert_uuid.empty() &&
             left.identity.client_cert_uuid == right.identity.client_cert_uuid;
    }

    bool
    pending_launch_has_rtsp_progress(const launch_session_t &session) {
      return session.setup_video ||
             session.setup_audio ||
             session.setup_control ||
             session.setup_mic;
    }
  }

  std::uint64_t
  foundation_streaming_feature_flags2() {
    return static_cast<std::uint64_t>(LI_FF2_QOS_FEEDBACK) |
           static_cast<std::uint64_t>(LI_FF2_INPUT_PRIORITY) |
           static_cast<std::uint64_t>(LI_FF2_LOW_BITRATE_QUALITY) |
           static_cast<std::uint64_t>(LI_FF2_CURSOR_PLANE) |
           static_cast<std::uint64_t>(LI_FF2_AUDIO_CONTINUITY) |
           static_cast<std::uint64_t>(LI_FF2_VISUAL_FRESHNESS);
  }

  int
  effective_stream_fec_percentage_for_client(int configured_fec_percentage,
                                             int ml_feature_flags,
                                             bool adaptive_controller_enabled) {
    configured_fec_percentage = std::clamp(configured_fec_percentage, 0, safe_max_stream_fec_percentage);
    if (adaptive_controller_enabled && (ml_feature_flags & ML_FF_NETWORK_FEEDBACK) != 0) {
      return std::min(configured_fec_percentage, enhanced_feedback_startup_fec_percentage);
    }
    return configured_fec_percentage;
  }

  int
  effective_stream_fec_percentage_for_client(int configured_fec_percentage,
                                             int ml_feature_flags) {
    return effective_stream_fec_percentage_for_client(configured_fec_percentage,
                                                      ml_feature_flags,
                                                      true);
  }

  int
  adaptive_stream_max_fec_percentage_for_client(int configured_fec_percentage,
                                                int ml_feature_flags,
                                                bool adaptive_controller_enabled) {
    configured_fec_percentage = std::clamp(configured_fec_percentage, 0, safe_max_stream_fec_percentage);
    if (!adaptive_controller_enabled ||
        (ml_feature_flags & ML_FF_NETWORK_FEEDBACK) == 0 ||
        configured_fec_percentage == 0) {
      return configured_fec_percentage;
    }

    return std::min(std::max(configured_fec_percentage, enhanced_feedback_adaptive_fec_percentage),
                    enhanced_feedback_adaptive_fec_percentage);
  }

  int
  adaptive_stream_max_fec_percentage_for_client(int configured_fec_percentage,
                                                int ml_feature_flags) {
    return adaptive_stream_max_fec_percentage_for_client(configured_fec_percentage,
                                                        ml_feature_flags,
                                                        true);
  }

  std::int64_t
  adjust_configured_video_bitrate_kbps(std::int64_t configured_bitrate_kbps,
                                       int fec_percentage,
                                       bool high_quality_audio,
                                       int audio_channels) {
    fec_percentage = std::clamp(fec_percentage, 0, safe_max_stream_fec_percentage);
    return stream_bitrate::encoding_bitrate_from_configured_total_kbps(configured_bitrate_kbps,
                                                                       fec_percentage,
                                                                       high_quality_audio,
                                                                       audio_channels);
  }

  std::int64_t
  video_quality_ceiling_bitrate_kbps(std::int64_t configured_bitrate_kbps,
                                     int fec_percentage,
                                     bool high_quality_audio,
                                     int audio_channels,
                                     int ml_feature_flags) {
    if ((ml_feature_flags & ML_FF_NETWORK_FEEDBACK) != 0) {
      return configured_bitrate_kbps;
    }

    return adjust_configured_video_bitrate_kbps(configured_bitrate_kbps,
                                                fec_percentage,
                                                high_quality_audio,
                                                audio_channels);
  }

  namespace {
    bool
    parse_legacy_surround_params(std::string_view params, int requested_channels, audio::stream_params_t &result) {
      if (params.length() <= 3 || !std::all_of(params.begin(), params.end(), [](char c) { return std::isdigit((unsigned char) c); })) {
        return false;
      }

      int channel_count = params[0] - '0';
      int streams = params[1] - '0';
      int coupled_streams = params[2] - '0';
      if (channel_count != requested_channels || channel_count < 2 || channel_count > platf::speaker::MAX_SPEAKERS ||
          streams + coupled_streams != channel_count || params.length() != (size_t) channel_count + 3) {
        return false;
      }

      for (int i = 0; i < channel_count; ++i) {
        auto map_value = params[i + 3] - '0';
        if (map_value < 0 || map_value >= channel_count) {
          return false;
        }

        result.mapping[i] = (std::uint8_t) map_value;
      }

      result.channelCount = channel_count;
      result.streams = streams;
      result.coupledStreams = coupled_streams;
      return true;
    }

    bool
    parse_delimited_surround_params(std::string_view params, int requested_channels, audio::stream_params_t &result) {
      std::vector<int> values;
      values.reserve(3 + platf::speaker::MAX_SPEAKERS);

      int current = 0;
      bool has_digit = false;
      for (char ch : params) {
        if (std::isdigit((unsigned char) ch)) {
          current = current * 10 + (ch - '0');
          has_digit = true;
          continue;
        }

        if (ch == ',' || ch == ';' || ch == ':' || ch == '|' || std::isspace((unsigned char) ch)) {
          if (has_digit) {
            values.push_back(current);
            current = 0;
            has_digit = false;
          }
          continue;
        }

        return false;
      }

      if (has_digit) {
        values.push_back(current);
      }

      if (values.size() < 3) {
        return false;
      }

      int channel_count = values[0];
      int streams = values[1];
      int coupled_streams = values[2];

      if (channel_count != requested_channels || channel_count < 2 || channel_count > platf::speaker::MAX_SPEAKERS ||
          streams + coupled_streams != channel_count || values.size() != (size_t) channel_count + 3) {
        return false;
      }

      for (int i = 0; i < channel_count; ++i) {
        auto map_value = values[i + 3];
        if (map_value < 0 || map_value >= channel_count) {
          return false;
        }

        result.mapping[i] = (std::uint8_t) map_value;
      }

      result.channelCount = channel_count;
      result.streams = streams;
      result.coupledStreams = coupled_streams;
      return true;
    }

    bool
    parse_surround_params(std::string_view params, int requested_channels, audio::stream_params_t &result) {
      return parse_legacy_surround_params(params, requested_channels, result) ||
             parse_delimited_surround_params(params, requested_channels, result);
    }
  }  // namespace

  void
  free_msg(PRTSP_MESSAGE msg) {
    freeMessage(msg);

    delete msg;
  }

#pragma pack(push, 1)

  struct encrypted_rtsp_header_t {
    // We set the MSB in encrypted RTSP messages to allow format-agnostic
    // parsing code to be able to tell encrypted from plaintext messages.
    static constexpr std::uint32_t ENCRYPTED_MESSAGE_TYPE_BIT = 0x80000000;

    uint8_t *
    payload() {
      return (uint8_t *) (this + 1);
    }

    std::uint32_t
    payload_length() {
      return util::endian::big<std::uint32_t>(typeAndLength) & ~ENCRYPTED_MESSAGE_TYPE_BIT;
    }

    bool
    is_encrypted() {
      return !!(util::endian::big<std::uint32_t>(typeAndLength) & ENCRYPTED_MESSAGE_TYPE_BIT);
    }

    // This field is the length of the payload + ENCRYPTED_MESSAGE_TYPE_BIT in big-endian
    std::uint32_t typeAndLength;

    // This field is the number used to initialize the bottom 4 bytes of the AES IV in big-endian
    std::uint32_t sequenceNumber;

    // This field is the AES GCM authentication tag
    std::uint8_t tag[16];
  };

#pragma pack(pop)

  class rtsp_server_t;

  using msg_t = util::safe_ptr<RTSP_MESSAGE, free_msg>;
  using cmd_func_t = std::function<void(rtsp_server_t *server, tcp::socket &, launch_session_t &, msg_t &&)>;

  void
  print_msg(PRTSP_MESSAGE msg);
  void
  cmd_not_found(tcp::socket &sock, launch_session_t &, msg_t &&req);
  void
  respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload);

  class socket_t: public std::enable_shared_from_this<socket_t> {
  public:
    socket_t(boost::asio::io_context &io_context, std::function<void(tcp::socket &sock, launch_session_t &, msg_t &&)> &&handle_data_fn):
        handle_data_fn { std::move(handle_data_fn) },
        sock { io_context } {
    }

    /**
     * @brief Queue an asynchronous read to begin the next message.
     */
    void
    read() {
      if (begin == std::end(msg_buf) || (session->rtsp_cipher && begin + sizeof(encrypted_rtsp_header_t) >= std::end(msg_buf))) {
        BOOST_LOG(error) << "RTSP: read(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);
        if (ec) {
          BOOST_LOG(debug) << "Error closing socket: "sv << ec.message();
        }

        return;
      }

      if (session->rtsp_cipher) {
        // For encrypted RTSP, we will read the the entire header first
        boost::asio::async_read(sock, boost::asio::buffer(begin, sizeof(encrypted_rtsp_header_t)), boost::bind(&socket_t::handle_read_encrypted_header, shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
      }
      else {
        sock.async_read_some(
          boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
          boost::bind(
            &socket_t::handle_read_plaintext,
            shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
      }
    }

    /**
     * @brief Handle the initial read of the header of an encrypted message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void
    handle_read_encrypted_header(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted_header(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec || bytes < sizeof(encrypted_rtsp_header_t)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      if (!header->is_encrypted()) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Rejecting unencrypted RTSP message"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      auto payload_length = header->payload_length();

      // Check if we have enough space to read this message
      if (socket->begin + sizeof(*header) + payload_length >= std::end(socket->msg_buf)) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted_header(): Exceeded maximum rtsp packet size: "sv << socket->msg_buf.size();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      // Read the remainder of the header and full encrypted payload
      boost::asio::async_read(socket->sock, boost::asio::buffer(socket->begin + bytes, payload_length), boost::bind(&socket_t::handle_read_encrypted_message, socket->shared_from_this(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
    }

    /**
     * @brief Handle the final read of the content of an encrypted message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void
    handle_read_encrypted_message(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_encrypted(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_encrypted_message(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      auto header = (encrypted_rtsp_header_t *) socket->begin;
      auto payload_length = header->payload_length();
      auto seq = util::endian::big<std::uint32_t>(header->sequenceNumber);

      if (ec || bytes < payload_length) {
        BOOST_LOG(error) << "RTSP: handle_read_encrypted(): Couldn't read from tcp socket: "sv << ec.message();

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RC' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // received from each client before the IV repeats.
      crypto::aes_t iv(12);
      std::copy_n((uint8_t *) &seq, sizeof(seq), std::begin(iv));
      iv[10] = 'C';  // Client originated
      iv[11] = 'R';  // RTSP

      std::vector<uint8_t> plaintext;
      if (socket->session->rtsp_cipher->decrypt(std::string_view { (const char *) header->tag, sizeof(header->tag) + bytes }, plaintext, &iv)) {
        BOOST_LOG(error) << "Failed to verify RTSP message tag"sv;

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      msg_t req { new msg_t::element_type {} };
      if (auto status = parseRtspMessage(req.get(), (char *) plaintext.data(), plaintext.size())) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      print_msg(req.get());

      socket->handle_data(std::move(req));
    }

    /**
     * @brief Queue an asynchronous read of the payload portion of a plaintext message.
     */
    void
    read_plaintext_payload() {
      if (begin == std::end(msg_buf)) {
        BOOST_LOG(error) << "RTSP: read_plaintext_payload(): Exceeded maximum rtsp packet size: "sv << msg_buf.size();

        respond(sock, *session, nullptr, 400, "BAD REQUEST", 0, {});

        boost::system::error_code ec;
        sock.close(ec);
        if (ec) {
          BOOST_LOG(debug) << "Error closing socket: "sv << ec.message();
        }

        return;
      }

      sock.async_read_some(
        boost::asio::buffer(begin, (std::size_t) (std::end(msg_buf) - begin)),
        boost::bind(
          &socket_t::handle_plaintext_payload,
          shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
    }

    /**
     * @brief Handle the read of the payload portion of a plaintext message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void
    handle_plaintext_payload(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_plaintext_payload(): Handle read of size: "sv << bytes << " bytes"sv;

      auto sock_close = util::fail_guard([&socket]() {
        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't close tcp socket: "sv << ec.message();
        }
      });

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_plaintext_payload(): Couldn't read from tcp socket: "sv << ec.message();

        return;
      }

      auto end = socket->begin + bytes;
      msg_t req { new msg_t::element_type {} };
      if (auto status = parseRtspMessage(req.get(), socket->msg_buf.data(), (std::size_t) (end - socket->msg_buf.data()))) {
        BOOST_LOG(error) << "Malformed RTSP message: ["sv << status << ']';

        respond(socket->sock, *socket->session, nullptr, 400, "BAD REQUEST", 0, {});
        return;
      }

      sock_close.disable();

      auto fg = util::fail_guard([&socket]() {
        socket->read_plaintext_payload();
      });

      auto content_length = 0;
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Content-length"sv == option->option) {
          BOOST_LOG(debug) << "Found Content-Length: "sv << option->content << " bytes"sv;

          // If content_length > bytes read, then we need to store current data read,
          // to be appended by the next read.
          std::string_view content { option->content };
          auto begin = std::find_if(std::begin(content), std::end(content), [](auto ch) {
            return (bool) std::isdigit(ch);
          });

          content_length = util::from_chars(begin, std::end(content));
          break;
        }
      }

      if (end - socket->crlf >= content_length) {
        if (end - socket->crlf > content_length) {
          BOOST_LOG(warning) << "(end - socket->crlf) > content_length -- "sv << (std::size_t) (end - socket->crlf) << " > "sv << content_length;
        }

        fg.disable();
        print_msg(req.get());

        socket->handle_data(std::move(req));
      }

      socket->begin = end;
    }

    /**
     * @brief Handle the read of the header portion of a plaintext message.
     * @param socket The socket the message was received on.
     * @param ec The error code of the read operation.
     * @param bytes The number of bytes read.
     */
    static void
    handle_read_plaintext(std::shared_ptr<socket_t> &socket, const boost::system::error_code &ec, std::size_t bytes) {
      BOOST_LOG(debug) << "handle_read_plaintext(): Handle read of size: "sv << bytes << " bytes"sv;

      if (ec) {
        BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't read from tcp socket: "sv << ec.message();

        boost::system::error_code ec;
        socket->sock.close(ec);

        if (ec) {
          BOOST_LOG(error) << "RTSP: handle_read_plaintext(): Couldn't close tcp socket: "sv << ec.message();
        }

        return;
      }

      auto fg = util::fail_guard([&socket]() {
        socket->read();
      });

      auto begin = std::max(socket->begin - 4, socket->begin);
      auto buf_size = bytes + (begin - socket->begin);
      auto end = begin + buf_size;

      constexpr auto needle = "\r\n\r\n"sv;

      auto it = std::search(begin, begin + buf_size, std::begin(needle), std::end(needle));
      if (it == end) {
        socket->begin = end;

        return;
      }

      // Emulate read completion for payload data
      socket->begin = it + needle.size();
      socket->crlf = socket->begin;
      buf_size = end - socket->begin;

      fg.disable();
      handle_plaintext_payload(socket, ec, buf_size);
    }

    void
    handle_data(msg_t &&req) {
      handle_data_fn(sock, *session, std::move(req));
    }

    std::function<void(tcp::socket &sock, launch_session_t &, msg_t &&)> handle_data_fn;

    tcp::socket sock;

    std::array<char, 2048> msg_buf;

    char *crlf;
    char *begin = msg_buf.data();

    std::shared_ptr<launch_session_t> session;
  };

  class rtsp_server_t {
  public:
    ~rtsp_server_t() {
      clear();
    }

    int
    bind(net::af_e af, std::uint16_t port, boost::system::error_code &ec) {
      acceptor.open(af == net::IPV4 ? tcp::v4() : tcp::v6(), ec);
      if (ec) {
        return -1;
      }

      acceptor.set_option(boost::asio::socket_base::reuse_address { true });

      auto bind_addr_str = net::get_bind_address(af);
      const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
      if (ec) {
        BOOST_LOG(error) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
        return -1;
      }

      acceptor.bind(tcp::endpoint(bind_addr, port), ec);
      if (ec) {
        return -1;
      }

      acceptor.listen(4096, ec);
      if (ec) {
        return -1;
      }

      next_socket = std::make_shared<socket_t>(io_context, [this](tcp::socket &sock, launch_session_t &session, msg_t &&msg) {
        handle_msg(sock, session, std::move(msg));
      });

      acceptor.async_accept(next_socket->sock, [this](const auto &ec) {
        handle_accept(ec);
      });

      return 0;
    }

    void
    handle_msg(tcp::socket &sock, launch_session_t &session, msg_t &&req) {
      touch_launch_session(session, req->message.request.command);
      auto func = _map_cmd_cb.find(req->message.request.command);
      if (func != std::end(_map_cmd_cb)) {
        func->second(this, sock, session, std::move(req));
      }
      else {
        cmd_not_found(sock, session, std::move(req));
      }

      boost::system::error_code ec;
      sock.shutdown(boost::asio::socket_base::shutdown_type::shutdown_both, ec);
      if (ec) {
        BOOST_LOG(debug) << "Error shutting down socket: "sv << ec.message();
      }
    }

    void
    handle_accept(const boost::system::error_code &ec) {
      if (ec) {
        BOOST_LOG(error) << "Couldn't accept incoming connections: "sv << ec.message();

        // Stop server
        clear();
        return;
      }

      auto socket = std::move(next_socket);
      boost::system::error_code remote_ec;
      const auto remote_address = net::addr_to_normalized_string(socket->sock.remote_endpoint(remote_ec).address());
      if (remote_ec) {
        BOOST_LOG(debug) << "Unable to resolve RTSP peer address: "sv << remote_ec.message();
      }

      auto launch_session { claim_launch_session(remote_ec ? std::string {} : remote_address) };
      if (launch_session) {
        if (!remote_ec &&
            !launch_session->rtsp_peer_address.empty() &&
            launch_session->rtsp_peer_address != remote_address) {
          BOOST_LOG(info) << "RTSP pending claim accepted with peer route change"
                          << " launchSession="sv << launch_session->unique_id
                          << " id="sv << launch_session->id
                          << " expectedPeer="sv << launch_session->rtsp_peer_address
                          << " observedPeer="sv << remote_address
                          << " pendingCount="sv << pending_session_count();
          const bool route_change_requires_remote_hint =
            session_runtime::rtsp_peer_route_change_requires_remote_hint(launch_session->rtsp_peer_address,
                                                                         remote_address);
          launch_session->rtsp_peer_address = remote_address;
          if (route_change_requires_remote_hint) {
            launch_session->rtsp_route_remote_hint = true;
          }
        }
        BOOST_LOG(info) << "RTSP pending claim accepted launchSession="sv << launch_session->unique_id
                        << " id="sv << launch_session->id
                        << " peer="sv << (remote_ec ? "unknown" : remote_address)
                        << " pendingCount="sv << pending_session_count();
        // Associate the current RTSP session with this socket and start reading
        socket->session = launch_session;
        socket->read();
      }
      else {
        // This can happen due to normal things like port scanning, so let's not make these visible by default
        BOOST_LOG(debug) << "No pending session for incoming RTSP connection"sv;

        // If there is no session pending, close the connection immediately
        boost::system::error_code ec;
        socket->sock.close(ec);
        if (ec) {
          BOOST_LOG(debug) << "Error closing socket: "sv << ec.message();
        }
      }

      // Queue another asynchronous accept for the next incoming connection
      next_socket = std::make_shared<socket_t>(io_context, [this](tcp::socket &sock, launch_session_t &session, msg_t &&msg) {
        handle_msg(sock, session, std::move(msg));
      });
      acceptor.async_accept(next_socket->sock, [this](const auto &ec) {
        handle_accept(ec);
      });
    }

    void
    map(const std::string_view &type, cmd_func_t cb) {
      _map_cmd_cb.emplace(type, std::move(cb));
    }

    /**
     * @brief Launch a new streaming session.
     * @note If the client does not begin streaming within the ping_timeout,
     *       the session will be discarded.
     * @param launch_session Streaming session information.
     */
    void
    session_raise(std::shared_ptr<launch_session_t> launch_session) {
      if (!launch_session) {
        return;
      }

      launch_session->pending_since = std::chrono::steady_clock::now();
      {
        auto lg = _pending_launch_sessions.lock();
        prune_launch_sessions_locked(*_pending_launch_sessions);

        auto same_client = std::find_if(_pending_launch_sessions->begin(), _pending_launch_sessions->end(), [&launch_session](const auto &pending_session) {
          return pending_session &&
                 !pending_launch_has_rtsp_progress(*pending_session) &&
                 same_known_launch_client(*pending_session, *launch_session);
        });
        if (same_client != _pending_launch_sessions->end()) {
          BOOST_LOG(info) << "RTSP pending launch session replace same client"
                          << " oldLaunchSession="sv << ((*same_client) ? (*same_client)->unique_id : std::string {})
                          << " newLaunchSession="sv << launch_session->unique_id
                          << " oldPeer="sv << ((*same_client) ? (*same_client)->rtsp_peer_address : std::string {})
                          << " newPeer="sv << launch_session->rtsp_peer_address
                          << " pendingCount="sv << _pending_launch_sessions->size();
          *same_client = std::move(launch_session);
          return;
        }

        if (!launch_session->rtsp_peer_address.empty()) {
          auto existing = std::find_if(_pending_launch_sessions->begin(), _pending_launch_sessions->end(), [&launch_session](const auto &pending_session) {
            return pending_session && pending_session->rtsp_peer_address == launch_session->rtsp_peer_address;
          });
          if (existing != _pending_launch_sessions->end()) {
            BOOST_LOG(info) << "RTSP pending launch session replace"
                            << " oldLaunchSession="sv << ((*existing) ? (*existing)->unique_id : std::string {})
                            << " newLaunchSession="sv << launch_session->unique_id
                            << " peer="sv << launch_session->rtsp_peer_address
                            << " pendingCount="sv << _pending_launch_sessions->size();
            *existing = std::move(launch_session);
            return;
          }
        }

        _pending_launch_sessions->push_back(std::move(launch_session));
        if (!_pending_launch_sessions->empty() && _pending_launch_sessions->back()) {
          BOOST_LOG(info) << "RTSP pending launch session raised"
                          << " launchSession="sv << _pending_launch_sessions->back()->unique_id
                          << " id="sv << _pending_launch_sessions->back()->id
                          << " peer="sv << _pending_launch_sessions->back()->rtsp_peer_address
                          << " pendingCount="sv << _pending_launch_sessions->size();
        }
      }

      // Arm the timer to expire pending launch sessions if clients time out
      raised_timer.expires_after(config::stream.ping_timeout);
      raised_timer.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
          auto lg = _pending_launch_sessions.lock();
          prune_launch_sessions_locked(*_pending_launch_sessions);
        } else {
          BOOST_LOG(debug) << "Timer error: "sv << ec.message();
        }
      });
    }

    /**
     * @brief Clear state for the oldest launch session.
     * @param launch_session_id The ID of the session to clear.
     */
    void
    session_clear(uint32_t launch_session_id) {
      auto lg = _pending_launch_sessions.lock();
      auto pos = std::find_if(_pending_launch_sessions->begin(), _pending_launch_sessions->end(), [launch_session_id](const auto &launch_session) {
        return launch_session && launch_session->id == launch_session_id;
      });
      if (pos != _pending_launch_sessions->end()) {
        BOOST_LOG(info) << "RTSP pending launch session cleared"
                        << " launchSession="sv << ((*pos) ? (*pos)->unique_id : std::string {})
                        << " id="sv << launch_session_id
                        << " pendingCountBefore="sv << _pending_launch_sessions->size();
        _pending_launch_sessions->erase(pos);
      }
    }

    /**
     * @brief Get the number of active sessions.
     * @return Count of active sessions.
     */
    int
    session_count() {
      auto lg = _session_slots.lock();
      return _session_slots->size();
    }

    int
    pending_session_count() {
      auto lg = _pending_launch_sessions.lock();
      prune_launch_sessions_locked(*_pending_launch_sessions);
      return static_cast<int>(_pending_launch_sessions->size());
    }

    std::shared_ptr<launch_session_t>
    claim_launch_session(const std::string &remote_address) {
      auto lg = _pending_launch_sessions.lock();
      prune_launch_sessions_locked(*_pending_launch_sessions);
      if (_pending_launch_sessions->empty()) {
        return nullptr;
      }

      auto pos = _pending_launch_sessions->end();
      if (!remote_address.empty()) {
        pos = std::find_if(_pending_launch_sessions->begin(), _pending_launch_sessions->end(), [&remote_address](const auto &launch_session) {
          return launch_session && launch_session->rtsp_peer_address == remote_address;
        });
      }

      if (pos == _pending_launch_sessions->end()) {
        pos = std::find_if(_pending_launch_sessions->begin(), _pending_launch_sessions->end(), [](const auto &launch_session) {
          return launch_session && launch_session->rtsp_peer_address.empty();
        });
      }

      if (pos == _pending_launch_sessions->end() && _pending_launch_sessions->size() == 1) {
        if (!_pending_launch_sessions->empty() && _pending_launch_sessions->front()) {
          BOOST_LOG(info) << "RTSP pending claim accepted singleton peer mismatch"
                          << " expectedPeer="sv << _pending_launch_sessions->front()->rtsp_peer_address
                          << " observedPeer="sv << remote_address
                          << " launchSession="sv << _pending_launch_sessions->front()->unique_id
                          << " id="sv << _pending_launch_sessions->front()->id;
        }
        pos = _pending_launch_sessions->begin();
      }

      if (pos == _pending_launch_sessions->end()) {
        BOOST_LOG(info) << "RTSP pending claim missed"
                        << " peer="sv << remote_address
                        << " pendingCount="sv << _pending_launch_sessions->size();
        for (const auto &pending : *_pending_launch_sessions) {
          if (pending) {
            BOOST_LOG(info) << "RTSP pending available on claim miss "
                            << rtsp_launch_state(*pending);
          }
        }
        return nullptr;
      }

      if (*pos) {
        (*pos)->pending_since = std::chrono::steady_clock::now();
        BOOST_LOG(debug) << "RTSP pending claim reused"
                         << " launchSession="sv << (*pos)->unique_id
                         << " id="sv << (*pos)->id
                         << " peer="sv << remote_address
                         << " pendingCount="sv << _pending_launch_sessions->size();
      }
      return *pos;
    }

    bool
    touch_launch_session(launch_session_t &session, std::string_view stage) {
      auto lg = _pending_launch_sessions.lock();
      auto pos = std::find_if(_pending_launch_sessions->begin(), _pending_launch_sessions->end(), [&session](const auto &launch_session) {
        return launch_session && launch_session->id == session.id;
      });
      if (pos == _pending_launch_sessions->end() || !*pos) {
        BOOST_LOG(debug) << "RTSP pending touch skipped"
                         << " stage="sv << stage
                         << " launchSession="sv << session.unique_id
                         << " id="sv << session.id
                         << " pendingCount="sv << _pending_launch_sessions->size();
        return false;
      }

      (*pos)->pending_since = std::chrono::steady_clock::now();
      BOOST_LOG(debug) << "RTSP pending touch"
                       << " stage="sv << stage
                       << " launchSession="sv << (*pos)->unique_id
                       << " id="sv << (*pos)->id
                       << " setup(video/audio/control/mic)="sv
                       << ((*pos)->setup_video ? 1 : 0) << "/"
                       << ((*pos)->setup_audio ? 1 : 0) << "/"
                       << ((*pos)->setup_control ? 1 : 0) << "/"
                       << ((*pos)->setup_mic ? 1 : 0)
                       << " pendingCount="sv << _pending_launch_sessions->size();
      return true;
    }

    void
    prune_launch_sessions_locked(std::vector<std::shared_ptr<launch_session_t>> &launch_sessions) {
      const auto now = std::chrono::steady_clock::now();
      launch_sessions.erase(std::remove_if(launch_sessions.begin(), launch_sessions.end(), [now](const auto &launch_session) {
                              if (!launch_session) {
                                return true;
                              }

                              const bool expired = now - launch_session->pending_since > config::stream.ping_timeout;
                              if (expired) {
                                BOOST_LOG(info) << "RTSP launch session timeout "
                                                << rtsp_launch_state(*launch_session);
                              }
                              return expired;
                            }),
                            launch_sessions.end());
    }

    sync_util::sync_t<std::vector<std::shared_ptr<launch_session_t>>> _pending_launch_sessions;

    /**
     * @brief Clear launch sessions.
     * @param all If true, clear all sessions. Otherwise, only clear timed out and stopped sessions.
     * @examples
     * clear(false);
     * @examples_end
     */
    void
    clear(bool all = true) {
      if (all) {
        auto pending_lg = _pending_launch_sessions.lock();
        _pending_launch_sessions->clear();
      }
      else {
        auto pending_lg = _pending_launch_sessions.lock();
        prune_launch_sessions_locked(*_pending_launch_sessions);
      }

      auto lg = _session_slots.lock();

      for (auto i = _session_slots->begin(); i != _session_slots->end();) {
        auto &slot = *(*i);
        if (all || stream::session::state(slot) == stream::session::state_e::STOPPING) {
          stream::session::stop(slot);
          stream::session::join(slot);

          i = _session_slots->erase(i);
        }
        else {
          i++;
        }
      }
    }

    /**
     * @brief Removes the provided session from the set of sessions.
     * @param session The session to remove.
     */
    void
    remove(const std::shared_ptr<stream::session_t> &session) {
      auto lg = _session_slots.lock();
      _session_slots->erase(session);
    }

    /**
     * @brief Inserts the provided session into the set of sessions.
     * @param session The session to insert.
     */
    void
    insert(const std::shared_ptr<stream::session_t> &session) {
      auto lg = _session_slots.lock();
      _session_slots->emplace(session);
      BOOST_LOG(info) << "New streaming session started [active sessions: "sv << _session_slots->size() << ']';
    }

    /**
     * @brief Runs an iteration of the RTSP server loop
     */
    void
    iterate() {
      // If we have a session, we will return to the server loop every
      // 500ms to allow session cleanup to happen.
      if (session_count() > 0) {
        io_context.run_one_for(500ms);
      }
      else {
        io_context.run_one();
      }
    }

    /**
     * @brief Stop the RTSP server.
     */
    void
    stop() {
      acceptor.close();
      io_context.stop();
      clear();
    }

  private:
    std::unordered_map<std::string_view, cmd_func_t> _map_cmd_cb;

    sync_util::sync_t<std::set<std::shared_ptr<stream::session_t>>> _session_slots;

    boost::asio::io_context io_context;
    tcp::acceptor acceptor { io_context };
    boost::asio::steady_timer raised_timer { io_context };

    std::shared_ptr<socket_t> next_socket;
  };

  rtsp_server_t server {};

  void
  launch_session_raise(std::shared_ptr<launch_session_t> launch_session) {
    server.session_raise(std::move(launch_session));
  }

  void
  launch_session_clear(uint32_t launch_session_id) {
    server.session_clear(launch_session_id);
  }

  int
  session_count() {
    // Ensure session_count is up-to-date
    server.clear(false);

    return server.session_count();
  }

  int
  pending_launch_session_count() {
    return server.pending_session_count();
  }

#ifdef SUNSHINE_TESTS
  pending_launch_session_test_result_t
  pending_launch_session_touch_for_tests() {
    rtsp_server_t local_server {};

    auto session = std::make_shared<launch_session_t>();
    session->id = 0xCAFE;
    session->unique_id = "rtsp-lifecycle-test";
    session->rtsp_peer_address = "203.0.113.10";
    local_server.session_raise(session);

    const auto first = local_server.claim_launch_session("203.0.113.10");
    if (first) {
      first->pending_since = std::chrono::steady_clock::now() - config::stream.ping_timeout / 2;
    }

    const auto before_touch = first ? first->pending_since : std::chrono::steady_clock::time_point {};
    const bool touched = first ? local_server.touch_launch_session(*first, "DESCRIBE") : false;
    const auto after_touch = first ? first->pending_since : std::chrono::steady_clock::time_point {};
    const auto second = local_server.claim_launch_session("203.0.113.10");

    const auto pending_after_touch = local_server.pending_session_count();
    local_server.session_clear(0xCAFE);

    return {
      .first_claim_ok = first != nullptr,
      .second_claim_reused = first != nullptr && second != nullptr && first.get() == second.get(),
      .touch_extended_ttl = touched && after_touch > before_touch,
      .pending_after_touch = pending_after_touch,
      .pending_after_clear = local_server.pending_session_count(),
    };
  }

  pending_launch_session_route_change_test_result_t
  pending_launch_session_route_change_for_tests() {
    rtsp_server_t exact_server {};

    auto exact = std::make_shared<launch_session_t>();
    exact->id = 0xA001;
    exact->unique_id = "rtsp-route-change-client";
    exact->identity.set_client_cert_uuid("route-change-cert");
    exact->rtsp_peer_address = "203.0.113.10";
    exact_server.session_raise(exact);

    const auto first = exact_server.claim_launch_session("203.0.113.10");

    rtsp_server_t route_change_server {};

    auto stale = std::make_shared<launch_session_t>();
    stale->id = 0xA101;
    stale->unique_id = "rtsp-route-change-client";
    stale->identity.set_client_cert_uuid("route-change-cert");
    stale->rtsp_peer_address = "203.0.113.10";
    route_change_server.session_raise(stale);

    auto fresh = std::make_shared<launch_session_t>();
    fresh->id = 0xA102;
    fresh->unique_id = "rtsp-route-change-client";
    fresh->identity.set_client_cert_uuid("route-change-cert");
    fresh->rtsp_peer_address = "198.51.100.20";
    route_change_server.session_raise(fresh);

    const auto pending_after_replace = route_change_server.pending_session_count();
    const auto mismatched_peer_claim = route_change_server.claim_launch_session("192.0.2.77");

    rtsp_server_t different_client_server {};

    auto first_client = std::make_shared<launch_session_t>();
    first_client->id = 0xB101;
    first_client->unique_id = "rtsp-route-client-a";
    first_client->identity.set_client_cert_uuid("route-change-cert-a");
    first_client->rtsp_peer_address = "203.0.113.10";
    different_client_server.session_raise(first_client);

    auto second_client = std::make_shared<launch_session_t>();
    second_client->id = 0xB102;
    second_client->unique_id = "rtsp-route-client-b";
    second_client->identity.set_client_cert_uuid("route-change-cert-b");
    second_client->rtsp_peer_address = "198.51.100.20";
    different_client_server.session_raise(second_client);

    const auto different_client_claim = different_client_server.claim_launch_session("192.0.2.77");

    return {
      .first_claim_ok = first != nullptr,
      .replaced_stale_same_client = pending_after_replace == 1,
      .mismatched_peer_claim_ok = mismatched_peer_claim != nullptr,
      .mismatched_peer_claimed_new_session = mismatched_peer_claim != nullptr && mismatched_peer_claim->id == 0xA102,
      .different_client_mismatch_rejected = different_client_claim == nullptr,
      .pending_after_replace = pending_after_replace,
    };
  }
#endif

  void
  terminate_sessions() {
    server.clear(true);
  }

  int
  send(tcp::socket &sock, const std::string_view &sv) {
    std::size_t bytes_send = 0;

    while (bytes_send != sv.size()) {
      boost::system::error_code ec;
      bytes_send += sock.send(boost::asio::buffer(sv.substr(bytes_send)), 0, ec);

      if (ec) {
        BOOST_LOG(error) << "RTSP: Couldn't send data over tcp socket: "sv << ec.message();
        return -1;
      }
    }

    return 0;
  }

  void
  respond(tcp::socket &sock, launch_session_t &session, msg_t &resp) {
    auto payload = std::make_pair(resp->payload, resp->payloadLength);

    // Restore response message for proper destruction
    auto lg = util::fail_guard([&]() {
      resp->payload = payload.first;
      resp->payloadLength = payload.second;
    });

    resp->payload = nullptr;
    resp->payloadLength = 0;

    int serialized_len;
    util::c_ptr<char> raw_resp { serializeRtspMessage(resp.get(), &serialized_len) };
    BOOST_LOG(debug)
      << "---Begin Response---"sv << std::endl
      << std::string_view { raw_resp.get(), (std::size_t) serialized_len } << std::endl
      << std::string_view { payload.first, (std::size_t) payload.second } << std::endl
      << "---End Response---"sv << std::endl;

    // Encrypt the RTSP message if encryption is enabled
    if (session.rtsp_cipher) {
      // We use the deterministic IV construction algorithm specified in NIST SP 800-38D
      // Section 8.2.1. The sequence number is our "invocation" field and the 'RH' in the
      // high bytes is the "fixed" field. Because each client provides their own unique
      // key, our values in the fixed field need only uniquely identify each independent
      // use of the client's key with AES-GCM in our code.
      //
      // The sequence number is 32 bits long which allows for 2^32 RTSP messages to be
      // sent to each client before the IV repeats.
      crypto::aes_t iv(12);
      session.rtsp_iv_counter++;
      std::copy_n((uint8_t *) &session.rtsp_iv_counter, sizeof(session.rtsp_iv_counter), std::begin(iv));
      iv[10] = 'H';  // Host originated
      iv[11] = 'R';  // RTSP

      // Allocate the message with an empty header and reserved space for the payload
      auto payload_length = serialized_len + payload.second;
      std::vector<uint8_t> message(sizeof(encrypted_rtsp_header_t));
      message.reserve(message.size() + payload_length);

      // Copy the complete plaintext into the message
      std::copy_n(raw_resp.get(), serialized_len, std::back_inserter(message));
      std::copy_n(payload.first, payload.second, std::back_inserter(message));

      // Initialize the message header
      auto header = (encrypted_rtsp_header_t *) message.data();
      header->typeAndLength = util::endian::big<std::uint32_t>(encrypted_rtsp_header_t::ENCRYPTED_MESSAGE_TYPE_BIT + payload_length);
      header->sequenceNumber = util::endian::big<std::uint32_t>(session.rtsp_iv_counter);

      // Encrypt the RTSP message in place
      session.rtsp_cipher->encrypt(std::string_view { (const char *) header->payload(), (std::size_t) payload_length }, header->tag, &iv);

      // Send the full encrypted message
      send(sock, std::string_view { (char *) message.data(), message.size() });
    }
    else {
      std::string_view tmp_resp { raw_resp.get(), (size_t) serialized_len };

      // Send the plaintext RTSP message header
      if (send(sock, tmp_resp)) {
        return;
      }

      // Send the plaintext RTSP message payload (if present)
      send(sock, std::string_view { payload.first, (std::size_t) payload.second });
    }
  }

  void
  respond(tcp::socket &sock, launch_session_t &session, POPTION_ITEM options, int statuscode, const char *status_msg, int seqn, const std::string_view &payload) {
    msg_t resp { new msg_t::element_type };
    createRtspResponse(resp.get(), nullptr, 0, const_cast<char *>("RTSP/1.0"), statuscode, const_cast<char *>(status_msg), seqn, options, const_cast<char *>(payload.data()), (int) payload.size());

    respond(sock, session, resp);
  }

  void
  cmd_not_found(tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    respond(sock, session, nullptr, 404, "NOT FOUND", req->sequenceNumber, {});
  }

  void
  cmd_option(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  void
  cmd_describe(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::stringstream ss;

    BOOST_LOG(info) << "RTSP DESCRIBE start "
                    << rtsp_launch_state(session)
                    << " remote="sv << sock.remote_endpoint().address().to_string()
                    << " cseq="sv << req->sequenceNumber
                    << " pending="sv << rtsp_stream::pending_launch_session_count()
                    << " active="sv << rtsp_stream::session_count();

    // Tell the client about our supported features
    auto host_feature_flags = static_cast<std::uint32_t>(platf::get_capabilities());
    if (!config::input.clipboard_sync) {
      host_feature_flags &= ~static_cast<std::uint32_t>(platf::platform_caps::clipboard_text |
                                                         platf::platform_caps::clipboard_image);
    }
    host_feature_flags |= static_cast<std::uint32_t>(platf::platform_caps::mic_session_id) |
                          static_cast<std::uint32_t>(platf::platform_caps::network_feedback);
    ss << "a=x-ss-general.featureFlags:" << host_feature_flags << std::endl;
    auto host_feature_flags2 = foundation_streaming_feature_flags2();
    ss << "a=x-ss-general.featureFlags2:" << host_feature_flags2 << std::endl;
    const auto core_capabilities = session_runtime::default_rtsp_capability_manifest();
    for (const auto &attribute : session_runtime::rtsp_capability_attributes(core_capabilities)) {
      ss << "a=" << attribute << std::endl;
    }
    // Always request new control stream encryption if the client supports it
    uint32_t encryption_flags_supported = SS_ENC_CONTROL_V2 | SS_ENC_AUDIO | SS_ENC_MIC;
    uint32_t encryption_flags_requested = SS_ENC_CONTROL_V2;

    // Determine the encryption desired for this remote endpoint
    auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address());
    if (encryption_mode != config::ENCRYPTION_MODE_NEVER) {
      // Advertise support for video encryption if it's not disabled
      encryption_flags_supported |= SS_ENC_VIDEO;

      // If it's mandatory, also request it to enable use if the client
      // didn't explicitly opt in, but it otherwise has support.
      if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
        encryption_flags_requested |= SS_ENC_VIDEO | SS_ENC_AUDIO | SS_ENC_MIC;
      } else {
        // Even if not mandatory, request audio and mic encryption if encryption is enabled
        // This ensures clients that check encryptionRequested will enable audio and MIC encryption
        encryption_flags_requested |= SS_ENC_AUDIO | SS_ENC_MIC;
      }
    }

    // Report supported and required encryption flags
    ss << "a=x-ss-general.encryptionSupported:" << encryption_flags_supported << std::endl;
    ss << "a=x-ss-general.encryptionRequested:" << encryption_flags_requested << std::endl;
    
    // 记录加密请求状态用于调试
    BOOST_LOG(info) << "RTSP DESCRIBE encryption flags: supported=0x" << std::hex << encryption_flags_supported << std::dec
                    << ", requested=0x" << std::hex << encryption_flags_requested << std::dec
                    << " (CONTROL_V2=" << ((encryption_flags_requested & SS_ENC_CONTROL_V2) ? "1" : "0")
                    << ", VIDEO=" << ((encryption_flags_requested & SS_ENC_VIDEO) ? "1" : "0")
                    << ", AUDIO=" << ((encryption_flags_requested & SS_ENC_AUDIO) ? "1" : "0")
                    << ", MIC=" << ((encryption_flags_requested & SS_ENC_MIC) ? "1" : "0") << ")";

    if (video::last_encoder_probe_supported_ref_frames_invalidation) {
      ss << "a=x-nv-video[0].refPicInvalidation:1"sv << std::endl;
    }

    if (video::active_hevc_mode != 1) {
      ss << "sprop-parameter-sets=AAAAAU"sv << std::endl;
    }

    if (video::active_av1_mode != 1) {
      ss << "a=rtpmap:98 AV1/90000"sv << std::endl;
    }

    if (!session.surround_params.empty()) {
      // If we have our own surround parameters, advertise them twice first
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
      ss << "a=fmtp:97 surround-params="sv << session.surround_params << std::endl;
    }

    // Advertise the microphone media only for sessions that actually requested
    // microphone redirection. Some legacy clients (voidlink) will SETUP every
    // media line they see even when /resume was mic=false; advertising mic
    // unconditionally leaves them stuck before control SETUP/PLAY.
    if (config::audio.stream_mic && session.enable_mic) {
      ss << "m=audio " << net::map_port(stream::MIC_STREAM_PORT) << " RTP/AVP 96" << std::endl;
      ss << "a=rtpmap:96 opus/48000/2" << std::endl;
      ss << "a=fmtp:96 minptime=10;useinbandfec=1" << std::endl;
    }

    for (int x = 0; x < audio::MAX_STREAM_CONFIG; ++x) {
      auto &stream_config = audio::stream_configs[x];
      std::uint8_t mapping[platf::speaker::MAX_SPEAKERS];

      auto mapping_p = stream_config.mapping;

      /**
       * GFE advertises incorrect mapping for normal quality configurations,
       * as a result, Moonlight rotates all channels from index '3' to the right
       * To work around this, rotate channels to the left from index '3'
       */
      if (x == audio::SURROUND51 || x == audio::SURROUND71) {
        std::copy_n(mapping_p, stream_config.channelCount, mapping);
        std::rotate(mapping + 3, mapping + 4, mapping + stream_config.channelCount);

        mapping_p = mapping;
      }

      // For channel counts > 8 (e.g., 7.1.4 with 12 channels), use comma-delimited format
      // because mapping values can exceed single digits (e.g., 10, 11)
      if (stream_config.channelCount > 8) {
        ss << "a=fmtp:97 surround-params="sv << stream_config.channelCount;

        // Use comma-delimited format: channelCount,streams,coupledStreams,m0,m1,...
        ss << ',' << (int) stream_config.streams << ',' << (int) stream_config.coupledStreams;

        std::for_each_n(mapping_p, stream_config.channelCount, [&ss](std::uint8_t val) {
          ss << ',' << (int) val;
        });
      }
      else {
        ss << "a=fmtp:97 surround-params="sv << stream_config.channelCount << stream_config.streams << stream_config.coupledStreams;

        std::for_each_n(mapping_p, stream_config.channelCount, [&ss](std::uint8_t digit) {
          ss << (char) (digit + '0');
        });
      }

      ss << std::endl;
    }

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, ss.str());
  }

  void
  cmd_setup(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM options[4] {};

    auto &seqn = options[0];
    auto &session_option = options[1];
    auto &port_option = options[2];
    auto &payload_option = options[3];

    seqn.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    seqn.content = const_cast<char *>(seqn_str.c_str());

    std::string_view target { req->message.request.target };
    auto begin = std::find(std::begin(target), std::end(target), '=') + 1;
    auto end = std::find(begin, std::end(target), '/');
    std::string_view type { begin, (size_t) std::distance(begin, end) };

    std::uint16_t port;
    if (type == "audio"sv) {
      session.setup_audio = true;
      port = net::map_port(stream::AUDIO_STREAM_PORT);
    }
    else if (type == "video"sv) {
      session.setup_video = true;
      port = net::map_port(stream::VIDEO_STREAM_PORT);
    }
    else if (type == "control"sv) {
      session.setup_control = true;
      port = net::map_port(stream::CONTROL_PORT);
    }
    else if (type == "mic"sv) {
      if (!session.enable_mic) {
        BOOST_LOG(warning) << "RTSP SETUP mic rejected because launch session did not request microphone "
                           << rtsp_launch_state(session);
        cmd_not_found(sock, session, std::move(req));
        return;
      }
      session.enable_mic = true;
      session.setup_mic = true;
      port = net::map_port(stream::MIC_STREAM_PORT);
    }
    else {
      cmd_not_found(sock, session, std::move(req));

      return;
    }

    BOOST_LOG(info) << "RTSP SETUP"
                    << " launchSession="sv << session.unique_id
                    << " id="sv << session.id
                    << " streamType="sv << type
                    << " serverPort="sv << port
                    << " setup(video/audio/control/mic)="sv
                    << (session.setup_video ? 1 : 0) << "/"
                    << (session.setup_audio ? 1 : 0) << "/"
                    << (session.setup_control ? 1 : 0) << "/"
                    << (session.setup_mic ? 1 : 0);

    seqn.next = &session_option;

    session_option.option = const_cast<char *>("Session");
    session_option.content = const_cast<char *>("DEADBEEFCAFE;timeout = 90");

    session_option.next = &port_option;

    // Moonlight merely requires 'server_port=<port>'
    auto port_value = "server_port=" + std::to_string(port);

    port_option.option = const_cast<char *>("Transport");
    port_option.content = port_value.data();

    // Send identifiers that will be echoed in the other connections
    auto connect_data = std::to_string(session.control_connect_data);
    if (type == "control"sv) {
      payload_option.option = const_cast<char *>("X-SS-Connect-Data");
      payload_option.content = connect_data.data();
    }
    else {
      payload_option.option = const_cast<char *>("X-SS-Ping-Payload");
      payload_option.content = session.av_ping_payload.data();
    }

    port_option.next = &payload_option;

    respond(sock, session, &seqn, 200, "OK", req->sequenceNumber, {});
  }

  void
  cmd_announce(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    std::string_view payload { req->payload, (size_t) req->payloadLength };

    BOOST_LOG(info) << "RTSP ANNOUNCE start"
                    << " launchSession="sv << session.unique_id
                    << " id="sv << session.id
                    << " payloadLength="sv << req->payloadLength
                    << " setup(video/audio/control/mic)="sv
                    << (session.setup_video ? 1 : 0) << "/"
                    << (session.setup_audio ? 1 : 0) << "/"
                    << (session.setup_control ? 1 : 0) << "/"
                    << (session.setup_mic ? 1 : 0);

    std::vector<std::string_view> lines;

    auto whitespace = [](char ch) {
      return ch == '\n' || ch == '\r';
    };

    {
      auto pos = std::begin(payload);
      auto begin = pos;
      while (pos != std::end(payload)) {
        if (whitespace(*pos++)) {
          lines.emplace_back(begin, pos - begin - 1);

          while (pos != std::end(payload) && whitespace(*pos)) {
            ++pos;
          }
          begin = pos;
        }
      }
    }

    std::string_view client;
    std::unordered_map<std::string_view, std::string_view> args;

    for (auto line : lines) {
      auto type = line.substr(0, 2);
      if (type == "s="sv) {
        client = line.substr(2);
      }
      else if (type == "a=") {
        auto pos = line.find(':');

        auto name = line.substr(2, pos - 2);
        auto val = line.substr(pos + 1);

        if (val[val.size() - 1] == ' ') {
          val = val.substr(0, val.size() - 1);
        }
        args.emplace(name, val);
      }
    }

    // Initialize any omitted parameters to defaults
    args.try_emplace("x-nv-video[0].encoderCscMode"sv, "0"sv);
    args.try_emplace("x-nv-vqos[0].bitStreamFormat"sv, "0"sv);
    args.try_emplace("x-nv-video[0].dynamicRangeMode"sv, "0"sv);
    args.try_emplace("x-nv-aqos.packetDuration"sv, "5"sv);
    args.try_emplace("x-nv-general.useReliableUdp"sv, "1"sv);
    args.try_emplace("x-nv-vqos[0].fec.minRequiredFecPackets"sv, "0"sv);
    args.try_emplace("x-nv-general.featureFlags"sv, "135"sv);
    args.try_emplace("x-ml-general.featureFlags"sv, "0"sv);
    args.try_emplace("x-ml-general.featureFlags2"sv, "0"sv);
    args.try_emplace("x-ml-core.sessionVersion"sv, "0"sv);
    args.try_emplace("x-ml-core.sessionId"sv, ""sv);
    args.try_emplace("x-ml-core.logicalSessionKey"sv, "0"sv);
    args.try_emplace("x-ml-core.launchSessionId"sv, "0"sv);
    args.try_emplace("x-ml-core.controlGeneration"sv, "0"sv);
    args.try_emplace("x-ml-core.state"sv, "idle"sv);
    args.try_emplace("x-ml-core.appId"sv, ""sv);
    args.try_emplace("x-ml-core.appName"sv, ""sv);
    args.try_emplace("x-ml-core.client.participantId"sv, ""sv);
    args.try_emplace("x-ml-core.client.participantKey"sv, "0"sv);
    args.try_emplace("x-ml-core.client.clientKey"sv, "0"sv);
    args.try_emplace("x-ml-core.client.deviceKey"sv, "0"sv);
    args.try_emplace("x-ml-core.client.displayName"sv, ""sv);
    args.try_emplace("x-ml-core.client.deviceName"sv, ""sv);
    args.try_emplace("x-ml-core.host.participantId"sv, ""sv);
    args.try_emplace("x-ml-core.host.participantKey"sv, "0"sv);
    args.try_emplace("x-ml-core.host.clientKey"sv, "0"sv);
    args.try_emplace("x-ml-core.host.deviceKey"sv, "0"sv);
    args.try_emplace("x-ml-core.host.displayName"sv, ""sv);
    args.try_emplace("x-ml-core.host.deviceName"sv, ""sv);
    args.try_emplace("x-ml-core.supportedCaps"sv, ""sv);
    args.try_emplace("x-ml-core.transportPaths"sv, ""sv);
    args.try_emplace("x-ml-core.leaseIntent"sv, ""sv);
    args.try_emplace("x-ml-core.featureBits"sv, "0"sv);
    args.try_emplace("x-ml-core.transportPath.routeId"sv, ""sv);
    args.try_emplace("x-ml-core.transportPath.kind"sv, ""sv);
    args.try_emplace("x-ml-core.transportPath.protocol"sv, ""sv);
    args.try_emplace("x-nv-vqos[0].qosTrafficType"sv, "5"sv);
    args.try_emplace("x-nv-aqos.qosTrafficType"sv, "4"sv);
    args.try_emplace("x-ml-video.configuredBitrateKbps"sv, "0"sv);
    args.try_emplace("x-ml-video.contentType"sv, "0"sv);
    args.try_emplace("x-ss-general.encryptionEnabled"sv, "0"sv);
    args.try_emplace("x-ss-video[0].chromaSamplingType"sv, "0"sv);
    args.try_emplace("x-ss-video[0].intraRefresh"sv, "0"sv);
    args.try_emplace("x-nv-video[0].clientRefreshRateX100"sv, "0"sv);  // NTSC framerate support (e.g., 5994 = 59.94fps)

    // Audio codec selection (Sunshine extension, opt-in by client).
    // 0 = Opus (default, backward compatible)
    // 1 = AC3 passthrough
    // 2 = E-AC3 passthrough
    args.try_emplace("x-ml-audio.codec"sv, "opus"sv);
    args.try_emplace("x-ml-audio.bitrate"sv, "0"sv);

    stream::config_t config;

    std::int64_t configuredBitrateKbps;
    int clientContentType = 0;
    config.audio.flags[audio::config_t::HOST_AUDIO] = session.host_audio;
    auto getArg = [&args](std::string_view key) {
      return util::from_view(args.at(key));
    };
    auto getArgU64 = [&args](std::string_view key) -> std::uint64_t {
      const auto value = args.at(key);
      if (value.empty()) {
        return 0;
      }
      return static_cast<std::uint64_t>(std::strtoull(std::string(value).c_str(), nullptr, 0));
    };

    try {
      config.audio.channels = getArg("x-nv-audio.surround.numChannels"sv);
      config.audio.mask = getArg("x-nv-audio.surround.channelMask"sv);
      config.audio.packetDuration = getArg("x-nv-aqos.packetDuration"sv);
      config.audio.flags[audio::config_t::HIGH_QUALITY] = getArg("x-nv-audio.surround.AudioQuality"sv);

      // Parse Moonlight audio codec selection (string -> enum).
      // Unknown values fall back to Opus to preserve compatibility.
      {
        const auto &codecStr = args.at("x-ml-audio.codec"sv);
        if (codecStr == "ac3"sv) {
          config.audio.codec = audio::CODEC_AC3;
        }
        else if (codecStr == "eac3"sv) {
          config.audio.codec = audio::CODEC_EAC3;
        }
        else if (codecStr == "pcm"sv || codecStr == "pcm_s16"sv || codecStr == "s16"sv) {
          // Accept several spellings: "pcm" (legacy short form), "pcm_s16"
          // (matches the codec_e enum name on both client and server) and
          // "s16" (FFmpeg-style sample format hint). All map to the same
          // signed-16-bit interleaved LPCM passthrough.
          config.audio.codec = audio::CODEC_PCM_S16;
        }
        else {
          config.audio.codec = audio::CODEC_OPUS;
        }
        config.audio.bitrate = getArg("x-ml-audio.bitrate"sv);

        // AC3/E-AC3 uses a fixed 1536-sample (32 ms) frame at 48 kHz, override
        // whatever Opus packet duration the client requested for QoS purposes.
        if (config.audio.codec == audio::CODEC_AC3 || config.audio.codec == audio::CODEC_EAC3) {
          config.audio.packetDuration = 32;

          // Validate the request can actually be honored. AC3 maxes out at
          // 5.1 (6 channels), and the linked FFmpeg may have been built
          // without audio encoders. If we silently fell back to Opus here
          // the client would still expect AC3 bitstream and play garbage,
          // so reject the ANNOUNCE explicitly to force the client to retry
          // with a valid configuration.
          AVCodecID needed = (config.audio.codec == audio::CODEC_EAC3)
                                 ? AV_CODEC_ID_EAC3 : AV_CODEC_ID_AC3;
          const char *codecName = (config.audio.codec == audio::CODEC_EAC3) ? "E-AC3" : "AC3";
          if (config.audio.channels > 6) {
            BOOST_LOG(warning) << codecName << " passthrough rejected: "sv
                               << config.audio.channels << " channels exceeds 5.1 limit"sv;
            respond(sock, session, &option, 415, "UNSUPPORTED MEDIA TYPE", req->sequenceNumber, {});
            return;
          }
          if (avcodec_find_encoder(needed) == nullptr) {
            BOOST_LOG(warning) << codecName << " passthrough rejected: encoder not built into linked FFmpeg "sv
                               << "(rebuild build-deps with --enable-encoder=ac3,eac3)"sv;
            respond(sock, session, &option, 415, "UNSUPPORTED MEDIA TYPE", req->sequenceNumber, {});
            return;
          }
        }
        else if (config.audio.codec == audio::CODEC_PCM_S16) {
          // Force 5 ms framing: 48k * 5ms = 240 samples, 5.1ch * 16bit = 2880 B
          // (fits the 4 KB receiver buffer).
          config.audio.packetDuration = 5;
          if (config.audio.channels > 6) {
            BOOST_LOG(warning) << "PCM_S16 passthrough rejected: "sv
                               << config.audio.channels << " channels exceeds 5.1 limit"sv;
            respond(sock, session, &option, 415, "UNSUPPORTED MEDIA TYPE", req->sequenceNumber, {});
            return;
          }
        }
      }

      config.controlProtocolType = getArg("x-nv-general.useReliableUdp"sv);
      config.packetsize = getArg("x-nv-video[0].packetSize"sv);
      config.minRequiredFecPackets = getArg("x-nv-vqos[0].fec.minRequiredFecPackets"sv);
      config.mlFeatureFlags = getArg("x-ml-general.featureFlags"sv);
      config.mlFeatureFlags2 = getArgU64("x-ml-general.featureFlags2"sv);
      config.mlCoreSessionVersion = getArg("x-ml-core.sessionVersion"sv);
      config.mlCoreFeatureBits = getArgU64("x-ml-core.featureBits"sv);
      config.mlCoreSupportedCaps = std::string(args.at("x-ml-core.supportedCaps"sv));
      config.mlCoreTransportPaths = std::string(args.at("x-ml-core.transportPaths"sv));
      if (config.mlCoreSessionVersion > 0) {
        auto logicalSessionId = std::string(args.at("x-ml-core.sessionId"sv));
        if (!logicalSessionId.empty()) {
          session.identity.logical_session_id = std::move(logicalSessionId);
        }
        auto logicalSessionKey = getArgU64("x-ml-core.logicalSessionKey"sv);
        if (logicalSessionKey != 0) {
          session.identity.logical_session_key = logicalSessionKey;
        }
        auto mlLaunchSessionId = getArgU64("x-ml-core.launchSessionId"sv);
        if (mlLaunchSessionId != 0) {
          session.identity.launch_session_id = static_cast<std::uint32_t>(mlLaunchSessionId);
        }
        auto controlGeneration = getArgU64("x-ml-core.controlGeneration"sv);
        if (controlGeneration != 0) {
          session.identity.control_generation = static_cast<std::uint32_t>(controlGeneration);
        }
        auto participantId = std::string(args.at("x-ml-core.client.participantId"sv));
        if (!participantId.empty()) {
          session.identity.participant_id = std::move(participantId);
        }
        auto participantKey = getArgU64("x-ml-core.client.participantKey"sv);
        if (participantKey != 0) {
          session.identity.participant_key = participantKey;
        }
        auto clientKey = getArgU64("x-ml-core.client.clientKey"sv);
        if (clientKey != 0) {
          session.identity.client_key = clientKey;
        }
        auto deviceKey = getArgU64("x-ml-core.client.deviceKey"sv);
        if (deviceKey != 0) {
          session.identity.device_key = deviceKey;
        }
        auto clientDeviceName = std::string(args.at("x-ml-core.client.deviceName"sv));
        if (!clientDeviceName.empty()) {
          session.identity.client_unique_id = std::move(clientDeviceName);
        }
        if (session.identity.client_unique_id.empty() && !session.identity.participant_id.empty()) {
          session.identity.client_unique_id = session.identity.participant_id;
        }
        if (session.identity.participant_id.empty() && !session.identity.client_unique_id.empty()) {
          session.identity.participant_id = session.identity.client_unique_id;
        }
        auto clientDisplayName = std::string(args.at("x-ml-core.client.displayName"sv));
        if (!clientDisplayName.empty()) {
          session.identity.client_name = std::move(clientDisplayName);
        }
        if (session.identity.client_name.empty()) {
          session.identity.client_name = std::string(client);
        }
        if (!session.identity.client_name.empty()) {
          session.client_name = session.identity.client_name;
        }
      }
      config.audioQosType = getArg("x-nv-aqos.qosTrafficType"sv);
      config.videoQosType = getArg("x-nv-vqos[0].qosTrafficType"sv);
      config.encryptionFlagsEnabled = getArg("x-ss-general.encryptionEnabled"sv);

      // Legacy clients use nvFeatureFlags to indicate support for audio encryption
      if (getArg("x-nv-general.featureFlags"sv) & 0x20) {
        config.encryptionFlagsEnabled |= SS_ENC_AUDIO;
      }

      auto &monitor = config.monitor;
      monitor.height = getArg("x-nv-video[0].clientViewportHt"sv);
      monitor.width = getArg("x-nv-video[0].clientViewportWd"sv);
      BOOST_LOG(info) << "Client requested stream resolution (clientViewport): " << monitor.width << "x" << monitor.height;
      monitor.framerate = getArg("x-nv-video[0].maxFPS"sv);
      monitor.bitrate = getArg("x-nv-vqos[0].bw.maximumBitrateKbps"sv);
      monitor.qualityCeilingBitrate = monitor.bitrate;
      monitor.qualityCeilingFramerate = monitor.framerate;
      monitor.contentType = 0;
      monitor.slicesPerFrame = getArg("x-nv-video[0].videoEncoderSlicesPerFrame"sv);
      monitor.numRefFrames = getArg("x-nv-video[0].maxNumReferenceFrames"sv);
      monitor.encoderCscMode = getArg("x-nv-video[0].encoderCscMode"sv);
      monitor.videoFormat = getArg("x-nv-vqos[0].bitStreamFormat"sv);
      monitor.dynamicRange = getArg("x-nv-video[0].dynamicRangeMode"sv);
      monitor.chromaSamplingType = getArg("x-ss-video[0].chromaSamplingType"sv);
      monitor.enableIntraRefresh = getArg("x-ss-video[0].intraRefresh"sv);
      monitor.preferCursorPlane =
        (config.mlFeatureFlags2 & static_cast<std::uint64_t>(ML_FF2_CURSOR_PLANE_ACTIVE)) != 0;
      BOOST_LOG(info) << "Client featureFlags2=0x" << std::hex << config.mlFeatureFlags2 << std::dec
                      << " cursorPlaneSupport="
                      << (((config.mlFeatureFlags2 & static_cast<std::uint64_t>(ML_FF2_CURSOR_PLANE)) != 0) ? 1 : 0)
                      << " preferCursorPlane=" << (monitor.preferCursorPlane ? 1 : 0);
      if (monitor.preferCursorPlane) {
        BOOST_LOG(info) << "Client requested active cursor plane; disabling video cursor burn-in for this stream";
      }

      int clientRefreshRateX100 = getArg("x-nv-video[0].clientRefreshRateX100"sv);
      clientContentType = getArg("x-ml-video.contentType"sv);
      monitor.contentType = clientContentType;

      // Only use clientRefreshRateX100 if it's within 2% of maxFPS
      bool useClientRefreshRate = false;
      if (clientRefreshRateX100 > 0 && monitor.framerate > 0) {
        double ratio = (clientRefreshRateX100 / 100.0) / monitor.framerate;
        useClientRefreshRate = (ratio > 0.98 && ratio < 1.02);
      }

      if (useClientRefreshRate) {
        int remainder = clientRefreshRateX100 % 100;
        monitor.frameRateNum = (remainder == 0) ? clientRefreshRateX100 / 100 : clientRefreshRateX100;
        monitor.frameRateDen = (remainder == 0) ? 1 : 100;

        BOOST_LOG(info) << "Client framerate: " << clientRefreshRateX100 / 100.0 << " fps ("
                        << monitor.frameRateNum << "/" << monitor.frameRateDen << ")";
      }
      else {
        monitor.frameRateNum = monitor.framerate;
        monitor.frameRateDen = 1;
      }

      configuredBitrateKbps = getArg("x-ml-video.configuredBitrateKbps"sv);

      // Set display_name from session environment or use global configuration
      if (auto it = session.env.find("SUNSHINE_CLIENT_DISPLAY_NAME"); it != session.env.end()) {
        monitor.display_name = it->to_string();
        BOOST_LOG(info) << "Session using specified display: " << monitor.display_name;
      }
      else {
        monitor.display_name = config::video.output_name;
      }
    }
    catch (std::out_of_range &) {
      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    // When using stereo audio, the audio quality is (strangely) indicated by whether the Host field
    // in the RTSP message matches a local interface's IP address. Fortunately, Moonlight always sends
    // 0.0.0.0 when it wants low quality, so it is easy to check without enumerating interfaces.
    if (config.audio.channels == 2) {
      for (auto option = req->options; option != nullptr; option = option->next) {
        if ("Host"sv == option->option) {
          std::string_view content { option->content };
          BOOST_LOG(debug) << "Found Host: "sv << content;
          config.audio.flags[audio::config_t::HIGH_QUALITY] = (content.find("0.0.0.0"sv) == std::string::npos);
        }
      }
    }
    else if (!session.surround_params.empty()) {
      config.audio.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] =
        parse_surround_params(session.surround_params, config.audio.channels, config.audio.customStreamParams);
    }

    if (config.audio.channels == 12 && !config.audio.flags[audio::config_t::CUSTOM_SURROUND_PARAMS]) {
      config.audio.customStreamParams.channelCount = 12;
      config.audio.customStreamParams.streams = 8;
      config.audio.customStreamParams.coupledStreams = 4;
      std::copy_n(std::begin(platf::speaker::map_surround714), 12, std::begin(config.audio.customStreamParams.mapping));
      config.audio.flags[audio::config_t::CUSTOM_SURROUND_PARAMS] = true;
    }
    if (session.continuous_audio) {
      BOOST_LOG(info) << "Client requested continuous audio"sv;
      config.audio.flags[audio::config_t::CONTINUOUS_AUDIO] = true;
    }

    // If the client sent a configured bitrate, choose the video encoder budget after
    // transport/audio overhead, then use low-bitrate clarity planning to spend that
    // budget on spatial clarity rather than forcing the bitrate above the user's cap.
    if (configuredBitrateKbps) {
      BOOST_LOG(debug) << "Client configured bitrate is "sv << configuredBitrateKbps << " Kbps"sv;

      const bool adaptive_controller_enabled =
        config::stream.adaptive_streaming_optimization &&
        (config.mlFeatureFlags & ML_FF_NETWORK_FEEDBACK) != 0;
      const auto rtspPeerAddress = net::addr_to_normalized_string(sock.remote_endpoint().address());
      const auto rtspPeerNetwork = net::from_address(rtspPeerAddress);
      const auto startupPathDecision = session_runtime::classify_startup_path({
        .peer_is_lan_or_pc = rtspPeerNetwork == net::PC || rtspPeerNetwork == net::LAN,
        .remote_streaming_hint = session.remote_streaming_hint,
        .rtsp_route_remote_hint = session.rtsp_route_remote_hint,
        .client_route_remote_hint = session.client_route_remote_hint,
        .client_route_tunnel = session.client_route_tunnel_hint,
        .client_vpn_active = session.client_vpn_hint,
        .startup_profile = session.startup_profile,
        .client_egress_kind = session.client_route_egress_kind,
        .client_route_host = session.client_route_host,
        .rtsp_route_host = session.rtsp_route_host,
        .host_observed_peer_endpoint = rtspPeerAddress,
        .host_observed_local_endpoint = session.rtsp_route_local_endpoint,
        .client_target_address_candidates = session.client_target_address_candidates,
        .host_public_candidates = session.host_public_candidates,
      });
      const bool lan_fast_start = startupPathDecision.allow_lan_fast_start;
      auto effectiveFecPercentage = effective_stream_fec_percentage_for_client(config::stream.fec_percentage,
                                                                               config.mlFeatureFlags,
                                                                               adaptive_controller_enabled);
      if (lan_fast_start && effectiveFecPercentage > 0) {
        effectiveFecPercentage = std::min(effectiveFecPercentage, 2);
      }

      configuredBitrateKbps = adaptive_controller_enabled ?
                                video_quality_ceiling_bitrate_kbps(
                                  configuredBitrateKbps,
                                  effectiveFecPercentage,
                                  config.audio.flags[audio::config_t::HIGH_QUALITY],
                                  config.audio.channels,
                                  config.mlFeatureFlags) :
                                adjust_configured_video_bitrate_kbps(
                                  configuredBitrateKbps,
                                  effectiveFecPercentage,
                                  config.audio.flags[audio::config_t::HIGH_QUALITY],
                                  config.audio.channels);
      const auto qualityCeilingBitrateKbps = configuredBitrateKbps;
      const auto qualityCeilingFramerate = config.monitor.framerate;

      if (adaptive_controller_enabled) {
        auto clarity_plan = stream_quality::plan_low_bitrate_clarity({
          .width = config.monitor.width,
          .height = config.monitor.height,
          .fps = config.monitor.framerate,
          .video_bitrate_kbps = static_cast<int>(configuredBitrateKbps),
          .video_format = config.monitor.videoFormat,
          .chroma_sampling_type = config.monitor.chromaSamplingType,
          .content_type = clientContentType == 1 ? stream_quality::content_type_e::text :
                          clientContentType == 2 ? stream_quality::content_type_e::motion :
                          clientContentType == 3 ? stream_quality::content_type_e::game :
                                                   stream_quality::content_type_e::desktop,
        });

        config.monitor.lowBitrateClarityIntentFlags = clarity_plan.intent_flags;
        config.monitor.lowBitrateTargetQp = clarity_plan.target_qp;
        config.monitor.lowBitrateSharpenAlpha = clarity_plan.sharpen_alpha;

        if (clarity_plan.enabled) {
          if (clarity_plan.effective_chroma_sampling_type != config.monitor.chromaSamplingType) {
            BOOST_LOG(info) << "Low-bitrate clarity mode: using 4:2:0 to preserve luma detail at "
                            << configuredBitrateKbps << " Kbps";
            config.monitor.chromaSamplingType = clarity_plan.effective_chroma_sampling_type;
          }

          if (clarity_plan.effective_fps > 0 &&
              clarity_plan.effective_fps < config.monitor.framerate) {
            BOOST_LOG(info) << "Low-bitrate clarity advisory: preserving requested "
                            << config.monitor.framerate
                            << " fps for adaptive pacing; static clarity would prefer "
                            << clarity_plan.effective_fps << " fps at "
                            << configuredBitrateKbps << " Kbps"
                            << " (" << clarity_plan.bits_per_pixel_per_frame
                            << " bpp/frame)";
          }

          if (clarity_plan.prefer_intra_refresh && config.monitor.enableIntraRefresh == 0) {
            BOOST_LOG(info) << "Low-bitrate clarity mode: enabling intra-refresh preference for moving content";
            config.monitor.enableIntraRefresh = 1;
          }

          if (clarity_plan.roi_enabled || clarity_plan.target_qp > 0 || clarity_plan.sharpen_alpha > 0.0f) {
            BOOST_LOG(info) << "Frame interest intent generated qp=" << clarity_plan.target_qp
                            << " roi=" << (clarity_plan.roi_enabled ? 1 : 0)
                            << " dirtyRegion=" << (clarity_plan.dirty_region_priority ? 1 : 0)
                            << " temporalLayers=" << (clarity_plan.prefer_temporal_layers ? 1 : 0)
                            << " discardableEnhancement=" << (clarity_plan.discardable_enhancement_layer ? 1 : 0)
                            << " ltr=" << (clarity_plan.prefer_long_term_reference ? 1 : 0)
                            << " flags=0x" << std::hex << clarity_plan.intent_flags << std::dec
                            << " sharpen=" << clarity_plan.sharpen_alpha;
          }
        }
      }

      if (adaptive_controller_enabled && !lan_fast_start) {
        stream_quality::stream_description_t startup_stream {
          .width = config.monitor.width,
          .height = config.monitor.height,
          .fps = qualityCeilingFramerate,
          .video_bitrate_kbps = static_cast<int>(qualityCeilingBitrateKbps),
          .video_format = config.monitor.videoFormat,
          .chroma_sampling_type = config.monitor.chromaSamplingType,
          .content_type = clientContentType == 1 ? stream_quality::content_type_e::text :
                          clientContentType == 2 ? stream_quality::content_type_e::motion :
                          clientContentType == 3 ? stream_quality::content_type_e::game :
                                                   stream_quality::content_type_e::desktop,
        };
        const auto startupPolicy =
          session_runtime::startup_ceiling_policy_for_path(startupPathDecision,
                                                           qualityCeilingFramerate);
        auto startupBitrateKbps = stream_quality::startup_bitrate_for_ceiling(startup_stream);
        if (startupPolicy.bitrate_seed_kbps > 0) {
          startupBitrateKbps =
            stream_quality::startup_bitrate_preserving_seed(startup_stream,
                                                            startupPolicy.bitrate_seed_kbps);
        }
        auto startupFps = stream_quality::startup_fps_for_bitrate(startup_stream, startupBitrateKbps);
        if (startupPolicy.fps_cap > 0) {
          startupFps = std::min(startupFps, startupPolicy.fps_cap);
        }
        if ((startupBitrateKbps > 0 && startupBitrateKbps < qualityCeilingBitrateKbps) ||
            (startupFps > 0 && startupFps < qualityCeilingFramerate)) {
          BOOST_LOG(info) << "Ceiling-aware startup: starting at "
                          << startupBitrateKbps << " Kbps / "
                          << startupFps
                          << " fps under quality ceiling "
                          << qualityCeilingBitrateKbps << " Kbps / "
                          << qualityCeilingFramerate
                          << " fps; weak-net keeps display/capture cadence at the quality ceiling"
                          << " pathReason=" << startupPathDecision.reason
                          << " startupPolicy=" << startupPolicy.reason
                          << " route=" << session_runtime::transport_route_name(startupPathDecision.route)
                          << " egressKind=" << session_runtime::li_path_egress_kind_name(startupPathDecision.egress_kind)
                          << " encapsulation=" << session_runtime::li_path_encapsulation_name(startupPathDecision.encapsulation);
          configuredBitrateKbps = startupBitrateKbps;
        }
      }
      else if (adaptive_controller_enabled) {
        BOOST_LOG(info) << "Strong LAN startup: keeping requested quality "
                        << qualityCeilingBitrateKbps << " Kbps / "
                        << qualityCeilingFramerate
                        << " fps for peer=" << rtspPeerAddress
                        << " pathReason=" << startupPathDecision.reason;
      }
      else {
        config.monitor.lowBitrateClarityIntentFlags = 0;
        config.monitor.lowBitrateTargetQp = 0;
        config.monitor.lowBitrateSharpenAlpha = 0.0f;
        BOOST_LOG(info) << "Adaptive streaming controller RTSP setup adaptiveController=off reason="
                        << (config::stream.adaptive_streaming_optimization ?
                              "client-feedback-unsupported" :
                              "config-disabled")
                        << " bitrate=" << configuredBitrateKbps << " Kbps"
                        << " fps=" << config.monitor.framerate
                        << " fec=" << effectiveFecPercentage << "%";
      }

      BOOST_LOG(debug) << "Final adjusted video encoding bitrate is "sv << configuredBitrateKbps << " Kbps"sv;
      config.monitor.qualityCeilingBitrate = static_cast<int>(qualityCeilingBitrateKbps);
      config.monitor.qualityCeilingFramerate = qualityCeilingFramerate;
      config.monitor.bitrate = configuredBitrateKbps;
    }

    if (config.monitor.videoFormat == 1 && video::active_hevc_mode == 1) {
      BOOST_LOG(warning) << "HEVC is disabled, yet the client requested HEVC"sv;

      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    if (config.monitor.videoFormat == 2 && video::active_av1_mode == 1) {
      BOOST_LOG(warning) << "AV1 is disabled, yet the client requested AV1"sv;

      respond(sock, session, &option, 400, "BAD REQUEST", req->sequenceNumber, {});
      return;
    }

    // 检测是否仅控制流会话（只有 control 流被设置，没有 video 和 audio）
    session.control_only = session.setup_control && !session.setup_video && !session.setup_audio;
    if (session.control_only) {
      BOOST_LOG(info) << "Control-only session detected: client ["sv << session.client_name << "] will only provide input control"sv;
    }

    // Check that any required encryption is enabled
    // 对于仅控制流会话，跳过视频/音频加密检查
    if (!session.control_only) {
      auto encryption_mode = net::encryption_mode_for_address(sock.remote_endpoint().address());
      if (encryption_mode == config::ENCRYPTION_MODE_MANDATORY &&
          (config.encryptionFlagsEnabled & (SS_ENC_VIDEO | SS_ENC_AUDIO)) != (SS_ENC_VIDEO | SS_ENC_AUDIO)) {
        BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

        respond(sock, session, &option, 403, "Forbidden", req->sequenceNumber, {});
        return;
      }
    }

    auto stream_session = stream::session::alloc(config, session);
    server->insert(stream_session);
    auto stream_session_cleanup = util::fail_guard([&]() {
      if (stream_session) {
        stream::session::stop(*stream_session);
        server->remove(stream_session);
      }
    });

    if (stream::session::start(*stream_session, sock.remote_endpoint().address().to_string())) {
      BOOST_LOG(error) << "Failed to start a streaming session"sv;

      respond(sock, session, &option, 500, "Internal Server Error", req->sequenceNumber, {});
      return;
    }

    BOOST_LOG(info) << "RTSP ANNOUNCE stream started"
                    << " launchSession="sv << session.unique_id
                    << " id="sv << session.id
                    << " peer="sv << sock.remote_endpoint().address().to_string()
                    << " fps="sv << config.monitor.framerate
                    << " bitrate="sv << config.monitor.bitrate
                    << " mic="sv << (session.enable_mic ? 1 : 0);

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
    stream_session_cleanup.disable();
  }

  void
  cmd_play(rtsp_server_t *server, tcp::socket &sock, launch_session_t &session, msg_t &&req) {
    OPTION_ITEM option {};

    // I know these string literals will not be modified
    option.option = const_cast<char *>("CSeq");

    auto seqn_str = std::to_string(req->sequenceNumber);
    option.content = const_cast<char *>(seqn_str.c_str());

    BOOST_LOG(info) << "RTSP PLAY"
                    << " launchSession="sv << session.unique_id
                    << " id="sv << session.id
                    << " setup(video/audio/control/mic)="sv
                    << (session.setup_video ? 1 : 0) << "/"
                    << (session.setup_audio ? 1 : 0) << "/"
                    << (session.setup_control ? 1 : 0) << "/"
                    << (session.setup_mic ? 1 : 0)
                    << " pending="sv << rtsp_stream::pending_launch_session_count()
                    << " active="sv << rtsp_stream::session_count()
                    << " remote="sv << sock.remote_endpoint().address().to_string();

    respond(sock, session, &option, 200, "OK", req->sequenceNumber, {});
  }

  void
  start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    server.map("OPTIONS"sv, &cmd_option);
    server.map("DESCRIBE"sv, &cmd_describe);
    server.map("SETUP"sv, &cmd_setup);
    server.map("ANNOUNCE"sv, &cmd_announce);
    server.map("PLAY"sv, &cmd_play);

    boost::system::error_code ec;
    if (server.bind(net::af_from_enum_string(config::sunshine.address_family), net::map_port(rtsp_stream::RTSP_SETUP_PORT), ec)) {
      BOOST_LOG(fatal) << "Couldn't bind RTSP server to port ["sv << net::map_port(rtsp_stream::RTSP_SETUP_PORT) << "], " << ec.message();
      shutdown_event->raise(true);

      return;
    }

    std::thread rtsp_thread { [&shutdown_event] {
      auto broadcast_shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);

      while (!shutdown_event->peek()) {
        server.iterate();

        if (broadcast_shutdown_event->peek()) {
          server.clear();
        }
        else {
          // cleanup all stopped sessions
          server.clear(false);
        }
      }

      server.clear();
    } };

    // Wait for shutdown
    shutdown_event->view();

    // Stop the server and join the server thread
    server.stop();
    rtsp_thread.join();
  }

  void
  print_msg(PRTSP_MESSAGE msg) {
    std::string_view type = msg->type == TYPE_RESPONSE ? "RESPONSE"sv : "REQUEST"sv;

    std::string_view payload { msg->payload, (size_t) msg->payloadLength };
    std::string_view protocol { msg->protocol };
    auto seqnm = msg->sequenceNumber;
    std::string_view messageBuffer { msg->messageBuffer };

    std::ostringstream log_stream;
    log_stream << "type ["sv << type << "], sequence number ["sv << seqnm << "], protocol :: "sv << protocol << ", payload :: "sv << payload;

    if (msg->type == TYPE_RESPONSE) {
      auto &resp = msg->message.response;

      auto statuscode = resp.statusCode;
      std::string_view status { resp.statusString };

      log_stream << "statuscode :: "sv << statuscode << ", status :: "sv << status;
    }
    else {
      auto &req = msg->message.request;

      std::string_view command { req.command };
      std::string_view target { req.target };

      log_stream << "command :: "sv << command << ", target :: "sv << target;
    }

    for (auto option = msg->options; option != nullptr; option = option->next) {
      std::string_view content { option->content };
      std::string_view name { option->option };

      log_stream << name << " :: "sv << content;
    }

    log_stream << std::endl
               << "---Begin MessageBuffer---"sv << std::endl
               << messageBuffer << std::endl
               << "---End MessageBuffer---"sv;
    BOOST_LOG(debug) << log_stream.str();
  }
}  // namespace rtsp_stream
