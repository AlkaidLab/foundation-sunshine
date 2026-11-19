/**
 * @file src/network_check.cpp
 * @brief The network check as a run with steps (see network_check.h).
 */
#include "network_check.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>

using namespace std::literals;

namespace network_check {
  namespace {
    using clock_t = std::chrono::steady_clock;

    std::string
    port_code(const reachability::port_status_t &port) {
      return port.proto + "/" + std::to_string(port.port);
    }

    step_t
    make_step(std::string_view id, step_state_e state, std::string reason = {}, std::string next = {}, nlohmann::json detail = nlohmann::json::object()) {
      step_t result;
      result.id = std::string(id);
      result.state = state;
      result.reason = std::move(reason);
      result.next = std::move(next);
      result.detail = std::move(detail);
      return result;
    }

    // ---- steps -----------------------------------------------------------------------------------

    step_t
    listening_step(const reachability::snapshot_t &snapshot) {
      nlohmann::json ports = nlohmann::json::array();
      std::vector<std::string> missing;
      for (const auto &port : snapshot.ports) {
        const bool on_demand = reachability::binds_on_demand(port);
        // The web UI port is not a streaming port: it only matters when the web UI is exposed to the internet.
        const bool required = port.role != reachability::port_role::web;
        ports.push_back({ { "proto", port.proto }, { "port", port.port }, { "role", port.role }, { "listening", port.listening }, { "on_demand", on_demand }, { "bound", port.bound } });
        if (required && !port.listening && !on_demand) missing.push_back(port_code(port));
      }
      nlohmann::json detail { { "ports", ports } };
      if (!missing.empty()) {
        detail["missing"] = missing;
        return make_step(step::listening, step_state_e::fail, "not_listening", "restart", detail);
      }
      return make_step(step::listening, step_state_e::ok, {}, {}, detail);
    }

    step_t
    firewall_step(const reachability::snapshot_t &snapshot) {
      std::vector<std::string> blocked;
      bool unknown = false;
      for (const auto &port : snapshot.ports) {
        if (port.role == reachability::port_role::web) continue;
        if (port.firewall == reachability::firewall_e::blocked) blocked.push_back(port_code(port));
        else if (port.firewall == reachability::firewall_e::unknown) unknown = true;
      }
      if (!blocked.empty()) return make_step(step::firewall, step_state_e::fail, "firewall_blocked", "firewall", { { "blocked", blocked } });
      if (unknown) return make_step(step::firewall, step_state_e::warn, "firewall_unknown", {});
      return make_step(step::firewall, step_state_e::ok);
    }

    nlohmann::json
    family_detail(const nat_discovery::family_discovery_t &family) {
      return { { "address", family.public_address.empty() ? nlohmann::json(nullptr) : nlohmann::json(family.public_address) },
        { "on_interface", family.on_interface },
        { "answered", std::count_if(family.servers.begin(), family.servers.end(), [](const auto &entry) { return entry.answered; }) },
        { "asked", family.servers.size() },
        { "consistent", family.distinct_addresses.size() <= 1 } };
    }

    step_t
    addresses_step(const reachability::snapshot_t &, const nat_discovery::discovery_t &discovery, bool stun_enabled) {
      if (!stun_enabled || !discovery.enabled) return make_step(step::addresses, step_state_e::skipped, "stun_off", {});
      nlohmann::json detail { { "ipv4", family_detail(discovery.ipv4) }, { "ipv6", family_detail(discovery.ipv6) }, { "contacted", discovery.contacted } };
      const bool any = !discovery.ipv4.public_address.empty() || !discovery.ipv6.public_address.empty();
      const bool fake_dns = std::find(discovery.ipv4.notes.begin(), discovery.ipv4.notes.end(), "dns_fake_ip") != discovery.ipv4.notes.end();
      if (!any) {
        const bool no_route = discovery.ipv4.servers.empty() ? false : std::all_of(discovery.ipv4.servers.begin(), discovery.ipv4.servers.end(), [](const auto &entry) { return entry.error == "no_route"; });
        return make_step(step::addresses, step_state_e::warn, no_route ? "no_route" : "stun_no_answer", {}, detail);
      }
      if (fake_dns || discovery.ipv4.distinct_addresses.size() > 1) {
        // Servers that disagree, or a resolver that hands out the fake-IP pool, mean a proxy sits on the path: the address shown may be its exit.
        return make_step(step::addresses, step_state_e::warn, fake_dns ? "proxy_dns" : "proxy_exit_suspected", {}, detail);
      }
      return make_step(step::addresses, step_state_e::ok, {}, {}, detail);
    }

