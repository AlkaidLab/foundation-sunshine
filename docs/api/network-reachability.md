# Network reachability

`GET /api/network/reachability` reports whether this host can be reached from
the public internet or from a mesh overlay. The check is performed by the host
itself. It never asks an external website or service.

The endpoint is authenticated exactly like the other configuration APIs, so it
is available to the panel and to the authenticated Web UI session.

## Request

```
GET /api/network/reachability
GET /api/network/reachability?refresh=1
```

The result is cached for 60 seconds. `refresh=1` (or `refresh=true`) runs the
probes again. Concurrent refreshes are serialized; the endpoint does not start
more than one router discovery or hairpin handshake at a time.

Probing is bounded: UPnP discovery waits at most 1.5 seconds, a host-name lookup
at most 2 seconds, every TLS handshake at most 2.5 seconds, and every other step
is a local read. The endpoints of the configuration are probed in parallel, so
the time does not grow with their number.

## Response (JSON v1)

```json
{
  "version": 1,
  "checked_at": "2026-11-03T01:54:56Z",
  "ports": [],
  "public": {},
  "mesh": {}
}
```

`checked_at` is UTC in `YYYY-MM-DDTHH:MM:SSZ` form.

### ports

Every Sunshine port derived from the configured base port:

| proto | offset | role | purpose |
| --- | --- | --- | --- |
| tcp | -5 | `https` | GameStream HTTPS / certificate port |
| tcp | 0 | `http` | GameStream HTTP |
| tcp | +1 | `web` | Web UI |
| tcp | +21 | `rtsp` | RTSP control channel |
| udp | +9 | `video` | video stream |
| udp | +10 | `control` | input and control |
| udp | +11 | `audio` | audio stream |
| udp | +12 | `microphone` | microphone stream |

Each entry has:

- `listening`: this Sunshine process holds a socket for the port.
- `on_demand`: `true` for the four UDP stream ports (`video`, `control`,
  `audio`, `microphone`). Sunshine opens those sockets when the first session
  starts, so an idle host has no listener on them. An idle `listening: false` on
  such a port is not a fault and does not produce `not_listening`.
- `bound`: local addresses that socket is bound to (`0.0.0.0` and `::` mean all
  interfaces).
- `firewall`: `allowed`, `blocked` or `unknown`. Windows reads the inbound
  `INetFwPolicy2` rules that match this executable or the port, intersected with
  the currently active profiles and their default inbound action. Linux and
  macOS report `unknown`.

### public

| field | meaning |
| --- | --- |
| `verdict` | `verified`, `likely`, `unknown` or `unreachable` |
| `reasons` | stable codes, see below |
| `host_public_ip` | a public address held by the default-route interface, or `null` |
| `router.protocol` | `upnp-igd` when an IGD answered, otherwise `none` |
| `router.wan_ip` | address reported by `GetExternalIPAddress`, or `null` |
| `router.wan_ip_class` | `public`, `private`, `cgnat` or `unknown` |
| `router.mappings` | one entry per port that was queried with `GetSpecificPortMappingEntry` |
| `hairpin` | TLS handshake result against `target` on the HTTPS port: the first public endpoint, otherwise the router's WAN address |
| `endpoints` | One entry per configured public endpoint, see below |
| `configured_address` | Deprecated. The first endpoint in the single-address form older clients read, see below |
| `evidence` | recent sessions that arrived from a public address, masked |

Verdict rules:

- `verified`: a session arrived from a public address within the last 7 days,
  or the hairpin handshake reached this host's own certificate and every
  required mapping exists, or the default-route interface holds a public address
  and the firewall allows the ports.
- `likely`: every required mapping exists, the WAN address is public and the
  firewall allows the ports, but no hairpin and no session evidence exists.
- `unreachable`: the WAN address is private or carrier-grade NAT, a required
  port is not listening, or the firewall blocks it.
- `unknown`: anything else, for example no IGD or incomplete mappings.

When neither `external_ip` nor `external_endpoints` is set, the missing
address is a configuration hint rather than a verdict: the public path is
`unknown` with `unset` in its reasons, even when the router reports a
private or carrier-grade NAT WAN address. A required port that is not listening
or a firewall block still makes it `unreachable`.

The Web UI port is only required when `origin_web_ui_allowed=wan`.

### endpoints

The public address a client gives to Moonlight is `host:HTTP-port`. The HTTP
port is the base port (`port` in the configuration, 47989 by default); the HTTPS
pairing port is five below it. The internal and the public port need not match:
port forwarding can map any external port to the base port. The operator
therefore enters the public address the way a host is added in Moonlight, with
the port the router exposes, and may enter several.

#### Configuration

`external_endpoints` is a list, separated by commas (semicolons, spaces and
new lines also work). Each entry is one of:

| form | example |
| --- | --- |
| DNS name | `home.example.net`, `home.example.net:60000` |
| IPv4 | `203.0.113.9`, `203.0.113.9:60000` |
| IPv6 | `[2001:db8::1]:60000`, `[2001:db8::1]` or `2001:db8::1` |

- The port is optional. Without it the entry stands for the configured base
  port. With it, the entry stands for exactly that external HTTP port.
- A port is 6 to 65535 (the HTTPS port is five below it and must exist).
- An IPv6 literal with a port is written in brackets (RFC 3986 section 3.2.2).
- No scheme, path, user name or zone identifier.
- At most 8 entries are kept; duplicates are dropped.
- The file stores the canonical form, `a.example.net:60000, [2001:db8::1]:60000`.
  One pair of outer square brackets around the whole list is accepted when
  reading.
- Entries that do not parse are dropped with a warning; a save that contains
  one is refused.

Compatibility: the single-value `external_ip` key is still read. It is used as
the only endpoint when `external_endpoints` is empty or has no valid entry; when
both are set, `external_endpoints` wins and `external_ip` is ignored. The Web UI
and the panel write `external_endpoints` and clear `external_ip` on the first
save. The pairing QR code uses the first endpoint, with its port.

#### Fields

| field | meaning |
| --- | --- |
| `input` | Canonical text as configured |
| `host`, `kind` | Host without brackets; `ip` or `hostname` |
| `port`, `port_explicit` | HTTP port the endpoint stands for, and whether the entry gave it |
| `https_port` | The port that is tested: `port` minus 5 |
| `resolved` | Resolver answers for a host name, or the IP literal itself |
| `scope` | `public`, `lan`, `mesh`, `loopback` or `unknown`. `public` when any address is public, else `mesh`, `lan`, `loopback` |
| `matches_wan` | An address of the router's family equals the router WAN address |
| `state` | `reached` or `failed` |
| `error` | `null` when reached, else why it failed, see below |
| `tested` | `{address, port, latency_ms}` of the attempt that decided the state, or `null` when none was made |
| `attempts` | Every attempt: `address`, `port`, `connected`, `identity_match`, `error`, `latency_ms`; at most 4 addresses per entry, in parallel |
| `reasons` | `address_mismatch`, `scope_lan`, `scope_mesh`, `scope_loopback` |

`state` is `reached` when a TLS handshake against `address:https_port`
completed and the certificate is this host's own (the hairpin). `error` is:

| value | meaning |
| --- | --- |
| `dns_failed` | the host name did not resolve |
| `refused` | a host answered and refused the port: the forward or the listener is wrong |
| `timeout` | nothing answered within 2.5 seconds: a firewall drop, no forward, or a router that does not loop connections back |
| `unreachable` | the network has no route to the address |
| `tls_failed` | something accepted the connection but is not a TLS server |
| `identity_mismatch` | a TLS server answered with a certificate that is not this host's: the address leads to another device |
| `not_tried` | no usable address, or the port leaves no HTTPS port |
| `failed` | any other local failure |

A `timeout` on a public endpoint is not proof that the internet cannot reach
the host: many routers do not loop connections back. The verdict therefore
stays `unknown` or `likely` in that case, never `verified`.

Effect on the verdict (see the rules above): one `reached` public endpoint is
enough for `verified` (`hairpin_verified`). Mesh, LAN and loopback endpoints are
probed and reported but do not decide the public verdict, unless every endpoint
is LAN or loopback (`address_private`, `unreachable`). When no endpoint resolves
and none is public, the verdict is `unknown` with `dns_failed`. When every
public endpoint is another address than the router's, it is `unknown` with
`address_mismatch`. Otherwise the router and hairpin rules apply.

### configured_address (deprecated)

Kept for clients written against the first version of this API. It describes the
first endpoint (`value` is the host without its port). New clients read
`endpoints`. `external_ip` accepts an IP literal or a DNS host name (a dynamic DNS name, for
example). The value is validated when the configuration is read and when it is
saved; a malformed value is not persisted and is dropped from the running
configuration with a warning.

| field | meaning |
| --- | --- |
| `value` | Normalized configured value, or `null` when unset |
| `kind` | `ip`, `hostname` or `empty` |
| `resolved` | System resolver answers for a host name; empty for an IP literal |
| `matches_wan` | A configured or resolved address equals the router WAN address |
| `reasons` | `address_mismatch`, `address_private`, `dns_failed` or `unset` |
| `hint` | `未设置公网地址时客户端使用自动发现的地址` when unset, otherwise empty |

The configured address is authoritative when it is set:

- a private value makes the public verdict `unreachable`; clients outside the
  LAN cannot use it;
- a host name that does not resolve makes the verdict `unknown` with
  `dns_failed`;
