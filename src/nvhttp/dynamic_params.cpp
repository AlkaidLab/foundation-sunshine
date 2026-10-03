#include "dynamic_params.h"
#include "legacy_control.h"

#include <sstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <Simple-Web-Server/server_http.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include "src/logging.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/transport/transport_policy_json.h"
#include "src/utility.h"
#include "src/video.h"

using namespace std::literals;

namespace nvhttp::dynamic_params {

  static void
  transport_request(resp_https_t response, req_https_t request, bool automatic_control) {
    using json = nlohmann::json;
    const SimpleWeb::CaseInsensitiveMultimap headers {{"Content-Type", "application/json"}, {"Cache-Control", "no-store"}};
    const auto write = [&](SimpleWeb::StatusCode code, json body) {
      response->write(code, body.dump(), headers);
      response->close_connection_after_response = true;
    };
    const auto identity = get_client_cert_uuid_from_request(request);
    // The TLS identity cache does not by itself prove that pairing is still
    // valid after revocation. Recheck it before each session policy operation.
    bool paired = false;
    for (const auto &client : nvhttp::get_all_clients()) {
      if (client.contains("uuid") && client.at("uuid").is_string() && client.at("uuid").get<std::string>() == identity) paired = true;
    }
    if (identity.empty() || !paired) {
      write(SimpleWeb::StatusCode::client_error_forbidden, {{"error", "paired_identity_required"}});
      return;
    }
    try {
      if (request->method == "GET") {
        const auto args = request->parse_query_string();
        if (args.count("sessionId") != 1 || args.count("connectionEpoch") > 1) throw std::invalid_argument("Invalid session query");
        const auto session_id = transport::parse_policy_identity(args.find("sessionId")->second);
        if (session_id > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("Invalid session identity");
        std::optional<uint64_t> epoch;
        if (const auto found = args.find("connectionEpoch"); found != args.end()) epoch = transport::parse_policy_identity(found->second);
        const auto state = stream::session::get_transport_policy(identity, static_cast<uint32_t>(session_id), epoch);
        if (!state) {
          write(SimpleWeb::StatusCode::client_error_not_found, {{"error", "session_not_found"}});
          return;
        }
        const auto snapshot = state->snapshot();
        auto status = transport::policy_status_json(snapshot, static_cast<uint32_t>(session_id));
        const auto statistics = stream::session::get_transport_network_statistics(identity,
          static_cast<uint32_t>(session_id), snapshot.accepted->connection_epoch);
        status["networkStatistics"] = statistics ? transport::network_statistics_json(*statistics) : json(nullptr);
        write(SimpleWeb::StatusCode::success_ok, std::move(status));
        return;
      }
      if (request->content.size() > 8192) throw std::invalid_argument("Policy body exceeds limit");
      const auto submit = [&](const auto &update, const auto &queue) {
        const auto state = stream::session::get_transport_policy(identity, update.session_id, update.connection_epoch);
        if (!state) {
          write(SimpleWeb::StatusCode::client_error_not_found, { { "error", "session_not_found" } });
          return;
        }
        const auto result = queue(identity, update);
        auto status = transport::policy_status_json(state->snapshot(), update.session_id);
        if (result.result == transport::policy_request_result_e::accepted) {
          status["requestRevision"] = std::to_string(result.policy->revision);
          status["requestId"] = update.request_id;
          write(SimpleWeb::StatusCode::success_accepted, std::move(status));
        }
        else {
          status["error"] = result.result == transport::policy_request_result_e::conflict ? "revision_or_request_conflict" :
                            result.result == transport::policy_request_result_e::stopped  ? "session_stopped" :
                                                                                            "invalid_policy";
          write(result.result == transport::policy_request_result_e::invalid ? SimpleWeb::StatusCode::client_error_bad_request :
                                                                               SimpleWeb::StatusCode::client_error_conflict,
            std::move(status));
        }
      };
      if (automatic_control)
        submit(transport::parse_control_update(request->content.string()), stream::session::queue_transport_control);
      else
        submit(transport::parse_policy_update(request->content.string()), stream::session::queue_transport_policy);
    }
    catch (const std::invalid_argument &error) {
      write(SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_request"}, {"reason", error.what()}});
    }
    catch (const json::exception &) {
      write(SimpleWeb::StatusCode::client_error_bad_request, {{"error", "invalid_json"}});
    }
  }

  void
  transport_policy(resp_https_t response, req_https_t request) {
    transport_request(std::move(response), std::move(request), false);
  }

  void
  transport_control(resp_https_t response, req_https_t request) {
    transport_request(std::move(response), std::move(request), true);
  }

  namespace {

    namespace pt = boost::property_tree;

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

    void
    write_tree(resp_https_t response, const pt::ptree &tree) {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    }

    void
    set_error(pt::ptree &tree, int code, const std::string &message) {
      tree.put("root.success", 0);
      tree.put("root.<xmlattr>.status_code", code);
      tree.put("root.<xmlattr>.status_message", message);
    }

  }  // namespace

  void
  change_bitrate(resp_https_t response, req_https_t request) {
    log_request(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      write_tree(response, tree);
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

      if (args.count("bitrate") != 1 || args.count("clientname") != 1) throw std::invalid_argument("Duplicate bitrate query");
      const auto bitrate_value = transport::parse_policy_identity(bitrate_param->second);
      if (!bitrate_value || bitrate_value > 800000) throw std::invalid_argument("Invalid bitrate");
      int bitrate = static_cast<int>(bitrate_value);
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

      const auto target = legacy_control::resolve(request, legacy_control::query_scope(args), client_name);
      bool success = legacy_control::submit(target, param);

      if (success) {
        tree.put("root.bitrate", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        tree.put("root.<xmlattr>.bitrate", bitrate);
        tree.put("root.<xmlattr>.clientname", client_name);
        tree.put("root.<xmlattr>.status_message", "Bitrate change request sent to client session");
        BOOST_LOG(info) << "NVHTTP API: Dynamic bitrate change requested for client '"
                        << client_name << "': " << bitrate << " Kbps";
      }
      else {
        tree.put("root.bitrate", 0);
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "No active streaming session found for client: " + client_name);
      }
    }
    catch (const legacy_control::error_t &error) {
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", error.code);
      tree.put("root.<xmlattr>.status_message", error.what());
    }
    catch (const std::invalid_argument &) {
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid bitrate parameter");
    }
    catch (const std::out_of_range &) {
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Bitrate parameter out of range");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "ChangeBitrate: "sv << e.what();
      tree.put("root.bitrate", 0);
      tree.put("root.<xmlattr>.status_code", 500);
      tree.put("root.<xmlattr>.status_message", "Internal server error");
    }
  }

  void
  change(resp_https_t response, req_https_t request) {
    log_request(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      write_tree(response, tree);
    });

    try {
      auto args = request->parse_query_string();
      auto param_type_param = args.find("type");
      auto param_value_param = args.find("value");
      auto clientname_param = args.find("clientname");

      if (param_type_param == args.end()) {
        BOOST_LOG(warning) << "Change dynamic param error: miss type";
        set_error(tree, 400, "Missing param_type parameter");
        return;
      }

      if (param_value_param == args.end()) {
        BOOST_LOG(warning) << "Change dynamic param error: miss value";
        set_error(tree, 400, "Missing param_value parameter");
        return;
      }

      if (clientname_param == args.end()) {
        BOOST_LOG(warning) << "Change dynamic param error: miss clientname";
        set_error(tree, 400, "Missing clientname parameter");
        return;
      }

      int param_type = std::stoi(param_type_param->second);
      std::string param_value = param_value_param->second;
      std::string client_name = clientname_param->second;

      if (param_type < 0 || param_type >= static_cast<int>(video::dynamic_param_type_e::MAX_PARAM_TYPE)) {
        BOOST_LOG(warning) << "Change dynamic param error: invalid type";
        set_error(tree, 400, "Invalid param_type value");
        return;
      }

      video::dynamic_param_t param;
      param.type = static_cast<video::dynamic_param_type_e>(param_type);
      param.valid = true;

      switch (param.type) {
        case video::dynamic_param_type_e::RESOLUTION: {
          BOOST_LOG(warning) << "Change dynamic param error: resolution change should be sent via control stream protocol, not HTTP API";
          set_error(tree, 400, "Resolution change should be sent via control stream protocol, not HTTP API");
          return;
        }
        case video::dynamic_param_type_e::FPS: {
          float fps = std::stof(param_value);
          if (fps <= 0.0f || fps > 1000.0f) {
            BOOST_LOG(warning) << "Change dynamic param error: invalid FPS value";
            set_error(tree, 400, "Invalid FPS value. Must be between 0 and 1000");
            return;
          }
          param.value.float_value = fps;
          break;
        }
        case video::dynamic_param_type_e::BITRATE: {
          int bitrate = std::stoi(param_value);
          if (bitrate <= 0 || bitrate > 800000) {
            BOOST_LOG(warning) << "Change dynamic param error: invalid bitrate value";
            set_error(tree, 400, "Invalid bitrate value. Must be between 1 and 800000 Kbps");
            return;
          }
          param.value.int_value = bitrate;
          break;
        }
        case video::dynamic_param_type_e::QP: {
          int qp = std::stoi(param_value);
          if (qp < 0 || qp > 51) {
            BOOST_LOG(warning) << "Change dynamic param error: invalid QP value";
            set_error(tree, 400, "Invalid QP value. Must be between 0 and 51");
            return;
          }
          param.value.int_value = qp;
          break;
        }
        case video::dynamic_param_type_e::FEC_PERCENTAGE: {
          int fec = std::stoi(param_value);
          if (fec < 0 || fec > 100) {
            BOOST_LOG(warning) << "Change dynamic param error: invalid FEC percentage value";
            set_error(tree, 400, "Invalid FEC percentage. Must be between 0 and 100");
            return;
          }
          param.value.int_value = fec;
          break;
        }
        case video::dynamic_param_type_e::ADAPTIVE_QUANTIZATION: {
          if (param_value == "true" || param_value == "1") {
            param.value.bool_value = true;
          }
          else if (param_value == "false" || param_value == "0") {
            param.value.bool_value = false;
          }
          else {
            BOOST_LOG(warning) << "Change dynamic param error: invalid adaptive quantization value";
            set_error(tree, 400, "Invalid adaptive quantization value. Must be true/false or 1/0");
            return;
          }
          break;
        }
        case video::dynamic_param_type_e::MULTI_PASS: {
          int multi_pass = std::stoi(param_value);
          if (multi_pass < 0 || multi_pass > 2) {
            BOOST_LOG(warning) << "Change dynamic param error: invalid multi-pass value";
            set_error(tree, 400, "Invalid multi-pass value. Must be between 0 and 2");
            return;
          }
          param.value.int_value = multi_pass;
          break;
        }
        case video::dynamic_param_type_e::VBV_BUFFER_SIZE: {
          int vbv = std::stoi(param_value);
          if (vbv <= 0) {
            BOOST_LOG(warning) << "Change dynamic param error: invalid VBV buffer size value";
            set_error(tree, 400, "Invalid VBV buffer size. Must be greater than 0");
            return;
          }
          param.value.int_value = vbv;
          break;
        }
        default:
          set_error(tree, 400, "Unsupported parameter type");
          return;
      }

      bool success;
      if (param.type == video::dynamic_param_type_e::BITRATE || param.type == video::dynamic_param_type_e::FEC_PERCENTAGE) {
        const auto target = legacy_control::resolve(request, legacy_control::query_scope(args), client_name);
        success = legacy_control::submit(target, param);
      }
      else success = stream::session::change_dynamic_param_for_client(client_name, param);

      if (success) {
        tree.put("root.success", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        tree.put("root.<xmlattr>.param_type", param_type);
        tree.put("root.<xmlattr>.param_value", param_value);
        tree.put("root.<xmlattr>.clientname", client_name);
        tree.put("root.<xmlattr>.status_message", "Dynamic parameter change request sent to client session");
        BOOST_LOG(info) << "NVHTTP API: Dynamic parameter change requested for client '"
                        << client_name << "': type=" << param_type << ", value=" << param_value;
      }
      else {
        BOOST_LOG(warning) << "Change dynamic param error: no active streaming session found for client";
        set_error(tree, 404, "No active streaming session found for client: " + client_name);
      }
    }
    catch (const legacy_control::error_t &error) {
      set_error(tree, error.code, error.what());
    }
    catch (const std::invalid_argument &) {
      set_error(tree, 400, "Invalid numeric parameter");
    }
    catch (const std::out_of_range &) {
      set_error(tree, 400, "Numeric parameter out of range");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "Change dynamic param error: "s << e.what();
      set_error(tree, 500, "Internal server error");
    }
  }

}  // namespace nvhttp::dynamic_params
