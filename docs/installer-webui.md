# Installer web front end

Status: 2026-11-09, branch `feat/installer-webui` (host repo, Panel repo, foundation-ui).

## Why

The decision: the installer must look and move exactly like the Panel. If the current architecture cannot do
that, change it. The native Win32 + Direct2D shell drew its own Material 3. Motion was hand-timed, child-control
buttons left transparent holes, the caption was duplicated and the corner radii did not match. The Panel is Tauri 2 +
WebView2 with the foundation-ui (m3e) Vue components and their motion. The way to look and move like the Panel is to
use the same components in the same engine.

## Shape

```
Sunshine-setup.exe  (outer, the one download)
 = installer shell (C++, tools/installer_shell.cpp) + setup UI exe + Inno engine + footer
   │
   ├─ /VERYSILENT, /SILENT ─────────► worker_main(): engine runs unattended, no UI (unchanged)
   ├─ --worker (elevated, runas) ───► protected run dir, engine, events.jsonl, install-result.json (unchanged)
   ├─ --native ─────────────────────► the Direct2D window (unchanged)
   └─ default ──► web_main() (tools/installer_web.inc)
                  │  no WebView2 Runtime / no UI payload / UI does not start or load in 15 s
                  │  ──► the Direct2D window (fallback; nothing has been shown yet)
                  ▼
                  extracts foundation-setup-ui.exe to %TEMP%\FoundationSunshine-setup-ui-<id>\
                  starts it with stdin/stdout on pipes (only these two handles inherited)
                     host → UI: {"type":"view","view":InstallerView}   (whole snapshot on every change)
                     UI → host: {"type":"hello","hwnd":N} {"type":"loaded"} {"type":"action","action":{…}}
                  every action is the native window's click() for that page: begin_install(), poll_run(),
                  finish_run(), launch_panel(), restart_windows() are the same code for both front ends

foundation-setup-ui.exe  (Panel repo installer-ui/, Tauri 2.12 like the Panel, 3.3 MB)
   820×560 undecorated window, not resizable; DWM rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = ROUND,
   exactly as the Panel main window) and shadow(true); shown cloaked until the page reports 'ready'
   pages: foundation-ui src/vue/installer/MInstaller.vue (pages agent), on src/vue/installer/bridge.js
```

### Which exe is outer, and why the shell

The outer exe stays the C++ shell. It must run where WebView2 cannot (no Runtime, session 0, a broken WebView2) and
in silent mode. A Tauri exe does not start without the WebView2 Runtime. The shell already holds the parts that work:
silent mode, the elevated worker, payload extraction, maintenance detection and the native fallback. The UI process
only draws. It never touches the install, so a crash of the page cannot break an install. While the worker runs the
host keeps the run going, and closing the window only minimises it, as before.

### Why a separate small crate and not a second bin of the Panel

`installer-ui/` has its own `Cargo.toml` and depends only on `tauri`, `serde_json`, `raw-window-handle`, `windows`
and `webview2-com`, at the versions in the Panel's `Cargo.lock` (copied). The Panel crate carries about 40 plugins and
native modules. A second bin there would rebuild with all of them and would share the Panel's capabilities and
`build.rs` command manifest. The small crate builds in 90 s cold on Windows test host with the same MSVC toolchain
(`msvc-env.cmd`).

### Bridge contract

`foundation-ui src/vue/installer/bridge.js` is the contract (JSDoc), with a scripted mock host that runs the same
state machine as the shell. `InstallerView` is the JSON form of `tools/installer_d2d.h struct view`. The host
writes it in `view_json()`. Session-0 runs check every key against `emptyView()`. Actions: ready, install, retry,
customize, back, toggle-component, select-all, uninstall, open-panel, restart-pc, copy-details, open-log-folder,
minimise, exit.

### WebView2 Runtime missing: fall back, do not bootstrap

The Runtime ships with Windows 11. On earlier systems, Microsoft says other apps have usually installed the Evergreen
Runtime, but "some devices might not have the Runtime pre-installed", so an app should check first
(installer-ms-webview2-distribution-20261109). The shell checks `pv` under
`HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-…}` and under HKCU. A missing value, an empty one
or 0.0.0.0 counts as not installed. In that case the shell uses its native window.

Bootstrapping was rejected for three reasons. The Evergreen Bootstrapper downloads the Runtime, which breaks an
offline install. The Standalone Installer is far larger than the whole package. Also, the native front end already
works for these machines.

### Silent mode

Unchanged: `/VERYSILENT` or `/SILENT` goes straight to `worker_main()` before any UI code.

## Fixes in the same change

- **a) Start guide on upgrade.** 开启串流旅程 passes `--start-guide` only on a fresh install, or when the host
  configuration (`<InstallDir>\config\sunshine.conf`) has no `guide_finished = true`. That is the key foundation-ui
  `guideHost.js` writes. Without the flag, the Panel's own `guideEntry` decides. Both front ends use this
  (`guide_pending()` in installer_shell_ui.inc).
- **b) Drivers reinstalled at the same version.** VDD (`vdd-device-helper.ps1`: healthy device with the bundled
  `DriverVer` and a working control interface), ViGEmBus (file version) and usbip-win2 (uninstall `DisplayVersion`)
  already skipped a current driver. The Windows test host `inno.log` files of 2026-11-09 17:50 and 17:51 show these steps
  finishing in 0.2–1 s. The Virtual Mouse did not skip: `install-vmouse.bat` removed every device node and
  reinstalled the driver on every run. It now skips when there is exactly one `Root\ZakoVirtualMouse` device, its
  status is OK, its `DEVPKEY_Device_DriverVersion` (Get-PnpDeviceProperty) equals the `DriverVer` version of the
  bundled INF, and the installed payload exists (installer-ms-inf-driverver-20261109,
  installer-ms-devpkey-driverversion-20261109, installer-ms-get-pnpdeviceproperty-20261109).

## Verification and its limits

- WebView2 cannot create its window in session 0 (ssh). Starting the setup UI there fails with HRESULT 0x80070578
  (invalid window handle). The shell then falls back to the native window, which is the designed path.
- The pipe protocol and the host logic were exercised in session 0 with `tools/installer_web_fake_ui.cpp`: hello,
  loaded, view snapshots, customize / toggle / select-all / back / exit, an invalid component id ignored, exit code
  0, temp directory removed.
- The Tauri side (commands, events, bridge.js Tauri transport, the page) runs on Linux (webkit2gtk) under Xvfb with
  a scripted host. WebView2, DWM corners, shadow and real motion on Windows need an interactive Windows test
  session. Until one exists they are unverified.

## Packaging

1. Panel repo: `npm run build:installer-pages` (vite, `FOUNDATION_UI_DIR` = the foundation-ui checkout with the
   pages), then `cargo build --release --manifest-path installer-ui/Cargo.toml` in `msvc-env.cmd`. Output:
   `installer-ui/target/release/foundation-setup-ui.exe`, or `%CARGO_TARGET_DIR%`.
2. Host repo: the CMake target `sunshine-installer-shell` as before.
3. `append_installer_payload.py <shell> <engine> <output> --ui foundation-setup-ui.exe`. The footer keeps the V1 keys
   first, so an older shell still finds the engine. Without `--ui`, the package is the native installer.
