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

1. GameStream HTTP, including initial pairing.
2. GameStream HTTPS, including pairing completion, apps, launch and resume.
3. RTSP.
4. Legacy video UDP port, unused by VoidLink in direct-video mode.
5. ENet control and input UDP.
6. Audio UDP.
7. Microphone UDP.

The iPad binds local proxies for the other six services and sends `OPEN <id> <service-index>\n` on the
first connection. Windows opens the requested Sunshine endpoint on the PC
locally, then opens another USB connection. Its size-prefixed JSON header has
`type` (`channel`), the same `session`, and the requested `id`. TCP services
carry bytes directly after the header. UDP services prefix each datagram with
an unsigned two-byte big-endian length and preserve the datagram boundary.
Each channel applies its own backpressure. Closing the offer closes its relays.

Video uses `OPEN <id> 7\n`. The size-prefixed response has `type: video`,
`session`, `id`, and a single-use `token`. VoidLink sends the token on its
paired HTTPS launch/resume in `X-SS-USB-Direct-Video`. RTSP ANNOUNCE must request
`x-ml-video.usbStreamVersion:1`; the host acknowledges the direct channel with
`X-SS-USB-Video-Version: 1`. The iPad sends a single `V` byte on the video
connection when common-c starts reading. Sunshine then sends the existing
video datagrams, including encryption prefixes and FEC, with two-byte
big-endian lengths. This channel has no local UDP forwarding or video UDP
ping. Its bounded queue drops queued data and requests a new IDR when USB
temporarily falls behind. A disconnected or stalled USB writer ends the
affected stream after the transport timeout.

The tunnel forwards only Sunshine's configured GameStream ports. GameStream
still handles authentication, certificate checks, RTSP and media encryption.
A USB selection uses iPad loopback endpoints and does not select a LAN address.
