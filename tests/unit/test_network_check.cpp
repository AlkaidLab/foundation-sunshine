#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "src/network_check.h"

namespace {
  using namespace network_check;
  using namespace std::chrono_literals;

  reachability::port_status_t
  port(std::string proto, std::uint16_t number, std::string role, bool listening = true, reachability::firewall_e firewall = reachability::firewall_e::allowed) {
    return { std::move(proto), number, std::move(role), listening, { "0.0.0.0" }, firewall };
  }

  std::vector<reachability::port_status_t>
  ports(bool listening = true, reachability::firewall_e firewall = reachability::firewall_e::allowed) {
    return {
      port("tcp", 47984, "https", listening, firewall), port("tcp", 47989, "http", listening, firewall), port("tcp", 47990, "web", listening, firewall),
      port("tcp", 48010, "rtsp", listening, firewall), port("udp", 47998, "video", false, firewall), port("udp", 47999, "control", false, firewall),
      port("udp", 48000, "audio", false, firewall), port("udp", 48001, "microphone", false, firewall),
    };
  }

  reachability::router_status_t
  igd(bool mappings) {
    reachability::router_status_t router;
    router.protocol = "upnp-igd";
    router.wan_ip = "203.0.113.9";
    router.wan_ip_class = reachability::ip_class_e::public_address;
    for (const auto &entry : ports()) {
      if (entry.role == "web") continue;
      router.mappings.push_back({ entry.proto, entry.port, mappings, mappings ? "10.0.1.10" : "" });
    }
    return router;
  }

  constexpr std::int64_t NOW = 1795000000;

  // The verdict the host would have computed from the facts in the snapshot.
  reachability::snapshot_t
  settle(reachability::snapshot_t snapshot) {
    reachability::public_inputs_t inputs;
    inputs.ports = snapshot.ports;
    inputs.router = snapshot.public_reachability.router;
    inputs.endpoints = snapshot.public_reachability.endpoints;
    inputs.evidence = snapshot.public_reachability.evidence;
    auto result = reachability::evaluate_public(inputs, NOW);
    result.endpoints = inputs.endpoints;
    result.evidence = inputs.evidence;
    snapshot.public_reachability = std::move(result);
    return snapshot;
  }

  reachability::snapshot_t
  healthy() {
    reachability::snapshot_t snapshot;
    snapshot.checked_at = "2026-11-18T00:00:00Z";
    snapshot.ports = ports();
    snapshot.public_reachability.router = igd(true);
    return settle(snapshot);
  }

  nat_discovery::discovery_t
  stun(const std::string &v4 = "203.0.113.9", const std::string &v6 = "") {
    nat_discovery::discovery_t discovery;
    discovery.enabled = true;
    discovery.ipv4.public_address = v4;
    if (!v4.empty()) discovery.ipv4.distinct_addresses = { v4 };
    discovery.ipv6.public_address = v6;
    if (!v6.empty()) discovery.ipv6.distinct_addresses = { v6 };
    nat_discovery::server_observation_t server;
    server.server = "stun.miwifi.com:3478";
    server.answered = !v4.empty();
    discovery.ipv4.servers.push_back(server);
    return discovery;
  }

  reachability::endpoint_result_t
  endpoint(const std::string &input, bool reached, const std::string &error = {}, bool inconclusive = false, reachability::scope_e scope = reachability::scope_e::public_address) {
    reachability::endpoint_result_t result;
    result.input = input;
    result.host = input;
    result.port = 47989;
    result.https_port = 47984;
    result.scope = scope;
    result.reached = reached;
    result.error = error;
    result.inconclusive = inconclusive;
    result.resolved = { input };
    return result;
  }

  const step_t *
  find(const std::vector<step_t> &steps, std::string_view id, const std::string &subject = {}) {
    for (const auto &entry : steps) {
      if (entry.id == id && (subject.empty() || entry.subject == subject)) return &entry;
    }
    return nullptr;
  }

  std::vector<step_t>
  steps_of(const reachability::snapshot_t &snapshot, const nat_discovery::discovery_t &discovery, const request_t &request = {}) {
    return results_from(request, snapshot, discovery);
  }
}  // namespace

