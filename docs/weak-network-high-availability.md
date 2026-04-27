# Weak-Network High Availability Streaming

## Goal

Foundation Sunshine should keep a stream usable under hostile networks without silently changing the user's selected display mode. Resolution, frame rate, chroma, audio topology, and microphone settings remain the session specification. The weak-network loop may only tune encoding bitrate, FEC percentage, sender pacing, recovery requests, and buffering/recovery behavior.

## Research Baseline

- RTP congestion feedback work such as [RFC 8888](https://datatracker.ietf.org/doc/html/rfc8888) points to receiver-originated feedback as the right place to report packet loss and delivery state.
- WebRTC-style congestion control uses deterministic estimators over packet loss, RTT/jitter, receiver feedback, and probing; the useful lesson is the closed loop, not a requirement to copy WebRTC transport.
- Remote desktop adaptive graphics systems preserve the desktop/session shape while changing codec quality and bandwidth usage; this matches the product requirement that resolution, FPS, chroma, audio topology, and microphone mode remain user-selected.
- Parsec/Steam Remote Play-style product behavior also points toward fast automatic quality/bitrate recovery instead of asking the user to manually lower display settings.
- UU Remote's public product material emphasizes smooth low-latency control, high frame-rate/4K/HDR, multi-device use, and file transfer. Its transport and congestion-control internals are proprietary, so we should learn the product-level behavior rather than copy unknown protocol details.

## Product Behavior Target

- The weak-network path is always automatic. There is no user toggle because the correct behavior is to stay invisible on healthy links and intervene only when telemetry proves the link is unhealthy.
- The user-selected stream settings are the ceiling. The controller can temporarily lower encoder budget, add/reduce FEC, pace sender bursts, and request recovery frames, then climb back toward the original budget when feedback is healthy.
- A bad link should degrade into lower encoded detail and occasional recovery, not into a frozen first frame, reconnect loop, or unresponsive input.
- If the link is below the minimum usable envelope, Moonlight should keep the connection state honest: report weak-network protection or unrecoverable network instead of pretending a reconnect will fix congestion.

## Comparative Notes

- WebRTC's useful pattern is receiver feedback plus sender-side deterministic control: estimate current delivery quality, change encoder budget, and probe back up after stable windows.
- RDP/AVD's useful pattern is preserving the desktop contract while adapting graphics quality; the remote desktop does not need to resize itself just because bandwidth falls temporarily.
- Game/remote desktop products such as Parsec, Steam Remote Play, and UU Remote all optimize for "usable under bad links" rather than "maximum quality only on good links". Public material does not expose their congestion algorithms, so Foundation Sunshine should implement our own transparent controller from measurable signals.
- For Moonlight/Foundation Sunshine, the highest-value gap is not a new tunnel protocol. It is the missing closed loop between receiver loss/FEC evidence and host-side encoder/FEC/pacing decisions.

## Architecture

### Client Feedback

Moonlight macOS Enhanced is the experimental client. It advertises `ML_FF_NETWORK_FEEDBACK_V1` and, only when the host advertises `LI_FF_NETWORK_FEEDBACK_V1`, sends `SS_NETWORK_FEEDBACK_V1` on the existing control stream. This keeps official Moonlight and old Sunshine-compatible clients unchanged.

The feedback packet is a 500 ms aggregate window:

- frame progress: seen, complete, recovered, unrecoverable
- FEC health: missing packets, data/parity totals, received data/parity
- transport health: video bytes, RTT, RTT variance
- audio placeholder: underrun counter for later renderer integration

Per-frame `SS_FRAME_FEC_STATUS` is also consumed by Foundation Sunshine for faster reaction when aggregate feedback is sparse.

### Host Controller

Foundation Sunshine owns the real-time loop. It uses a deterministic EWMA state machine:

- `healthy`: no action; stay at the user-defined baseline
- `constrained`: moderate loss/jitter; reduce encoding budget and add FEC
- `crisis`: unrecoverable frames or severe loss; cut bitrate faster, add FEC, request IDR
- `recovering`: stable windows; probe upward toward the baseline and reduce FEC

The controller is per runtime session and does not depend on WebUI, LLM, or manual toggles.

### Sender Pacing

The video sender no longer assumes an approximately 1 Gbps in-frame burst budget. It derives a per-session pacing budget from the weak-network controller and sends smaller batches on poor links. This protects input, audio, and control packets from being drowned by video bursts.

### Encoder Budget Consistency

Foundation tracks three values per runtime session:

- `current_total_bitrate`: the total network budget for video, including FEC overhead
- `current_fec_percentage`: the FEC percentage used by packet generation
- `pacing_total_bitrate`: the token-bucket style sender budget after FEC overhead

The encoder receives the total bitrate and its current per-session FEC percentage, then computes the encoding bitrate as `total * (100 - fec) / 100`. This keeps the encoded video budget, generated FEC shards, and sender pacing coherent when the controller changes FEC dynamically.

Dynamic parameter delivery must be queued, not event-coalesced. A single weak-network decision can update FEC and bitrate together; if the channel keeps only the latest event, the encoder can miss the FEC update and continue using the global configured value.

For feedback-capable clients, startup FEC is capped at the controller maximum before the first feedback window. This avoids beginning a weak-network session with an extreme configured FEC value that expands packet volume before the controller has evidence.

## Compatibility Rules

- No new ports are opened; feedback reuses the existing control stream.
- If the host does not advertise `LI_FF_NETWORK_FEEDBACK_V1`, the client does not send the new packet.
- If the client does not advertise `ML_FF_NETWORK_FEEDBACK_V1`, Foundation Sunshine ignores network-feedback packets.
- Old clients still stream normally; they simply do not participate in the new closed loop.

## qiin-Aligned common-c Plan

If the experiment works, extract only these protocol pieces into the `skyhua0224/moonlight-common-c` branch aligned with `qiin2333/moonlight-common-c:mic`:

- `ML_FF_NETWORK_FEEDBACK_V1`
- `LI_FF_NETWORK_FEEDBACK_V1`
- `SS_NETWORK_FEEDBACK_PTYPE`
- `SS_NETWORK_FEEDBACK_V1`
- automatic feedback sending gated by host capability

Foundation-specific controller, pacing, encoder policy, and macOS diagnostics must not be part of the qiin protocol PR.

## Test Plan

- LAN regression: no visible behavior change under stable links.
- Bad network: packet loss and jitter should move the controller into `constrained` or `crisis`.
- Recovery: stable feedback windows should return toward the original bitrate and FEC baseline.
- Compatibility: official/old clients should keep streaming without negotiation failures.
- Real target: test `47.103.131.32:57989` for at least 60 seconds, requiring continuing video progress and usable input.
