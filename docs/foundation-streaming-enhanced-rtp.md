# Foundation Streaming Enhanced RTP

## Goal

Foundation Streaming keeps the legacy Moonlight/Sunshine RTSP, RTP, and ENet path intact, then layers opt-in Enhanced RTP features on top. New behavior must be negotiated and safe to ignore. Old clients and old servers continue to stream with no feature loss.

## Compatibility Matrix

- Foundation Sunshine + Enhanced common-c: uses Enhanced RTP feedback, input priority, and low-bitrate quality hints.
- Foundation Sunshine + current common-c: gets server-side stream-quality control, FEC caps, IDR cooldown, pacing, and low-bitrate defaults.
- Standard Sunshine + Enhanced common-c: gets client-side decode/render queue discipline and input coalescing, but host-only feedback is ignored.
- Standard Sunshine + legacy common-c: stays on the legacy path.

## Advertised Feature Bits

Only implemented features may be advertised in `featureFlags2`.

- `QOS_FEEDBACK_V2`: client sends `SS_NETWORK_FEEDBACK_V2` with decode/render/input pressure.
- `INPUT_PRIORITY_V1`: client can separate urgent reliable input from latest-state pointer motion; host coalesces stale pointer input instead of replaying backlog.
- `LOW_BITRATE_QUALITY_V1`: client sends `x-ml-video.contentType`; host applies content-aware clarity planning within the user's bitrate cap.

Not advertised yet:

- `CURSOR_PLANE_V1`
- `CLIPBOARD_FLOW_V1`
- `PATH_PROBE_V1`
- `FT2_QUIC_DATAGRAM_V1`
- `FRAME_TIMING_V1`
- `INPUT_FOCUS_V1`

These remain design targets until both sides contain real implementations.

## Implemented Enhanced RTP Loop

- Client feedback reports packet/FEC health plus decode queue depth, render queue depth, late frames, displayed frames, input queue depth, and input send latency.
- Sunshine's stream-quality controller prioritizes deadline pressure and input pressure before cutting bitrate hard.
- FEC is capped to a safe maximum and IDR requests have cooldown to avoid recovery storms.
- Recovery probes upward after stable windows instead of staying degraded.
- Sender pacing uses per-session controller output to avoid video bursts drowning control/input.

## Low-Bitrate Quality

The client sends `x-ml-video.contentType` as a hint:

- `0`: desktop
- `1`: text
- `2`: motion/dragging
- `3`: game

Foundation Sunshine uses this hint to adjust encode cadence, chroma choice, intra-refresh preference, ROI/LTR hints, QP target, and mild sharpening hints. The bitrate remains a ceiling, not a command to blur by forced resolution drops.

## Input Path

Enhanced input keeps reliable semantics for clicks, key presses, text, controller state transitions, and correction tails. Pointer motion is treated as latest-state data:

- absolute desktop pointer motion may be unreliable with a reliable correction tail
- relative game mouse motion may be low-latency sequenced delta
- host-side input queue batches stale pointer moves and drains with one worker, avoiding per-packet task storms

This does not implement a fake local cursor plane. Cursor plane remains future work because it needs host cursor shape/visibility and game custom cursor support to be correct.

## Session Rules

Enhanced features must bind to the active runtime session, not client name or guessed IP:

- feedback routes through the active connection/control context
- dynamic quality is per runtime session
- future cursor plane, input focus, clipboard bulk, and transport QoS must declare owner scope and fallback behavior

## Future Transport

Foundation Transport v2 is intentionally not advertised yet. The intended direction is QUIC DATAGRAM for video/audio/input and reliable QUIC streams for control/clipboard, with legacy RTP/ENet kept as permanent fallback.
