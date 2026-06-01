#include "apps.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <Simple-Web-Server/server_http.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <nlohmann/json.hpp>

#include "src/logging.h"
#include "src/process.h"
#include "src/utility.h"
#include "src/video.h"

using json = nlohmann::json;
using namespace std::literals;

namespace nvhttp::apps {

  namespace {

    using args_t = SimpleWeb::CaseInsensitiveMultimap;
    namespace pt = boost::property_tree;

    void
    log_request(const req_https_t &request) {
      BOOST_LOG(debug) << "Request - Protocol: HTTPS"
                       << ", IP: " << request->remote_endpoint().address().to_string()
                       << ", PORT: " << request->remote_endpoint().port()
                       << ", METHOD: " << request->method
                       << ", PATH: " << request->path;
    }

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

  }  // namespace

  void
  list(resp_https_t response, req_https_t request) {
    log_request(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
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

        json_cmds.push_back(json_cmd);
      }

      app.put("SuperCmds"s, json_cmds.dump(4));

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void
  asset(resp_https_t response, req_https_t request) {
    log_request(request);

    try {
      auto args = request->parse_query_string();
      auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

      std::ifstream in(app_image, std::ios::binary);
      if (!in.is_open()) {
        response->write(SimpleWeb::StatusCode::client_error_not_found, "App asset not found");
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "image/png");
      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    }
    catch (const std::exception &e) {
      BOOST_LOG(warning) << "AppAsset error: "s << e.what();
      response->write(SimpleWeb::StatusCode::client_error_bad_request, "Missing or invalid parameters");
    }
  }

  void
  exec_super_cmd(resp_https_t response, req_https_t request) {
    log_request(request);

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

}  // namespace nvhttp::apps
