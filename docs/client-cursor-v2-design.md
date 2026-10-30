# Client cursor v2 and automatic mouse mode

Status: implemented and unit tested; not yet verified on hardware. Covers Foundation Sunshine
(host), qiin2333/moonlight-common-c `mic` and Moonlight V+ for PC.

## What changes for users

With a V+ client that has "Automatic mouse mode" on, against a Windows host:

- On the desktop, the client draws the host's cursor as a real local OS cursor with absolute
  input, so pointer movement has no stream latency and there is no second cursor in the video.
- When a game hides, locks or recentres the cursor, the client switches to relative input on the
  first mouse motion, before sending it. It switches back as soon as the host shows a free cursor,
  once no mouse button is held. No mode toggle is needed.
- Ctrl+Alt+Shift+M switches to the other mode and locks it; Ctrl+Alt+Shift+A locks the current
  mode, or returns to automatic.

Every other client and host combination behaves exactly as today (see
[Compatibility](#compatibility)).

## Baseline: cursor protocol v1 (0x5509)

| Part | Today |
|---|---|
| Advertisement | `platform_caps::cursor_shape` / `LI_FF_CURSOR_SHAPE` (0x40), only while DDA or Zako Direct VDD capture is active (`cursor_channel::producer_available()`). |
| Request | Client sends `0x5509` `{version=1, mode, 0, 0}`, mode 0 = video cursor, 1 = local cursor (`LiSetCursorMode`). Ignored on an unencrypted control stream. |
| Updates | Host sends `0x5509` with a 28-byte header (version 1, flags `SHAPE`/`VISIBLE`, shape id, size, hotspot, chunk offset and length) on ENet channel 0, shapes split into 60000-byte chunks. The v1 parser rejects other versions, header sizes and flags. |
| Source | Capture thread: DDA `GetFramePointerShape` and visibility, or the VDD driver's cursor snapshot, through `cursor_channel`. WGC is not a producer. |
| Compositing | DDA and VDD stop blending the cursor for every session as soon as any one session is in local mode. |
| V+ use | Absolute mode with "Show local cursor" on. Visibility hides are debounced by 150 ms because DDA's per-frame `Visible` bit flickers. No automatic switching. |

v1 stays exactly as it is. v2 is a second version of the same message type, negotiated per
session.

## Negotiation

1. **Host advertises** a new caps bit, `platform_caps::cursor_state` / `LI_FF_CURSOR_STATE` =
   `0x400` (the next free bit), in `a=x-ss-general.featureFlags`. Only Windows sets it, and it does
   not depend on the capture backend: v2 state comes from `GetCursorInfo`, so it also works with
   WGC. It is not set while WGC captures a single window (`capture_target = window`): absolute
   input and the cursor geometry still describe the whole display there, so a client-drawn cursor
   would not line up with the window's video.
2. **Client opts in** by sending `0x5509` with version 2 and `SUBSCRIBE`, only if the host
   advertised `0x400` and automatic mouse mode is on. Until then the host sends no v2 message and
   the tracker does not run.
3. **Client stays on its fallback** (the user's existing mouse settings) until the first state
   with `RESYNC` arrives. A host that sets `0x400` for another reason and never answers leaves the
   client on today's behaviour.

A session uses one version at a time. A v2 request with `SUBSCRIBE` ends v1 local mode for that
session. A v1 request ends its v2 subscription. V+ chooses the version once per session and never
switches.

## Wire format (version 2)

All messages are type `0x5509` inside the encrypted control stream (`0x0001` envelope).
Multibyte fields are little-endian. Byte 0 is the version in both directions, as in v1.

### Request (client to host, 8 bytes)

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 1 | version | 2 |
| 1 | 1 | flags | `0x01 SUBSCRIBE`, `0x02 CLIENT_DRAWS`; other bits 0 |
| 2 | 2 | reserved | 0 |
| 4 | 4 | resend_shape_id | 0 = none |

Each request replaces the session's v2 state. `SUBSCRIBE` going from 0 to 1 schedules a resync.
`CLIENT_DRAWS` tells the host this client draws the cursor itself. A non-zero `resend_shape_id`
asks for a shape the client was told about but does not have. The host accepts it only on an
encrypted control stream, as v1 does.

### Host to client

Sent only to sessions subscribed to v2, on **reliable ENet channel `0x09`**
(`CTRL_CHANNEL_CURSOR`; the client connects with 0x30 channels and 0x06 to 0x0F are unused).
Shapes and states share the channel, so they stay in order relative to each other. Byte 1 is the
kind.

**State** (kind 1, 44 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | version (2) |
| 1 | 1 | kind (1) |
| 2 | 1 | flags: `0x01 VISIBLE`, `0x02 FALLBACK`, `0x04 HAS_POSITION`, `0x08 HAS_CLIP`, `0x10 COMPOSITED`, `0x20 RESYNC` |
| 3 | 1 | reserved (0) |
| 4 | 4 | seq (per session, +1 per state) |
| 8 | 4 | shape_id (0 = none) |
| 12 | 2 | ref_width (stream coordinate space, > 0) |
| 14 | 2 | ref_height (> 0) |
| 16 | 4 | scale_q16: stream pixels per shape pixel (16.16, > 0) |
| 20 | 4 | x (i32, stream coordinates, valid with `HAS_POSITION`) |
| 24 | 4 | y |
| 28 | 4 | clip_left (i32, valid with `HAS_CLIP`) |
| 32 | 4 | clip_top |
| 36 | 4 | clip_right (exclusive) |
| 40 | 4 | clip_bottom (exclusive) |

**Shape chunk** (kind 2, 28-byte header and pixels):

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 1 | version | 2 |
| 1 | 1 | kind | 2 |
| 2 | 1 | format | 1 = B8G8R8A8, straight alpha, top-down, tightly packed |
| 3 | 1 | reserved | 0 |
| 4 | 4 | shape_id | non-zero |
| 8 | 2 | width | 1 to 256 |
| 10 | 2 | height | 1 to 256 |
| 12 | 2 | hotspot_x | < width |
| 14 | 2 | hotspot_y | < height |
| 16 | 4 | total_size | width × height × 4 |
| 20 | 4 | chunk_offset | bytes already received for this id |
| 24 | 2 | chunk_length | 1 to 16384, offset + length ≤ total_size |
| 26 | 2 | reserved | 0 |
| 28 | n | pixels | `chunk_length` bytes |

- A shape arrives as consecutive chunks of up to 16 KiB. The client keeps one assembly slot of at
  most 256 KiB. A chunk that does not continue it (another id, a wrong offset, a changed header)
  discards the partial shape. Only a complete shape reaches the application.
- Every shape a state names is sent before that state, once per session. A state naming an id the
  client lacks (shape budget exhausted, or a discarded assembly) is answered with
  `resend_shape_id`.
- Coordinates use the stream's space, `(0,0)` to `(ref_width, ref_height)`: the whole encoded
  frame, letterbox included. That is the space the client already maps absolute input with.
- Larger Windows cursors are box-filtered by an integer factor to fit 256x256, and `scale_q16`
  accounts for it.

**State rules** (unchanged from the Vibeshine design):

- Sent only on change, plus a full state with `RESYNC`, after every shape it needs, whenever a
  session subscribes. Nothing is sent until the session's encoder has published its input
  geometry.
- `HAS_POSITION` is set on `RESYNC`, when the cursor becomes visible, when the clip changes, when
  the host moved the cursor itself (application warp, local mouse, gamepad mouse emulation), and
  on every move while the client is not drawing. Absolute positions injected for a client are
  never reported back to it as warps.
- `FALLBACK`: cursor information is unavailable (tracker failure, input desktop not reachable).
  The host composites again.
- `COMPOSITED`: frames currently contain the cursor (see [Compositing](#compositing)). It reflects
  the capture backend too: AMD Direct Capture only draws the cursor with `amf_draw_mouse_cursor`.

### Validation

Neither side trusts the other. A wrong size, version or kind, unknown flag bits, non-zero
reserved bytes, out-of-range dimensions or inconsistent chunk bookkeeping drops the message
(logged once per session). Truncation tests cover every field boundary on both sides. The v1
parser and handler are not changed; a v1 client never receives version 2 because it never
subscribes.

### Why this framing

- **A version on 0x5509, not a new type.** v1 already starts with a version byte in both
  directions, so a v2 client and host tell the versions apart without another type. One type
  keeps a single cursor protocol with one dispatch point on each side.
- **A dedicated channel.** v1 shares channel 0 with rumble, HDR and termination, so a lost
  fragment of a large shape holds them up. On channel 0x09 it holds up only cursor traffic. ENet's
  reliable window is per peer, so a shape still shares bandwidth with channel 0. Shapes are
  deduplicated, sent once per session and budgeted at 2 MB/s; typical cursors are 4 to 16 KiB.
- **16 KiB chunks.** On its own channel the chunk size no longer affects other traffic; small
  chunks keep the host's per-message encryption buffer fixed and small.

## Compositing

One captured image feeds every session. The host leaves the cursor out of frames when:

```
not composited = v1_local_session_exists                          (today's v1 rule, unchanged)
              or (connected > 0
                  and every connected session is a v2 session with CLIENT_DRAWS
                  and the tracker is available)
```

Otherwise it composites, subject to the existing Ctrl+Alt+Shift+N toggle (`display_cursor`).
Sessions still waiting for their control peer count as connected and not drawing.

| Sessions on one host | Frames contain the cursor | Notes |
|---|---|---|
| Stock clients only | Yes | Unchanged. |
| v1 local session plus anything | No | Today's rule: other viewers lose the video cursor. A v2 client sees `COMPOSITED` clear and keeps drawing. |
| v2 drawing only | No | |
| v2 drawing plus a stock client, a v2 client in relative mode, or a v2 client with capture released | Yes | The drawing client sees `COMPOSITED` and hides its local cursor after a 300 ms grace (covering the round trip of its own request), so the cursor comes from the video. Absolute input and automatic switching continue. |
| v2 drawing plus v1 local | No | |

**Multi-client trade-off (for the PR).** v2 blends unless every viewer draws for itself, so a new
viewer never loses the cursor and a v2 client learns from `COMPOSITED` whether it must draw.
v1 sessions keep today's rule because a v1 client cannot read `COMPOSITED`: under the v2 rule it
would draw its own cursor on top of the video cursor whenever another client connected. The cost
is that a v1 local session still takes the video cursor from everyone else, as it does today.

## Host design

| Piece | Location | Notes |
|---|---|---|
| Wire format | `src/host_cursor_protocol.h` | Header-only encode and parse, unit tested. |
| Session policy | `src/host_cursor.{h,cpp}` | Per-session planner (diff, resync, warps, shape budget), shape registry deduplicated by content hash, injection history, compositing aggregate. Portable, unit tested. |
| GDI conversion | `src/host_cursor_bitmap.{h,cpp}` | Monochrome, colour, and colour plus AND mask cursors read with `GetIconInfo`, with the display-DPI size fix. Portable, unit tested. |
| Tracker | `src/platform/windows/cursor_tracker.{h,cpp}` | 200 Hz `GetCursorInfo` thread, running only while a session is subscribed: visibility, position, `GetClipCursor`, warp detection, rebinding to the input desktop. |
| Capture shapes | `cursor_channel`, `display_cursor.cpp` | Existing bridge, extended (below). It also holds the compositing decision the capture paths read. |
| Sending | `src/stream.cpp` | On the control thread, like every other control message. |
| Geometry | `mail::cursor_geometry` | The session's touch port, raised next to `mail::touch_port`. The input thread consumes the touch-port event, so the control thread gets its own copy. |

### One shape pipeline

Shapes are not converted twice:

- **DDA and VDD shapes** keep coming from the capture thread through `cursor_channel`, converted
  by Foundation's existing `normalize_cursor_shape` and `make_local_cursor_image`. The capture
  side publishes while any v1 local session or any v2 subscription exists. The v1 path (its
  revisions, republish on enable, failure handling) is unchanged.
- The tracker takes the newest capture shape from `cursor_channel` while DDA or VDD is
  capturing. That is what Windows draws on the captured output, at its DPI and the user's cursor
  size. Otherwise (WGC, AMD, before the first capture shape) it reads the `HCURSOR` with
  `GetIconInfo`/`GetDIBits` through `host_cursor_bitmap`, sized for the DPI of the display the
  cursor is on (Vibeshine `5cc738ba7`).
- An inverting pixel cannot be drawn by a client cursor. The GDI conversion uses Foundation's
  convention, a white fill with a black outline, so a cursor looks the same whichever source
  produced it and whichever protocol version delivers it.
- v2 shape ids come from the registry. The capture-local ids v1 uses are not sent in v2.

### Capture paths

Every Windows capture goes through `display_base_t::capture`, which passes `display_cursor` to
`snapshot()`. It now passes `display_cursor && compositing_required()`:

- **DDA** (`display_ram.cpp`, `display_vram.cpp`): `blend_mouse_cursor_flag` already ANDs it with
  v1's `use_local_cursor`.
- **WGC** (`display_wgc.cpp`, in process): `set_cursor_visible()` toggles
  `IsCursorCaptureEnabled` at runtime, so no capture restart.
- **Zako Direct VDD** (`display_vdd_vram.cpp`): `cursor_overlay_required` already uses it. The
  driver's cursor is polled while v2 wants shapes too, not only while it is composited or a v1
  session is local. While frames leave the cursor out, a cursor-only update produces no frame,
  except one that removes a cursor the previous frame still showed.

A switch takes effect on the next captured frame. Desktop Duplication reports a pointer shape
only when it changes, so `cursor_channel` keeps a publish epoch that moves when the first
consumer (a v1 local session or the v2 tracker) arrives, and capture publishes its stored shape
again; v1's republish on activation keeps working the same way.

### Control loop and input

- The control loop waits 5 ms while any session is subscribed to v2, as for DS5 haptics, and keeps
  16 ms for v1 local mode and 150 ms otherwise.
- `abs_mouse` records each injected absolute position (`note_absolute_injection`) so the tracker
  can tell client moves from host moves.
- The VRR input-activity boost counts mouse motion as activity while frames do not contain the
  cursor, for v1 or v2, because motion then produces no new frame.
- Linux and macOS hosts have no tracker, so they do not advertise `0x400` and ignore v2 requests.

## Client design

### moonlight-common-c (`mic`)

- `LI_FF_CURSOR_STATE 0x400`.
- `LiSetCursorStateCallbacks(stateCallback, shapeCallback)` registers the v2 callbacks before
  `LiStartConnection()`, following `LiSetRemoteTextContextCallback` and
  `LiSetDs5HapticsStateCallback`, so `CONNECTION_LISTENER_CALLBACKS` does not grow. NULL
  unregisters; unregistered v2 messages are dropped.
- `LiSendCursorControl(flags, resendShapeId)` sends the v2 request. It returns the existing
  `LI_CURSOR_MODE_*` codes: unsupported without `0x400` or on an unencrypted stream, not
  connected, send failed.
- `CursorStream.{h,c}` gains the v2 parser and shape assembly next to v1's, in the same
  state-plus-callback-plus-`void* context` style. `ControlStream.c` dispatches `0x5509` by its
  version byte. States and complete shapes are delivered synchronously on the control receive
  thread; callbacks copy and return.
- `LiSetCursorMode`, `cursorUpdate` and v1 parsing are unchanged.

### V+

- **State machine:** `cursormodepolicy.h` and `hostcursormath.h` from moonlight-qt
  `feature/client-side-cursor`, unchanged in behaviour, with the glue in input, mouse, keyboard
  and session, and the stats line in `ffmpeg.cpp`.
- **No delays:** no hysteresis, dwell, debounce or time-based suppression. Relative is entered on
  the first mouse motion after the host wants it, before that motion is sent. The return to
  absolute waits only for held buttons. A host lock or hidden cursor never confines the local
  cursor. v1's 150 ms hide debounce stays on the v1 path only: v2 visibility comes from
  `GetCursorInfo`, not DDA's flickering per-frame bit.
- **Ordering:** v2 states and shapes go to the main thread as one SDL event each, in order. V+'s
  v1 single-slot mailbox would drop a shape when a newer update replaced it.
- **Reconnect:** V+'s automatic reconnect is a new host session, so the client subscribes again,
  drops its shapes and stays in fallback until the new resync.
- **Rendering:** reuses V+'s cursor installation and native system-cursor substitution
  (`MOONLIGHT_NATIVE_CURSOR=0` turns it off), so recognized shapes use the client OS cursor.
  Other shapes are built at `scale_q16 × (video width in window units / ref_width)` with V+'s
  premultiplied smooth scaling, so they match their in-stream size. On macOS window units are
  points, which replaces V+'s backing-scale correction; the cursor is rebuilt when the scale
  changes (resize, fullscreen, moving to another display).
- **Settings:** "Automatic mouse mode" (`automaticMouseMode`), on by default, first in the Mouse
  card of `LegacySettingsPage.qml` (the only page with mouse options). While it is active:
  - "Optimize mouse for remote desktop" and Ctrl+Alt+Shift+M choose the fallback mode, and M is
    the switch-and-lock override.
  - "Show local cursor" applies only in fallback. In automatic absolute mode the client always
    draws the host's cursor, and Ctrl+Alt+Shift+C switches it to the video cursor for the session
    (and back).
  - The descriptions say so. `--automatic-mouse` / `--no-automatic-mouse` override it. With it
    off, nothing changes from today and no v2 callback is registered.
- **Translations** through `lupdate` into the `.ts` files, with zh_CN translated; `.qm` files are
  built, not edited.
- **common-c gitlink** moves from `2aa14ff` to the branch head on `mic`, which brings in the four
  newer `mic` commits (MAT transport, motion queue, DS5 actuator controls, IR v2 extended); V+ is
  built and tested against them.

## Compatibility

| Client | Host | Cursor protocol | Behaviour |
|---|---|---|---|
| Stock Moonlight | New Foundation | none | Unchanged. It never sends 0x5509, the tracker never starts, frames contain the cursor. |
| V+ today | New Foundation | v1 when absolute with Show local cursor, on DDA or VDD | Unchanged: same v1 messages on channel 0 and the same compositing. It ignores `0x400`. |
| New V+, automatic on | New Foundation, Windows | v2 | Automatic mode. v1 is never requested in that session. |
| New V+, automatic on | Old Foundation | v1 as today | `0x400` absent, so automatic mode is inactive and the client behaves as V+ does today. The stats line reads "fallback: host does not support automatic mode". |
| New V+, automatic on | Foundation on Linux, stock Sunshine, Apollo | as today | `0x400` absent. A host that sets it for another reason and never answers leaves the client in fallback. |
| New V+, automatic off | Any | as today | Identical to V+ today. |

## Testing

- Host (`tests/unit`): wire vectors and truncation, GDI conversion, geometry inverse mapping,
  session planner (diff, resync, warps, budget), compositing aggregate including v1 sessions,
  v1/v2 request handling.
- common-c: v2 vectors shared with the host, reassembly (chunking, reordering, oversize, 256 KiB
  cap), truncation at every byte, dispatch by version, v1 still parsed.
- V+ (`tests/`): state machine (first-motion switching, held buttons, clips, recentring, locks,
  focus), scaling, settings defaults and their interplay with the existing toggles.
- Hardware: Vibeshine's manual checklist, adapted for Foundation, run with DDA, WGC and Zako
  Direct VDD.

## Known limitations

- Animated cursors show their current frame.
- Window capture doesn't offer v2 and keeps the cursor in the video, because absolute input is
  mapped to the whole display rather than the captured window.
- With several clients moving one host cursor, one client's absolute moves are reported to the
  others as host moves.
- macOS and Wayland clients draw the cursor at point resolution; Wayland cannot warp.
- The tight-clip and recentring thresholds are heuristics; Ctrl+Alt+Shift+M or A locks a title
  they misjudge.
- Linux hosts: a PipeWire `SPA_META_Cursor` source could feed the same planner later.
