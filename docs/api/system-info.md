# System information

`GET /api/system/info` reports the CPU, the GPUs, the network adapters, the
operating system and the host name for the Web UI home tiles. Every value is
read from the host itself; the endpoint makes no external request.

The endpoint is authenticated exactly like the other configuration APIs.

## Request

```
GET /api/system/info
GET /api/system/info?refresh=1
```

The result is cached for 60 seconds. `refresh=1` (or `refresh=true`) reads the
host again. Concurrent refreshes are serialized, so a refresh cannot start more
than one enumeration at a time. A refresh takes the same local reads as the
first call and does not wait for the network.

## Response (JSON v1)

```json
{
  "version": 1,
  "checked_at": "2026-11-03T02:10:00Z",
  "cpu": { "model": "...", "vendor": "...", "cores": 8, "threads": 16, "base_mhz": 3400 },
  "gpus": [
    { "name": "...", "vendor": "nvidia", "driver_version": "...", "vram_mb": 16384, "is_streaming_adapter": true }
  ],
  "nics": [
    {
      "name": "Ethernet",
      "description": "...",
      "type": "ethernet",
      "up": true,
      "link_speed_mbps": 2500,
      "ipv4": ["192.0.2.10"],
      "ipv6": ["2001:db8::10"],
      "mac": "aa:bb:cc",
      "is_default_route": true,
      "wifi": { "ssid_masked": "", "signal": null }
    }
  ],
  "os": { "name": "...", "version": "...", "build": "..." },
  "host": { "name": "..." }
}
```

`checked_at` is UTC in `YYYY-MM-DDTHH:MM:SSZ` form. Fields that the platform
cannot provide are `null`, and a list that cannot be enumerated is empty; the
shape does not change.

### cpu

| field | meaning |
| --- | --- |
| `model` | Marketing name, for example `AMD Ryzen 9 7950X 16-Core Processor` |
| `vendor` | `intel`, `amd`, `arm`, another vendor string, or empty |
| `cores` | Physical cores |
| `threads` | Logical processors |
| `base_mhz` | Base clock in MHz when the platform exposes it, otherwise `null` |

### gpus

Only hardware adapters are listed; the Microsoft Basic Render Driver and other
software adapters are skipped.

| field | meaning |
| --- | --- |
| `name` | Adapter description; on Linux the DRM product name when sysfs provides one |
| `vendor` | `nvidia`, `amd`, `intel`, `arm`, `qualcomm`, a `0xNNNN` vendor id, or empty |
| `driver_version` | Display driver version when known, otherwise empty |
| `vram_mb` | Dedicated video memory in MiB, or `null` |
| `is_streaming_adapter` | The adapter Sunshine encodes on |