    step_t
    cgnat_step(const reachability::snapshot_t &snapshot, const nat_discovery::discovery_t &discovery) {
      const auto verdict = nat_discovery::assess_cgnat(snapshot, discovery);
      nlohmann::json detail { { "state", verdict.state }, { "reasons", verdict.reasons },
        { "wan_ip", snapshot.public_reachability.router.wan_ip.empty() ? nlohmann::json(nullptr) : nlohmann::json(snapshot.public_reachability.router.wan_ip) } };
      if (verdict.state == "yes") return make_step(step::cgnat, step_state_e::fail, "cgnat", "mesh_or_ipv6", detail);
      if (verdict.state == "unknown") return make_step(step::cgnat, step_state_e::warn, "cgnat_unknown", {}, detail);
      return make_step(step::cgnat, step_state_e::ok, {}, {}, detail);
    }

    step_t
    router_step(const reachability::snapshot_t &snapshot) {
      const auto &router = snapshot.public_reachability.router;
      nlohmann::json mappings = nlohmann::json::array();
      std::vector<std::string> missing;
      for (const auto &mapping : router.mappings) {
        mappings.push_back({ { "proto", mapping.proto }, { "port", mapping.port }, { "present", mapping.present }, { "internal", mapping.internal } });
        if (!mapping.present) missing.push_back(mapping.proto + "/" + std::to_string(mapping.port));
      }
      nlohmann::json detail { { "protocol", router.protocol }, { "wan_ip", router.wan_ip.empty() ? nlohmann::json(nullptr) : nlohmann::json(router.wan_ip) },
        { "wan_ip_class", reachability::to_string(router.wan_ip_class) }, { "mappings", mappings } };
      if (router.protocol == "none") return make_step(step::router, step_state_e::warn, "no_igd", "forward", detail);
      if (!missing.empty()) {
        detail["missing"] = missing;
        return make_step(step::router, step_state_e::warn, "mapping_missing", "upnp", detail);
      }
      return make_step(step::router, step_state_e::ok, {}, {}, detail);
    }

    step_t
    endpoint_step(const reachability::endpoint_result_t &endpoint) {
      nlohmann::json detail {
        { "port", endpoint.port }, { "https_port", endpoint.https_port }, { "scope", reachability::to_string(endpoint.scope) }, { "resolved", endpoint.resolved },
        { "latency_ms", endpoint.latency_ms }, { "error", endpoint.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(endpoint.error) },
        { "matches_wan", endpoint.matches_wan },
      };
      step_t result;
      if (endpoint.reached || endpoint.outside.state == "reachable") {
        result = make_step(step::endpoint, step_state_e::ok, {}, {}, detail);
      }
      else if (endpoint.inconclusive) {
        // This computer cannot reach its own public address. That is what a router without NAT loopback does, and it says nothing about the internet.
        result = make_step(step::endpoint, step_state_e::warn, "loopback_unverifiable", "outside_test", detail);
      }
      else if (endpoint.error == "dns_failed") {
        result = make_step(step::endpoint, step_state_e::fail, "dns_failed", "address", detail);
      }
      else if (endpoint.error == "identity_mismatch" || endpoint.error == "tls_failed") {
        result = make_step(step::endpoint, step_state_e::fail, endpoint.error, "address", detail);
      }
      else if (endpoint.scope == reachability::scope_e::lan || endpoint.scope == reachability::scope_e::loopback) {
        result = make_step(step::endpoint, step_state_e::fail, "address_private", "address", detail);
      }
      else if (endpoint.scope == reachability::scope_e::mesh) {
        result = make_step(step::endpoint, endpoint.reached ? step_state_e::ok : step_state_e::warn, endpoint.reached ? std::string {} : "mesh_address", {}, detail);
      }
      else {
        result = make_step(step::endpoint, step_state_e::warn, endpoint.error.empty() ? "not_tried" : endpoint.error, "address", detail);
      }
      result.subject = endpoint.input;
      return result;
    }

