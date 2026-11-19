# Network overview

`GET /api/network/overview` describes the host's network in one answer: its
interfaces with their IPv4 and IPv6 addresses, the public IPv4 and IPv6
addresses seen by STUN servers, the NAT mapping behaviour, whether the WAN is a
carrier-grade NAT, the UPnP router state, the firewall, and a per-family
reachability verdict. The panel page 网络概览 and the doctor (体检) read it.

Unlike `GET /api/network/reachability`, this endpoint **contacts external STUN
servers** (RFC 8489), and only when it is requested. Configuration key
`stun_servers = none` turns that off; the rest of the answer still works.

## Privacy and load

The STUN servers are run by third parties (Xiaomi, Bilibili, Douyu, Mango TV,
Cloudflare, Google). A Binding request carries no payload, but the server
learns the host's public IP address and the time of the request.

- STUN runs **only on demand**: when 网络概览 is opened, on 重新检测, or when
  the doctor runs its network group. Nothing runs it in the background or on a
  timer, and the host does not run it at start-up.
- The result is kept **10 minutes**; opening the page again inside that time
  contacts nobody. `refresh=1` (重新检测) asks again.
- The servers this run contacted are listed in `stun.contacted` and on the
  page (the privacy line at the top and the STUN 服务器 table).
- `stun_servers = none` turns STUN off; then no server is contacted at all.

The endpoint is authenticated exactly like the other configuration APIs.

## Request

```
GET /api/network/overview
GET /api/network/overview?refresh=1
```

The STUN part is cached for 10 minutes, the reachability part for 60 seconds.
`refresh=1` (or `refresh=true`) runs both again. The two parts run in parallel.

Time bounds: host-name lookups 2 s; STUN retransmission per RFC 8489 section
6.2.1 with RTO 500 ms and Rc reduced from 7 to 3 for an interactive page
(requests at 0, 500 and 1500 ms, a transaction fails at 2500 ms); all servers of
a family are asked at once; IPv4 and IPv6 run in parallel; the mapping tests II
and III add at most 2.5 s each. Worst case about 9.5 s; the Linux run below took
6.2 s including the reachability probes.

## Response (JSON v1)

```json
{
  "version": 1,
  "checked_at": "2026-11-04T09:05:12Z",
  "reachability_checked_at": "2026-11-04T09:05:12Z",
  "stun_enabled": true,
  "stun": { "enabled": true, "region": "china", "contacted": ["stun.miwifi.com:3478"], "cache_seconds": 600 },
  "interfaces": [],
  "ipv4": {},
  "ipv6": {},
  "cgnat": {},
  "upstream_nat": {},
  "upnp": {},
  "hairpin": {},
  "firewall": {},
  "reachability": {}
}
```

A complete example is [network-overview.example.json](network-overview.example.json).

`stun.region` is `china` or `global` (the order of the built-in list, below) or
`configured` (`stun_servers` is set); `stun.contacted` lists every server
`host:port` this run sent a request to; `stun.cache_seconds` is 600.
Consumers must ignore fields they do not know; a breaking change raises `version`.

### interfaces

One entry per adapter (loopback excluded):

| field | meaning |
| --- | --- |
| `name`, `description` | adapter name; `description` is empty outside Windows |
| `up` | operational |
| `egress` | the host leaves for the internet from an address of this adapter (the source address the routing table picks for the first STUN server) |
| `virtual` | a container, VM or tunnel bridge by name (`br-`, `docker`, `virbr`, `veth`, `vEthernet` …) |
| `mesh` | `tailscale`, `zerotier`, `netbird`, `easytier`, `wireguard`, `other` or `none` (same classification as the reachability check) |
| `addresses[]` | `address`, `family` (`ipv4`/`ipv6`), `prefix_length`, `scope` |

`scope` is one of `global`, `private` (RFC 1918), `shared` (100.64.0.0/10,
RFC 6598), `ula` (fc00::/7, RFC 4193), `link-local`, `loopback`, `other`
(documentation, benchmarking, multicast, reserved).