TEST(NetworkCheckPlan, ListsEveryStepPendingInTheOrderTheyAreShown) {
  const auto steps = planned_steps({}, true, { "a.example.net:60000" });
  std::vector<std::string> ids;
  for (const auto &entry : steps) ids.push_back(entry.id);
  EXPECT_EQ(ids, (std::vector<std::string> { "listening", "firewall", "addresses", "cgnat", "router", "endpoint", "external_tcp", "external_udp" }));
  for (const auto &entry : steps) EXPECT_EQ(entry.state, step_state_e::pending);
  EXPECT_EQ(find(steps, "endpoint")->subject, "a.example.net:60000");
}

TEST(NetworkCheckPlan, WithoutStunTheAddressStepIsSkippedFromTheStart) {
  const auto steps = planned_steps({}, false, {});
  EXPECT_EQ(find(steps, "addresses")->state, step_state_e::skipped);
  EXPECT_EQ(find(steps, "addresses")->reason, "stun_off");
}

TEST(NetworkCheckSteps, AHealthyHostIsOkEverywhere) {
  const auto steps = steps_of(healthy(), stun());
  EXPECT_EQ(steps.size(), 7U);
  for (const auto &entry : steps) {
    if (entry.id == "external_tcp" || entry.id == "external_udp") continue;  // nothing from outside has tried: see the outside tests
    EXPECT_EQ(entry.state, step_state_e::ok) << entry.id << " " << entry.reason;
  }
}

TEST(NetworkCheckSteps, ANotListeningPortFailsWithTheRestartAction) {
  auto snapshot = healthy();
  snapshot.ports = ports(false);
  const auto steps = steps_of(snapshot, stun());
  const auto *listening = find(steps, "listening");
  EXPECT_EQ(listening->state, step_state_e::fail);
  EXPECT_EQ(listening->reason, "not_listening");
  EXPECT_EQ(listening->next, "restart");
  EXPECT_EQ(listening->detail["missing"].size(), 3U);  // https, http and rtsp; the web port and the idle UDP ports are not faults
}

TEST(NetworkCheckSteps, ABlockedFirewallNamesThePortsAndTheAction) {
  auto snapshot = healthy();
  snapshot.ports.at(1).firewall = reachability::firewall_e::blocked;
  const auto steps_for_step = steps_of(snapshot, stun());
  const auto *step = find(steps_for_step, "firewall");
  EXPECT_EQ(step->state, step_state_e::fail);
  EXPECT_EQ(step->next, "firewall");
  EXPECT_EQ(step->detail["blocked"][0], "tcp/47989");
}

TEST(NetworkCheckSteps, NoStunAnswerSaysWhichSortOfFailureItWas) {
  auto discovery = stun("");
  const auto steps_for_step = steps_of(healthy(), discovery);
  const auto *step = find(steps_for_step, "addresses");
  EXPECT_EQ(step->state, step_state_e::warn);
  EXPECT_EQ(step->reason, "stun_no_answer");
}

TEST(NetworkCheckSteps, AProxyFakeIpResolverAndServersThatDisagreeAreNamedAsAProxy) {
  auto discovery = stun();
  discovery.ipv4.notes.push_back("dns_fake_ip");
  EXPECT_EQ(find(steps_of(healthy(), discovery), "addresses")->reason, "proxy_dns");
  discovery = stun();
  discovery.ipv4.distinct_addresses = { "203.0.113.9", "206.223.84.176" };
  const auto steps_for_step = steps_of(healthy(), discovery);
  const auto *step = find(steps_for_step, "addresses");
  EXPECT_EQ(step->reason, "proxy_exit_suspected");
  EXPECT_EQ(step->next, "");
}

TEST(NetworkCheckSteps, ASharedAddressIsAFailureWithAWayOut) {
  auto snapshot = healthy();
  snapshot.public_reachability.router.wan_ip = "100.72.1.2";
  snapshot.public_reachability.router.wan_ip_class = reachability::ip_class_e::cgnat;
  const auto steps_for_step = steps_of(snapshot, stun("198.51.100.4"));
  const auto *step = find(steps_for_step, "cgnat");
  EXPECT_EQ(step->state, step_state_e::fail);
  EXPECT_EQ(step->reason, "cgnat");
  EXPECT_EQ(step->next, "mesh_or_ipv6");
}

TEST(NetworkCheckSteps, NoRouterAndMissingMappingsAreSeparateFindings) {
  auto snapshot = healthy();
  snapshot.public_reachability.router = {};
  EXPECT_EQ(find(steps_of(snapshot, stun()), "router")->reason, "no_igd");
  snapshot = healthy();
  snapshot.public_reachability.router = igd(false);
  const auto steps_for_step = steps_of(snapshot, stun());
  const auto *step = find(steps_for_step, "router");
  EXPECT_EQ(step->reason, "mapping_missing");
  EXPECT_EQ(step->next, "upnp");
  EXPECT_EQ(step->detail["missing"].size(), 7U);  // three TCP and four UDP ports; the web port is not mapped
}