    step_t
    external_tcp_step(const reachability::snapshot_t &snapshot) {
      const auto &endpoints = snapshot.public_reachability.endpoints;
      bool asked = false;
      const reachability::endpoint_result_t *reached = nullptr;
      const reachability::endpoint_result_t *not_reached = nullptr;
      const reachability::endpoint_result_t *another = nullptr;
      const reachability::endpoint_result_t *refused = nullptr;
      const reachability::endpoint_result_t *proxied = nullptr;
      const reachability::endpoint_result_t *unavailable = nullptr;
      for (const auto &endpoint : endpoints) {
        if (!endpoint.outside.asked) continue;
        asked = true;
        const auto &state = endpoint.outside.state;
        if (state == "reachable") reached = reached ? reached : &endpoint;
        else if (state == "closed" || state == "timeout") not_reached = not_reached ? not_reached : &endpoint;
        else if (state == "another_host") another = another ? another : &endpoint;
        else if (state == "unavailable" && endpoint.outside.reason == "router_proxy") proxied = proxied ? proxied : &endpoint;
        else if (state == "unavailable" && (endpoint.outside.reason == "ip_mismatch" || endpoint.outside.reason == "dns_mismatch" || endpoint.outside.reason == "dns_failed")) refused = refused ? refused : &endpoint;
        else unavailable = unavailable ? unavailable : &endpoint;
      }
      const auto subject = [](step_t step, const reachability::endpoint_result_t *endpoint) {
        if (endpoint) {
          step.subject = endpoint->input;
          step.detail["outside"] = endpoint->outside.state;
          if (!endpoint->outside.reason.empty()) step.detail["why"] = endpoint->outside.reason;
        }
        return step;
      };
      if (reached) return subject(make_step(step::external_tcp, step_state_e::ok), reached);
      if (not_reached) return subject(make_step(step::external_tcp, step_state_e::fail, "not_reached_from_outside", "forward"), not_reached);
      if (another) return subject(make_step(step::external_tcp, step_state_e::fail, "another_host_answers", "address"), another);
      // The service saw the exit of a proxy that runs on the router, not the router's own address: nothing here can bypass it, and it says nothing about whether the internet reaches this host.
      if (proxied) {
        auto step = subject(make_step(step::external_tcp, step_state_e::warn, "router_proxy"), proxied);
        step.detail["seen_by_service"] = proxied->outside.caller;
        step.detail["router_wan"] = snapshot.public_reachability.router.wan_ip;
        return step;
      }
      if (refused) return subject(make_step(step::external_tcp, step_state_e::warn, "address_not_this_computer", "address"), refused);
      if (unavailable) return subject(make_step(step::external_tcp, step_state_e::skipped, "external_unavailable"), unavailable);
      return make_step(step::external_tcp, step_state_e::skipped, asked ? "external_unavailable" : "external_off");
    }

    // ---- summary ---------------------------------------------------------------------------------

    void
    add(protocol_summary_t &summary, std::string reason) {
      if (std::find(summary.because.begin(), summary.because.end(), reason) == summary.because.end()) summary.because.push_back(std::move(reason));
    }

