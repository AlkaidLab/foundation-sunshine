# Windows USB GameStream tunnel

The host opt-in is `usb_wired_streaming_enabled = enabled` in `sunshine.conf`.
It defaults to disabled.

Select the PC from the **USB** button on VoidLink's host page. If the iPad has not
paired with this Sunshine instance, use the PIN shown on the iPad in Sunshine's
web UI on the PC. Pairing traffic and the entire streaming session can use the
USB cable while iPad Wi-Fi is off.

Windows connects to the Apple Mobile Device Service at 127.0.0.1:27015 and
considers only physical USB devices. The iPad listens at 127.0.0.1:41849. The
first connection carries a two-byte big-endian size and a JSON offer with
`type` (`offer`), `session`, `hostId`, `hostName` and the seven mapped port
numbers in this order. It also advertises `directVideo: true`:

| `service-index` | Service | Transport |
| --- | --- | --- |
| 0 | GameStream HTTP, including initial pairing | TCP |
| 1 | GameStream HTTPS, including pairing completion, apps, launch and resume | TCP |
| 2 | RTSP | TCP |
| 3 | Legacy video UDP port, unused by VoidLink in direct-video mode | UDP |
| 4 | ENet control and input | UDP |
| 5 | Audio | UDP |
| 6 | Microphone | UDP |
| 7 | Direct video channel, not backed by any port in the offer | Direct USB |

The indices are zero-based. The `ports` array holds entries 0 to 6 in the
order above; index 7 opens the direct video channel instead of a port.
The iPad binds a local proxy for each port in the offer and sends
`OPEN <id> <service-index>\n` on the first connection. Windows opens the
requested Sunshine endpoint on the PC
locally, then opens another USB connection. Its size-prefixed JSON header has
`type` (`channel`), the same `session`, and the requested `id`. TCP services
carry bytes directly after the header. UDP services prefix each datagram with
an unsigned two-byte big-endian length and preserve the datagram boundary.
Each channel applies its own backpressure. Closing the offer closes its relays.

Video uses `OPEN <id> 7\n`. The size-prefixed response has `type: video`,
`session`, `id`, and a single-use `token`. VoidLink sends the token on its
paired HTTPS launch/resume in `X-SS-USB-Direct-Video`. RTSP ANNOUNCE must request
`x-ml-video.usbStreamVersion:1`; the host acknowledges the direct channel with
`X-SS-USB-Video-Version: 1`. `x-nv-video[0].packetSize` must be at least 1024:
the host answers 461 when the packet size would lower the per-frame ceiling of
the RS layout below what Moonlight's smallest client packet size provides.
The iPad sends a single `V` byte on the video
connection when common-c starts reading. Sunshine then sends the existing
video datagrams, including encryption prefixes and FEC, with two-byte
big-endian lengths. This channel has no local UDP forwarding or video UDP
ping. Its bounded queue drops queued data and requests a new IDR when USB
temporarily falls behind. A disconnected or stalled USB writer ends the
affected stream after the transport timeout.

The tunnel forwards only Sunshine's configured GameStream ports. GameStream
still handles authentication, certificate checks, RTSP and media encryption.
A USB selection uses iPad loopback endpoints and does not select a LAN address.
Sunshine connects to its own bind address: empty or `0.0.0.0` becomes
`127.0.0.1`, `::` becomes `::1`, and a loopback address is used as is. The
host-only checks classify a connection by the source address the server sees;
the system selects that source for the target, and on Windows a target on the
host's own loopback or interface address is sourced from that same address.
Loopback targets therefore classify as local, while any other bind address
sources the tunnel requests from a LAN address and the host-only checks reject
them.
