#include "dynamic_params.h"

#include <sstream>
#include <stdexcept>
#include <string>

#include <Simple-Web-Server/server_http.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include "src/logging.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/utility.h"
#include "src/video.h"

using namespace std::literals;

namespace nvhttp::dynamic_params {

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
      }
      else {
        tree.put("root.bitrate", 0);
        tree.put("root.<xmlattr>.status_code", 404);
        tree.put("root.<xmlattr>.status_message", "No active streaming session found for client: " + client_name);
      }
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
      }
      else {
        BOOST_LOG(warning) << "Change dynamic param error: no active streaming session found for client";
        set_error(tree, 404, "No active streaming session found for client: " + client_name);
      }
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
