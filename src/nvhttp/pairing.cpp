#include "pairing.h"
#include "pairing_pending.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
  #include <windows.h>
#endif

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/globals.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/tray/system_tray.h"
#include "src/tray/tray_state.h"
#include "src/utility.h"
#include "src/uuid.h"

using namespace std::literals;

namespace nvhttp {

  void
  remove_session(const pair_session_t &sess);

  bool
  getservercert_checked(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &pin, const std::string &client_name);

  void
  clientchallenge(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &challenge);

  void
  serverchallengeresp(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &encrypted_response);

  void
  clientpairingsecret(pair_session_t &sess, boost::property_tree::ptree &tree, const std::string &client_pairing_secret);

  std::string
  consume_preset_pin();

  namespace {

    namespace fs = std::filesystem;
    namespace pt = boost::property_tree;

    fs::path
    state_file_path() {
      return file_handler::path_from_utf8(config::nvhttp.file_state);
    }

    std::string
    masked_client_id(std::string_view uuid) {
      const auto visible = std::min<std::size_t>(uuid.size(), 8);
      return std::string { uuid.substr(0, visible) } + "...";
    }

    struct named_cert_t {
      std::string name;
      std::string uuid;
      std::string cert;
      bool enabled { true };
      client_family::record_t family;
      std::int64_t last_session_ended { 0 };
    };

    struct client_t {
      std::vector<named_cert_t> named_devices;
    };

    struct conf_intern_t {
      std::string servercert;
      std::string pkey;
    } conf_intern;

    struct pair_rate_limiter_t {
      std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> attempts;
      std::mutex mutex;

      bool
      check_and_record(const std::string &ip) {
        const int max_attempts = config::nvhttp.pair_max_attempts;
        constexpr int window_seconds = 60;
        if (max_attempts <= 0) {
          return true;
        }

        std::lock_guard lock { mutex };
        auto now = std::chrono::steady_clock::now();
        auto &entry = attempts[ip];

        if (now - entry.second > std::chrono::seconds(window_seconds)) {
          entry = { 0, now };
        }

        if (entry.first >= max_attempts) {
          return false;
        }

        entry.first++;
        return true;
      }

      void
      cleanup() {
        constexpr int window_seconds = 60;
        std::lock_guard lock { mutex };
        auto now = std::chrono::steady_clock::now();
        for (auto it = attempts.begin(); it != attempts.end();) {
          if (now - it->second.second > std::chrono::seconds(window_seconds * 2)) {
            it = attempts.erase(it);
          }
          else {
            ++it;
          }
        }
      }
    };

    static crypto::cert_chain_t cert_chain;
    static std::unordered_map<std::string, pair_session_t> map_id_sess;
    static client_t client_root;
    static std::string last_pair_name;
    static std::mutex map_id_sess_mutex;
    static std::shared_mutex client_state_mutex;
    static std::recursive_mutex client_mutation_mutex;
    static pair_rate_limiter_t pair_rate_limit;

    using args_t = web::CaseInsensitiveMultimap;

    std::string
    get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
      auto it = args.find(name);
      if (it == std::end(args)) {
        if (default_value != nullptr) {
          return std::string(default_value);
        }

        throw std::out_of_range(name);
      }
      return it->second;
    }

    template <class T>
    constexpr std::string_view
    tunnel_name() {
      if constexpr (std::is_same_v<T, SunshineHTTPS>) {
        return "HTTPS"sv;
      }
      return "NONE"sv;
    }

