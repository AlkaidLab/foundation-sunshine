# Network check

`POST /api/network/check` starts a check of how this host can be reached from outside; `GET /api/network/check` reads it. A check is a run with steps:
each step is pending, running or has a result, and a run that cannot finish still ends, so a screen never waits on nothing.

Both calls are authenticated like the other configuration APIs. A write needs the CSRF header like any other `POST`.

## Why a run

`GET /api/network/reachability?refresh=1` probes in one blocking request of up to ten seconds and shows nothing until it is done. A client that loses the
answer (a proxy in between, a timeout, a stale session) has nothing to show. A run reports each step as it happens. The report itself is still
`GET /api/network/reachability`: the run fills the cache of that call, so a client reads it once the run is done and nothing is probed twice.

## Start

```
POST /api/network/check
```

No body. The answer is the run, with state `running`. When a run is already going, that run is returned and no second one starts.

## Read

```
GET /api/network/check
GET /api/network/check?id=<id>
```

`404` `no_run` when none has been started. Poll while `state` is `running`; every 400 ms is plenty.

```json
{
  "version": 1,
  "id": "9f2c41aa07be13d5",
  "state": "done",
  "started_at": "2026-11-18T12:00:00Z",
  "finished_at": "2026-11-18T12:00:04Z",
  "elapsed_ms": 4120,
  "timed_out": false,
  "steps": [ { "id": "listening", "subject": null, "state": "ok", "reason": null, "next": null, "checked": true, "detail": {}, "started_ms": 0, "elapsed_ms": 12 } ],
  "bypass": { "state": "pinned", "adapter": "Ethernet" },
  "summary": {
    "tcp": { "state": "reachable", "because": ["handshake_through_public_address"] },
    "udp": { "state": "unverified", "because": ["mappings_complete", "not_verified_from_outside"] },
    "cgnat": "no", "ipv4": "203.0.113.9", "ipv6": null, "suggested_address": "203.0.113.9"
  }
}
```

### Steps

In the order they are shown. `state` is `pending`, `running`, `ok`, `warn`, `fail` or `skipped`. While a run is going, `checked: true` says the probe of that step has ended and
its result comes with the run.

| `id` | what it checks | `reason` codes (with `next`) |
| --- | --- | --- |
| `listening` | The TCP ports (HTTPS, HTTP, RTSP) and the UDP stream ports this computer listens on. The UDP stream sockets open with the first session, so idle is not a fault. | `not_listening` (`restart`) |
| `firewall` | Whether an inbound rule blocks a port (Windows reads `INetFwPolicy2`; Linux and macOS cannot say). | `firewall_blocked` (`firewall`), `firewall_unknown` |
| `addresses` | The public IPv4 and IPv6 address as STUN servers see them. | `stun_no_answer`, `no_route`, `proxy_dns`, `proxy_exit_suspected`, `stun_off` (skipped) |
| `cgnat` | Whether the router's outside address is shared (RFC 6598, `100.64.0.0/10`). | `cgnat` (`mesh_or_ipv6`), `cgnat_unknown` |
| `router` | UPnP IGD answers and, per port, whether the mapping exists (`detail.missing`). | `no_igd` (`forward`), `mapping_missing` (`upnp`) |
| `external_tcp` | The outside check (`network_external_check = enabled`): a connection from the internet to the TCP ports of each public address, comparing the unique id of this host. | `not_reached_from_outside` (`forward`), `another_host_answers` (`address`), `address_not_this_computer` (`address`, the service says the entered address is not the one the internet sees from this computer), `router_proxy` (warn, no action: the service saw neither the entered address nor the router's outside address, so a proxy on the router sits on the path; `detail` has `seen_by_service` and `router_wan`; the result of TCP is never `unreachable` for it), `external_unavailable` (skipped: the service did not answer or has used up its day), `external_off` (skipped) |
| `external_udp` | UDP cannot be tried from outside by a service that speaks TCP. Always skipped. | `udp_not_verified` |
| `endpoint` | One per saved public address (`subject`): a TLS handshake against its HTTPS port. | `loopback_unverifiable` (`outside_test`), `dns_failed`, `identity_mismatch`, `tls_failed`, `address_private` (`address`), `mesh_address`, `not_tried` |

`bypass` says how the probes left this computer (`network_bypass_proxy`): `pinned` (through the physical adapter `adapter`, and answered), `fallback` (nothing answered through the physical adapter, so the probe was repeated on the system route and answered:
the physical adapter has no way out, and the `addresses` step carries `detail.bypass = "fallback"`; screens show the warning "the physical adapter has no way out, back to the system route"; the states of the steps are not changed by it), `off` (the setting is not `enabled`)
or `unavailable` (no physical adapter was found, the system refused to pin the socket, or this system cannot pin: macOS). `adapter` is `null` unless a physical adapter was found.

A run that reaches its deadline (20 seconds) ends: the steps that were still open are `warn` with `timeout` and `next: retry`, `timed_out` is `true`, and nothing is concluded from them.

### `loopback_unverifiable`

The handshake is a connection from this computer to its own public address. A router that does not loop connections back (RFC 4787 REQ-9 asks for it, many do not) refuses or ignores it,
and so does a path that has no route. That says nothing about the internet, so the step is `warn`, never `fail`. A step is `fail` only when something specific is wrong: the name does not resolve, another
device answered with a certificate that is not this host's, a server that is not TLS, or the address is private.

### Summary

`tcp.state` is `reachable` (a connection from outside was made: a handshake through a public address, or a session from a public address), `likely` (everything this computer can see says yes), `unreachable`
(something proves it cannot be: not listening, blocked by the firewall, a shared address with no IPv6) or `unknown`. A handshake that was made outranks the listener table and the firewall policy: both are
inferences, the handshake is a connection.

`tcp.state` is also `reachable` when the outside check reached this host (`because`: `outside_probe`) and `unreachable` when it did not (`not_reached_from_outside`); a service that did not answer changes nothing.

`udp.state` is `reachable` only when a session came from a public address. Otherwise it is `unverified`, never `unreachable`: a TCP handshake says nothing about UDP, and nothing here connects to a UDP port from
outside. `because` ends with `not_verified_from_outside`.

`suggested_address` is the address a client should enter: a configured address a handshake reached, else one without a known fault, else the router's outside address when it is public, else a public address of this computer.
