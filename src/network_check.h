/**
 * @file src/network_check.h
 * @brief The network check as a run with steps: what was checked, what each step found, and what to do next.
 *
 * One run covers the listeners of this computer, the firewall, the public IPv4 and IPv6 addresses (STUN), the
 * carrier-grade NAT, the router (UPnP IGD), and the public addresses of the configuration (a TLS handshake each). The result keeps TCP and UDP apart:
 * Sunshine's session setup is TCP (HTTP, HTTPS, RTSP), its streams are UDP, and a router that forwards one does not forward the other.
 *
 * A run is asynchronous. `start()` returns at once, the steps move from pending to running to a result, and `GET /api/network/check` reads the state, so a
 * screen can show the steps as they happen and a run that cannot finish still ends: every run has a deadline, and a step that does not finish by then
 * is reported as `timeout` instead of leaving the screen waiting. See docs/api/network-check.md.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nat_discovery.h"
#include "network_pin.h"
#include "network_reachability.h"

namespace network_check {
  /// Version of the JSON contract served by /api/network/check.
  constexpr int JSON_VERSION = 1;

  /// A run that has not finished by now ends; the steps that are still open report `timeout`.
  constexpr std::chrono::milliseconds DEADLINE { 20000 };

  enum class step_state_e {
    pending,  ///< not started
    running,
    ok,  ///< checked, nothing to do
    warn,  ///< checked, something to know or a limit of this check (inconclusive)
    fail,  ///< checked, a real problem
    skipped,  ///< not run, with the reason
  };

  /// The steps of a run, in the order they are shown.
  namespace step {
    constexpr std::string_view listening = "listening";  ///< the ports this computer listens on (TCP and UDP)
    constexpr std::string_view firewall = "firewall";
    constexpr std::string_view addresses = "addresses";  ///< public IPv4 and IPv6 as the internet sees them
    constexpr std::string_view cgnat = "cgnat";
    constexpr std::string_view router = "router";  ///< UPnP and the port mappings, TCP and UDP
    constexpr std::string_view endpoint = "endpoint";  ///< one per public address: a TLS handshake
    constexpr std::string_view external_tcp = "external_tcp";  ///< a connection from the internet to the TCP ports (the outside check)
    constexpr std::string_view external_udp = "external_udp";  ///< UDP cannot be tried from outside: always not verified
  }  // namespace step

  struct step_t {
    std::string id;
    std::string subject;  ///< the address for an `endpoint` step; empty for the others
    step_state_e state = step_state_e::pending;
    /// Why: a stable code (`not_listening`, `firewall_blocked`, `cgnat`, `loopback_unverifiable`, ...); empty for a plain ok.
    std::string reason;
    /// The thing to do about it: a stable action code the screens turn into a sentence and a button (`restart`, `firewall`, `forward`, `upnp`, `outside_test`, `mesh_or_ipv6`, `address`, `interface`, `bypass_off`, ...). Empty when there is nothing to do.
    std::string next;
    nlohmann::json detail = nlohmann::json::object();
    bool checked = false;  ///< the probe of this step has ended; its result comes with the summary of the run
    std::int64_t started_ms = 0;  ///< since the run began; 0 until it starts
    std::int64_t elapsed_ms = 0;
  };

  enum class protocol_state_e {
    reachable,  ///< a connection from outside was made, or a session came from a public address
    likely,  ///< everything this computer can see says yes; nothing outside has tried
    unreachable,  ///< something proves it cannot be (TCP only: not listening, blocked, shared address)
    unverified,  ///< UDP only: nothing has tried it from outside, and this computer cannot tell
    unknown,
  };

  struct protocol_summary_t {
    protocol_state_e state = protocol_state_e::unknown;
    std::vector<std::string> because;  ///< the reasons, in the order of weight
  };

  struct summary_t {
    protocol_summary_t tcp;
    protocol_summary_t udp;
    std::string cgnat = "unknown";  ///< "yes", "no" or "unknown"
    std::string ipv4;  ///< the public IPv4 address as the internet sees it; empty when unknown
    std::string ipv6;
    std::string suggested_address;  ///< the address a client should enter (host or host:port); empty when none
  };

  /// What a run was asked to do. Nothing yet: a run checks the saved public addresses.
  struct request_t {};

  enum class run_state_e {
    running,
    done,
  };

  struct run_t {
    std::string id;
    run_state_e state = run_state_e::running;
    std::string started_at;
    std::string finished_at;
    std::int64_t elapsed_ms = 0;
    request_t request;
    std::vector<step_t> steps;
    summary_t summary;
    network_pin::outcome_t bypass;  ///< whether the probes left through the physical adapter (`bypass` in the JSON)
    bool timed_out = false;
  };

  /// Step ids a run starts with (endpoint steps are added as the addresses are known).
  std::vector<step_t>
  planned_steps(const request_t &request, bool stun_enabled, const std::vector<std::string> &endpoint_inputs);

  /// The result of every step from what the probes found. Pure: no I/O.
  std::vector<step_t>
  results_from(const request_t &request, const reachability::snapshot_t &snapshot, const nat_discovery::discovery_t &discovery);

  /// TCP and UDP and the rest of the summary, from the finished steps and what the probes found. Pure.
  summary_t
  summarize(const request_t &request, const reachability::snapshot_t &snapshot, const nat_discovery::discovery_t &discovery, const std::vector<step_t> &steps);

  /// The JSON contract of a run.
  nlohmann::json
  to_json(const run_t &run);

  std::string_view
  to_string(step_state_e value);
  std::string_view
  to_string(protocol_state_e value);

  /// What the runner needs from the host; tests supply fakes.
  struct inputs_t {
    /// Runs the reachability probe; `progress` is called as the stages start and end.
    std::function<reachability::snapshot_t(const request_t &, const std::function<void(std::string_view, bool)> &progress)> probe;
    /// Runs the STUN discovery.
    std::function<nat_discovery::discovery_t(const request_t &)> discover;
    bool stun_enabled = true;
    /// The probes leave through the physical adapter, not a proxy tunnel (config `network_bypass_proxy`); the probes themselves take it from their own options.
    bool bypass = false;
    /// How long a run may take before its open steps report `timeout`.
    std::chrono::milliseconds deadline = DEADLINE;
    /// The endpoints of the saved configuration, for the planned steps when the request names none.
    std::vector<std::string> saved_endpoints;
  };

  /// Starts a run on its own thread and returns its first state at once. A run that is still going is returned instead of starting another.
  std::shared_ptr<const run_t>
  start(const inputs_t &inputs, const request_t &request);

  /// The run with this id, or the latest when `id` is empty; null when none. The returned copy is a consistent snapshot.
  std::shared_ptr<const run_t>
  find(const std::string &id);

  /// Waits for the current run to end; for tests.
  bool
  wait_idle(std::chrono::milliseconds limit);
}  // namespace network_check
