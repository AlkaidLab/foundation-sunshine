# Cursor Render Mode Protocol

This document defines Sunshine's `cursor_render_mode` host extension. The
current repository contains the Sunshine host; Moonlight clients are maintained
outside this workspace, so this is the client implementation contract.

## Modes

`cursor_render_mode` controls how the cursor is displayed. It is separate from
the existing mouse input mode.

| Mode | Behavior |
|------|----------|
| `remote` | Legacy behavior. The host captures or blends the system cursor into video frames. |
| `client` | The host keeps the system cursor out of video and sends cursor metadata out of band. |
| `auto` | Use `client` only when the client and capture backend both support cursor metadata; otherwise use `remote`. |

Per-application `cursor-render-mode` supports `inherit`, `remote`, `client`,
and `auto`. Non-`inherit` app values override the global setting before
capability and backend fallback are applied.

## Capability Negotiation

The host advertises cursor-channel support in RTSP DESCRIBE:

```text
a=x-ss-general.featureFlags:<flags with cursor_channel bit set>
a=x-ss-general.cursorChannel:1
```

A new client opts in through RTSP ANNOUNCE keys:

```text
x-ss-general.cursorChannel:1
x-ss-general.cursorNative:0|1
x-ss-general.cursorOverlay:0|1
x-ss-general.cursorStateUnreliable:0|1
```

If `x-ss-general.cursorChannel` is missing or false, Sunshine forces the
effective mode to `remote` and does not send cursor TLVs. This keeps old
Moonlight clients on the existing behavior and avoids unknown top-level control
packets.

## Transport

Cursor messages are Sunshine encrypted control extensions using the existing
`control_header_v2` wrapper. Multi-byte fields are little-endian.

| Packet type | Name | Reliability |
|-------------|------|-------------|
| `0x5509` | `CURSOR_SHAPE` | Reliable |
| `0x550A` | `CURSOR_STATE` | Unreliable if the client announced `cursorStateUnreliable`, otherwise latest-only reliable fallback |
| `0x550B` | `CURSOR_MODE` | Reliable |

The host sends these packets only when `controlProtocolType == 13`, the client
announced `cursorChannel`, and the effective mode requires the packet.

`CURSOR_STATE` uses a `seq` counter for ordering. Clients must drop stale state
packets using wrap-safe unsigned sequence comparison. `host_qpc_time` is only
for diagnostics and must not be used for ordering.

## CURSOR_MODE

Host-to-client reliable notification:

```c
struct cursor_mode_v1 {
  uint8_t  version; // 1
  uint8_t  mode;    // 0=remote, 1=client
  uint8_t  flags;   // bit1=fallback_remote
  uint8_t  reserved;
  uint32_t seq;
};
```

When the host enters `remote` or temporary `fallback_remote`, the client must
hide any native or overlay cursor override and rely on the video cursor.

## CURSOR_SHAPE

Host-to-client reliable shape payload:

```c
struct cursor_shape_v1 {
  uint8_t  version;   // 1
  uint8_t  format;    // 1=BGRA32 straight, 2=BGRA32 premul, 3=mono/XOR, 4=masked color
  uint16_t width;
  uint16_t height;
  uint16_t pitch;
  int16_t  hotspot_x;
  int16_t  hotspot_y;
  uint32_t shape_id;
  uint32_t bytes_len;
  uint8_t  bytes[bytes_len];
};
```

MVP host behavior normalizes DXGI color cursors to `BGRA32 straight`. Unsupported
DXGI monochrome/XOR or invalid shapes trigger a remote fallback until a
supported shape is available. `shape_id` is a hash of format, dimensions,
hotspot, pitch, and pixels; unchanged shapes are not resent.

## CURSOR_STATE

Host-to-client cursor state payload:

```c
struct cursor_state_v1 {
  uint8_t  version;       // 1
  uint8_t  flags;         // bit0 visible, bit1 fallback_remote, bit2 shape_valid, bit3 client_overlay_allowed
  uint16_t display_id;
  uint32_t seq;
  uint32_t shape_id;
  int32_t  shape_left;    // desktop physical pixels, cursor bitmap left
  int32_t  shape_top;     // desktop physical pixels, cursor bitmap top
  int16_t  hotspot_x;     // relative to shape_left
  int16_t  hotspot_y;     // relative to shape_top
  uint16_t dpi_scale_q8;  // 1.0 = 256
  uint16_t reserved;
  uint64_t host_qpc_time; // diagnostics only
};
```

Coordinates are desktop physical pixels. `shape_left` and `shape_top` are the
cursor bitmap origin, not the hotspot. The host-side click hotspot is:

```text
server_hotspot_x = shape_left + hotspot_x
server_hotspot_y = shape_top + hotspot_y
```

## Client Rendering Rules

Clients should prefer native cursor APIs when available and use an overlay
cursor when native cursor creation is not possible. A client must never wait for
host `CURSOR_STATE` before moving the local cursor in response to local mouse
motion; local movement remains immediate, and host state is used for shape,
visibility, fallback, and correction.

If `visible` is false, hide the client cursor but do not request remote fallback
just because the OS cursor is hidden. Games commonly hide the system cursor for
raw relative mouse or self-drawn reticles.

If `fallback_remote` is set, hide native or overlay cursor rendering. If a state
references an unknown `shape_id` while visible, use a default arrow temporarily
or request a shape refresh; do not render a wrong cursor.

If native or overlay cursor creation fails, the client should report the
failure through a future feedback extension so the host can force
`fallback_remote`.

## Capture Backend Behavior

The first host implementation supports Desktop Duplication RAM/VRAM cursor
metadata. In `client` mode, DDA still reads pointer shape, position, hotspot,
visibility, display id, and QPC time, but it does not blend a supported cursor
into the video frame.

When DDA reports only a pointer update and no desktop frame update, the host:

1. Updates the cursor metadata cache.
2. Enqueues `CURSOR_SHAPE` if the shape changed.
3. Enqueues latest-only `CURSOR_STATE`.
4. Releases the Desktop Duplication frame.
5. Returns no video frame so the encoder is not triggered.

WGC supports `IsCursorCaptureEnabled(false)` for `client` mode, but it is not a
complete metadata provider in this MVP. `auto` therefore falls back to `remote`
when WGC is the selected backend.

## Compatibility

Old clients do not announce `cursorChannel`, so the host forces `remote` and
does not send cursor TLVs. New clients using `cursor_render_mode=remote` also
receive the legacy video-baked cursor path.