IPv6 addresses also carry:

- `lifetime`: `stable`, `temporary` or `unknown`. A temporary address is an
  RFC 8981 privacy address that rotates; it must not be given to clients.
  - Linux: the `IFA_F_TEMPORARY` flag in `/proc/net/if_inet6` (kernel
    `include/uapi/linux/if_addr.h`, `net/ipv6/addrconf.c` `if6_seq_show`).
  - Windows: `IP_ADAPTER_UNICAST_ADDRESS.SuffixOrigin == IpSuffixOriginRandom`
    **and** `Flags` without `IP_ADAPTER_ADDRESS_DNS_ELIGIBLE`. Windows also
    randomizes the identifier of its stable ("public") address, so a random
    suffix alone does not mean temporary; the temporary one is the address that
    may not be registered in DNS. *Not yet checked on a real Windows host.*
  - macOS: `unknown` (not read yet).
- `deprecated`: the preferred lifetime has ended (Linux `IFA_F_DEPRECATED`;
  Windows `DadState == IpDadStateDeprecated` or `PreferredLifetime == 0`).

### ipv4, ipv6

| field | meaning |
| --- | --- |
| `stun` | STUN ran for this family (a socket opened and a route existed) |
| `local_address` | the source address the host uses towards the internet |
| `public_address` | the address to show: the first STUN answer in list order; without one, the preferred global interface address; else empty. For IPv6 without translation (`on_interface`) it is always the preferred **stable** interface address |
| `stun_address` | the first STUN answer as the servers saw it (empty without one) |
| `source` | `stun`, `interface` or `none` |
| `on_interface` | the STUN address is one of the host's own addresses (no NAT for this family) |
| `consistent` | every answering server saw the same public address |
| `seen_addresses` | all public addresses the servers reported |
| `nat` | mapping behaviour, below |
| `servers[]` | `server`, `address` (the server IP asked), `answered`, `error`, `mapped`, `other_address`, `rtt_ms` |
| `notes` | `dns_fake_ip`: the resolver answered from 198.18.0.0/15, the pool proxy "fake-IP" DNS modes use; STUN then goes to the proxy, and the result is the proxy's view or nothing |

IPv6 is tested from the preferred stable global address: the socket is bound to
it, because an unbound IPv6 socket leaves from a temporary address (RFC 6724
section 5 rule 7) and the servers would report an address that clients must not
be given. If the bind fails the routing table chooses, and `public_address`
still shows the stable address.

`error` is `""`, `resolve_failed`, `other_family` (an IP literal of the other
family), `no_route`, `timeout`, `send_failed` or `error_response`.

The preferred interface address (`source: "interface"`, and the IPv6 verdict)
is chosen among adapters that are up, not virtual and not mesh, only the egress
adapter when it is known, **stable before temporary**, preferred before
deprecated (RFC 6724 section 5 rule 7 prefers temporary addresses for outgoing
connections; a server wants the opposite: the address that does not change).

`consistent: false` with several `seen_addresses` usually means split routing
(a proxy or VPN sends some servers through another exit) or a NAT with
"arbitrary" address pooling (RFC 4787 section 4.1). The default list puts the
mainland servers first so that the first answer is the direct exit.

### nat (per family)