    template <class T>
    void
    log_request(std::shared_ptr<typename web::ServerBase<T>::Request> request) {
      BOOST_LOG(debug) << "Request - Protocol: " << tunnel_name<T>()
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    bool
    save_state(bool preserve_existing = true, const client_t *snapshot = nullptr) {
      std::lock_guard mutation_lock { client_mutation_mutex };
      pt::ptree root;
      const auto state_path = state_file_path();

      if (preserve_existing) {
        try {
          if (fs::exists(state_path)) {
            std::ifstream state_file { state_path };
            if (!state_file.is_open()) {
              throw std::runtime_error("unable to open state file for reading");
            }
            pt::read_json(state_file, root);
          }
        }
        catch (std::exception &e) {
          BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
          return false;
        }
      }

      root.erase("root"s);

      root.put("root.uniqueid", http::unique_id);
      pt::ptree named_cert_nodes;
      const auto serialize_devices = [&](const client_t &state) {
        for (const auto &named_cert : state.named_devices) {
          pt::ptree named_cert_node;
          named_cert_node.put("name"s, named_cert.name);
          named_cert_node.put("cert"s, named_cert.cert);
          named_cert_node.put("uuid"s, named_cert.uuid);
          named_cert_node.put("enabled", named_cert.enabled);
          named_cert_node.put("last_session_ended"s, named_cert.last_session_ended);
          named_cert_node.put("family"s, client_family::to_string(named_cert.family.family));
          named_cert_node.put("family_version"s, named_cert.family.version);
          named_cert_node.put("family_os"s, named_cert.family.os);
          named_cert_node.put("family_source"s, named_cert.family.source);
          named_cert_node.put("family_last_seen"s, named_cert.family.last_seen);
          named_cert_node.put("family_rank"s, named_cert.family.rank);
          named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
        }
      };
      if (snapshot)
        serialize_devices(*snapshot);
      else {
        std::shared_lock<std::shared_mutex> cl(client_state_mutex);
        serialize_devices(client_root);
      }
      root.add_child("root.named_devices"s, named_cert_nodes);

      try {
        auto temporary_path = state_path;
        temporary_path += ".tmp";
        auto cleanup = util::fail_guard([&]() {
          std::error_code ignored;
          fs::remove(temporary_path, ignored);
        });
        std::ofstream state_file { temporary_path };
        if (!state_file.is_open()) {
          throw std::runtime_error("unable to open state file for writing");
        }
        pt::write_json(state_file, root);
        state_file.flush();
        if (!state_file) {
          throw std::runtime_error("unable to write state file");
        }
        state_file.close();
        if (!state_file) throw std::runtime_error("unable to close state file");
#ifdef _WIN32
        if (!MoveFileExW(temporary_path.c_str(), state_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          throw std::runtime_error("unable to replace state file");
        }
#else
        fs::rename(temporary_path, state_path);
#endif
      }
      catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
        return false;
      }
      return true;
    }

    void
    add_authorized_client(const std::string &name, std::string &&cert, client_family::record_t family) {
      std::lock_guard mutation_lock { client_mutation_mutex };
      named_cert_t named_cert;
      named_cert.name = name;
      named_cert.cert = std::move(cert);
      named_cert.uuid = uuid_util::uuid_t::generate().string();
      named_cert.family = std::move(family);
      {
        std::unique_lock<std::shared_mutex> ul(client_state_mutex);
        // A client that pairs again with the certificate it already has is the same device: it keeps its entry and its UUID
        // (the per-client settings and the session history are keyed by it) and takes the name and family of the new pairing.
        // Before this, every re-pairing added an entry with the same certificate, which no client could tell apart.
        const auto same_cert = std::find_if(client_root.named_devices.begin(), client_root.named_devices.end(), [&](const auto &existing) {
          return existing.cert == named_cert.cert;
        });
        if (same_cert != client_root.named_devices.end()) {
          BOOST_LOG(info) << "Client paired again with its certificate; keeping its entry [client="sv << masked_client_id(same_cert->uuid) << ']';
          same_cert->name = named_cert.name;
          if (named_cert.family.family != client_family::family_e::unknown) {
            same_cert->family = std::move(named_cert.family);
          }
        }
        else {
          client_root.named_devices.emplace_back(std::move(named_cert));
        }
      }

      if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
        save_state();
      }
    }

    template <class T>
    void
    pair_impl(std::shared_ptr<typename web::ServerBase<T>::Response> response, std::shared_ptr<typename web::ServerBase<T>::Request> request) {
      log_request<T>(request);

      auto client_ip = request->remote_endpoint().address().to_string();
      pair_rate_limit.cleanup();

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

      std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);

