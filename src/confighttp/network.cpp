/**
 * @file src/confighttp/network.cpp
 * @brief Authenticated host reachability assessment and network overview for the panel and Web UI.
 */
#include "network.h"

#include <future>

#include "src/config.h"
#include "src/confighttp.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/logging.h"
#include "src/nat_discovery.h"
#include "src/network_check.h"
#include "src/network.h"
#include "src/network_reachability.h"
#include "src/nvhttp.h"
#include "src/rtsp.h"
#include "src/stream.h"

using namespace std::literals;

namespace confighttp::network {
  reachability::probe_options_t
  configured_probe() {
    reachability::probe_options_t options;
    options.ports = {
      { "tcp", net::map_port(nvhttp::PORT_HTTPS), std::string(reachability::port_role::https) },
      { "tcp", net::map_port(nvhttp::PORT_HTTP), std::string(reachability::port_role::http) },
      { "tcp", net::map_port(confighttp::PORT_HTTPS), std::string(reachability::port_role::web) },
      { "tcp", net::map_port(rtsp_stream::RTSP_SETUP_PORT), std::string(reachability::port_role::rtsp) },
      { "udp", net::map_port(stream::VIDEO_STREAM_PORT), std::string(reachability::port_role::video) },
      { "udp", net::map_port(stream::CONTROL_PORT), std::string(reachability::port_role::control) },
      { "udp", net::map_port(stream::AUDIO_STREAM_PORT), std::string(reachability::port_role::audio) },
      { "udp", net::map_port(stream::MIC_STREAM_PORT), std::string(reachability::port_role::microphone) },
    };
    options.certificate_path = config::nvhttp.cert;
    options.external_ip = config::nvhttp.external_ip;
    options.external_endpoints = config::nvhttp.external_endpoints;
    options.default_http_port = net::map_port(nvhttp::PORT_HTTP);
    options.origin_web_ui_allowed = config::nvhttp.origin_web_ui_allowed.empty() ? "pc" : config::nvhttp.origin_web_ui_allowed;
    options.hairpin_port = net::map_port(nvhttp::PORT_HTTPS);
    return options;
  }

  namespace {
    void
    getReachability(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;

      bool refresh = false;
      const auto query = request->parse_query_string();
      const auto found = query.find("refresh");
      if (found != query.end()) refresh = found->second == "1" || found->second == "true";

      // `test`: addresses to try without saving them. They replace the configured ones for this run only (the cache key holds the addresses, so
      // the next plain read is not answered with this run), and the run is never served from the cache.
      auto options = configured_probe();
      const auto test = query.find("test");
      if (test != query.end()) {
        const auto addresses = reachability::test_endpoints_option(test->second);
        if (!addresses.empty()) {
          options.external_endpoints = addresses;
          options.external_ip.clear();
          refresh = true;
        }
      }

      try {
        const auto snapshot = reachability::probe(options, refresh);
        BOOST_LOG(debug) << "Reachability check: public="sv << reachability::to_string(snapshot.public_reachability.verdict)
                         << ", mesh="sv << reachability::to_string(snapshot.mesh.verdict);
        write_json(response, web::StatusCode::ok, reachability::to_json(snapshot));
      }
      catch (const std::exception &error) {
        write_json_error(response, web::StatusCode::internal_server_error, error.what());
      }
    }

    bool
    refresh_requested(req_https_t &request) {
      const auto query = request->parse_query_string();
      const auto found = query.find("refresh");
      return found != query.end() && (found->second == "1" || found->second == "true");
    }

    /// GET /api/network/overview: interfaces, STUN public addresses, NAT mapping and the reachability facts.
    void
    getOverview(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      const bool refresh = refresh_requested(request);

      try {
        // Built-in servers in the order of the host's region (its locale or its time zone).
        const auto region = nat_discovery::region_of(config::sunshine.locale, nat_discovery::host_time_zone());
        const auto list = nat_discovery::parse_server_list(config::nvhttp.stun_servers, region);
        nat_discovery::discover_options_t options;
        options.servers = list.servers;
        options.enabled = !list.disabled;
        options.region = list.configured ? "configured" : region == nat_discovery::region_e::china ? "china" : "global";
        // The STUN exchanges and the reachability probes are independent; run them side by side.
        auto discovery = std::async(std::launch::async, [options, refresh]() { return nat_discovery::discover(options, refresh); });
        const auto snapshot = reachability::probe(configured_probe(), refresh);
        const auto result = discovery.get();
        const auto body = nat_discovery::overview_json(result, snapshot);
        BOOST_LOG(debug) << "Network overview: ipv4="sv << body["reachability"]["ipv4"]["verdict"].get<std::string>()
                         << ", ipv6="sv << body["reachability"]["ipv6"]["verdict"].get<std::string>()
                         << ", mapping="sv << body["ipv4"]["nat"]["mapping"].get<std::string>();
        write_json(response, web::StatusCode::ok, body);
      }
      catch (const std::exception &error) {
        write_json_error(response, web::StatusCode::internal_server_error, error.what());
      }
    }

    network_check::inputs_t
    check_inputs() {
      network_check::inputs_t inputs;
      const auto region = nat_discovery::region_of(config::sunshine.locale, nat_discovery::host_time_zone());
      const auto list = nat_discovery::parse_server_list(config::nvhttp.stun_servers, region);
      inputs.stun_enabled = !list.disabled;
      const auto saved = public_address::effective_endpoints(config::nvhttp.external_endpoints, config::nvhttp.external_ip);
      for (const auto &endpoint : saved.endpoints) inputs.saved_endpoints.push_back(public_address::format_endpoint(endpoint));

      inputs.probe = [](const network_check::request_t &, const std::function<void(std::string_view, bool)> &progress) {
        auto options = configured_probe();
        options.progress = progress;
        return reachability::probe(options, true);
      };
      inputs.discover = [list, region](const network_check::request_t &) {
        nat_discovery::discover_options_t options;
        options.servers = list.servers;
        options.enabled = !list.disabled;
        options.region = list.configured ? "configured" : region == nat_discovery::region_e::china ? "china" : "global";
        return nat_discovery::discover(options, true);
      };
      return inputs;
    }

    /// POST /api/network/check: start a run, or return the one that is going. The answer is the run, with the state of every step.
    void
    startCheck(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      try {
        const auto run = network_check::start(check_inputs(), {});
        write_json(response, web::StatusCode::ok, network_check::to_json(*run));
      }
      catch (const std::exception &error) {
        write_json_error(response, web::StatusCode::internal_server_error, error.what());
      }
    }

    /// GET /api/network/check[?id=...]: the latest run (or the named one) with the state of every step.
    void
    getCheck(resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      const auto query = request->parse_query_string();
      const auto id = query.find("id");
      const auto run = network_check::find(id == query.end() ? std::string {} : id->second);
      if (!run) {
        write_json_error(response, web::StatusCode::not_found, "no_run");
        return;
      }
      write_json(response, web::StatusCode::ok, network_check::to_json(*run));
    }
  }  // namespace

  void
  register_routes(https_server_t &server) {
    server.resource["^/api/network/check$"]["POST"] = startCheck;
    server.resource["^/api/network/check$"]["GET"] = getCheck;
    server.resource["^/api/network/reachability$"]["GET"] = getReachability;
    server.resource["^/api/network/overview$"]["GET"] = getOverview;
  }
}  // namespace confighttp::network
