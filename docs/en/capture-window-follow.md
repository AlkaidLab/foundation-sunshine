# Session-scoped window-follow capture

[Project home](../readme/README.en.md) · [Documentation](README.md) · [简体中文](../zh-CN/capture-window-follow.md)

## What it is

By default Sunshine captures an entire display. When Playnite is streamed through Moonlight, a **windowed game** launched by Playnite therefore only fills a small part of the phone screen.

This feature adds a **local, session-scoped control endpoint**: a trusted local client (for example the Playnite plugin) can temporarily change the capture target of **one streaming session** from the display to **a followed window**:

- Only the session that sent the request is affected; other sessions and scenarios are untouched.
- `sunshine.conf` and the global capture settings are not modified.
- When the request fails or the window is missing, capture falls back to the display and the stream keeps running.
- When the override is disabled or the session ends, the original behavior is restored.

## How it works

1. Every video session owns a `streaming::capture_target_t` (created by `stream::session::alloc()`), shared with the capture backend through `video::config_t::capture_target`.
2. The local control endpoint writes a `window_capture_override_t` into that object and bumps `window_override_generation`.
3. The capture thread (`captureThread` in `src/video.cpp`) notices the generation change and triggers one capture rebuild using the existing reinit mechanism.
4. The Windows WGC backend reads the override during `init`:
   - `foreground`: follow the current foreground window (desktop, taskbar, tool windows, minimized windows and Sunshine's own windows are excluded). To avoid following a transient window while a game exits, foreground candidates are filtered to the **process tree of the app Sunshine launched** (Playnite and its children) or to **processes started within 10 seconds around the enable request** (covering Steam/launcher chains); when neither matches, capture falls back to the display.
   - `title`: substring match on the window title.
   - `pid`: the window of the given process.
5. While capturing, the target is checked roughly every 250 ms: when the foreground changes to another eligible window, the target window closes/is minimized, or the window reappears, `reinit` is returned so the capture thread rebuilds.
6. When no eligible window exists, capture falls back to the display and keeps retrying; when a window appears it switches back automatically (and reports the state to the client).

> Difference from the existing global window capture (`capture_target=window` + `window_title`): that one is **global configuration, affects all sessions and needs a restart**; this feature is **session-scoped, takes effect at runtime and affects one session only**.

### Window geometry and absolute input

While a window is captured, Sunshine tracks the window's visible rectangle in virtual-screen coordinates (`DwmGetWindowAttribute`/`GetWindowRect`, clamped to the virtual desktop) and republishes the touch/pen input port whenever it changes. This keeps absolute mouse/touch/pen input aligned when the window moves between monitors, changes size, or lives on a monitor with a different DPI scale.

## HTTP API

- Path: `GET|POST /api/capture/window-follow`
- Authentication: the same as the Web UI; **localhost requests only** (`authenticate()` + `require_localhost()`).
- Returns 404 outside Windows (the route is not registered).
- Requires **user mode** (WGC cannot capture a SYSTEM service); otherwise returns 409 `wgc_requires_user_mode`.

### GET status

```http
GET /api/capture/window-follow
```

Example response:

```json
{
  "ok": true,
  "feature": "window-follow",
  "supported": true,
  "sessions": [
    {
      "session_id": 1,
      "client_uuid": "…",
      "client_name": "Moonlight",
      "app_name": "Playnite",
      "running": true,
      "control_only": false,
      "override": { "enabled": true, "mode": "foreground", "window_title": "", "process_id": 0 },
      "observed": {
        "active": true,
        "fallback_to_display": false,
        "window_title": "Example Game",
        "process_id": 1234,
        "hwnd": "0x0000000000012345",
        "fallback_reason": ""
      }
    }
  ]
}
```

When `supported=false`, `reason` explains why (for example `wgc_requires_user_mode`).

### POST enable / disable / status

```http
POST /api/capture/window-follow
Content-Type: application/json

{
  "action": "enable",            // enable | disable | status (default: enable)
  "mode": "foreground",          // foreground | title | pid (default: foreground)
  "window_title": "",            // required when mode=title
  "process_id": 0,               // required when mode=pid
  "session_id": 0,               // optional: target one session by ID (enable still returns 409 with multiple sessions)
  "client_uuid": ""              // optional: target by client certificate UUID (must match exactly one session)
}
```

Session selection rules:

- `action=enable`: only allowed when **exactly one active video session** exists. With multiple active video sessions it always returns 409, even if `session_id` or `client_uuid` is supplied. This is a transition-time restriction: multiple sessions may share the capture thread/backend and the rebuild on enable cannot be guaranteed to affect only the selected session.
- `action=disable` / `action=status`: session targeting is supported:
  - `session_id` present → match by session ID;
  - `client_uuid` present → match by client certificate UUID; multiple matches return 409 and require `session_id`;
  - neither → exactly one active video session is selected automatically; 0 → 404; multiple → 409 requiring an explicit selector.
- Multiple sessions are **never** modified silently; `disable` only clears the window-follow state of the selected session so it can be rolled back during multi-session transitions.

A successful response matches the GET body and additionally contains `selected_session_id` / `selected_client_uuid`.

### Error codes

| HTTP | error | Meaning |
|---|---|---|
| 400 | `invalid_action` / `invalid_mode` / `window_title_required_for_title_mode` / `process_id_required_for_pid_mode` | Invalid parameters |
| 401 | — | Authentication failed |
| 403 | — | Non-local request |
| 404 | `no_active_video_session` / `session_not_found` / `client_session_not_found` | No controllable session |
| 409 | `multiple_video_sessions_enable_not_supported` / `multiple_video_sessions_specify_session_id` / `multiple_client_sessions_specify_session_id` / `wgc_requires_user_mode` | Enable not allowed with multiple sessions / disable·status need an explicit selector / client_uuid matched multiple sessions / service mode unsupported |
| 500 | `session_capture_target_unavailable` | The session has no capture target object |

## Playnite plugin integration

Companion Playnite plugin (separate project):

- `OnGameStarted` (after a short delay for the window to appear) → `POST {action:enable, mode:foreground}`
- `OnGameStopped` → `POST {action:disable}`
- With no active stream the session list is empty → the endpoint returns 404 and the plugin silently skips (local play is unaffected)

## Compatibility and default behavior

- **Off by default**: as long as no client calls the endpoint, the capture path, configuration parsing, Web UI and native Sunshine behavior are identical to upstream.
- No new required settings; `sunshine.conf` and `apps.json` need no migration.
- Not affected: desktop streaming, launching apps directly from Moonlight, Web UI configuration, virtual displays / Zako Direct (a window-follow session temporarily uses WGC capture, for that session only).
- Multi-session transition restriction: this version only allows enabling window-follow with **exactly one active video session**. With multiple active video sessions, `enable` returns 409 even with an explicit `session_id`/`client_uuid`; `disable`/`status` can still target a session by `session_id`, and `client_uuid` matching multiple sessions returns 409 and requires `session_id`. Window-follow only rebuilds the shared capture thread of the selected session; until capture threads are isolated per session, avoid mixing enable operations across multiple active sessions.

## Known limitations

1. Window-follow is based on **WGC**: Windows 10 1903+, running in user mode. **Exclusive fullscreen / DRM-protected windows may not be captured**; capture then falls back to the display.
2. Foreground candidates are filtered to the process tree Sunshine launched or to newly started processes, so a browser or other long-running application is not followed. After a game exits, another window grabbing the foreground only causes a fallback to display capture, not a switch to the unrelated application. Use `pid` mode when stricter matching is required.
3. Covers the **asynchronous shared capture path** (the default for Windows hardware encoders); the synchronous software-capture path does not rebuild at runtime and applies the override only when the session starts.
4. HDR: window-capture HDR metadata is not fully equivalent to display capture; prefer display capture for HDR streaming.
5. Window titles and foreground switching depend on Windows window state; some launcher chains (Playnite → launcher → game) briefly follow the launcher window before switching to the game.
6. While more than one active video session exists a new window-follow cannot be enabled (`enable` returns 409); end other video sessions first so the target session is the only active one. An already enabled session can still be restored with a targeted `disable`.

## Manual verification checklist

1. Start Sunshine in **user mode** (not as a SYSTEM service) on Windows 10 22H2+ with WGC available.
2. Connect from Moonlight → launch Playnite (Fullscreen) → the phone shows Playnite.
3. Locally run:

   ```powershell
   # List sessions
   curl.exe -k "https://127.0.0.1:47990/api/capture/window-follow"
   # Enable window-follow
   curl.exe -k -X POST "https://127.0.0.1:47990/api/capture/window-follow" -H "Content-Type: application/json" -d '{"action":"enable","mode":"foreground"}'
   # Disable
   curl.exe -k -X POST "https://127.0.0.1:47990/api/capture/window-follow" -H "Content-Type: application/json" -d '{"action":"disable"}'
   ```

4. Start a **windowed** game from Playnite: the phone picture should switch from the desktop/Playnite to the game window and fill the screen; the log shows `Window-follow: capturing window [...]` or `Window-follow: switching captured window to [...]`.
5. Close the game: the phone picture should return to the Playnite fullscreen UI.
6. Without launching a game, stream the browser/desktop: behavior is identical to before the patch (the feature stays out of the way by default).

## Rollback

The feature is merged as normal commits; the repository does not ship a reverse patch. To roll back, revert the commits in reverse order:

```powershell
cd <foundation-sunshine>
git log --oneline --grep="Plan4"   # find the Plan4 commits
git revert <plan4-commit>...       # apply reverts newest to oldest
```

To discard uncommitted local changes only:

```powershell
git restore -- src cmake tests docs/en/capture-window-follow.md `
  docs/zh-CN/capture-window-follow.md
```

No configuration migration is involved; nothing has to be cleaned up after a rollback.