      args_t::const_iterator it;
      if (it = args.find("phrase"); it != std::end(args)) {
        if (it->second == "getservercert"sv) {
          if (!pair_rate_limit.check_and_record(client_ip)) {
            BOOST_LOG(warning) << "Pairing rate limited for IP: " << client_ip;
            tree.put("root.<xmlattr>.status_code", 429);
            tree.put("root.<xmlattr>.status_message", "Too many pairing attempts. Try again later.");
            tree.put("root.paired", 0);
            return;
          }

          if (pairing_pending::has_pending(uniqID)) {
            BOOST_LOG(warning) << "Rejecting duplicate pairing request with the same uniqueid";
            tree.put("root.<xmlattr>.status_code", 409);
            tree.put("root.<xmlattr>.status_message", "A pairing request with this uniqueid is already waiting for PIN entry");
            tree.put("root.paired", 0);
            return;
          }

          pair_session_t sess;

          sess.client.uniqueID = uniqID;
          sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
          // The Foundation extension sends clientname; standard Moonlight (macOS Enhanced among them) sends devicename.
          last_pair_name = client_family::pairing_device_name(get_arg(args, "clientname", ""), get_arg(args, "devicename", ""));
          if (last_pair_name.empty()) {
            last_pair_name = "Named Zako";
          }

          // Evidence is recorded, never trusted as identity. Headers go in
          // first so a long query string cannot crowd out the user agent.
          sess.client.family_observation = {};
          sess.client.family_observation.now = client_family::unix_now();
          for (const auto &[header_name, header_value] : request->header) {
            client_family::add_header(sess.client.family_observation, header_name, header_value);
          }
          for (const auto &[query_name, query_value] : args) {
            client_family::add_query(sess.client.family_observation, query_name, query_value);
          }

          BOOST_LOG(verbose) << "Client certificate bytes: " << sess.client.cert.size();
          auto ptr = map_id_sess.insert_or_assign(sess.client.uniqueID, std::move(sess)).first;

          ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
          if (config::sunshine.flags[config::flag::PIN_STDIN]) {
            std::string pin;

            std::cout << "Please insert pin: "sv;
            std::getline(std::cin, pin);

            if (!getservercert_checked(ptr->second, tree, pin, last_pair_name)) {
              return;
            }
            return;
          }
          else {
            auto remote_addr = request->remote_endpoint().address();
            auto nettype = net::from_address(remote_addr.to_string());
            auto preset = (nettype == net::net_e::PC || nettype == net::net_e::LAN) ? consume_preset_pin() : std::string {};
            if (!preset.empty()) {
              BOOST_LOG(info) << "Using preset PIN for QR code pairing with device_bytes="
                              << pairing_pending::sanitize_device_name(last_pair_name).size()
                              << " from " << remote_addr.to_string();
              ptr->second.client.name = last_pair_name;
              if (!getservercert_checked(ptr->second, tree, preset, last_pair_name)) {
                return;
              }
              return;
            }
            else {
              if (!pairing_pending::can_add(client_ip)) {
                BOOST_LOG(warning) << "Pairing pending request rate limited for IP: " << client_ip;
                tree.put("root.<xmlattr>.status_code", 429);
                tree.put("root.<xmlattr>.status_message", "Too many pending pairing requests. Try again later.");
                tree.put("root.paired", 0);
                remove_session(ptr->second);
                return;
              }

              pairing_pending::response_t pending_response = std::move(response);
              fg.disable();
              const auto add_result = pairing_pending::add(
                ptr->second.client.uniqueID,
                last_pair_name,
                client_ip,
                pending_response);
              if (add_result == pairing_pending::add_result_e::rate_limited) {
                pairing_pending::send_xml(
                  pending_response,
                  429,
                  "Too many pending pairing requests. Try again later.");
              }
              return;
            }
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
      }
      else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
        auto encrypted_response = util::from_hex_vec(it->second, true);
        serverchallengeresp(sess_it->second, tree, encrypted_response);
      }
      else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
        auto pairingsecret = util::from_hex_vec(it->second, true);
        clientpairingsecret(sess_it->second, tree, pairingsecret);
      }
      else {
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
      }
    }

  }  // namespace

  namespace pairing {

    void
    set_credentials(const std::string &pkey, const std::string &cert) {
      conf_intern.pkey = pkey;
      conf_intern.servercert = cert;
    }

    void
    load_state() {
      std::lock_guard mutation_lock { client_mutation_mutex };
      const auto state_path = state_file_path();
      pt::ptree tree;
      try {
        if (!fs::exists(state_path)) {
          BOOST_LOG(debug) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
          http::unique_id = uuid_util::uuid_t::generate().string();
          return;
        }
        std::ifstream state_file { state_path };
        if (!state_file.is_open()) {
          throw std::runtime_error("unable to open state file for reading");
        }
        pt::read_json(state_file, tree);
      }
      catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        http::unique_id = uuid_util::uuid_t::generate().string();
        {
          std::unique_lock<std::shared_mutex> ul(client_state_mutex);
          cert_chain.clear();
          client_root = client_t {};
        }
        save_state(false);
        return;
      }

      auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
      if (!unique_id_p) {
        http::unique_id = uuid_util::uuid_t::generate().string();
        return;
      }
      http::unique_id = std::move(*unique_id_p);

      auto root = tree.get_child("root");
      client_t client;

      if (root.get_child_optional("devices")) {
        auto device_nodes = root.get_child("devices");
        for (auto &[_, device_node] : device_nodes) {
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
          named_cert.enabled = el.get<bool>("enabled", true);
          named_cert.last_session_ended = el.get<std::int64_t>("last_session_ended", 0);
          named_cert.family.family = client_family::family_from_string(el.get<std::string>("family", "unknown"));
          named_cert.family.version = el.get<std::string>("family_version", "");
          named_cert.family.os = el.get<std::string>("family_os", "");
          named_cert.family.source = el.get<std::string>("family_source", "");
          named_cert.family.last_seen = el.get<std::int64_t>("family_last_seen", 0);
          named_cert.family.rank = el.get<std::uint8_t>("family_rank", 0);
          client.named_devices.emplace_back(named_cert);
        }
      }

      {
        std::unique_lock<std::shared_mutex> ul(client_state_mutex);
        cert_chain.clear();
        for (auto &named_cert : client.named_devices) {
          if (named_cert.enabled) cert_chain.add(crypto::x509(named_cert.cert));
        }
        client_root = std::move(client);
      }
    }

    std::string
    client_uuid_for_cert(const X509 *cert) {
      if (!cert) {
        return {};
      }

      std::shared_lock<std::shared_mutex> cl(client_state_mutex);
      for (const auto &named_cert : client_root.named_devices) {
        if (named_cert.enabled && crypto::x509_matches_pem(cert, named_cert.cert)) {
          return named_cert.uuid;
        }
      }
      return {};
    }

    const char *
    verify_client_certificate(X509 *cert, bool close_verify_safe) {
      std::shared_lock<std::shared_mutex> sl(client_state_mutex);
      // Relaxed certificate verification must never bypass an explicit denial.
      for (const auto &client : client_root.named_devices) {
        if (!client.enabled && crypto::x509_matches_pem(cert, client.cert)) {
          return "Client disabled";
        }
      }
      if (!close_verify_safe) {
        return cert_chain.verify_safe(cert);
      }
      return cert_chain.verify(cert);
    }

    void
    pair_https(resp_https_t response, req_https_t request) {
      pair_impl<SunshineHTTPS>(std::move(response), std::move(request));
    }

    void
    pair_http(resp_http_t response, req_http_t request) {
      pair_impl<web::HTTP>(std::move(response), std::move(request));
    }

  }  // namespace pairing

  void
  remove_session(const pair_session_t &sess) {
    const auto unique_id = sess.client.uniqueID;
    map_id_sess.erase(unique_id);
    pairing_pending::remove(unique_id);
  }

  void
  fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);
  }

  bool
  getservercert_checked(pair_session_t &sess, pt::ptree &tree, const std::string &pin, const std::string &client_name) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return false;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return false;
    }

    std::string_view salt_view { sess.async_insert_pin.salt.data(), 32 };

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.pairname", client_name);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
    return true;
  }

  void
  getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin, const std::string &client_name) {
    (void) getservercert_checked(sess, tree, pin, client_name);
  }

  void
  clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
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
    auto serverchallenge = crypto::rand(16);
    if (serversecret.size() != 16 || serverchallenge.size() != 16) {
      fail_pair(sess, tree, "Failed to generate pairing challenge");
      return;
    }

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({ (char *) decrypted.data(), decrypted.size() });

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

  void
  serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
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

  void
  clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
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

    std::string_view secret { client_pairing_secret.data(), 16 };
    std::string_view sign { client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size() };

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

    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);

      {
        std::unique_lock<std::shared_mutex> ul(client_state_mutex);
        cert_chain.add(crypto::x509(client.cert));
      }

      client.family_observation.now = client_family::unix_now();
      auto family = client_family::merge({}, client.family_observation);
      add_authorized_client(client.name, std::move(client.cert), std::move(family));
    }
    else {
      tree.put("root.paired", 0);
    }
    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void
  clientpairingsecret(pair_session_t &sess, std::shared_ptr<safe::queue_t<crypto::x509_t>> & /*add_cert*/, pt::ptree &tree, const std::string &client_pairing_secret) {
    clientpairingsecret(sess, tree, client_pairing_secret);
  }

  bool
  pin(std::string pin, std::string name) {
    return pin_for_pending_request({}, std::move(pin), std::move(name));
  }

  bool
  pin_for_pending_request(const std::string &request_id, std::string pin, std::string name) {
    if (pin.size() != 4) {
      BOOST_LOG(warning) << "Cannot apply PIN: expected 4 digits, got " << pin.size();
      return false;
    }
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      BOOST_LOG(warning) << "Cannot apply PIN: non-numeric value";
      return false;
    }

    std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);
    auto pending = pairing_pending::take(request_id);
    if (!pending) {
      BOOST_LOG(warning) << "Cannot apply PIN because the pending pairing session is no longer available";
      return false;
    }

    const auto session = map_id_sess.find(pending->unique_id);
    if (session == std::end(map_id_sess)) {
      pairing_pending::send_xml(pending->response, 400, "Pairing session is no longer available");
      return false;
    }

    pt::ptree tree;
    session->second.client.name = name;
    if (!getservercert_checked(session->second, tree, pin, name)) {
      std::ostringstream data;
      pt::write_xml(data, tree);
      pairing_pending::write_xml(pending->response, data.str());
      return false;
    }

    std::ostringstream data;
    pt::write_xml(data, tree);
    pairing_pending::write_xml(pending->response, data.str());
    BOOST_LOG(info) << "Pairing PIN accepted for pending request: id="
                    << pairing_pending::mask_for_log(pending->request_id);
    return true;
  }

  bool
  reject_pending_pair_request(const std::string &request_id) {
    return pairing_pending::reject(request_id);
  }

  pending_pair_snapshot_t
  get_pending_pair_requests() {
    return pairing_pending::snapshot();
  }

  bool
  wait_for_pending_pair_change(std::uint64_t generation, std::chrono::milliseconds timeout) {
    return pairing_pending::wait(generation, timeout);
  }

