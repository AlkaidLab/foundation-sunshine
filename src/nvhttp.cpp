/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device/display_device.h"
#include "display_device/session.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"
#include "video.h"
#include "webhook.h"

using json = nlohmann::json;

using namespace std::literals;
namespace nvhttp {

  static constexpr std::string_view EMPTY_PROPERTY_TREE_ERROR_MSG = "Property tree is empty. Probably, control flow got interrupted by an unexpected C++ exception. This is a bug in Sunshine. Moonlight-qt will report Malformed XML (missing root element)."sv;

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
  };

  struct client_t {
    std::vector<named_cert_t> named_devices;
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  static std::string last_pair_name;

  // Map to store certificate UUIDs keyed by request pointer
  // Using weak_ptr to track request lifetime and prevent memory leaks
  static std::map<const void*, std::pair<std::weak_ptr<void>, std::string>> request_cert_uuid_map;
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
                    // Find matching certificate UUID
                    for (const auto &named_cert : client_root.named_devices) {
                      if (named_cert.cert == client_cert_pem) {
                        // Store UUID in map using request pointer as key
                        std::lock_guard<std::mutex> lock(request_cert_uuid_map_mutex);
                        request_cert_uuid_map[session->request.get()] = 
                          std::make_pair(std::weak_ptr<void>(std::static_pointer_cast<void>(session->request)), named_cert.uuid);
                        break;
                      }
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

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

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
        } else {
          // Request expired, remove from map
          request_cert_uuid_map.erase(it);
        }
      }
    }
    catch (const std::exception &e) {
      BOOST_LOG(debug) << "获取客户端证书UUID失败: " << e.what();
    }
    return "";
  }

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

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

  void
  save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      }
      catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      pt::write_json(config::nvhttp.file_state, root);
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void
  load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(debug) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        client.named_devices.emplace_back(named_cert);
      }
    }

    // Empty certificate chain and import certs from file
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(crypto::x509(named_cert.cert));
    }

    client_root = client;
  }

  void
  add_authorized_client(const std::string &name, std::string &&cert) {
    client_t &client = client_root;
    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = std::move(cert);
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    client.named_devices.emplace_back(named_cert);

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
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

    // set vdd auto enable sops
    if (config::video.preferUseVdd) {
      launch_session->enable_sops = "1";
    }

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
    }

    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    map_id_sess.erase(sess.client.uniqueID);
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin, const std::string &client_name) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view { sess.async_insert_pin.salt.data(), 32 };

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    // 增加自定义客户端名字告诉客户端
    tree.put("root.pairname", client_name);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }

    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({ (char *) decrypted.data(), decrypted.size() });
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      // The client is now successfully paired and will be authorized to connect
      add_authorized_client(client.name, std::move(client.cert));
    }
    else {
      tree.put("root.paired", 0);
    }
    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
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
  not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template <class T>
  void
  pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID { get_arg(args, "uniqueid") };

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
        last_pair_name = get_arg(args, "clientname", "Named Zako");

        BOOST_LOG(verbose) << "Client cert: " << sess.client.cert.substr(0, 100) << "...";
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin, last_pair_name);
        }
        else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin(last_pair_name);
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      }
      else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, add_cert, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool
  pin(std::string pin, std::string name) {
    pt::ptree tree;
    if (map_id_sess.empty()) {
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message", "Pin must be 4 digits, " + std::to_string(pin.size()) + " provided");
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = std::begin(map_id_sess)->second;
    getservercert(sess, tree, pin, name);
    sess.client.name = name;

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    }
    else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    }
    else {
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
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

    // pt::ptree display_nodes;
    // for (auto &resolution : config::nvhttp.resolutions) {
    //   auto pred = [](auto ch) { return ch == ' ' || ch == '\t' || ch == 'x'; };

    //   auto middle = std::find_if(std::begin(resolution), std::end(resolution), pred);
    //   if (middle == std::end(resolution)) {
    //     BOOST_LOG(warning) << resolution << " is not in the proper format for a resolution: WIDTHxHEIGHT"sv;
    //     continue;
    //   }

    //   auto width = util::from_chars(&*std::begin(resolution), &*middle);
    //   auto height = util::from_chars(&*(middle + 1), &*std::end(resolution));
    //   for (auto fps : config::nvhttp.fps) {
    //     pt::ptree display_node;
    //     display_node.put("Width", width);
    //     display_node.put("Height", height);
    //     display_node.put("RefreshRate", fps);

    //     display_nodes.add_child("DisplayMode", display_node);
    //   }
    // }

    // if (!config::nvhttp.resolutions.empty()) {
    //   tree.add_child("root.SupportedDisplayMode", display_nodes);
    // }
    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  std::string
  get_pair_name() {
    return last_pair_name;
  }

  void
  applist(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID"s, proc.id);

      json json_cmds;

      for (auto &cmd : proc.menu_cmds) {
        json json_cmd;
        json_cmd["id"] = cmd.id;
        json_cmd["name"] = cmd.name;
        json_cmd["do_cmd"] = cmd.do_cmd;
        json_cmd["elevated"] = cmd.elevated;

        json_cmds.push_back(json_cmd);
      }

      app.put("SuperCmds"s, json_cmds.dump(4));

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void
  changeBitrate(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    try {
      auto args = request->parse_query_string();
      auto bitrate_param = args.find("bitrate");
      auto clientname_param = args.find("clientname");

      if (bitrate_param == args.end()) {
        tree.put("root.bitrate", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing bitrate parameter");
        return;
      }

      if (clientname_param == args.end()) {
        tree.put("root.bitrate", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing clientname parameter");
        return;
      }

      int bitrate = std::stoi(bitrate_param->second);
      std::string client_name = clientname_param->second;

      if (bitrate <= 0 || bitrate > 800000) {
        tree.put("root.bitrate", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Invalid bitrate value. Must be between 1 and 800000 Kbps");
        return;
      }

      video::dynamic_param_t param;
      param.type = video::dynamic_param_type_e::BITRATE;
      param.value.int_value = bitrate;
      param.valid = true;
      
      bool success = stream::session::change_dynamic_param_for_client(client_name, param);

      if (success) {
        tree.put("root.bitrate", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        tree.put("root.<xmlattr>.bitrate", bitrate);
        tree.put("root.<xmlattr>.clientname", client_name);
        tree.put("root.<xmlattr>.status_message", "Bitrate change request sent to client session");
        BOOST_LOG(info) << "NVHTTP API: Dynamic bitrate change requested for client '" 
                       << client_name << "': " << bitrate << " Kbps";
      } else {
        tree.put("root.bitrate", 0);
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "No active streaming session found for client: " + client_name);
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "ChangeBitrate: "sv << e.what();
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 500);
      tree.put("root.<xmlattr>.status_message", e.what());
    }
  }

  void
  changeDynamicParam(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    try {
      auto args = request->parse_query_string();
      auto param_type_param = args.find("type");
      auto param_value_param = args.find("value");
      auto clientname_param = args.find("clientname");

      if (param_type_param == args.end()) {
        tree.put("root.success", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing param_type parameter");
        return;
      }

      if (param_value_param == args.end()) {
        tree.put("root.success", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing param_value parameter");
        return;
      }

      if (clientname_param == args.end()) {
        tree.put("root.success", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Missing clientname parameter");
        return;
      }

      int param_type = std::stoi(param_type_param->second);
      std::string param_value = param_value_param->second;
      std::string client_name = clientname_param->second;

      // 验证参数类型
      if (param_type < 0 || param_type >= (int)video::dynamic_param_type_e::MAX_PARAM_TYPE) {
        tree.put("root.success", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "Invalid param_type value");
        return;
      }

      // 创建动态参数
      video::dynamic_param_t param;
      param.type = static_cast<video::dynamic_param_type_e>(param_type);
      param.valid = true;

      // 根据参数类型解析值
      switch (param.type) {
        case video::dynamic_param_type_e::BITRATE: {
          int bitrate = std::stoi(param_value);
          if (bitrate <= 0 || bitrate > 800000) {
            tree.put("root.success", 0);
            tree.put("root.<xmlattr>.status_code", 400);
            tree.put("root.<xmlattr>.status_message", "Invalid bitrate value. Must be between 1 and 800000 Kbps");
            return;
          }
          param.value.int_value = bitrate;
          break;
        }
        case video::dynamic_param_type_e::QP: {
          int qp = std::stoi(param_value);
          if (qp < 0 || qp > 51) {
            tree.put("root.success", 0);
            tree.put("root.<xmlattr>.status_code", 400);
            tree.put("root.<xmlattr>.status_message", "Invalid QP value. Must be between 0 and 51");
            return;
          }
          param.value.int_value = qp;
          break;
        }
        case video::dynamic_param_type_e::FEC_PERCENTAGE: {
          int fec = std::stoi(param_value);
          if (fec < 0 || fec > 100) {
            tree.put("root.success", 0);
            tree.put("root.<xmlattr>.status_code", 400);
            tree.put("root.<xmlattr>.status_message", "Invalid FEC percentage. Must be between 0 and 100");
            return;
          }
          param.value.int_value = fec;
          break;
        }
        case video::dynamic_param_type_e::ADAPTIVE_QUANTIZATION: {
          param.value.bool_value = (param_value == "true" || param_value == "1");
          break;
        }
        case video::dynamic_param_type_e::MULTI_PASS: {
          int multi_pass = std::stoi(param_value);
          if (multi_pass < 0 || multi_pass > 2) {
            tree.put("root.success", 0);
            tree.put("root.<xmlattr>.status_code", 400);
            tree.put("root.<xmlattr>.status_message", "Invalid multi-pass value. Must be between 0 and 2");
            return;
          }
          param.value.int_value = multi_pass;
          break;
        }
        case video::dynamic_param_type_e::VBV_BUFFER_SIZE: {
          int vbv = std::stoi(param_value);
          if (vbv <= 0) {
            tree.put("root.success", 0);
            tree.put("root.<xmlattr>.status_code", 400);
            tree.put("root.<xmlattr>.status_message", "Invalid VBV buffer size. Must be greater than 0");
            return;
          }
          param.value.int_value = vbv;
          break;
        }
        default:
          tree.put("root.success", 0);
          tree.put("root.<xmlattr>.status_code", 400);
          tree.put("root.<xmlattr>.status_message", "Unsupported parameter type");
          return;
      }

      bool success = stream::session::change_dynamic_param_for_client(client_name, param);

      if (success) {
        tree.put("root.success", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        tree.put("root.<xmlattr>.param_type", param_type);
        tree.put("root.<xmlattr>.param_value", param_value);
        tree.put("root.<xmlattr>.clientname", client_name);
        tree.put("root.<xmlattr>.status_message", "Dynamic parameter change request sent to client session");
        BOOST_LOG(info) << "NVHTTP API: Dynamic parameter change requested for client '" 
                       << client_name << "': type=" << param_type << ", value=" << param_value;
      } else {
        tree.put("root.success", 0);
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "No active streaming session found for client: " + client_name);
      }
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "ChangeDynamicParam: "sv << e.what();
      tree.put("root.success", 0);
      tree.put("root.<xmlattr>.status_code", 500);
      tree.put("root.<xmlattr>.status_message", e.what());
    }
  }

  void
  getSessionsInfo(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    // 限制只允许 localhost 访问
    auto client_address = request->remote_endpoint().address();
    auto address = net::addr_to_normalized_string(client_address);
    auto ip_type = net::from_address(address);
    if (ip_type != net::PC) {
      json response_json;
      response_json["success"] = false;
      response_json["status_code"] = 403;
      std::ostringstream msg_stream;
      msg_stream << "Access denied. Only localhost requests are allowed. Client IP: " << client_address.to_string();
      response_json["status_message"] = msg_stream.str();

      response->write(response_json.dump());
      response->close_connection_after_response = true;
      return;
    }

    try {
      auto sessions_info = stream::session::get_all_sessions_info();
      
      json response_json;
      response_json["success"] = true;
      response_json["status_code"] = 200;
      response_json["status_message"] = "Success";
      response_json["total_sessions"] = sessions_info.size();
      
      json sessions_array = json::array();
      
      for (const auto &session_info : sessions_info) {
        json session_obj;
        session_obj["client_name"] = session_info.client_name;
        session_obj["client_address"] = session_info.client_address;
        session_obj["state"] = session_info.state;
        session_obj["session_id"] = session_info.session_id;
        session_obj["width"] = session_info.width;
        session_obj["height"] = session_info.height;
        session_obj["fps"] = session_info.fps;
        session_obj["host_audio"] = session_info.host_audio;
        session_obj["enable_hdr"] = session_info.enable_hdr;
        session_obj["enable_mic"] = session_info.enable_mic;
        session_obj["app_name"] = session_info.app_name;
        session_obj["app_id"] = session_info.app_id;
        
        sessions_array.push_back(session_obj);
      }
      
      response_json["sessions"] = sessions_array;
      
      BOOST_LOG(info) << "NVHTTP API: Session info requested from localhost, returned " << sessions_info.size() << " sessions";
      
      response->write(response_json.dump());
      response->close_connection_after_response = true;
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "GetSessionsInfo: "sv << e.what();
      
      json error_json;
      error_json["success"] = false;
      error_json["status_code"] = 500;
      error_json["status_message"] = e.what();
      
      response->write(error_json.dump());
      response->close_connection_after_response = true;
    }
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

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    const auto launch_session = make_launch_session(host_audio, args);
    
    // 获取客户端证书UUID（稳定的客户端标识符）
    std::string client_cert_uuid = get_client_cert_uuid_from_request(request);
    if (!client_cert_uuid.empty()) {
      launch_session->env["SUNSHINE_CLIENT_CERT_UUID"] = client_cert_uuid;
    }

    if (rtsp_stream::session_count() == 0) {
      // We want to prepare display only if there are no active sessions at
      // the moment. This should to be done before probing encoders as it could
      // change display device's state.
      display_device::session_t::get().configure_display(config::video, *launch_session, true);

      // The display should be restored by the fail guard in case something happens.
      need_to_restore_display_state = true;

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
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
    webhook::send_event_async(webhook::event_t{
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
        {"resolution", std::to_string(launch_session->width) + "x" + std::to_string(launch_session->height)},
        {"fps", std::to_string(launch_session->fps)},
        {"host_audio", launch_session->host_audio ? "true" : "false"}
      }
    });

    // Stream was started successfully, we will restore the state when the app or session terminates
    need_to_restore_display_state = false;
  }

  void
  resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);
    
    print_request_ip<SunshineHTTPS>(request, "Resume request");

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
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

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
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
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
      display_device::session_t::get().configure_display(config::video, *launch_session, false);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

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
    webhook::send_event_async(webhook::event_t{
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
        {"resolution", std::to_string(launch_session->width) + "x" + std::to_string(launch_session->height)},
        {"fps", std::to_string(launch_session->fps)},
        {"host_audio", launch_session->host_audio ? "true" : "false"}
      }
    });
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

    boost::process::environment _env = boost::this_process::environment();
    auto working_dir = boost::filesystem::path();
    std::error_code ec;
    std::string cmd = "rundll32.exe powrprof.dll,SetSuspendState 0,1,0";

    auto child = platf::run_command(false, true, cmd, working_dir, _env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't run cmd ["sv << cmd << "]: System: "sv << ec.message();
    }
    else {
      BOOST_LOG(info) << "Executing sleep cmd ["sv << cmd << "]"sv;
      child.detach();
    }

    pt::ptree tree;
    tree.put("root.pcsleep", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  void
  execSuperCmd(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto cmdId = get_arg(args, "cmdId", "");
    proc::proc.run_menu_cmd(cmdId);

    pt::ptree tree;
    tree.put("root.supercmd", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  void
  appasset(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void
  get_displays(resp_https_t response, req_https_t request) {
    print_req<SunshineHTTPS>(request);

    json response_json;
    response_json["status_code"] = 200;
    response_json["status_message"] = "OK";

    try {
      std::vector<std::string> display_names;
      
#ifdef _WIN32
      display_names = platf::display_names(platf::mem_type_e::dxgi);
#elif defined(__linux__)
      for (auto mem_type : {platf::mem_type_e::vaapi, platf::mem_type_e::cuda, platf::mem_type_e::system}) {
        display_names = platf::display_names(mem_type);
        if (!display_names.empty()) break;
      }
#elif defined(__APPLE__)
      display_names = platf::display_names(platf::mem_type_e::videotoolbox);
#else
      display_names = platf::display_names(platf::mem_type_e::system);
#endif

      json displays_array = json::array();
      
#ifdef _WIN32
      // Build GDI name -> (device_id, friendly_name) mapping
      std::unordered_map<std::string, std::pair<std::string, std::string>> display_info_map;
      try {
        for (const auto &[device_id, device_info] : display_device::enum_available_devices()) {
          if (std::string gdi_name = display_device::get_display_name(device_id); !gdi_name.empty()) {
            display_info_map[gdi_name] = {device_id, display_device::get_display_friendly_name(device_id)};
          }
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "Failed to get display friendly names: " << e.what();
      }
      
      for (size_t i = 0; i < display_names.size(); ++i) {
        const auto &name = display_names[i];
        auto it = display_info_map.find(name);
        bool found = (it != display_info_map.end());
        displays_array.push_back({
          {"index", static_cast<int>(i)},
          {"display_name", name},
          {"device_id", found ? it->second.first : name},
          {"friendly_name", (found && !it->second.second.empty()) ? it->second.second : name}
        });
      }
#else
      for (size_t i = 0; i < display_names.size(); ++i) {
        const auto &name = display_names[i];
        displays_array.push_back({
          {"index", static_cast<int>(i)},
          {"display_name", name},
          {"device_id", name},
          {"friendly_name", name}
        });
      }
#endif

      response_json["displays"] = std::move(displays_array);
      response_json["count"] = static_cast<int>(display_names.size());
    }
    catch (const std::exception &e) {
      BOOST_LOG(error) << "Error getting display list: " << e.what();
      response_json["status_code"] = 500;
      response_json["status_message"] = "Internal server error";
      response_json["displays"] = json::array();
      response_json["count"] = 0;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(SimpleWeb::StatusCode::success_ok, response_json.dump(), headers);
    response->close_connection_after_response = true;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
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
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server { config::nvhttp.cert, config::nvhttp.pkey };
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [add_cert, close_verify_safe](SSL *ssl) {
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

      while (add_cert->peek()) {
        char subject_name[256];

        auto cert = add_cert->pop();
        X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << "Added cert ["sv << subject_name << ']';
        cert_chain.add(std::move(cert));
      }

      const char * err_str;
      if (!close_verify_safe) {
        err_str = cert_chain.verify_safe(x509.get());//default
      } else {
        err_str = cert_chain.verify(x509.get());
      }
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
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) { pair<SunshineHTTPS>(add_cert, resp, req); };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/displays$"]["GET"] = get_displays;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) { launch(host_audio, resp, req); };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) { resume(host_audio, resp, req); };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/pcsleep$"]["GET"] = sleep;
    https_server.resource["^/supercmd$"]["GET"] = execSuperCmd;
    https_server.resource["^/bitrate$"]["GET"] = changeBitrate;
    https_server.resource["^/stream/settings$"]["GET"] = changeDynamicParam;
    https_server.resource["^/sessions$"]["GET"] = getSessionsInfo;

    https_server.config.reuse_address = true;
    https_server.config.address = net::get_bind_address(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) { pair<SimpleWeb::HTTP>(add_cert, resp, req); };

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

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void
  erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
  }

  int
  unpair_client(std::string uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it).uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();
    return removed;
  }
}  // namespace nvhttp