    bool
    session_from_public(const reachability::snapshot_t &snapshot) {
      return std::any_of(snapshot.public_reachability.evidence.begin(), snapshot.public_reachability.evidence.end(), [](const reachability::evidence_t &entry) {
        return entry.kind == "client-connected-from-public";
      });
    }

    /// The address a client should enter: an address of the configuration that a handshake reached, else one with no known fault, else the router's
    /// outside address when it is public, else a public address of this computer. Empty when there is none.
    std::string
    suggested_address(const reachability::public_result_t &check) {
      for (const auto &endpoint : check.endpoints) {
        if (endpoint.reached && endpoint.scope == reachability::scope_e::public_address) return endpoint.input;
      }
      for (const auto &endpoint : check.endpoints) {
        if (endpoint.scope != reachability::scope_e::public_address) continue;
        if (endpoint.error == "dns_failed" || endpoint.error == "identity_mismatch") continue;
        if (std::find(endpoint.reasons.begin(), endpoint.reasons.end(), "address_mismatch") != endpoint.reasons.end()) continue;
        return endpoint.input;
      }
      if (check.router.wan_ip_class == reachability::ip_class_e::public_address && !check.router.wan_ip.empty()) return check.router.wan_ip;
      return check.host_public_ip;
    }

    const step_t *
    find_step(const std::vector<step_t> &steps, std::string_view id) {
      for (const auto &entry : steps) {
        if (entry.id == id) return &entry;
      }
      return nullptr;
    }
  }  // namespace

  std::string_view
  to_string(step_state_e value) {
    switch (value) {
      case step_state_e::pending:
        return "pending";
      case step_state_e::running:
        return "running";
      case step_state_e::ok:
        return "ok";
      case step_state_e::warn:
        return "warn";
      case step_state_e::fail:
        return "fail";
      case step_state_e::skipped:
        return "skipped";
    }
    return "pending";
  }

  std::string_view
  to_string(protocol_state_e value) {
    switch (value) {
      case protocol_state_e::reachable:
        return "reachable";
      case protocol_state_e::likely:
        return "likely";
      case protocol_state_e::unreachable:
        return "unreachable";
      case protocol_state_e::unverified:
        return "unverified";
      case protocol_state_e::unknown:
        return "unknown";
    }
    return "unknown";
  }

  std::vector<step_t>
  planned_steps(const request_t &, bool stun_enabled, const std::vector<std::string> &endpoint_inputs) {
    std::vector<step_t> steps;
    steps.push_back(make_step(step::listening, step_state_e::pending));
    steps.push_back(make_step(step::firewall, step_state_e::pending));
    steps.push_back(make_step(step::addresses, stun_enabled ? step_state_e::pending : step_state_e::skipped, stun_enabled ? "" : "stun_off"));
    steps.push_back(make_step(step::cgnat, step_state_e::pending));
    steps.push_back(make_step(step::router, step_state_e::pending));
    for (const auto &input : endpoint_inputs) {
      auto entry = make_step(step::endpoint, step_state_e::pending);
      entry.subject = input;
      steps.push_back(std::move(entry));
    }
    steps.push_back(make_step(step::external_tcp, step_state_e::pending));
    steps.push_back(make_step(step::external_udp, step_state_e::pending));
    return steps;
  }

  std::vector<step_t>
  results_from(const request_t &, const reachability::snapshot_t &snapshot, const nat_discovery::discovery_t &discovery) {
    std::vector<step_t> steps;
    steps.push_back(listening_step(snapshot));
    steps.push_back(firewall_step(snapshot));
    steps.push_back(addresses_step(snapshot, discovery, discovery.enabled));
    steps.push_back(cgnat_step(snapshot, discovery));
    steps.push_back(router_step(snapshot));
    for (const auto &endpoint : snapshot.public_reachability.endpoints) steps.push_back(endpoint_step(endpoint));
    steps.push_back(external_tcp_step(snapshot));
    // UDP cannot be tried from outside by a service that speaks TCP: it is not verified, whatever else is known
    steps.push_back(make_step(step::external_udp, step_state_e::skipped, "udp_not_verified"));
    return steps;
  }