#ifdef SUNSHINE_TESTS
  void
  set_pending_pair_timeout_for_test(std::chrono::milliseconds timeout) {
    pairing_pending::set_timeout_for_test(timeout);
  }

  void
  reset_pairing_sessions_for_test() {
    std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);
    map_id_sess.clear();
    pairing_pending::reset_for_test();
    {
      std::lock_guard lock { pair_rate_limit.mutex };
      pair_rate_limit.attempts.clear();
    }
    tray_state::clear_pairing_required();
  }

  void
  add_paired_client_for_test(const std::string &name, std::string cert, client_family::record_t family) {
    add_authorized_client(name, std::move(cert), std::move(family));
  }
#endif

  nlohmann::json
  get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    std::shared_lock<std::shared_mutex> cl(client_state_mutex);
    for (auto &named_cert : client_root.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_node["enabled"] = named_cert.enabled;
      named_cert_node["lastSessionEnded"] = named_cert.last_session_ended;
      named_cert_node.update(client_family::record_json(named_cert.family));
      named_cert_nodes.push_back(named_cert_node);
    }
    return named_cert_nodes;
  }
  client_family::record_t
  observe_client_family(std::string_view client_uuid, const client_family::observation_t &observation) {
    client_family::record_t result;
    if (client_uuid.empty()) {
      return result;
    }

    bool changed = false;
    {
      std::lock_guard mutation_lock { client_mutation_mutex };
      {
        std::unique_lock<std::shared_mutex> ul(client_state_mutex);
        for (auto &named_cert : client_root.named_devices) {
          if (named_cert.uuid != client_uuid) {
            continue;
          }
          const auto previous = named_cert.family;
          named_cert.family = client_family::merge(previous, observation);
          result = named_cert.family;
          changed = result.family != previous.family ||
                    result.version != previous.version ||
                    result.os != previous.os ||
                    result.source != previous.source ||
                    result.last_seen != previous.last_seen;
          break;
        }
      }
      if (changed && !config::sunshine.flags[config::flag::FRESH_STATE]) {
        save_state();
      }
    }

    if (changed && result.family != client_family::family_e::unknown) {
      BOOST_LOG(info) << "Client family: "sv << client_family::to_string(result.family)
                      << " [os="sv << (result.os.empty() ? "unknown"sv : std::string_view { result.os })
                      << ", version="sv << (result.version.empty() ? "unknown"sv : std::string_view { result.version })
                      << ", evidence="sv << result.source
                      << ", client="sv << masked_client_id(client_uuid) << ']';
    }
    return result;
  }

  std::optional<paired_client_t>
  find_paired_client(std::string_view client_uuid) {
    if (client_uuid.empty()) {
      return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> cl(client_state_mutex);
    for (const auto &named_cert : client_root.named_devices) {
      if (named_cert.uuid == client_uuid) {
        return paired_client_t { named_cert.name, named_cert.family };
      }
    }
    return std::nullopt;
  }

  std::string
  get_pair_name() {
    std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);
    return last_pair_name;
  }

  void
  erase_all_clients() {
    std::lock_guard mutation_lock { client_mutation_mutex };
    {
      std::unique_lock<std::shared_mutex> ul(client_state_mutex);
      client_root = client_t {};
      cert_chain.clear();
    }
    save_state();
  }

  int
  unpair_client(std::string uuid) {
    std::lock_guard mutation_lock { client_mutation_mutex };
    bool removed = false;
    {
      std::unique_lock<std::shared_mutex> ul(client_state_mutex);
      auto &devices = client_root.named_devices;
      for (auto it = devices.begin(); it != devices.end();) {
        if (it->uuid == uuid) {
          it = devices.erase(it);
          removed = true;
        }
        else {
          ++it;
        }
      }
    }

    save_state();
    pairing::load_state();
    return removed;
  }

  bool
  is_client_enabled(const std::string &uuid) {
    std::shared_lock lock { client_state_mutex };
    return std::any_of(client_root.named_devices.begin(), client_root.named_devices.end(), [&](const auto &client) {
      return client.uuid == uuid && client.enabled;
    });
  }

  bool
  set_client_enabled(const std::string &uuid, bool enabled) {
    std::lock_guard mutation_lock { client_mutation_mutex };
    std::unique_lock lock { client_state_mutex };
    auto found = std::find_if(client_root.named_devices.begin(), client_root.named_devices.end(), [&](const auto &client) {
      return client.uuid == uuid;
    });
    if (found == client_root.named_devices.end()) return false;
    const bool previous = found->enabled;
    found->enabled = enabled;
    // Readers must not observe a permission change until it is persisted.
    if (!config::sunshine.flags[config::flag::FRESH_STATE] && !save_state(true, &client_root)) {
      found->enabled = previous;
      return false;
    }
    cert_chain.clear();
    for (const auto &client : client_root.named_devices) {
      if (client.enabled) cert_chain.add(crypto::x509(client.cert));
    }
    return true;
  }

  bool
  rename_client(const std::string &uuid, const std::string &new_name) {
    std::lock_guard mutation_lock { client_mutation_mutex };
    bool renamed = false;
    {
      std::unique_lock<std::shared_mutex> ul(client_state_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert.uuid == uuid) {
          named_cert.name = new_name;
          renamed = true;
          break;
        }
      }
    }
    if (renamed) {
      save_state();
    }
    return renamed;
  }

  bool
  touch_client_last_session(const std::string &uuid, std::int64_t timestamp_ms) {
    if (uuid.empty() || timestamp_ms <= 0) {
      return false;
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
                          .count();
    // A mark left by a run with a fast clock sits far beyond the corrected
    // host clock; accept a fresh end instead of rejecting every session
    // until real time catches up. One hour absorbs ordinary skew.
    if (timestamp_ms > now_ms) {
      timestamp_ms = now_ms;
    }
    std::lock_guard mutation_lock { client_mutation_mutex };
    bool touched = false;
    {
      std::unique_lock<std::shared_mutex> ul(client_state_mutex);
      for (auto &named_cert : client_root.named_devices) {
        if (named_cert.uuid == uuid) {
          if (named_cert.last_session_ended > now_ms + 3600000 ||
              timestamp_ms > named_cert.last_session_ended) {
            named_cert.last_session_ended = timestamp_ms;
            touched = true;
          }
          break;
        }
      }
    }
    if (!touched) {
      return false;
    }
    if (config::sunshine.flags[config::flag::FRESH_STATE]) {
      // Fresh state never persists; like add_authorized_client, the in-memory
      // mark is all this run keeps.
      return true;
    }
    return save_state();
  }

}  // namespace nvhttp
