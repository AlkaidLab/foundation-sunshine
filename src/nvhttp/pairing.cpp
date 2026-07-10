#include "pairing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/crypto.h"
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

    struct named_cert_t {
      std::string name;
      std::string uuid;
      std::string cert;
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
    static std::string pending_pin_unique_id;
    static std::mutex map_id_sess_mutex;
    static std::shared_mutex client_state_mutex;
    static pair_rate_limiter_t pair_rate_limit;
    constexpr auto pending_pin_timeout = 5min;

    static struct {
      std::string pin;
      std::string name;
      std::chrono::steady_clock::time_point expires_at;
      bool paired = false;
      std::mutex mutex;
    } preset_pin_state;

    using args_t = SimpleWeb::CaseInsensitiveMultimap;

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
    log_request(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
      BOOST_LOG(debug) << "Request - Protocol: " << tunnel_name<T>()
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    void
    save_state(bool preserve_existing = true) {
      pt::ptree root;

      if (preserve_existing && fs::exists(config::nvhttp.file_state)) {
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
      pt::ptree named_cert_nodes;
      {
        std::shared_lock<std::shared_mutex> cl(client_state_mutex);
        for (auto &named_cert : client_root.named_devices) {
          pt::ptree named_cert_node;
          named_cert_node.put("name"s, named_cert.name);
          named_cert_node.put("cert"s, named_cert.cert);
          named_cert_node.put("uuid"s, named_cert.uuid);
          named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
        }
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
    add_authorized_client(const std::string &name, std::string &&cert) {
      named_cert_t named_cert;
      named_cert.name = name;
      named_cert.cert = std::move(cert);
      named_cert.uuid = uuid_util::uuid_t::generate().string();
      {
        std::unique_lock<std::shared_mutex> ul(client_state_mutex);
        client_root.named_devices.emplace_back(std::move(named_cert));
      }

      if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
        save_state();
      }
    }

    template <class T>
    void
    pair_impl(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
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

          pair_session_t sess;

          sess.client.uniqueID = uniqID;
          sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);
          last_pair_name = get_arg(args, "clientname", "Named Zako");

          BOOST_LOG(verbose) << "Client cert: " << sess.client.cert.substr(0, 100) << "...";
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
              BOOST_LOG(info) << "Using preset PIN for QR code pairing with " << last_pair_name
                              << " from " << remote_addr.to_string();
              ptr->second.client.name = last_pair_name;
              if (!getservercert_checked(ptr->second, tree, preset, last_pair_name)) {
                return;
              }
              return;
            }
            else {
              if (!pending_pin_unique_id.empty() && map_id_sess.find(pending_pin_unique_id) != std::end(map_id_sess)) {
                BOOST_LOG(warning) << "Rejecting pairing request while another client is waiting for PIN entry";
                tree.put("root.<xmlattr>.status_code", 409);
                tree.put("root.<xmlattr>.status_message", "Another pairing request is waiting for PIN entry");
                tree.put("root.paired", 0);
                remove_session(ptr->second);
                return;
              }
              tray_state::set_pairing_required(last_pair_name);
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
              system_tray::update_tray_require_pin(last_pair_name);
#endif
              pending_pin_unique_id = ptr->second.client.uniqueID;
              ptr->second.async_insert_pin.response = std::move(response);

              const auto pending_id = pending_pin_unique_id;
              task_pool.pushDelayed([pending_id]() {
                std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);
                if (pending_pin_unique_id != pending_id) {
                  return;
                }

                const auto session = map_id_sess.find(pending_id);
                if (session == std::end(map_id_sess)) {
                  pending_pin_unique_id.clear();
                  tray_state::clear_pairing_required();
                  return;
                }

                pt::ptree timeout_tree;
                timeout_tree.put("root.paired", 0);
                timeout_tree.put("root.<xmlattr>.status_code", 408);
                timeout_tree.put("root.<xmlattr>.status_message", "Pairing PIN entry timed out");
                std::ostringstream data;
                pt::write_xml(data, timeout_tree);

                auto &async_response = session->second.async_insert_pin.response;
                if (async_response.has_left() && async_response.left()) {
                  async_response.left()->write(data.str());
                }
                else if (async_response.has_right() && async_response.right()) {
                  async_response.right()->write(data.str());
                }

                BOOST_LOG(info) << "Pairing request timed out while waiting for PIN entry";
                remove_session(session->second);
              }, pending_pin_timeout);

              fg.disable();
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
          client.named_devices.emplace_back(named_cert);
        }
      }

      {
        std::unique_lock<std::shared_mutex> ul(client_state_mutex);
        cert_chain.clear();
        for (auto &named_cert : client.named_devices) {
          cert_chain.add(crypto::x509(named_cert.cert));
        }
        client_root = std::move(client);
      }
    }

    std::string
    client_uuid_for_cert(const std::string &cert) {
      std::shared_lock<std::shared_mutex> cl(client_state_mutex);
      for (const auto &named_cert : client_root.named_devices) {
        if (named_cert.cert == cert) {
          return named_cert.uuid;
        }
      }
      return {};
    }

    const char *
    verify_client_certificate(X509 *cert, bool close_verify_safe) {
      std::shared_lock<std::shared_mutex> sl(client_state_mutex);
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
      pair_impl<SimpleWeb::HTTP>(std::move(response), std::move(request));
    }

  }  // namespace pairing

  void
  remove_session(const pair_session_t &sess) {
    if (pending_pin_unique_id == sess.client.uniqueID) {
      pending_pin_unique_id.clear();
      tray_state::clear_pairing_required();
    }
    map_id_sess.erase(sess.client.uniqueID);
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

      add_authorized_client(client.name, std::move(client.cert));
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
    pt::ptree tree;
    std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);
    if (map_id_sess.empty()) {
      return false;
    }
    auto sess_it = map_id_sess.find(pending_pin_unique_id);
    if (sess_it == std::end(map_id_sess)) {
      BOOST_LOG(warning) << "Cannot apply PIN because the pending pairing session is no longer available";
      return false;
    }

    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be 4 digits, " + std::to_string(pin.size()) + " provided");
      return false;
    }

    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = sess_it->second;
    sess.client.name = name;
    if (!getservercert_checked(sess, tree, pin, name)) {
      return false;
    }

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

    async_response = std::decay_t<decltype(async_response.left())>();
    pending_pin_unique_id.clear();
    tray_state::clear_pairing_required();
    return true;
  }

  nlohmann::json
  get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    std::shared_lock<std::shared_mutex> cl(client_state_mutex);
    for (auto &named_cert : client_root.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert.name;
      named_cert_node["uuid"] = named_cert.uuid;
      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  std::string
  get_pair_name() {
    std::lock_guard<std::mutex> map_lock(map_id_sess_mutex);
    return last_pair_name;
  }

  bool
  set_preset_pin(const std::string &pin, const std::string &name, int timeout_seconds) {
    if (pin.size() != 4 || !std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      BOOST_LOG(warning) << "Invalid preset PIN: must be 4 digits";
      return false;
    }

    std::lock_guard lock { preset_pin_state.mutex };
    preset_pin_state.pin = pin;
    preset_pin_state.name = name;
    preset_pin_state.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    preset_pin_state.paired = false;
    BOOST_LOG(info) << "Preset PIN set for QR pairing, expires in " << timeout_seconds << "s";
    return true;
  }

  std::string
  consume_preset_pin() {
    std::lock_guard lock { preset_pin_state.mutex };
    if (preset_pin_state.pin.empty()) {
      return std::string {};
    }
    if (std::chrono::steady_clock::now() > preset_pin_state.expires_at) {
      preset_pin_state.pin.clear();
      preset_pin_state.name.clear();
      return std::string {};
    }
    std::string pin = std::move(preset_pin_state.pin);
    preset_pin_state.pin.clear();
    preset_pin_state.name.clear();
    return pin;
  }

  void
  clear_preset_pin() {
    std::lock_guard lock { preset_pin_state.mutex };
    preset_pin_state.pin.clear();
    preset_pin_state.name.clear();
    preset_pin_state.paired = true;
  }

  std::string
  get_qr_pair_status() {
    std::lock_guard lock { preset_pin_state.mutex };
    if (preset_pin_state.paired) return "paired";
    if (preset_pin_state.pin.empty()) return "inactive";
    if (std::chrono::steady_clock::now() > preset_pin_state.expires_at) {
      preset_pin_state.pin.clear();
      preset_pin_state.name.clear();
      return "expired";
    }
    return "active";
  }

  void
  erase_all_clients() {
    {
      std::unique_lock<std::shared_mutex> ul(client_state_mutex);
      client_root = client_t {};
      cert_chain.clear();
    }
    save_state();
  }

  int
  unpair_client(std::string uuid) {
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
  rename_client(const std::string &uuid, const std::string &new_name) {
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

}  // namespace nvhttp