  namespace {
    // the firewall step names the ports it found blocked as "tcp/47984", "udp/47998"
    bool
    blocks_tcp(const step_t &firewall) {
      const auto found = firewall.detail.find("blocked");
      if (found == firewall.detail.end() || !found->is_array()) return false;
      return std::any_of(found->begin(), found->end(), [](const nlohmann::json &port) { return port.is_string() && port.get<std::string>().rfind("tcp/", 0) == 0; });
    }
  }  // namespace

  summary_t
  summarize(const request_t &, const reachability::snapshot_t &snapshot, const nat_discovery::discovery_t &discovery, const std::vector<step_t> &steps) {
    summary_t summary;
    const auto &check = snapshot.public_reachability;
    summary.cgnat = nat_discovery::assess_cgnat(snapshot, discovery).state;
    summary.ipv4 = discovery.ipv4.public_address;
    summary.ipv6 = discovery.ipv6.public_address;
    summary.suggested_address = suggested_address(check);

    const auto *listening = find_step(steps, step::listening);
    const auto *firewall = find_step(steps, step::firewall);
    const auto *router = find_step(steps, step::router);
    const bool evidence = session_from_public(snapshot);
    const bool reached = std::any_of(check.endpoints.begin(), check.endpoints.end(), [](const reachability::endpoint_result_t &entry) {
      return entry.reached && entry.scope == reachability::scope_e::public_address;
    });
    const bool shared_address = summary.cgnat == "yes";
    const bool has_global_v6 = !summary.ipv6.empty();

    auto &tcp = summary.tcp;
    if (reached) {
      tcp.state = protocol_state_e::reachable;
      add(tcp, "handshake_through_public_address");
    }
    if (evidence) {
      tcp.state = protocol_state_e::reachable;
      add(tcp, "session_evidence");
    }
    const auto *outside = find_step(steps, step::external_tcp);
    if (outside && outside->state == step_state_e::ok) {
      tcp.state = protocol_state_e::reachable;
      add(tcp, "outside_probe");
    }
    if (tcp.state != protocol_state_e::reachable) {
      // the outside check saw the exit of a proxy on the router: said first, and never a reason to call the host unreachable
      if (outside && outside->reason == "router_proxy") add(tcp, "router_proxy");
      if (outside && outside->state == step_state_e::fail) {
        tcp.state = protocol_state_e::unreachable;
        add(tcp, "not_reached_from_outside");
      }
      else if (listening && listening->state == step_state_e::fail) {
        tcp.state = protocol_state_e::unreachable;
        add(tcp, "not_listening");
      }
      else if (firewall && firewall->state == step_state_e::fail && blocks_tcp(*firewall)) {
        // only a blocked TCP port says anything about TCP: a UDP rule that is missing leaves the step failed and TCP untouched
        tcp.state = protocol_state_e::unreachable;
        add(tcp, "firewall_blocked");
      }
      else if (shared_address && !has_global_v6) {
        tcp.state = protocol_state_e::unreachable;
        add(tcp, "cgnat");
      }
      else if (check.verdict == reachability::verdict_e::likely || check.verdict == reachability::verdict_e::verified) {
        tcp.state = protocol_state_e::likely;
        for (const auto &reason : check.reasons) add(tcp, reason);
      }
      else {
        for (const auto &reason : check.reasons) add(tcp, reason);
        if (tcp.because.empty()) add(tcp, "no_evidence");
      }
    }

    // UDP is proven only by a session from a public address or by a connection made from outside. Nothing here does the second, so without a
    // session it is "not verified", never a failure: a router that forwards TCP may or may not forward UDP, and this computer cannot tell.
    auto &udp = summary.udp;
    if (evidence) {
      udp.state = protocol_state_e::reachable;
      add(udp, "session_evidence");
    }
    else {
      udp.state = protocol_state_e::unverified;
      if (router && router->reason == "mapping_missing") add(udp, "mapping_missing");
      else if (router && router->reason == "no_igd") add(udp, "no_igd");
      else if (router && router->state == step_state_e::ok) add(udp, "mappings_complete");
      add(udp, "not_verified_from_outside");
    }
    return summary;
  }

