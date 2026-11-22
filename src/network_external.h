/**
 * @file src/network_external.h
 * @brief The outside check of a public address: ask a service on the internet to connect to it over TCP and say whether this host answers.
 *
 * A computer cannot prove from inside that the internet reaches it: a router without NAT loopback refuses its own outside address, a proxy's tunnel reroutes the probe, a dynamic name may point
 * elsewhere. So the host asks the Worker behind the telemetry endpoint (POST /v1/reach-probe, see the Worker's docs/reach-probe.md): it connects from outside to the caller's own address and the TCP
 * ports named here, reads GameStream's /serverinfo on the HTTP port, and compares the uniqueid with the one sent. UDP cannot be tried that way and is always reported as not verified.
 *
 * Only the address the person entered and the ports of this host leave the computer, and only when this check is on (config `network_external_check`).
 * A service that does not answer is "not available", and the result then rests on the host's own evidence: it is never turned into "not reachable".
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "network_reachability.h"

namespace network_external {
  /// What one outside check of one address asks.
  struct request_t {
    std::string install_id;  ///< a UUID: the brake per install of the service
    std::string uniqueid;  ///< this host's GameStream uniqueid
    std::string target;  ///< the address as entered: an IP literal or a host name, no port
    std::vector<reachability::external_port_t> ports;  ///< the TCP ports as the internet reaches them
  };

  struct tcp_port_t {
    std::string role;
    std::uint16_t port = 0;
    std::string state;  ///< open, closed, timeout or blocked
    std::string identity;  ///< match, mismatch, unverified or not_http (HTTP port only)
    std::int64_t ms = 0;
  };

  enum class availability_e {
    answered,  ///< the service connected and told what it found
    refused,  ///< the service refused the target (403): the address is not the one the internet sees from this computer
    unavailable,  ///< off, no answer, rate limited or an answer that is not one: the outside evidence is missing, nothing more
  };

  struct result_t {
    availability_e availability = availability_e::unavailable;
    /// why not: `disabled`, `no_answer`, `rate_limited`, `bad_answer`, `bad_request`, or the service's reason of a refusal (`ip_mismatch`, `dns_mismatch`, `dns_failed`)
    std::string reason;
    std::vector<tcp_port_t> tcp;
    long retry_after_s = 0;
    std::string caller;  ///< the address the service saw the request come from (it says so in an answer and in a refusal); empty when it did not
  };

  /// The JSON body of the request (v 1).
  std::string
  build_request(const request_t &request);

  /// The answer of the service: its HTTP status (0: nothing answered), the Retry-After and the body.
  result_t
  parse_response(int status, long retry_after_s, const std::string &body);

  /// One blocking POST; the host supplies the real one (telemetry::post_once), tests a fake.
  using post_fn = std::function<int(const std::string &url, const std::string &body, std::string &answer, long &retry_after_s)>;

  /// Ask the service. `endpoint` is its base URL.
  result_t
  probe(const std::string &endpoint, const request_t &request, const post_fn &post);

  /**
   * @brief The state of a TCP port as the check reports it: `reachable` (open, and on the HTTP port the uniqueid matched; a connection to another port only proves a listener),
   *        `another_host` (open on the HTTP port but another uniqueid answered), `closed`, `timeout`, `unknown`.
   */
  std::string
  tcp_summary(const result_t &result);

  /// The configuration value: "enabled" or "disabled" (anything else is disabled).
  bool
  enabled(std::string_view setting);

  /// The service that is asked when the configuration names none (`network_external_endpoint`).
  inline constexpr std::string_view DEFAULT_ENDPOINT = "https://reach.alkaidlab.com";

  /// The base URL to ask: the configured one without a trailing slash, or the built-in service when it is empty.
  std::string
  endpoint_of(std::string_view configured);

  /**
   * @brief What one answer is worth and how long it is kept, so a person who presses "check" again does not ask the service again. An answer (an address that was reached, or one that was not for a
   *        reason the service names) and a service that is out of budget or does not answer are kept 10 minutes; a connection that found a closed port is kept one minute, so that a person who has just
   *        opened the port and checks again is told the truth.
   */
  long
  ttl_seconds(const result_t &result);

  /// The answers of the last minutes, by address and ports (never by install or unique id). Thread safe; the clock is the caller's.
  class cache_t {
  public:
    std::optional<result_t>
    find(const std::string &key, std::int64_t now_s);
    void
    put(const std::string &key, const result_t &result, std::int64_t now_s);
    void
    clear();

  private:
    struct entry_t {
      result_t result;
      std::int64_t expires_at = 0;
      std::uint64_t stored_at = 0;  ///< the order in which answers were stored
    };
    std::uint64_t stored_counter_ = 0;
    std::mutex mutex_;
    std::map<std::string, entry_t> entries_;
  };

  /// The cache key of a request: the address and the ports asked, in the order given.
  std::string
  cache_key(const request_t &request);
}  // namespace network_external
