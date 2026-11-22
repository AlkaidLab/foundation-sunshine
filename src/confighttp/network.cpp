/**
 * @file src/confighttp/network.cpp
 * @brief Authenticated host reachability assessment and network overview for the panel and Web UI.
 */
#include "network.h"

#include <chrono>
#include <future>

#include "src/config.h"
#include "src/confighttp.h"
#include "src/confighttp/auth.h"
#include "src/confighttp/common.h"
#include "src/logging.h"
#include "src/nat_discovery.h"
#include "src/network_check.h"
#include "src/network_external.h"
#include "src/network_pin.h"
#include "src/httpcommon.h"
#include "src/telemetry/telemetry.h"
#include "src/telemetry/transport.h"
#include <random>
#include "src/network.h"
#include "src/network_reachability.h"
#include "src/nvhttp.h"
#include "src/rtsp.h"
#include "src/stream.h"

using namespace std::literals;

namespace confighttp::network {
  namespace {
    network_external::cache_t &
    outside_cache() {
      static network_external::cache_t cache;
      return cache;
    }

    std::int64_t
    now_s() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
  }  // namespace

  /// `ask_outside`: the person pressed "check" (or asked to test an address): the outside service may be asked. Without it the answers of the last 10 minutes are still used, and nothing is sent.
  reachability::probe_options_t
  configured_probe(bool ask_outside) {
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
    // The probes leave through the physical adapter unless `network_bypass_proxy` says otherwise (network_pin.h).
    options.bypass = network_pin::enabled(config::nvhttp.network_bypass_proxy);
    // The outside check (network_external.h): only when it is switched on in the configuration (`network_external_check = enabled`).
    if (network_external::enabled(config::nvhttp.network_external_check)) {
      options.outside = [ask_outside, bypass = options.bypass](const reachability::endpoint_result_t &endpoint) {
        network_external::request_t request;
        auto install = telemetry::install_id();
        if (install.empty()) {
          // Both statistics choices are off: no install id exists. A random id for this run of the program keeps the service's per-install brake working without naming the install.
          static const std::string per_run = []() {
            std::random_device device;
            char text[37];
            std::snprintf(text, sizeof(text), "%08x-%04x-4%03x-a%03x-%08x%04x", device(), device() & 0xffff, device() & 0xfff, device() & 0xfff, device(), device() & 0xffff);
            return std::string(text);
          }();
          install = per_run;
        }
        request.install_id = install;
        request.uniqueid = http::unique_id;
        request.target = endpoint.host;
        for (const auto &port : endpoint.ports) request.ports.push_back(port);
        const auto key = network_external::cache_key(request);
        auto answer = outside_cache().find(key, now_s());
        if (!answer) {
          // Not asked for by a person: nothing is sent, and what the host does not know it does not say.
          if (!ask_outside) return reachability::outside_t {};
          answer = network_external::probe(network_external::endpoint_of(config::nvhttp.network_external_endpoint), request, [bypass](const std::string &url, const std::string &body, std::string &out, long &retry_after) {
            // Pinned to the physical adapter; when nothing answers through it, once more on the system route (a service on this computer or the LAN is not pinned).
            telemetry::post_result_t posted;
            network_pin::attempt(bypass && network_pin::pinnable_url(url), [&](const network_pin::pin_t *pin) {
              auto failed = std::make_shared<bool>(false);
              posted = telemetry::post_once(url, "application/json", body, "foundation-sunshine-network-check", 8, 4,
                pin ? network_pin::socket_hook(*pin, failed) : std::function<void(int, std::uintptr_t)> {});
              return network_pin::attempt_t { posted.status != 0, *failed };
            });
            out = posted.body;
            retry_after = posted.retry_after_s;
            return posted.status;
          });
          outside_cache().put(key, *answer, now_s());
        }
        reachability::outside_t outside;
        outside.asked = true;
        outside.caller = answer->caller;
        if (answer->availability == network_external::availability_e::answered) {
          outside.state = network_external::tcp_summary(*answer);
        }
        else {
          outside.state = "unavailable";
          outside.reason = answer->reason;
        }
        return outside;
      };
    }
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

      // `refresh=1` is the person asking for a new check, and so is a `test` address: only these may ask the outside service.
      // `test`: addresses to try without saving them. They replace the configured ones for this run only (the cache key holds the addresses, so
      // the next plain read is not answered with this run), and the run is never served from the cache.
      auto options = configured_probe(refresh || query.find("test") != query.end());
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
        options.bypass = network_pin::enabled(config::nvhttp.network_bypass_proxy);
        // The STUN exchanges and the reachability probes are independent; run them side by side.
        auto discovery = std::async(std::launch::async, [options, refresh]() { return nat_discovery::discover(options, refresh); });
        const auto snapshot = reachability::probe(configured_probe(false), refresh);
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
      inputs.bypass = network_pin::enabled(config::nvhttp.network_bypass_proxy);
      const auto saved = public_address::effective_endpoints(config::nvhttp.external_endpoints, config::nvhttp.external_ip);
      for (const auto &endpoint : saved.endpoints) inputs.saved_endpoints.push_back(public_address::format_endpoint(endpoint));

      inputs.probe = [](const network_check::request_t &, const std::function<void(std::string_view, bool)> &progress) {
        auto options = configured_probe(true);
        options.progress = progress;
        return reachability::probe(options, true);
      };
      inputs.discover = [list, region, bypass = inputs.bypass](const network_check::request_t &) {
        nat_discovery::discover_options_t options;
        options.bypass = bypass;
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