`is_streaming_adapter` follows `video.adapter_name`: when the setting is empty
the first enumerated hardware adapter is marked (Sunshine's automatic choice);
when it is set, the adapter whose name, card name or render node matches is
marked. A configured name that matches no adapter marks none, which mirrors the
capture path falling back to automatic selection.

### nics

Loopback interfaces are not listed. IPv6 link-local addresses (`fe80::/10`) are
excluded because they are not usable by a remote client.

| field | meaning |
| --- | --- |
| `name` | OS interface name or friendly name |
| `description` | Adapter description; the interface name when the platform has no separate description |
| `type` | `ethernet`, `wifi`, `mesh`, `virtual` or `other` |
| `up` | Link and administrative state |
| `link_speed_mbps` | Current transmit link speed in Mbit/s, or `null` |
| `ipv4` / `ipv6` | Unicast addresses held by the interface |
| `mac` | OUI only (`aa:bb:cc`); the device part is never served |
| `is_default_route` | The interface carries the default route |
| `wifi.ssid_masked` | At most two leading characters followed by `***`, or empty |
| `wifi.signal` | Link quality 0..100 when the platform reports one |

`mesh` covers overlay adapters recognised from their name or address range
(the same classification as the reachability endpoint). A `virtual` adapter is
a bridge, container, hypervisor or tunnel interface that is not an overlay.

### os and host

`os.name` is the product name, `os.version` the release or display version and
`os.build` the build identifier (Windows build and UBR, the Linux kernel
release, the macOS build). `host.name` is the DNS host name; the NetBIOS name
is used only as a Windows fallback.

## Platform state

| capability | Windows | Linux | macOS |
| --- | --- | --- | --- |
| adapters and addresses | `GetAdaptersAddresses` | `getifaddrs`, `/sys/class/net` | `getifaddrs` |
| link type and speed | `IfType`, `TransmitLinkSpeed` | `/sys/class/net/<if>/type`, `speed` | `NET_RT_IFLIST2` |
| default route | `GetIpForwardTable` | `/proc/net/route`, `/proc/net/ipv6_route` | `NET_RT_DUMP` |
| Wi-Fi | WLAN API | `/proc/net/wireless` signal, no SSID | not read |
| CPU | processor registry key, `GetLogicalProcessorInformationEx` | `/proc/cpuinfo`, `base_frequency` | `sysctlbyname` |
| GPUs | DXGI, display class registry | `/sys/class/drm` | not implemented (empty) |
| OS | version registry key | `/etc/os-release`, kernel release | `sysctlbyname` |

Source is implemented for all three systems. This change built and ran the
Linux backend; the Windows backend is queued for a Windows build and the macOS
backend was not compiled in this change.

Deliberately not implemented: the macOS GPU list (empty), the Linux Wi-Fi SSID
(empty; the kernel does not expose it through sysfs or procfs without netlink),
and per-adapter driver details beyond the fields above. None of them changes
the response shape.

## Privacy

- The MAC address is truncated to its OUI before it enters the response or a
  log. The SSID is truncated to two characters and `***`.
- Addresses are this host's own; they are visible to an authenticated caller
  and are not written to the info log.
- The inventory never contacts another host. The only name resolution in this
  feature belongs to the reachability endpoint and is bounded (see
  [network-reachability.md](network-reachability.md)).

## Why these facilities

The Windows backend uses the documented IP Helper, DXGI and WLAN structures and
the processor registry key, which expose interface type, transmit link speed,
operating state and adapter identity without polling or a WMI service round
trip. Linux uses the stable `/proc` and sysfs interfaces that the kernel
documents for CPU identity, interface state, link speed and DRM adapters. macOS
uses `getifaddrs` plus the routing and interface sysctls. The authoritative
records are listed in [docs/authority/source-index.tsv](../authority/source-index.tsv)
under the `ms-*`, `linux-*`, `man-os-release` and `apple-*` keys; each backend
comment cites its key.

### Emergence and alternatives

Windows offers WMI (`Win32_Processor`, `Win32_VideoController`) and the newer
Windows.Devices.Enumeration and GameInput families. WMI gives fewer fields per
call but requires a COM service round trip and can take tens to hundreds of
milliseconds; the registry and IP Helper path is a local read and fits the
60-second cache. DXGI is the same adapter enumeration the capture path uses, so
`is_streaming_adapter` describes the adapter Sunshine actually sees. NVAPI is
already vendored for driver settings; it is used only when a driver-version
query exists there, otherwise the display class registry value is used.

Linux has no single documented API for the marketing name of a GPU; sysfs is
the stable interface and is used for the fields it exposes. `ethtool` through
`SIOCETHTOOL` would add duplex and driver details that this contract does not
serve.

## Performance

One refresh is a single pass over interfaces and registry values: no
per-interface process, no WMI query, no external request. The endpoint is for
display tiles, not a hot path; the 60-second cache bounds the cost to at most
one enumeration per minute and the response is a few kilobytes.

Measured on the build host (Linux, one warm run, Debug): `collect()` including
CPU, DRM, adapter, OS and host-name reads took 2-3 ms in the
`SystemInfoProbe.CollectsThisHost` unit test. The 60-second cache makes the
steady-state cost one JSON render per request. There is no competing
implementation on this machine to compare against, and no throughput or
allocation target is claimed beyond keeping a refresh off the request's hot
path.

## Example

See [system-info.example.json](system-info.example.json).