TEST(NetworkCheckSteps, EachEndpointIsOneStepWithItsOwnFinding) {
  auto snapshot = healthy();
  snapshot.public_reachability.endpoints = {
    endpoint("203.0.113.9:60000", true),
    endpoint("198.51.100.7", false, "timeout", true),
    endpoint("gone.example.net", false, "dns_failed"),
    endpoint("203.0.113.55", false, "identity_mismatch"),
    endpoint("10.0.1.10", false, "timeout", false, reachability::scope_e::lan),
  };
  const auto steps = steps_of(snapshot, stun());
  EXPECT_EQ(find(steps, "endpoint", "203.0.113.9:60000")->state, step_state_e::ok);
  const auto *silent = find(steps, "endpoint", "198.51.100.7");
  EXPECT_EQ(silent->state, step_state_e::warn);
  EXPECT_EQ(silent->reason, "loopback_unverifiable");
  EXPECT_EQ(silent->next, "outside_test");
  EXPECT_EQ(find(steps, "endpoint", "gone.example.net")->reason, "dns_failed");
  EXPECT_EQ(find(steps, "endpoint", "203.0.113.55")->reason, "identity_mismatch");
  EXPECT_EQ(find(steps, "endpoint", "10.0.1.10")->reason, "address_private");
  EXPECT_EQ(find(steps, "endpoint", "10.0.1.10")->state, step_state_e::fail);
}

TEST(NetworkCheckSummary, AHandshakeThroughAPublicAddressMakesTcpReachableEvenWhenLocalGuessesDisagree) {
  auto snapshot = healthy();
  snapshot.ports = ports(false, reachability::firewall_e::blocked);
  snapshot.public_reachability.endpoints = { endpoint("203.0.113.9", true) };
  snapshot = settle(snapshot);
  const auto discovery = stun();
  const auto steps = steps_of(snapshot, discovery);
  const auto summary = summarize({}, snapshot, discovery, steps);
  EXPECT_EQ(summary.tcp.state, protocol_state_e::reachable);
  EXPECT_EQ(summary.tcp.because.front(), "handshake_through_public_address");
  EXPECT_EQ(summary.suggested_address, "203.0.113.9");
  // The handshake says nothing about UDP, and nothing here can: it is not verified, and it is not red.
  EXPECT_EQ(summary.udp.state, protocol_state_e::unverified);
}

TEST(NetworkCheckSummary, UdpIsNeverRedAndAlwaysSaysItWasNotVerifiedFromOutside) {
  const auto snapshot = healthy();
  const auto discovery = stun();
  auto summary = summarize({}, snapshot, discovery, steps_of(snapshot, discovery));
  EXPECT_EQ(summary.udp.state, protocol_state_e::unverified);
  EXPECT_EQ(summary.udp.because, (std::vector<std::string> { "mappings_complete", "not_verified_from_outside" }));
  auto blocked = healthy();
  blocked.ports = ports(true, reachability::firewall_e::blocked);
  summary = summarize({}, blocked, discovery, steps_of(blocked, discovery));
  EXPECT_EQ(summary.udp.state, protocol_state_e::unverified);
  auto shared = healthy();
  shared.public_reachability.router.wan_ip = "100.72.1.2";
  shared.public_reachability.router.wan_ip_class = reachability::ip_class_e::cgnat;
  summary = summarize({}, shared, stun("198.51.100.4"), steps_of(shared, stun("198.51.100.4")));
  EXPECT_EQ(summary.udp.state, protocol_state_e::unverified);
  EXPECT_EQ(summary.tcp.state, protocol_state_e::unreachable);
}

TEST(NetworkCheckSummary, ASessionFromAPublicAddressProvesBothKindsOfPort) {
  auto snapshot = healthy();
  snapshot.public_reachability.evidence = { { "client-connected-from-public", "198.51.100.0", reachability::format_utc_iso(NOW - 3600) } };
  snapshot = settle(snapshot);
  const auto discovery = stun();
  const auto summary = summarize({}, snapshot, discovery, steps_of(snapshot, discovery));
  EXPECT_EQ(summary.tcp.state, protocol_state_e::reachable);
  EXPECT_EQ(summary.udp.state, protocol_state_e::reachable);
}

