# Launch splash (启动画面)

While an app starts, the display the stream captures shows the app's hero, its name, an indeterminate progress bar and one stage line, until the
game's window is up. Every client sees it, because it is drawn on the host display. Setting: `launch_splash` (default on, 常规 page of the host
settings, applied at the next launch without a restart).

## Who does what

| Part | Does |
| --- | --- |
| Host (Core) | Decides the state: stage, one notice line, the process ids to look for, a deadline. Publishes it as `launch` in the tray state (`launch-splash-v1`). |
| Launcher (`sparxie-launch`, any other) | Writes its stage to the status file the host names in `SUNSHINE_LAUNCH_STATUS_FILE`. |
| Panel (sunshine-gui, user session) | Draws the splash, finds the game's window, closes the splash. Core may be a service in session 0 and cannot see the user's windows. |

## Host state (`launch` in the tray state)

`id`, `active`, `kind` (`launch` or `resume`), `app_id`, `app_name`, `stage`, `notice`, `display` (the Windows display the stream captures, empty for the
primary one), `display_ready`, `pids`, `started_at_ms`, `deadline_ms`. After the launch ends the state stays with `active: false` and the same `id`.

Stages in order: `starting`, `broker`, `preparing`, `injecting`, `waiting-window`; `failed` ends a launch that did not work. A plain app goes
`starting` then `waiting-window` once its process exists.

Ends, whichever comes first: the Panel sees the game's window; the host ends it (the app exited, the last video session ended); `deadline_ms`
(60 s after the start; 8 s for a resume, 5 s after a failure so the reason can be read).

## Order of a launch (the splash comes first)

1. The launch request is accepted (app known, no app running): the host publishes the launch at once with `display_ready: false`. The Panel readies its
   window (made, page loaded, picture decoded) and does **not** show it: the display the stream captures does not exist yet (virtual display, mode, DPI), and a
   window on the old display would land on a screen that is about to change.
2. The display and encoder setup runs (seconds).
3. The host publishes `display_ready: true` and the display name. The Panel puts the window on that display and, when the first frame is painted there (two
   animation frames after the window is shown), tells the host with the tray action `launch_splash_shown` (`launch_id`, `panel_ms` = ready-state received to
   first frame, `prepare_ms` = announcement received to window ready).
4. Just before the app command starts, the host waits for that answer, at most 800 ms. It does not wait when no Panel that can answer is connected (the
   Panel asks for events with `launch_splash_ack=1`; `launch-splash-shown-v1` in the capabilities says the host takes the answer). A Panel that is slow or
   gone costs at most the bound, and the host logs the measured time on every launch.
5. A request that fails before the app starts withdraws the announcement (the launch ends, the Panel hides what it had readied).

A resume asks about an app that already runs: the Panel shows the splash only when no window of the app is found 400 ms after the request.

## Status file (launcher to host)

The host sets two environment variables on the app command: `SUNSHINE_LAUNCH_ID` and `SUNSHINE_LAUNCH_STATUS_FILE`
(`<config>/launch/status-<id>.json`). The launcher writes the whole file to a temporary name beside it and moves it over (never a partial write):

```json
{ "launch": "<SUNSHINE_LAUNCH_ID>", "stage": "preparing", "pids": [1234], "notice": "direct" }
```

- `launch` must equal `SUNSHINE_LAUNCH_ID`; a file of another launch is ignored.
- `stage`: one of the stages above; stages only advance, an older one is ignored. `failed` needs `reason`.
- `reason`, `notice`: short codes (`a-z`, `0-9`, `-`, at most 32); anything else is dropped. The Panel turns a code into a sentence.
  Reasons: `broker`, `rejected`, `rpc`, `session-failed`, `lost`, `timed-out`, `config`, `usage`, `internal`, `start`, `prep`.
  Notices: `direct` (Sparxie could not take over, the game was started directly: shown for 5 s, the launch goes on).
- `pids`: the game's own processes once the launcher knows them (the game is not a child of the launcher: the Broker starts it).
- The host removes the file when the launch ends. A launcher that cannot write the file loses only the stage lines.
