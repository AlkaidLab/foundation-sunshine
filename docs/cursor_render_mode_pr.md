# PR: Cursor Render Mode

## Problem

Remote cursor rendering currently depends on the server cursor being present in
the encoded video frame. During remote streaming this makes pointer movement
look delayed and can increase video traffic when the desktop did not otherwise
change.

## Design Summary

This change adds `cursor_render_mode`, separate from the existing mouse input
mode. Mouse input injection remains unchanged. The new setting controls only how
the cursor is displayed:

- `remote`: legacy video-baked cursor behavior.
- `client`: keep the system cursor out of video and send cursor shape/state over
  a cursor metadata channel.
- `auto`: select `client` only when the client announces cursor-channel support
  and the capture backend can provide cursor metadata.

Per-app `cursor-render-mode` supports `inherit`, `remote`, `client`, and `auto`.
Old clients do not announce `x-ss-general.cursorChannel`, so the host forces
`remote` and sends no cursor TLVs.

## Code Map

- Config and app parsing: `src/config.*`, `src/process.*`, `src/rtsp.h`
- Mode resolution and cursor metadata helpers: `src/cursor_render.*`
- RTSP capability negotiation: `src/rtsp.cpp`
- Encrypted control TLVs: `src/stream.cpp`, `src/stream.h`, `src/globals.h`
- Video/capture propagation: `src/video.cpp`, `src/video.h`
- DDA RAM metadata and cursor-only skip: `src/platform/windows/display_ram.cpp`
- DDA VRAM metadata and cursor-only skip: `src/platform/windows/display_vram.cpp`
- Capture base plumbing: `src/platform/windows/display_base.cpp`, `src/platform/windows/display.h`
- UI and i18n: `Inputs.vue`, `AppEditor.vue`, `useConfig.js`, English/Chinese locale files
- Client contract: `docs/cursor_render_mode_protocol.md`

## Implemented Paths

- Desktop Duplication RAM and VRAM publish `CURSOR_SHAPE` and `CURSOR_STATE`.
- DXGI color cursor shapes are sent as BGRA32 straight-alpha payloads.
- Shape IDs are hashed from format, dimensions, hotspot, pitch, and pixels.
- Shape packets are reliable and deduplicated by `shape_id`.
- State packets are latest-only and rate-limited to 240 Hz. They use unreliable
  ENet packets when the client announces `cursorStateUnreliable`.
- In `client` mode, DDA mouse-only updates publish cursor metadata and return
  no video frame, so the encoder is not triggered.
- Unsupported DDA cursor shape types set `fallback_remote`, notify the client
  with reliable `CURSOR_MODE(remote/fallback)`, and temporarily blend the cursor
  into video until a supported shape appears.
- WGC is treated as a non-metadata backend for `auto`; `remote` keeps WGC cursor
  capture enabled, while `client` effective mode is not selected for WGC in this
  MVP.

## Compatibility

Legacy clients remain on `remote` because they do not announce the cursor
channel. `cursor_render_mode=remote` also preserves existing behavior for new
clients. The existing `mouse-mode`, relative mouse packets, absolute mouse
packets, button handling, and wheel handling are not refactored.

## Fallback

Fallback to remote occurs when:

- The client lacks cursor-channel capability.
- The capture target/backend cannot provide cursor metadata.
- The app override or requested mode is `remote`.
- A DDA cursor shape is unsupported or invalid.

OS cursor invisibility does not force remote fallback; the client cursor should
be hidden while video remains cursor-free.

## Tests And Validation

- Added unit coverage for mode parsing, app override precedence, client
  capability fallback, backend fallback, shape ID stability, sequence ordering,
  global config parse, app config parse, and mouse-only no-frame logic.
- `git diff --check`: pass.
- `c++ -std=c++23 -I. -c src/cursor_render.cpp`: pass.
- Locale JSON parse for `en.json` and `zh.json`: pass.
- `npm run build`: not run to completion because this checkout has no local
  `vite` binary (`node_modules` is not installed).
- `cmake -S . -B /tmp/foundation-sunshine-build -DBUILD_TESTS=ON -DBUILD_DOCS=OFF`:
  not run because `cmake` is not installed in this environment.
- `npm run i18n:validate`: fails on the repository's existing locale baseline;
  after adding the new English base keys, non-English locale files also report
  missing translations unless synced/translated.

## Follow-Up

- Add real Moonlight client implementations for native and overlay cursor
  rendering.
- Add client feedback for native/overlay cursor creation failure.
- Convert masked color and monochrome/XOR cursor shapes instead of falling back
  remote.
- Revisit mixed multi-client sessions where one client requests remote and
  another requests client cursor rendering.
- Promote cursor state to an explicit unreliable sequenced control path if the
  client ecosystem needs stronger transport semantics than latest-only ENet.