TEST(NetworkCheckSummary, IpV6IsAWayOutOfASharedAddress) {
  auto snapshot = healthy();
  snapshot.public_reachability.router.wan_ip = "100.72.1.2";
  snapshot.public_reachability.router.wan_ip_class = reachability::ip_class_e::cgnat;
  const auto discovery = stun("198.51.100.4", "2001:db8::10");
  const auto summary = summarize({}, snapshot, discovery, steps_of(snapshot, discovery));
  EXPECT_NE(summary.tcp.state, protocol_state_e::unreachable);
}

TEST(NetworkCheckJson, ContractShapeIsStable) {
  run_t run;
  run.id = "abc";
  run.state = run_state_e::done;
  run.started_at = "2026-11-18T00:00:00Z";
  run.finished_at = "2026-11-18T00:00:03Z";
  const auto snapshot = healthy();
  const auto discovery = stun();
  run.steps = steps_of(snapshot, discovery);
  run.summary = summarize({}, snapshot, discovery, run.steps);
  const auto json = to_json(run);
  EXPECT_EQ(json["version"], 1);
  EXPECT_EQ(json["state"], "done");
  EXPECT_TRUE(json["timed_out"].is_boolean());
  for (const auto *key : { "id", "started_at", "finished_at", "elapsed_ms", "steps", "summary" }) EXPECT_TRUE(json.contains(key)) << key;
  for (const auto *key : { "id", "subject", "state", "reason", "next", "checked", "detail", "started_ms", "elapsed_ms" }) EXPECT_TRUE(json["steps"][0].contains(key)) << key;
  EXPECT_EQ(json["summary"]["tcp"]["state"], "likely");
  EXPECT_TRUE(json["summary"]["tcp"]["because"].is_array());
}

TEST(NetworkCheckRun, StepsRunThenResultsAndOnlyOneRunAtATime) {
  inputs_t inputs;
  std::atomic<int> probes { 0 };
  inputs.probe = [&](const request_t &, const std::function<void(std::string_view, bool)> &progress) {
    ++probes;
    progress("listening", false);
    std::this_thread::sleep_for(120ms);
    progress("listening", true);
    std::this_thread::sleep_for(60ms);
    return healthy();
  };
  inputs.discover = [](const request_t &) { return stun(); };
  inputs.saved_endpoints = { "203.0.113.9" };

  const auto first = start(inputs, {});
  EXPECT_EQ(first->state, run_state_e::running);
  EXPECT_EQ(find(first->steps, "listening")->state, step_state_e::pending);
  const auto again = start(inputs, {});
  EXPECT_EQ(again->id, first->id);

  std::this_thread::sleep_for(60ms);
  const auto during = network_check::find(first->id);
  ASSERT_NE(during, nullptr);
  EXPECT_EQ(find(during->steps, "listening")->state, step_state_e::running);

  ASSERT_TRUE(wait_idle(5s));
  const auto done = network_check::find(first->id);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(done->state, run_state_e::done);
  EXPECT_FALSE(done->timed_out);
  EXPECT_EQ(probes.load(), 1);
  EXPECT_EQ(find(done->steps, "listening")->state, step_state_e::ok);
  EXPECT_EQ(find(done->steps, "router")->state, step_state_e::ok);
  EXPECT_NE(done->summary.tcp.state, protocol_state_e::unknown);
  // The plan named one endpoint; the probe found none, and the run says it was not run instead of dropping it.
  EXPECT_EQ(find(done->steps, "endpoint", "203.0.113.9")->reason, "not_run");
}

TEST(NetworkCheckRun, ARunThatPassesItsDeadlineEndsAndNamesTheOpenSteps) {
  inputs_t inputs;
  inputs.deadline = 150ms;
  inputs.probe = [](const request_t &, const std::function<void(std::string_view, bool)> &progress) {
    progress("listening", false);
    std::this_thread::sleep_for(700ms);
    return healthy();
  };
  inputs.discover = [](const request_t &) { return stun(); };
  const auto first = start(inputs, {});
  ASSERT_TRUE(wait_idle(2s));
  const auto done = network_check::find(first->id);
  ASSERT_NE(done, nullptr);
  EXPECT_EQ(done->state, run_state_e::done);
  EXPECT_TRUE(done->timed_out);
  EXPECT_LT(done->elapsed_ms, 600);
  const auto *listening = find(done->steps, "listening");
  EXPECT_EQ(listening->state, step_state_e::warn);
  EXPECT_EQ(listening->reason, "timeout");
  EXPECT_EQ(listening->next, "retry");
  EXPECT_EQ(done->summary.tcp.state, protocol_state_e::unknown);
  std::this_thread::sleep_for(800ms);  // the late probe finishes into a slot nobody reads
}

