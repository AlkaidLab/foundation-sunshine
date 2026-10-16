/**
 * @file src/confighttp/clients.cpp
 * @brief Web UI clients handlers and routes.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include "clients.h"
#include "src/config.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/crypto.h"
#include "src/display_device/color_profile.h"
#include "src/hdr/client_display_capabilities.h"
#include "src/logging.h"
#include "src/network.h"
#include "src/nvhttp.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/utility.h"
#include "src/webhook/webhook.h"
#include <boost/property_tree/json_parser.hpp>
#include <cmath>
#include <filesystem>
#include <set>
#include <sstream>
#ifdef _WIN32
  #include "src/platform/windows/display_device/color_profile.h"
  #include <iphlpapi.h>
#endif

using namespace std::literals;
using json = nlohmann::json;

namespace confighttp::clients {
  namespace pt = boost::property_tree;

  using namespace confighttp;

  namespace {
    void
    saveClients(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      nlohmann::json output {
        { "status", false },
      };

      try {
        std::stringstream body;
        body << request->content.rdbuf();
        const auto input = nlohmann::json::parse(body.str());
        if (!input.is_object()) {
          throw std::invalid_argument("clients must be a serialized JSON array");
        }
        const auto clients_it = input.find("clients");
        if (clients_it == input.end() || !clients_it->is_string()) {
          throw std::invalid_argument("clients must be a serialized JSON array");
        }

        const auto serialized_clients = clients_it->get<std::string>();
        constexpr std::size_t max_clients_config_size { 1024 * 1024 };
        if (serialized_clients.size() > max_clients_config_size) {
          throw std::invalid_argument("clients configuration is too large");
        }

        const auto clients = nlohmann::json::parse(serialized_clients);
        if (!clients.is_array()) {
          throw std::invalid_argument("clients must contain a JSON array");
        }
        if (clients.size() > 1024) {
          throw std::invalid_argument("too many client settings");
        }

        std::set<std::string> uuids;
        for (const auto &client : clients) {
          if (!client.is_object()) {
            throw std::invalid_argument("each client setting must be an object");
          }

          if (const auto uuid_it = client.find("uuid"); uuid_it != client.end()) {
            if (!uuid_it->is_string() || uuid_it->get_ref<const std::string &>().size() > 128) {
              throw std::invalid_argument("client uuid must be a string of at most 128 bytes");
            }
            const auto &uuid = uuid_it->get_ref<const std::string &>();
            if (!uuid.empty() && !uuids.insert(uuid).second) {
              throw std::invalid_argument("client uuid must be unique");
            }
          }

          if (const auto name_it = client.find("name"); name_it != client.end() &&
                                                        (!name_it->is_string() || name_it->get_ref<const std::string &>().size() > 256)) {
            throw std::invalid_argument("client name must be a string of at most 256 bytes");
          }

          if (const auto profile_it = client.find("hdrProfile"); profile_it != client.end()) {
            if (!profile_it->is_string()) {
              throw std::invalid_argument("HDR profile must be a string");
            }
            const auto &profile = profile_it->get_ref<const std::string &>();
            if (!profile.empty() && !display_device::color_profile::is_valid_profile_basename(profile)) {
              throw std::invalid_argument("HDR profile must be an installed .icc or .icm filename");
            }
          }

          const auto brightness_mode = client.value("hdrBrightnessMode", "auto");
          if (brightness_mode != "auto" && brightness_mode != "manual") {
            throw std::invalid_argument("HDR brightness mode must be auto or manual");
          }
          if (brightness_mode == "manual") {
            const auto read_brightness = [&](const char *field) -> float {
              const auto value = client.find(field);
              if (value == client.end() || !value->is_number()) {
                throw std::invalid_argument(std::string { "manual HDR brightness requires numeric " } + field);
              }
              const float number = value->get<float>();
              if (!std::isfinite(number)) {
                throw std::invalid_argument(std::string { field } + " must be finite");
              }
              return number;
            };

            const auto validated = hdr::validate_client_display_capabilities(
              read_brightness("hdrBrightnessMaxNits"),
              read_brightness("hdrBrightnessMinNits"),
              read_brightness("hdrBrightnessMaxFullFrameNits"));
            if (!validated.capabilities.reported) {
              throw std::invalid_argument(validated.fallback_reason);
            }
          }
        }

        if (!config::save_clients_config(serialized_clients)) {
          output["error"] = "failed to persist client settings";
        }
        else {
          output["status"] = true;
        }
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "SaveClients: "sv << e.what();
        output["error"] = e.what();
      }

      send_response(response, output);
    }

    void
    savePin(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      std::stringstream ss;
      ss << request->content.rdbuf();

      pt::ptree inputTree, outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      try {
        pt::read_json(ss, inputTree);
        std::string pin = inputTree.get<std::string>("pin");
        std::string name = inputTree.get<std::string>("name");

        // Validate PIN: must be numeric digits only, 4-8 characters
        if (pin.size() < 4 || pin.size() > 8 || !std::all_of(pin.begin(), pin.end(), ::isdigit)) {
          outputTree.put("status", false);
          outputTree.put("error", "PIN must be 4-8 digits");
          return;
        }
        // Validate client name
        if (name.empty() || name.size() > 256) {
          outputTree.put("status", false);
          outputTree.put("error", "Client name must be 1-256 characters");
          return;
        }

        bool pin_result = nvhttp::pin(pin, name);
        outputTree.put("status", pin_result);

        try {
          webhook::send_event_async(webhook::event_t {
            .type = pin_result ? webhook::event_type_t::CONFIG_PIN_SUCCESS : webhook::event_type_t::CONFIG_PIN_FAILED,
            .timestamp = webhook::get_current_timestamp(),
            .client_name = name,
            .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
            .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()) });
        }
        catch (...) {
          BOOST_LOG(error) << "Webhook pairing event construction failed"sv;
        }
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "SavePin: "sv << e.what();
        outputTree.put("status", false);
        outputTree.put("error", e.what());

        try {
          webhook::send_event_async(webhook::event_t {
            .type = webhook::event_type_t::CONFIG_PIN_FAILED,
            .timestamp = webhook::get_current_timestamp(),
            .client_ip = net::addr_to_normalized_string(request->remote_endpoint().address()),
            .server_ip = net::addr_to_normalized_string(request->local_endpoint().address()),
            .extra_data = { { "error", "invalid_request" } } });
        }
        catch (...) {
          BOOST_LOG(error) << "Webhook pairing failure event construction failed"sv;
        }
        return;
      }
    }

    void
    getQrPairStatus(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      nlohmann::json j;
      j["status"] = nvhttp::get_qr_pair_status();

      std::string content = j.dump();
      web::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(web::StatusCode::ok, content, headers);
    }

    void
    generateQrPairInfo(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      pt::ptree outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      // Generate a random 4-digit PIN using OpenSSL CSPRNG
      const auto random_bytes = crypto::rand(sizeof(std::uint16_t));
      if (random_bytes.size() != sizeof(std::uint16_t)) {
        outputTree.put("status", false);
        outputTree.put("error", "Failed to generate pairing PIN");
        return;
      }
      std::uint16_t random_val;
      std::memcpy(&random_val, random_bytes.data(), sizeof(random_val));
      int pin_num = random_val % 10000;
      char pin_buf[5];
      std::snprintf(pin_buf, sizeof(pin_buf), "%04d", pin_num);
      std::string pin(pin_buf);

      // Set the preset PIN in nvhttp (valid for 120 seconds)
      std::string server_name = config::nvhttp.sunshine_name;
      if (!nvhttp::set_preset_pin(pin, server_name, 120)) {
        outputTree.put("status", false);
        outputTree.put("error", "Failed to set preset PIN");
        return;
      }

      // Get server address info
      auto local_addr = net::addr_to_normalized_string(request->local_endpoint().address());
      auto port = net::map_port(nvhttp::PORT_HTTP);

      // Determine the host address for the QR code URL
      std::string host;
      if (!config::nvhttp.external_ip.empty()) {
        // User explicitly configured an external IP (could be WAN for port-forwarded setups)
        host = config::nvhttp.external_ip;
      }
      else {
        // Detect a usable LAN IP (local_endpoint may be loopback or VPN)
        host = local_addr;
        auto host_net_type = net::from_address(host);
        if (host_net_type != net::LAN) {
          std::string resolved_host;

          // Method 1: UDP connect trick to find default outgoing interface
          try {
            boost::asio::io_context io_ctx;
            boost::asio::ip::udp::socket socket(io_ctx);
            socket.connect(boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("8.8.8.8"), 53));
            auto lan_addr = socket.local_endpoint().address();
            socket.close();
            auto candidate = net::addr_to_normalized_string(lan_addr);
            if (net::from_address(candidate) == net::LAN) {
              resolved_host = candidate;
            }
          }
          catch (...) {
          }

#ifdef _WIN32
          // Method 2 (Windows): enumerate adapters via GetAdaptersAddresses
          if (resolved_host.empty()) {
            ULONG bufLen = 15000;
            std::vector<uint8_t> buf(bufLen);
            auto pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
            if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, pAddresses, &bufLen) == NO_ERROR) {
              for (auto adapter = pAddresses; adapter; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp) continue;
                if (adapter->IfType == IF_TYPE_TUNNEL || adapter->IfType == IF_TYPE_PPP) continue;
                for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                  if (unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
                  auto sin = reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr);
                  boost::asio::ip::address_v4 addr(ntohl(sin->sin_addr.s_addr));
                  auto candidate = addr.to_string();
                  if (net::from_address(candidate) == net::LAN) {
                    resolved_host = candidate;
                    break;
                  }
                }
                if (!resolved_host.empty()) break;
              }
            }
          }
#endif

          if (!resolved_host.empty()) {
            host = resolved_host;
            BOOST_LOG(info) << "QR pair: resolved to LAN IP " << host;
          }
          else {
            BOOST_LOG(warning) << "QR pair: could not find a LAN IP, using " << host;
          }
        }
      }

      // Build the moonlight:// URL
      std::string url = "moonlight://pair?host=" + host +
                        "&port=" + std::to_string(port) +
                        "&pin=" + pin +
                        "&name=" + server_name;

      outputTree.put("status", true);
      outputTree.put("pin", pin);
      outputTree.put("host", host);
      outputTree.put("port", port);
      outputTree.put("name", server_name);
      outputTree.put("url", url);
      outputTree.put("expires_in", 120);
    }

    void
    cancelQrPair(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      nvhttp::clear_preset_pin();

      pt::ptree outputTree;
      outputTree.put("status", true);

      std::ostringstream data;
      pt::write_json(data, outputTree);
      response->write(data.str());
    }

    void
    unpairAll(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);

      pt::ptree outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });
      nvhttp::erase_all_clients();
      proc::proc.terminate();
      outputTree.put("status", true);
    }

    void
    unpair(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      std::stringstream ss;
      ss << request->content.rdbuf();

      pt::ptree inputTree, outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      try {
        pt::read_json(ss, inputTree);
        std::string uuid = inputTree.get<std::string>("uuid");

        // Validate UUID format (hex + hyphens, reasonable length)
        if (uuid.empty() || uuid.size() > 64) {
          outputTree.put("status", false);
          outputTree.put("error", "Invalid client UUID");
          return;
        }

        const bool removed = nvhttp::unpair_client(uuid);
        outputTree.put("status", removed);
        if (removed) rtsp_stream::terminate_client_sessions(uuid);

        if (removed && nvhttp::get_all_clients().empty()) {
          proc::proc.terminate();
        }
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "Unpair: "sv << e.what();
        outputTree.put("status", false);
        outputTree.put("error", e.what());
        return;
      }
    }

    void
    updateClient(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;
      json output { { "status", false } };
      try {
        const auto input = json::parse(request->content.string());
        if (!input.is_object() || !input.contains("uuid") || !input["uuid"].is_string() ||
            !input.contains("enabled") || !input["enabled"].is_boolean()) {
          throw std::invalid_argument("uuid and a boolean enabled value are required");
        }
        const auto uuid = input["uuid"].get<std::string>();
        if (uuid.empty() || uuid.size() > 64) throw std::invalid_argument("Invalid client UUID");
        const bool enabled = input["enabled"].get<bool>();
        if (!nvhttp::set_client_enabled(uuid, enabled)) {
          output["error"] = "Client not found or permission could not be saved";
        }
        else {
          if (!enabled) rtsp_stream::terminate_client_sessions(uuid);
          output["status"] = true;
          output["enabled"] = enabled;
        }
      }
      catch (const std::exception &error) {
        output["error"] = error.what();
      }
      send_response(response, output);
    }

    void
    renameClient(resp_https_t response, req_https_t request) {
      if (!check_content_type(response, request, "application/json")) return;
      if (!authenticate(response, request)) return;

      print_req(request);

      pt::ptree inputTree, outputTree;

      auto g = util::fail_guard([&]() {
        std::ostringstream data;
        pt::write_json(data, outputTree);
        response->write(data.str());
      });

      try {
        std::stringstream ss;
        ss << request->content.rdbuf();
        pt::read_json(ss, inputTree);

        std::string uuid = inputTree.get<std::string>("uuid");
        std::string new_name = inputTree.get<std::string>("name");

        if (new_name.empty()) {
          outputTree.put("status", false);
          outputTree.put("error", "Name cannot be empty");
          return;
        }

        bool result = nvhttp::rename_client(uuid, new_name);
        outputTree.put("status", result);
        if (!result) {
          outputTree.put("error", "Client not found");
        }
      }
      catch (std::exception &e) {
        BOOST_LOG(warning) << "Rename client: "sv << e.what();
        outputTree.put("status", false);
        outputTree.put("error", e.what());
      }
    }

    void
    listClients(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      print_req(request);
      nlohmann::json named_certs = nvhttp::get_all_clients();
      const auto sessions = stream::session::get_all_sessions_info();
      for (auto &client : named_certs) {
        if (!client.is_object()) continue;
        const auto uuid = client.value("uuid", std::string {});
        const auto name = client.value("name", std::string {});
        client["streaming"] = false;
        const auto session = std::find_if(sessions.begin(), sessions.end(), [&](const auto &candidate) {
          return (!uuid.empty() && candidate.client_uuid == uuid) ||
                 (uuid.empty() && !name.empty() && candidate.client_name == name);
        });
        if (session == sessions.end()) continue;

        client["streaming"] = true;

        client["hdrBrightnessRuntime"] = {
          { "active", true },
          { "reported", session->hdr_brightness_reported },
          { "source", session->hdr_brightness_source },
          { "maxNits", session->hdr_max_nits },
          { "minNits", session->hdr_min_nits },
          { "maxFullFrameNits", session->hdr_max_full_frame_nits },
        };
      }
      nlohmann::json output_tree;
      output_tree["named_certs"] = named_certs;
      output_tree["status"] = "true";
      send_response(response, output_tree);
    }

    void
    listColorProfiles(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

#ifdef _WIN32
      const auto windows_directory = []() -> std::filesystem::path {
        if (const char *windir = std::getenv("WINDIR"); windir && *windir) {
          return windir;
        }
        return R"(C:\Windows)";
      }();
      const auto color_directory = windows_directory / "System32" / "spool" / "drivers" / "color";

      std::error_code error;
      std::vector<std::string> profiles;
      std::filesystem::directory_iterator entry { color_directory, std::filesystem::directory_options::skip_permission_denied, error };
      const std::filesystem::directory_iterator end;
      while (!error && entry != end) {
        std::error_code entry_error;
        if (entry->is_regular_file(entry_error) && !entry_error) {
          const auto utf8_name = entry->path().filename().u8string();
          const std::string profile_name {
            reinterpret_cast<const char *>(utf8_name.data()),
            utf8_name.size()
          };
          if (display_device::color_profile::is_valid_profile_basename(profile_name)) {
            profiles.push_back(profile_name);
          }
        }
        entry.increment(error);
      }

      if (error) {
        send_response(response, nlohmann::json {
                                  { "status", false },
                                  { "supported", true },
                                  { "profiles", nlohmann::json::array() },
                                  { "error", "failed to enumerate installed color profiles" },
                                });
        return;
      }

      std::sort(profiles.begin(), profiles.end());
      send_response(response, nlohmann::json {
                                { "status", true },
                                { "supported", true },
                                { "profiles", profiles },
                              });
#else
      send_response(response, nlohmann::json {
                                { "status", true },
                                { "supported", false },
                                { "profiles", nlohmann::json::array() },
                              });
#endif
    }

  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/qr-pair$"]["POST"] = generateQrPairInfo;
    server.resource["^/api/qr-pair/cancel$"]["POST"] = cancelQrPair;
    server.resource["^/api/qr-pair$"]["GET"] = getQrPairStatus;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = listClients;
    server.resource["^/api/clients/list$"]["POST"] = saveClients;
    server.resource["^/api/color-profiles$"]["GET"] = listColorProfiles;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/rename$"]["POST"] = renameClient;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
  }

}  // namespace confighttp::clients