  nlohmann::json
  to_json(const run_t &run) {
    nlohmann::json steps = nlohmann::json::array();
    for (const auto &entry : run.steps) {
      steps.push_back({
        { "id", entry.id },
        { "subject", entry.subject.empty() ? nlohmann::json(nullptr) : nlohmann::json(entry.subject) },
        { "state", to_string(entry.state) },
        { "reason", entry.reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(entry.reason) },
        { "next", entry.next.empty() ? nlohmann::json(nullptr) : nlohmann::json(entry.next) },
        { "checked", entry.checked },
        { "detail", entry.detail },
        { "started_ms", entry.started_ms },
        { "elapsed_ms", entry.elapsed_ms },
      });
    }
    const auto protocol = [](const protocol_summary_t &value) {
      return nlohmann::json { { "state", to_string(value.state) }, { "because", value.because } };
    };
    return {
      { "version", JSON_VERSION },
      { "id", run.id },
      { "state", run.state == run_state_e::running ? "running" : "done" },
      { "started_at", run.started_at },
      { "finished_at", run.finished_at.empty() ? nlohmann::json(nullptr) : nlohmann::json(run.finished_at) },
      { "elapsed_ms", run.elapsed_ms },
      { "timed_out", run.timed_out },
      { "steps", steps },
      { "bypass", { { "state", network_pin::to_string(run.bypass.state) }, { "adapter", run.bypass.adapter_name.empty() ? nlohmann::json(nullptr) : nlohmann::json(run.bypass.adapter_name) } } },
      { "summary", { { "tcp", protocol(run.summary.tcp) }, { "udp", protocol(run.summary.udp) }, { "cgnat", run.summary.cgnat },
                     { "ipv4", run.summary.ipv4.empty() ? nlohmann::json(nullptr) : nlohmann::json(run.summary.ipv4) },
                     { "ipv6", run.summary.ipv6.empty() ? nlohmann::json(nullptr) : nlohmann::json(run.summary.ipv6) },
                     { "suggested_address", run.summary.suggested_address.empty() ? nlohmann::json(nullptr) : nlohmann::json(run.summary.suggested_address) } } },
    };
  }

  // ---- runner ------------------------------------------------------------------------------------

  namespace {
    struct job_t {
      std::mutex mutex;
      run_t run;
      clock_t::time_point began;
      std::atomic<bool> finished { false };
    };

    std::mutex registry_mutex;
    std::shared_ptr<job_t> current_job;

    std::string
    new_id() {
      std::random_device device;
      std::ostringstream text;
      text << std::hex << ((static_cast<std::uint64_t>(device()) << 32) ^ device());
      return text.str();
    }

    std::int64_t
    since(const clock_t::time_point &began) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(clock_t::now() - began).count();
    }

    void
    set_running(job_t &job, const std::function<bool(const step_t &)> &select, bool finished) {
      std::lock_guard lock { job.mutex };
      // a probe that reports after the deadline finds the run already over: what a finished run says does not change when it is read again
      if (job.run.state == run_state_e::done) return;
      const auto now = since(job.began);
      for (auto &entry : job.run.steps) {
        if (!select(entry)) continue;
        if (!finished) {
          if (entry.state == step_state_e::pending) {
            entry.state = step_state_e::running;
            entry.started_ms = now;
          }
        }
        else {
          entry.checked = true;
          entry.elapsed_ms = now - entry.started_ms;
        }
      }
    }

    // One value handed from a probing thread to the run, which may stop waiting for it.
    template<class T>
    struct slot_t {
      std::mutex mutex;
      std::condition_variable ready;
      std::optional<T> value;
      bool failed = false;  ///< the probing thread ended with an exception: there is no value to wait for
    };