| field | values |
| --- | --- |
| `present` | `true`, `false` (test I saw the socket's own address and port), `null` (no answer) |
| `mapping` | `endpoint-independent`, `address-dependent`, `address-and-port-dependent`, `endpoint-dependent`, `unknown` |
| `method` | `rfc5780`, `two-servers`, `none` |
| `mapped_address_varies` | the tests saw different public IP addresses, not just ports |
| `filtering` | always `unknown` in v1 |
| `filtering_reason` | `not_tested` |
| `reasons` | `no_answer`, `not_natted`, `port_translated`, `single_server`, `test2_no_answer`, `test3_not_possible`, `test3_no_answer` |
| `tests[]` | `test` (`I`, `II`, `III`), `target`, `mapped` |

Mapping behaviour follows **RFC 5780 section 4.3** with the terms of
**RFC 4787 section 4.1**, all on one UDP socket:

1. **Test I**: Binding request to the primary address of a server that
   returns OTHER-ADDRESS (RFC 5780 section 7.4). If XOR-MAPPED-ADDRESS equals
   the socket's own address and port, there is no NAT: `present: false`,
   `mapping: endpoint-independent`.
2. **Test II**: to the alternate IP address and the primary port. The same
   mapped address and port as test I is `endpoint-independent`.
3. **Test III**: to the alternate IP address and alternate port. The same as
   test II is `address-dependent`, otherwise `address-and-port-dependent`.

`method: rfc5780` is used when an answering server returns an OTHER-ADDRESS at
a different public IP. In the 2026-11-04 test only stun.miwifi.com did
(stun.douyucdn.cn returns its own IP with another port, which cannot serve test
II). Without one, `method: two-servers` compares the answers of two servers at
different addresses: equal is `endpoint-independent`; different is
`endpoint-dependent`, because the test that separates the two dependent kinds
needs an alternate port at the same address. With one answer only,
`method: none` can show that there is a NAT, not how it maps.

**Filtering is not tested** (RFC 5780 section 4.4). The test needs
CHANGE-REQUEST answered from another address. On 2026-11-04 neither
stun.miwifi.com nor stun.douyucdn.cn answered any CHANGE-REQUEST, not even
"change port", from a NAT that does answer them on the plain request. A server
that ignores CHANGE-REQUEST and a NAT that drops the changed source look the
same, so the contract says `unknown` instead of guessing.

How the panel words it (no "Full Cone" or "Symmetric" labels; RFC 4787 section
3 explains why those names mislead):

| mapping | 网络概览 |
| --- | --- |
| `endpoint-independent` with `present: false` | 无 NAT（公网地址在本机网卡上） |
| `endpoint-independent` | 映射与目标无关：P2P 直连通常可行 |
| `address-dependent`, `address-and-port-dependent`, `endpoint-dependent` | 映射随目标变化：P2P 直连多半要靠中继 |
| `unknown` | 未知（说明原因） |

### cgnat, upstream_nat

`{ "state": "yes" | "no" | "unknown", "reasons": [...] }`

- `cgnat` is `yes` when the router's WAN address is in 100.64.0.0/10
  (`wan_shared_address`). It is `no` when STUN sees an address on the host
  (`stun_on_interface`), when STUN sees the router's public WAN address
  (`stun_matches_wan`), or when the WAN is public and STUN did not answer
  (`wan_public`). Otherwise `unknown` (`stun_differs_from_wan`, `wan_private`,
  `no_igd`, `no_wan_ip`).
- `upstream_nat` is `yes` when the router's WAN is private or shared, or when
  STUN sees a public address other than the router's public WAN address
  (`stun_differs_from_wan`: a second NAT, a VPN or a split-routing proxy above
  the router). `no` for `stun_on_interface` and `stun_matches_wan`.

### upnp, hairpin, firewall

Copied from the reachability check: `upnp.protocol` (`upnp-igd` or `none`),
`upnp.available`, `wan_ip`, `wan_ip_class`, `mappings[]`; `hairpin.tried`,
`ok`, `identity_match`, `target`; `firewall.state` (`allowed`, `blocked`,
`unknown`), `firewall.blocked[]` (`tcp/47984` …) and `firewall.ports[]`.

NAT-PMP (RFC 6886) and PCP (RFC 6887) are not queried, so the contract has no
fields for them.

### reachability

```json
"reachability": {
  "ipv4": { "verdict": "likely", "reasons": ["mappings_complete"] },
  "ipv6": { "verdict": "unknown", "reasons": ["ipv6_stable_global", "ipv6_inbound_filter_unknown"] },
  "check_verdict": "verified"
}
```

`verdict` is `likely` (可能可达), `unreachable` (不可达) or `unknown` (未知).
There is no `verified`: none of these checks is a connection made from outside
right now. `check_verdict` is the untouched verdict of
`/api/network/reachability` for consumers that need it.

**IPv4** starts from the reachability verdict and its reasons
(`network-reachability.md`): `verified` and `likely` become `likely`. Then:

- `upstream_nat` is added when `upstream_nat.state` is `yes`; a `likely` without
  `session_evidence` becomes `unknown`, because forwarding on the NAT above the
  router cannot be seen.
- `stun_on_interface` is added when STUN sees an address of the host itself.

**IPv6**:

| condition | verdict | reasons |
| --- | --- | --- |
| no global address on a usable adapter | `unreachable` | `ipv6_no_global` |
| the client TCP ports (https, http, rtsp) listen on IPv4 only | `unreachable` | `ipv6_not_listening` |
| a client port is blocked by the host firewall | `unreachable` | `firewall_blocked:tcp/<port>` |
| STUN sees another IPv6 address (NPTv6 or NAT66) | `unknown` | `ipv6_translated` |
| a client reached the host over IPv6 from the internet in the last 7 days | `likely` | `session_evidence` |
| otherwise | `unknown` | `ipv6_inbound_filter_unknown` |

The first reason is `ipv6_stable_global` or `ipv6_only_temporary`. The last row
is deliberate: RFC 6092 describes a stateful filter in residential IPv6
gateways that passes only flows started from inside, unless the gateway runs in
the "transparent mode" of section 3.4 (REC-49, which may be the default). The
host cannot see which one the router uses, and many home routers ship with the
filter on.

## Doctor checks

The doctor maps the contract to checks; suggested ids:

| check | pass | warn | fail / skip |
| --- | --- | --- | --- |
| `network.nat.cgnat` | `cgnat.state == "no"` | `unknown` | fail on `yes` |
| `network.nat.double` | `upstream_nat.state == "no"` | `unknown` | warn on `yes` |
| `network.nat.mapping` | `ipv4.nat.mapping == "endpoint-independent"` | dependent kinds | skip on `unknown` |
| `network.v6.global` | `ipv6.public_address` not empty | — | warn when empty |
| `network.v6.stable` | first IPv6 reason `ipv6_stable_global` | `ipv6_only_temporary` | skip without IPv6 |
| `network.public` | `reachability.ipv4` or `.ipv6` `likely` | `unknown` | fail when both `unreachable` |

## STUN servers

Configuration key `stun_servers`: up to 8 entries of `host` or `host:port`
(IPv6 literals in brackets), separated by commas; empty uses the built-in list;
`none` turns STUN off. Each family uses the entries that resolve to it.

Built-in list. The order follows the host's region: **China** when config
`locale` is zh-CN (`zh`, `zh_CN`, `zh-Hans`; `zh_TW` is not) or the host's time
zone is a China one (IANA `Asia/Shanghai`, `Asia/Chongqing`, `Asia/Harbin`,
`Asia/Urumqi`, `PRC` …, Windows `China Standard Time`):

1. stun.miwifi.com, stun.chat.bilibili.com, stun.douyucdn.cn, stun.hitv.com,
   then stun.cloudflare.com (Google is not asked: it does not answer on the
   direct route from mainland China);

**anywhere else**:

2. stun.cloudflare.com, stun.l.google.com, then the four mainland servers.

The servers:

| server | operator | IPv4 | IPv6 | RFC 5780 | source |
| --- | --- | --- | --- | --- | --- |
| `stun.miwifi.com:3478` | Xiaomi | answered, 70–110 ms | no AAAA | OTHER-ADDRESS at another IP (111.206.174.2/.3, port 3479) | no public documentation; operator by domain; probed 2026-11-04 |
| `stun.douyucdn.cn:18000` | Douyu | answered, 70 ms | no AAAA | OTHER-ADDRESS at the same IP, port 18001 | no public documentation; probed |
| `stun.chat.bilibili.com:3478` | Bilibili | answered, 50 ms | no AAAA | no | no public documentation; probed |
| `stun.hitv.com:3478` | Mango TV | answered, 60 ms | AAAA 240e:c2:1800:16:4000::15 (not reachable from the test site, which has no global IPv6) | no | no public documentation; probed |
| `stun.cloudflare.com:3478` | Cloudflare | answered, 150–230 ms | AAAA 2606:4700:49:: (not tested, same reason) | no | [Cloudflare Realtime TURN: service address and ports](https://developers.cloudflare.com/realtime/turn/), "STUN over UDP stun.cloudflare.com 3478/udp"; [FAQ](https://developers.cloudflare.com/realtime/turn/faq/): the STUN service "is free and unlimited" |
| `stun.l.google.com:19302` | Google | timeout on the direct route from mainland China; answered only through a proxy exit | AAAA (not tested) | no | no service documentation; the address used by WebRTC examples. Kept last for hosts outside mainland China |

How the list was verified, 2026-11-04, from the build host in Shanghai (China
Telecom, home NAT, no global IPv6), with RFC 8489 Binding requests:

- 23 candidates were asked. Those that did not answer on the direct route or
  did not resolve were dropped: stun.qq.com (timeout), stun.yy.com (timeout),
  stun.syncthing.net (resolves to 192.0.2.42), stun.stunprotocol.org and
  stun.netease.im (no address), stun.ekiga.net and stun.xten.com (timeout).
  International servers that answered only through the site's proxy exit
  (nextcloud, twilio, sipgate, voipgate, mixvoip, freeswitch, blackberry) are
  not in the list: they would show the proxy's address.
- The four mainland servers answered on the direct route and all reported the
  same public address.
- During the same afternoon the site's DNS switched to a proxy fake-IP mode
  (answers in 198.19.0.0/16). STUN to those answers timed out; the overview
  reports this as `notes: ["dns_fake_ip"]` instead of a NAT result.
- No server was verified over IPv6: the site has no global IPv6. The IPv6 path
  is covered by unit tests and is reported honestly (`no_route`) on such a host.

The servers have no published availability promise except Cloudflare's. A
failing server only drops out; the first answer in list order wins.

## Linux run (2026-11-04)

`tests/tools/network_overview_sample 47989 "<the four mainland servers as IP literals>, 162.159.207.0:3478, [2606:4700:49::]:3478, [240e:c2:1800:16:4000::15]:3478"`
on the build host (addresses replaced by documentation ranges):

- `ipv4.public_address` = the home NAT's public address, `source: stun`,
  `consistent: true`, all five IPv4 servers saw the same mapped port.
- `ipv4.nat`: `present: true`, `mapping: endpoint-independent`,
  `method: rfc5780`; test I to 111.206.174.3:3478 and test II to
  111.206.174.2:3478 returned the same address and port.
- `ipv6`: both IPv6 servers `no_route`; the egress adapter has only a ULA, so
  `reachability.ipv6` is `unreachable` with `ipv6_no_global`.
- The Docker bridges on the host use 11.77.0.0/16 (not RFC 1918); they are
  `virtual: true` and never chosen as the public interface address.

## Sources

STUN RFC 8489 (sections 5, 6.1, 6.2.1, 6.3, 14.1, 14.2, 18.6); NAT behaviour
discovery RFC 5780 (sections 4.3, 4.4, 7.2–7.4); NAT UDP behaviour RFC 4787
(sections 3, 4.1); RFC 6598 (100.64.0.0/10); RFC 1918; RFC 4193; RFC 4291
section 2.4 (2000::/3); temporary addresses RFC 8981; address selection RFC
6724; IPv6 CPE filtering RFC 6092 (sections 3.2, 3.4); Microsoft Learn
IP_ADAPTER_UNICAST_ADDRESS_LH and NL_SUFFIX_ORIGIN; Linux v6.12
`include/uapi/linux/if_addr.h` and `net/ipv6/addrconf.c`; Cloudflare Realtime
TURN docs. Each is registered in `docs/authority/source-index.tsv`.

The STUN wire codec is the Platform's `core/network/stun` module, taken as a
source drop into `third-party/alkaidlab-stun-codec` (see its
`source-drop-manifest.json`).
