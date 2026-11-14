# Runtime bitrate and host-audio controls

This host keeps the existing `0x5506` dynamic-parameter packet for client-to-host
requests. A compatible client may opt in to the host-to-client callback by setting
bit `0x400` in `x-ml-general.featureFlags`; the host advertises the same bit in
`x-ss-general.featureFlags` and sends no callback when the client did not opt in.
The feature bit follows the host-feature flag layout described by
`ba-moonlight-feature-flags-20261109`.

The callback is an encrypted control-stream `0x5506` payload of exactly eight
bytes: little-endian `u32 parameter_type = 2` (`BITRATE`), followed by a
little-endian `i32 total_bitrate_kbps`. The value is the applied session budget,
including FEC, after the host cap and budget controller have processed the
request. The message direction is host to client; the existing client to host
direction remains unchanged. Legacy clients and clients without control protocol
v2 continue to work because the host omits the callback.

The client change required for end-to-end display is precise: advertise bit
`0x400`, accept encrypted control message `0x5506`, validate the eight-byte
payload and range `1..800000`, and notify the session quality model with the
applied total bitrate. A client must ignore the packet when the feature bit was
not advertised or the payload is malformed.

`host_audio` is a persistent host default. During an active session the Web UI
and Panel use `POST /api/runtime/audio` with `{ "clientname": "...",
"enabled": true|false }`. The host stores a per-session atomic value and
reopens the platform capture endpoint on a change, selecting the configured
host sink or the existing virtual null sink. This keeps the audio packet
negotiation unchanged; it only changes whether the host capture source is the
host playback path. The platform backend's ability to reopen a live capture
endpoint remains platform-specific.