// ---- the outside steps (net-check-1118) ----

namespace {
  reachability::endpoint_result_t
  with_outside(reachability::endpoint_result_t entry, const std::string &state, const std::string &reason = {}) {
    entry.outside.asked = true;
    entry.outside.state = state;
    entry.outside.reason = reason;
    return entry;
  }
}  // namespace

TEST(NetworkCheckOutside, WithoutTheCheckTheOutsideStepsSaySoAndUdpIsNeverVerified) {
  const auto steps = steps_of(healthy(), stun());
  EXPECT_EQ(find(steps, "external_tcp")->state, step_state_e::skipped);
  EXPECT_EQ(find(steps, "external_tcp")->reason, "external_off");
  EXPECT_EQ(find(steps, "external_udp")->reason, "udp_not_verified");
}

TEST(NetworkCheckOutside, AConnectionFromOutsideMakesTcpReachableAndUdpStaysNotVerified) {
  auto snapshot = healthy();
  snapshot.public_reachability.endpoints = { with_outside(endpoint("203.0.113.9:60000", false, "timeout", true), "reachable") };
  snapshot = settle(snapshot);
  const auto discovery = stun();
  const auto steps = steps_of(snapshot, discovery);
  EXPECT_EQ(find(steps, "external_tcp")->state, step_state_e::ok);
  EXPECT_EQ(find(steps, "endpoint", "203.0.113.9:60000")->state, step_state_e::ok);
  const auto summary = summarize({}, snapshot, discovery, steps);
  EXPECT_EQ(summary.tcp.state, protocol_state_e::reachable);
  EXPECT_EQ(summary.tcp.because.front(), "outside_probe");
  EXPECT_EQ(summary.udp.state, protocol_state_e::unverified);
}

TEST(NetworkCheckOutside, EachKindOfOutsideAnswerHasItsOwnFindingAndNextAction) {
  const auto step_for = [](const std::string &state, const std::string &reason = {}) {
    auto snapshot = healthy();
    snapshot.public_reachability.endpoints = { with_outside(endpoint("203.0.113.9:60000", false, "timeout", true), state, reason) };
    const auto steps = steps_of(snapshot, stun());
    return *find(steps, "external_tcp");
  };
  EXPECT_EQ(step_for("closed").reason, "not_reached_from_outside");
  EXPECT_EQ(step_for("closed").next, "forward");
  EXPECT_EQ(step_for("timeout").state, step_state_e::fail);
  EXPECT_EQ(step_for("another_host").reason, "another_host_answers");
  EXPECT_EQ(step_for("unavailable", "ip_mismatch").reason, "address_not_this_computer");
  EXPECT_EQ(step_for("unavailable", "ip_mismatch").state, step_state_e::warn);
  EXPECT_EQ(step_for("unavailable", "dns_mismatch").next, "address");
  // a service that does not answer is not a failure of the host
  const auto down = step_for("unavailable", "no_answer");
  EXPECT_EQ(down.state, step_state_e::skipped);
  EXPECT_EQ(down.reason, "external_unavailable");
  EXPECT_EQ(step_for("unavailable", "rate_limited").reason, "external_unavailable");
}

TEST(NetworkCheckOutside, ABlockedConnectionFromOutsideMakesTcpUnreachableButAnUnavailableServiceDoesNot) {
  auto snapshot = healthy();
  snapshot.public_reachability.endpoints = { with_outside(endpoint("203.0.113.9:60000", false, "timeout", true), "timeout") };
  snapshot = settle(snapshot);
  auto discovery = stun();
  auto summary = summarize({}, snapshot, discovery, steps_of(snapshot, discovery));
  EXPECT_EQ(summary.tcp.state, protocol_state_e::unreachable);
  EXPECT_EQ(summary.tcp.because.front(), "not_reached_from_outside");
  auto unknown = healthy();
  unknown.public_reachability.endpoints = { with_outside(endpoint("203.0.113.9:60000", false, "timeout", true), "unavailable", "no_answer") };
  unknown = settle(unknown);
  summary = summarize({}, unknown, discovery, steps_of(unknown, discovery));
  EXPECT_NE(summary.tcp.state, protocol_state_e::unreachable);
}