- a value that differs from the WAN address makes the verdict `unknown` with
  `address_mismatch`, unless the hairpin handshake against the configured
  address reached this host's certificate, which makes it `verified`;
- a successful hairpin handshake against the configured address makes the
  verdict `verified` even when no gateway mapping is available;
- when it matches the WAN address, the existing router and hairpin rules apply.

`hairpin.target` names the address that was tried, or `null`. Without a public
endpoint the host tests its own WAN address instead.

### mesh

| field | meaning |
| --- | --- |
| `adapters` | detected mesh interfaces with `kind`, `name`, `addresses` and `up` |
| `verdict` | `verified`, `likely`, `unknown` or `none` |
| `reasons` | stable codes |
| `listening_on_mesh` | every required port is bound to a wildcard or a mesh address |
| `web_ui_origin_allows_mesh` | the configured origin policy admits every mesh address |
| `evidence` | recent sessions from a mesh address, masked |

`kind` is one of `netbird`, `tailscale`, `zerotier`, `easytier`, `wireguard`,
`other` or `none`. It is derived from the adapter name or description and from
the address range. The `none` verdict is returned when no mesh adapter exists.

This tree classifies `100.64.0.0/10` as LAN, so the default
`origin_web_ui_allowed=lan` admits Tailscale and NetBird addresses. A policy of
`pc` blocks them and produces `origin_blocks_mesh`. A host that wants only the
session ports over the mesh can keep a LAN policy; the Web UI flag tells the UI
whether the browser origin policy also permits it.

## Reason codes

| code | meaning |
| --- | --- |
| `session_evidence` | a recent session arrived from the matching address family |
| `hairpin_verified` | the WAN hairpin reached this host's certificate |
| `host_public_ip` | the default-route interface holds a public address |
| `mappings_complete` | every required port has a gateway mapping |
| `no_igd` | no gateway answered UPnP discovery |
| `cgnat` | the WAN address is in `100.64.0.0/10` |
| `private_wan` | the WAN address is private or otherwise non-routable |
| `no_wan_ip` | the protocol reported no usable WAN address |
| `hairpin_not_tried` | no public WAN address was available for the test |
| `hairpin_unsupported` | the WAN address did not loop back to this host |
| `address_mismatch` | the configured public address is not the router WAN address |
| `address_private` | the configured public addresses are all private, loopback or otherwise non-routable |
| `dns_failed` | the configured host name did not resolve on this host |
| `unset` | neither `external_ip` nor `external_endpoints` is set; clients use the automatically discovered address |
| `firewall_unknown` | the platform cannot decide the firewall state |
| `not_listening:proto/port` | this process has no listener for the port |
| `not_listening_on_mesh:proto/port` | the listener is not bound to a mesh address |
| `firewall_blocked:proto/port` | an inbound rule blocks the port |
| `mapping_missing:proto/port` | the gateway has no mapping for the port |
| `no_mesh_adapter` | no mesh interface was detected |
| `mesh_adapter_down` | a mesh interface exists but none is up |
| `mesh_adapter_up` | a mesh interface is up and the ports are bound to it |
| `origin_blocks_mesh` | `origin_web_ui_allowed` does not admit the mesh address |

Codes are stable; the UI maps each one to an explanation and a link to the
relevant setting (enable UPnP, add a firewall rule, change
`origin_web_ui_allowed`).

## Privacy

- Evidence addresses are masked (`/24` for IPv4, `/64` for IPv6) before they
  enter the response or the log.
- `host_public_ip` and `router.wan_ip` are this host's own addresses; they are
  visible to an authenticated caller and are not written to the info log.
- The configured endpoints and their resolver answers are this host's own
  configuration; they are returned to the authenticated caller only.
- The only off-host traffic is the router discovery on the LAN and a TLS
  handshake to each address the operator configured (or, without any, to the
  router's WAN address). The handshake sends no application data and does not
  verify the peer. A host-name lookup uses the system resolver and is bounded to
  two seconds; no checking service is contacted.

## Platform state

| capability | Windows | Linux | macOS |
| --- | --- | --- | --- |
| adapters | `GetAdaptersAddresses` | `getifaddrs` | `getifaddrs` |
| own listeners | `GetExtendedTcpTable` / `GetExtendedUdpTable` | `/proc/net/*` plus `/proc/self/fd` inodes | `libproc` `PROC_PIDFDSOCKETINFO` |
| firewall | `INetFwPolicy2` rules and active profiles | `unknown` | `unknown` |
| router | UPnP IGD (`miniupnpc`) | UPnP IGD | UPnP IGD |
| hairpin | TLS against each endpoint's `address:HTTP port - 5` | same | same |

NAT-PMP and PCP are reported as `none`: no small, already vendored library
implements them in this tree.

## Example

See [network-reachability.example.json](network-reachability.example.json).