    std::shared_ptr<const run_t>
    copy_of(job_t &job) {
      std::lock_guard lock { job.mutex };
      return std::make_shared<const run_t>(job.run);
    }

    void
    run_job(const std::shared_ptr<job_t> &job, inputs_t inputs, request_t request) {
      const auto deadline = job->began + inputs.deadline;
      auto probe_slot = std::make_shared<slot_t<reachability::snapshot_t>>();
      auto discover_slot = std::make_shared<slot_t<nat_discovery::discovery_t>>();

      // An exception must never leave a detached thread: it would end the whole process. A probe that throws is a probe that has no result.
      std::thread { [inputs, request, probe_slot, job]() {
        try {
          auto snapshot = inputs.probe(request, [&job](std::string_view stage, bool finished) {
            set_running(*job, [id = std::string(stage)](const step_t &entry) { return id == "endpoints" ? entry.id == step::endpoint : entry.id == id; }, finished);
          });
          std::lock_guard lock { probe_slot->mutex };
          probe_slot->value = std::move(snapshot);
          probe_slot->ready.notify_all();
        }
        catch (...) {
          std::lock_guard lock { probe_slot->mutex };
          probe_slot->failed = true;
          probe_slot->ready.notify_all();
        }
      } }.detach();

      if (inputs.stun_enabled) {
        set_running(*job, [](const step_t &entry) { return entry.id == step::addresses || entry.id == step::cgnat; }, false);
        std::thread { [inputs, request, discover_slot, job]() {
          try {
            auto discovery = inputs.discover(request);
            set_running(*job, [](const step_t &entry) { return entry.id == step::addresses; }, true);
            std::lock_guard lock { discover_slot->mutex };
            discover_slot->value = std::move(discovery);
            discover_slot->ready.notify_all();
          }
          catch (...) {
            std::lock_guard lock { discover_slot->mutex };
            discover_slot->failed = true;
            discover_slot->ready.notify_all();
          }
        } }.detach();
      }
      else {
        std::lock_guard lock { discover_slot->mutex };
        nat_discovery::discovery_t off;
        off.enabled = false;
        discover_slot->value = std::move(off);
      }

      const auto wait_for = [&](auto &slot) {
        std::unique_lock lock { slot->mutex };
        slot->ready.wait_until(lock, deadline, [&]() { return slot->value.has_value() || slot->failed; });
        return slot->value.has_value();
      };
      const bool have_probe = wait_for(probe_slot);
      const bool have_discovery = wait_for(discover_slot);
      const bool probe_failed = [&]() { std::lock_guard lock { probe_slot->mutex }; return probe_slot->failed; }();

      std::lock_guard lock { job->mutex };
      auto &run = job->run;
      run.elapsed_ms = since(job->began);
      run.finished_at = reachability::format_utc_iso(reachability::now_epoch_seconds());
      run.bypass = network_pin::run_outcome();
      if (have_probe) {
        // The local probe has its result: it is used even when the address discovery did not finish in time; only the steps that depend on the discovery say "timeout".
        const auto snapshot = *probe_slot->value;
        nat_discovery::discovery_t discovery;
        if (have_discovery) {
          discovery = *discover_slot->value;
        }
        else {
          discovery.enabled = true;
          run.timed_out = true;
        }
        auto results = results_from(request, snapshot, discovery);
        // Keep the plan's timings; replace what the steps found.
        std::vector<step_t> merged;
        std::vector<bool> used(results.size(), false);
        for (const auto &planned : run.steps) {
          bool replaced = false;
          for (std::size_t index = 0; index < results.size(); ++index) {
            // a step that exists once matches by its id; only the endpoint steps, one per address, also match by the address
            if (used[index] || results[index].id != planned.id || (planned.id == step::endpoint && results[index].subject != planned.subject)) continue;
            auto entry = results[index];
            entry.started_ms = planned.started_ms;
            entry.elapsed_ms = planned.checked ? planned.elapsed_ms : run.elapsed_ms - planned.started_ms;
            entry.checked = true;
            merged.push_back(std::move(entry));
            used[index] = true;
            replaced = true;
            break;
          }
          if (!replaced) {
            auto entry = planned;
            if (entry.state == step_state_e::pending || entry.state == step_state_e::running) {
              entry.state = step_state_e::skipped;
              entry.reason = "not_run";
            }
            merged.push_back(std::move(entry));
          }
        }
        // An address the plan did not know (the saved list changed during the run) is still reported.
        for (std::size_t index = 0; index < results.size(); ++index) {
          if (!used[index]) merged.push_back(results[index]);
        }
        run.steps = std::move(merged);
        if (!have_discovery) {
          for (auto &entry : run.steps) {
            if (entry.id != step::addresses && entry.id != step::cgnat) continue;
            entry.state = step_state_e::warn;
            entry.reason = "timeout";
            entry.next = "retry";
            entry.detail = nlohmann::json::object();
            entry.elapsed_ms = run.elapsed_ms - entry.started_ms;
          }
        }
        // after the timeouts: the step that timed out keeps its state and its note
        if (run.bypass.state == network_pin::state_e::fallback) {
          for (auto &entry : run.steps) {
            if (entry.id == step::addresses) entry.detail["bypass"] = "fallback";
          }
        }
        run.summary = summarize(request, snapshot, discovery, run.steps);
      }
      else {
        // The run reached its deadline, or the probe could not run: the steps that did not finish say so, and nothing is concluded from them.
        run.timed_out = !probe_failed;
        for (auto &entry : run.steps) {
          if (entry.state == step_state_e::pending || entry.state == step_state_e::running) {
            entry.state = step_state_e::warn;
            entry.reason = probe_failed ? "probe_failed" : "timeout";
            entry.next = "retry";
            entry.elapsed_ms = run.elapsed_ms - entry.started_ms;
          }
        }
      }
      run.state = run_state_e::done;
      job->finished = true;
    }
  }  // namespace

  std::shared_ptr<const run_t>
  start(const inputs_t &inputs, const request_t &request) {
    std::lock_guard lock { registry_mutex };
    if (current_job && !current_job->finished) return copy_of(*current_job);

    auto job = std::make_shared<job_t>();
    job->began = clock_t::now();
    job->run.id = new_id();
    job->run.request = request;
    job->run.started_at = reachability::format_utc_iso(reachability::now_epoch_seconds());
    const std::vector<std::string> &endpoint_inputs = inputs.saved_endpoints;
    job->run.steps = planned_steps(request, inputs.stun_enabled, endpoint_inputs);
    network_pin::begin_run(inputs.bypass);
    job->run.bypass = network_pin::run_outcome();
    current_job = job;
    std::thread { [job, inputs, request]() { run_job(job, inputs, request); } }.detach();
    return copy_of(*job);
  }

  std::shared_ptr<const run_t>
  find(const std::string &id) {
    std::shared_ptr<job_t> job;
    {
      std::lock_guard lock { registry_mutex };
      job = current_job;
    }
    if (!job) return nullptr;
    {
      std::lock_guard lock { job->mutex };
      if (!id.empty() && job->run.id != id) return nullptr;
      // A run that is going shows its elapsed time.
      if (job->run.state == run_state_e::running) {
        job->run.elapsed_ms = since(job->began);
        job->run.bypass = network_pin::run_outcome();
      }
    }
    return copy_of(*job);
  }

  bool
  wait_idle(std::chrono::milliseconds limit) {
    const auto until = clock_t::now() + limit;
    while (clock_t::now() < until) {
      std::shared_ptr<job_t> job;
      {
        std::lock_guard lock { registry_mutex };
        job = current_job;
      }
      if (!job || job->finished) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
}  // namespace network_check
