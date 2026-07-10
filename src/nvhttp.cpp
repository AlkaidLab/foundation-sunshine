/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// lib includes
#include <Simple-Web-Server/server_http.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>

// local includes
#include "config.h"
#include "confighttp.h"
#include "display_device/session.h"
#include "file_handler.h"
#include "file_mapping/file_mapping_http.h"
#include "file_mapping/service.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "nvhttp/abr_api.h"
#include "nvhttp/ai_api.h"
#include "nvhttp/apps.h"
#include "nvhttp/clipboard_api.h"
#include "nvhttp/display_control.h"
#include "nvhttp/display_scale.h"
#include "nvhttp/dynamic_params.h"
#include "nvhttp/pairing.h"
#include "nvhttp/sessions.h"
#include "nvhttp_stream_start.h"
#include "platform/common.h"
#include "platform/run_command.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "tray/system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "webhook.h"

using json = nlohmann::json;

using namespace std::literals;
namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace pt = boost::property_tree;

  static const std::unordered_set<std::string> blocked_paths = {
    "/", "/index.html", "/index.htm", "/index",
    "/favicon.ico", "/favicon.png", "/favicon.svg"
  };

  std::atomic<uint32_t> session_id_counter;

  // Map to store certificate UUIDs keyed by request pointer
  // Using weak_ptr to track request lifetime and prevent memory leaks
  static std::map<const void *, std::pair<std::weak_ptr<void>, std::string>> request_cert_uuid_map;
  static std::mutex request_cert_uuid_map_mutex;

  class SunshineHTTPSServer: public SimpleWeb::ServerBase<SunshineHTTPS> {
  public:
    SunshineHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SunshineHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<int(SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void
    after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void
    accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock)
          return;

        if (ec != SimpleWeb::error::operation_aborted)
          this->accept();

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock)
              return;
            if (!ec) {
              // Extract and store certificate UUID during handshake
              try {
                SSL *ssl = session->connection->socket->native_handle();
                if (ssl) {
                  crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
                    SSL_get1_peer_certificate(ssl)
#else
                    SSL_get_peer_certificate(ssl)
#endif
                  };
                  if (x509) {
                    std::string client_cert_pem = crypto::pem(x509);
                    if (auto uuid = pairing::client_uuid_for_cert(client_cert_pem); !uuid.empty()) {
                      // Store UUID in map using request pointer as key.
                      // Opportunistically sweep expired entries on every
                      // insert so that requests which never reach
                      // get_client_cert_uuid_from_request() (e.g. serverinfo
                      // polling, applist) and never trigger on_error don't
                      // accumulate forever. This bounds the map size at
                      // ~(live requests + recently completed requests).
                      std::lock_guard<std::mutex> lock(request_cert_uuid_map_mutex);
                      for (auto it = request_cert_uuid_map.begin(); it != request_cert_uuid_map.end();) {
                        if (it->second.first.expired()) {
                          it = request_cert_uuid_map.erase(it);
                        }
                        else {
                          ++it;
                        }
                      }
                      request_cert_uuid_map[session->request.get()] =
                        std::make_pair(std::weak_ptr<void>(std::static_pointer_cast<void>(session->request)), std::move(uuid));
                    }
                  }
                }
              }
              catch (const std::exception &e) {
                BOOST_LOG(debug) << "Failed to extract certificate UUID during handshake: " << e.what();
              }

              if (verify && !verify(session->connection->socket->native_handle()))
                this->write(session, on_verify_failed);
              else
                this->read(session);
            }
            else if (this->on_error)
              this->on_error(session->request, ec);
          });
        }
        else if (this->on_error)
          this->on_error(session->request, ec);
      });
    }
  };

  using https_server_t = SunshineHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SunshineHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  // Get client certificate UUID from request
  std::string
  get_client_cert_uuid_from_request(req_https_t request) {
    try {
      // Retrieve UUID from map using request pointer as key
      std::lock_guard<std::mutex> lock(request_cert_uuid_map_mutex);
      auto it = request_cert_uuid_map.find(request.get());
      if (it != request_cert_uuid_map.end()) {
        // Check if request is still valid (not expired)
        if (!it->second.first.expired()) {
          std::string uuid = it->second.second;
          // Clean up after retrieval to prevent memory leaks
          // (assuming UUID is only needed once per request)
          request_cert_uuid_map.erase(it);
          return uuid;
        }
        else {
          // Request expired, remove from map
          request_cert_uuid_map.erase(it);
        }
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(debug) << "Failed to get client certificate UUID: " << e.what();
    }
    return "";
  }

  std::string
  get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != NULL) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  std::shared_ptr<rtsp_stream::launch_session_t>
  make_launch_session(bool host_audio, const args_t &args) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) launch_session->width = atoi(segment.c_str());
      if (x == 1) launch_session->height = atoi(segment.c_str());
      if (x == 2) launch_session->fps = atoi(segment.c_str());
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->client_name = (get_arg(args, "clientname", "unknown"));
    launch_session->appid = util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->continuous_audio = util::from_view(get_arg(args, "continuousAudio", "0"));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    launch_session->use_vdd = util::from_view(get_arg(args, "useVdd", "0"));
    launch_session->custom_screen_mode = util::from_view(get_arg(args, "customScreenMode", "-1"));
    launch_session->max_nits = std::stof(get_arg(args, "maxBrightness", "1000"));
    launch_session->min_nits = std::stof(get_arg(args, "minBrightness", "0.001"));
    launch_session->max_full_nits = std::stof(get_arg(args, "maxAverageBrightness", "1000"));

    // Get display_name from query parameter if provided
    std::string display_name = get_arg(args, "display_name", "");
    if (!display_name.empty()) {
      launch_session->env["SUNSHINE_CLIENT_DISPLAY_NAME"] = display_name;
      BOOST_LOG(info) << "Launch session will use specified display: " << display_name;
    }

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key, false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));

    // set auto enable sops
    launch_session->enable_sops = "1";

    launch_session->env["SUNSHINE_CLIENT_ID"] = std::to_string(launch_session->id);
    launch_session->env["SUNSHINE_CLIENT_UNIQUE_ID"] = launch_session->unique_id;
    launch_session->env["SUNSHINE_CLIENT_NAME"] = launch_session->client_name;
    launch_session->env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    launch_session->env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    launch_session->env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    launch_session->env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    launch_session->env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    launch_session->env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    launch_session->env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    launch_session->env["SUNSHINE_CLIENT_ENABLE_MIC"] = launch_session->enable_mic ? "true" : "false";
    launch_session->env["SUNSHINE_CLIENT_USE_VDD"] = launch_session->use_vdd ? "true" : "false";
    launch_session->env["SUNSHINE_CLIENT_CUSTOM_SCREEN_MODE"] = std::to_string(launch_session->custom_screen_mode);
    int channelCount = launch_session->surround_info & (65535);
    switch (channelCount) {
      case 2:
        launch_session->env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        launch_session->env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        launch_session->env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
      case 12:
        launch_session->env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1.4";
        break;
    }

    return launch_session;
  }

  template <class T>
  struct tunnel;

  template <>
  struct tunnel<SunshineHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template <>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template <class T>
  void
  print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    auto debug_flag = debug.open_record();
    auto verbose_flag = verbose.open_record();
    if (!debug_flag && !verbose_flag) {
      return;
    }
    std::ostringstream log_stream;
    log_stream << "Request - Protocol: " << tunnel<T>::to_string
               << ", IP: " << request->remote_endpoint().address().to_string()
               << ", PORT: " << request->remote_endpoint().port()
               << ", METHOD: " << request->method
               << ", PATH: " << request->path;

    if (verbose_flag) {
      // Headers
      if (!request->header.empty()) {
        log_stream << ", HEADERS: ";
        bool first = true;
        for (auto &[name, val] : request->header) {
          if (!first) log_stream << ", ";
          log_stream << name << "=" << val;
          first = false;
        }
      }

      // Query parameters
      auto query_params = request->parse_query_string();
      if (!query_params.empty()) {
        log_stream << ", PARAMS: ";
        bool first = true;
        for (auto &[name, val] : query_params) {
          if (!first) log_stream << "&";
          log_stream << name << "=" << val;
          first = false;
        }
      }
    }
    BOOST_LOG(debug) << log_stream.str();
  }

  template <class T>
  void
  print_request_ip(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request, const std::string &message) {
    BOOST_LOG(info) << message << " from IP: " << request->remote_endpoint().address().to_string() << ", Port: " << request->remote_endpoint().port();
  }

  template <class T>
  void
  print_request_warning_ip(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request, const std::string &message) {
    BOOST_LOG(warning) << message << " [" << request->query_string << "] from IP: " << request->remote_endpoint().address().to_string() << ", Port: " << request->remote_endpoint().port();
  }

  template <class T>
  void
  not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    // Security hardening: Return 444 for root paths to prevent probing
    if (blocked_paths.count(request->path)) {
      *response << "HTTP/1.1 444 No Response\r\n";
      response->close_connection_after_response = true;
      return;
    }

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(SimpleWeb::StatusCode::client_error_not_found, data.str());
    response->close_connection_after_response = true;
  }

  template <class T>
  void
  serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.SunshineVersion", SUNSHINE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SunshineHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    }
    else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    }
    else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::last_encoder_probe_supported_yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (video::last_encoder_probe_supported_yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");
    tree.put("root.appListEtag", proc::proc.get_apps_etag());

    // AI capability: inform client if AI proxy is available
    tree.put("root.AiCapability", confighttp::isAiEnabled() ? 1 : 0);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
  }

  void
  launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    print_request_ip<SunshineHTTPS>(request, "Launch request");

    pt::ptree tree;
    bool need_to_restore_display_state { false };
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;

      if (need_to_restore_display_state) {
        display_device::session_t::get().restore_state();
      }
    });

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    // Early validation of AppID to prevent starting VDD or other expensive operations
    // if the requested app does not exist.
    if (proc::proc.get_app_name(appid).empty()) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "App not found");
      BOOST_LOG(error) << "Launch couldn't find app with ID ["sv << appid << ']';
      return;
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    const auto launch_session = make_launch_session(host_audio, args);

    // Store the stable client certificate UUID in the launch environment.
    std::string client_cert_uuid = get_client_cert_uuid_from_request(request);
    if (!client_cert_uuid.empty()) {
      launch_session->env["SUNSHINE_CLIENT_CERT_UUID"] = client_cert_uuid;
    }

    if (rtsp_stream::session_count() == 0) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should to be done before probing encoders as it could
      // change display device's state.
      // The display should be restored by the fail guard in case something happens.
      need_to_restore_display_state = true;

      if (!stream_start::prepare_display_and_probe_encoders(tree, *launch_session, true)) {
        tree.put("root.gamesession", 0);

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute(appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.sessionUrl0", launch_session->rtsp_url_scheme +
                                   net::addr_to_url_escaped_string(request->local_endpoint().address()) + ':' +
                                   std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT)));
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // Send webhook notification for successful launch
    webhook::send_event_async(webhook::event_t {
      .type = webhook::event_type_t::NV_APP_LAUNCH,
      .alert_type = "nv_app_launch",
      .timestamp = webhook::get_current_timestamp(),
      .client_name = launch_session->client_name,
      .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
      .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()),
      .app_name = proc::proc.get_app_name(appid),
      .app_id = appid,
      .session_id = std::to_string(launch_session->id),
      .extra_data = {
        { "resolution", std::to_string(launch_session->width) + "x" + std::to_string(launch_session->height) },
        { "fps", std::to_string(launch_session->fps) },
        { "host_audio", launch_session->host_audio ? "true" : "false" } } });

    // Stream was started successfully, we will restore the state when the app or session terminates
    need_to_restore_display_state = false;
  }

  void
  resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    print_request_ip<SunshineHTTPS>(request, "Resume request");

    // If the system is in Away Mode, exit it now since we're resuming a session
    if (platf::is_away_mode_active()) {
      BOOST_LOG(info) << "Exiting Away Mode due to incoming resume request"sv;
      platf::exit_away_mode();
    }

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      if (tree.empty()) {
        BOOST_LOG(error) << EMPTY_PROPERTY_TREE_ERROR_MSG;
      }

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      stream_start::set_sunshine_error(
        tree,
        503,
        "There is no running app to resume. Start the app again from the client.",
        "NO_APP_TO_RESUME",
        "The previous session is no longer active or was already stopped.",
        "relaunch_app_from_client",
        "session",
        "resume",
        true);

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions { rtsp_stream::session_count() == 0 };
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    const auto launch_session = make_launch_session(host_audio, args);

    // Get client certificate UUID (stable client identifier) and store it in env
    std::string client_cert_uuid = get_client_cert_uuid_from_request(request);
    if (!client_cert_uuid.empty()) {
      launch_session->env["SUNSHINE_CLIENT_CERT_UUID"] = client_cert_uuid;
    }

    if (no_active_sessions) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should be done before probing encoders as it could
      // change the active displays.
      if (!stream_start::prepare_display_and_probe_encoders(tree, *launch_session, false)) {
        tree.put("root.resume", 0);

        return;
      }
    }
    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.sessionUrl0", launch_session->rtsp_url_scheme +
                                   net::addr_to_url_escaped_string(request->local_endpoint().address()) + ':' +
                                   std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT)));
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);

    // Send webhook notification for successful resume
    webhook::send_event_async(webhook::event_t {
      .type = webhook::event_type_t::NV_APP_RESUME,
      .alert_type = "nv_app_resume",
      .timestamp = webhook::get_current_timestamp(),
      .client_name = launch_session->client_name,
      .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
      .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()),
      .app_name = proc::proc.get_app_name(proc::proc.running()),
      .app_id = proc::proc.running(),
      .session_id = std::to_string(launch_session->id),
      .extra_data = {
        { "resolution", std::to_string(launch_session->width) + "x" + std::to_string(launch_session->height) },
        { "fps", std::to_string(launch_session->fps) },
        { "host_audio", launch_session->host_audio ? "true" : "false" } } });
  }

  void
  cancel(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    print_request_ip<SunshineHTTPS>(request, "Cancel request");

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }

    // The state needs to be restored regardless of whether "proc::proc.terminate()" was called or not.
    display_device::session_t::get().restore_state();
  }

  void
  sleep(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    bool success = true;
    switch (config::nvhttp.sleep_mode) {
      case config::SLEEP_MODE_HIBERNATE:
        BOOST_LOG(info) << "Sleep command: hibernate (S4)"sv;
        success = platf::system_hibernate();
        break;
      case config::SLEEP_MODE_AWAY:
        BOOST_LOG(info) << "Sleep command: away mode (display off)"sv;
        platf::enter_away_mode();
        break;
      case config::SLEEP_MODE_SUSPEND:
      default:
        BOOST_LOG(info) << "Sleep command: suspend (S3)"sv;
        success = platf::system_sleep();
        break;
    }

    if (!success) {
      BOOST_LOG(warning) << "Sleep command failed"sv;
    }

    pt::ptree tree;
    tree.put("root.pcsleep", success ? 1 : 0);
    tree.put("root.<xmlattr>.status_code", success ? 200 : 500);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }


  void
  setup(const std::string &pkey, const std::string &cert) {
    pairing::set_credentials(pkey, cert);
  }

  void
  start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];
    bool close_verify_safe = config::sunshine.flags[config::flag::CLOSE_VERIFY_SAFE];
    if (close_verify_safe) {
      BOOST_LOG(warning) << "SSL close safe verify: " << close_verify_safe;
    }

    if (!clean_slate) {
      pairing::load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    auto bind_address = net::get_bind_address(address_family);
    auto is_file_mapping_client_paired = [](std::string_view client_uuid) {
      if (client_uuid.empty()) {
        return false;
      }

      const auto clients = nvhttp::get_all_clients();
      for (const auto &client : clients) {
        if (client.contains("uuid") && client["uuid"].is_string() && std::string_view { client["uuid"].get_ref<const std::string &>() } == client_uuid) {
          return true;
        }
      }
      return false;
    };

    file_mapping::service_t file_mapping_service;
    file_mapping::service_t::config_t file_mapping_config;
    file_mapping_config.bind_address = bind_address.empty() ? "0.0.0.0" : bind_address;
    file_mapping_config.port = config::nvhttp.file_mapping_port;
    file_mapping_config.certificate_file = config::nvhttp.cert;
    file_mapping_config.private_key_file = config::nvhttp.pkey;
    file_mapping_config.mappings_json = config::nvhttp.file_mappings;
    file_mapping_config.authorize_client = is_file_mapping_client_paired;
    file_mapping_service.start(std::move(file_mapping_config));

    https_server_t https_server { config::nvhttp.cert, config::nvhttp.pkey };
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [close_verify_safe](SSL *ssl) {
      crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
        SSL_get1_peer_certificate(ssl)
#else
        SSL_get_peer_certificate(ssl)
#endif
      };
      if (!x509) {
        BOOST_LOG(info) << "SSL client unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      const char *err_str = pairing::verify_client_certificate(x509.get(), close_verify_safe);
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      verified = 1;

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SunshineHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SunshineHTTPS>;
    https_server.resource["^/pair$"]["GET"] = pairing::pair_https;
    https_server.resource["^/applist$"]["GET"] = apps::list;
    https_server.resource["^/appasset$"]["GET"] = apps::asset;
    https_server.resource["^/displays$"]["GET"] = display_control::get_displays;
    https_server.resource["^/display-scale-options$"]["GET"] = display_scale::get_options;
    https_server.resource["^/display-scale$"]["POST"] = display_scale::set;
    https_server.resource["^/rotate-display$"]["GET"] = display_control::rotate;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) { launch(host_audio, resp, req); };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) { resume(host_audio, resp, req); };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/pcsleep$"]["GET"] = sleep;
    https_server.resource["^/supercmd$"]["GET"] = apps::exec_super_cmd;
    https_server.resource["^/bitrate$"]["GET"] = dynamic_params::change_bitrate;
    https_server.resource["^/stream/settings$"]["GET"] = dynamic_params::change;
    https_server.resource["^/sessions$"]["GET"] = sessions::get;

    // Clipboard blob routes are mirrored onto nvhttp so paired Moonlight
    // clients can reuse their existing certificate-authenticated GameStream
    // channel for large clipboard payloads. The local GUI agent continues to
    // use the confighttp /api/v1/clipboard/* endpoints on loopback.
    https_server.resource["^/api/v1/clipboard/blob$"]["POST"] = clipboard_api::upload_blob;
    https_server.resource["^/api/v1/clipboard/blob/([A-Za-z0-9_\\-]{1,128})$"]["GET"] = clipboard_api::get_blob;

    https_server.resource["^/api/v1/file-mapping/capability$"]["GET"] =
      [&](resp_https_t resp, req_https_t req) {
        auto header_value = [](const SimpleWeb::CaseInsensitiveMultimap &headers, const std::string &name) {
          auto it = headers.find(name);
          return it == headers.end() ? std::string {} : it->second;
        };
        auto write_response = [](resp_https_t response, const file_mapping_http::http_response_t &out) {
          response->write(out.status, out.body, out.headers);
        };

        write_response(
          std::move(resp),
          file_mapping_service.make_capability_response(
            get_client_cert_uuid_from_request(req),
            header_value(req->header, "host")));
      };

    https_server.resource["^/api/v1/file-mapping/session$"]["GET"] =
      [](resp_https_t resp, req_https_t req) {
        auto out = file_mapping_http::make_session_placeholder_response(req->header);
        resp->write(out.status, out.body, out.headers);
      };

    // ABR (Adaptive Bitrate) API routes - client-facing with cert auth
    https_server.resource["^/api/abr/capabilities$"]["GET"] = abr_api::capabilities;
    https_server.resource["^/api/abr$"]["POST"] = abr_api::configure;
    https_server.resource["^/api/abr/feedback$"]["POST"] = abr_api::feedback;

    // AI LLM proxy route uses client cert auth from pairing.
    https_server.resource["^/ai/completions$"]["POST"] = ai_api::completions;

    https_server.config.reuse_address = true;
    https_server.config.address = bind_address;
    https_server.config.port = port_https;
    // Run nvhttps server with a small thread pool. The HTTPS endpoint serves
    // SSL handshakes + request handlers on the same io_service. With the default
    // single thread, any slow handshake / aborted SSL cleanup (e.g. a client
    // sending TCP RST while in-flight HTTP/2 streams are open) blocks accept
    // for all other clients until Sunshine is restarted. Multiple worker
    // threads keep the listener responsive under such conditions.
    https_server.config.thread_pool_size = 4;

    // Clean up request_cert_uuid_map entries when a request fails before
    // get_client_cert_uuid_from_request() is reached, otherwise stale entries
    // (with expired weak_ptr) accumulate over the lifetime of the process.
    https_server.on_error = [](req_https_t request, const SimpleWeb::error_code & /*ec*/) {
      std::lock_guard<std::mutex> lock(request_cert_uuid_map_mutex);
      request_cert_uuid_map.erase(request.get());
      // Opportunistic sweep of any other entries whose request has gone away.
      for (auto it = request_cert_uuid_map.begin(); it != request_cert_uuid_map.end();) {
        if (it->second.first.expired()) {
          it = request_cert_uuid_map.erase(it);
        } else {
          ++it;
        }
      }
    };

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = pairing::pair_http;

    http_server.config.reuse_address = true;
    http_server.config.address = net::get_bind_address(address_family);
    http_server.config.port = port_http;

    auto accept_and_run_https = [&](nvhttp::https_server_t *server) {
      try {
        BOOST_LOG(info) << "Starting nvhttps server on port ["sv << server->config.port << "]";
        server->start();
      }
      catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }
        BOOST_LOG(fatal) << "Couldn't start nvhttps server on ports ["sv << server->config.port << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };

    auto accept_and_run_http = [&](nvhttp::http_server_t *server) {
      try {
        BOOST_LOG(info) << "Starting nvhttp server on port ["sv << server->config.port << "]";
        server->start();
      }
      catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start nvhttp server on ports ["sv << server->config.port << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl { accept_and_run_https, &https_server };
    std::thread tcp { accept_and_run_http, &http_server };

    // Wait for any event
    shutdown_event->view();

    file_mapping_service.stop();
    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

}  // namespace nvhttp
