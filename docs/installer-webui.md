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
customize, back, toggle-component, select-all, choose-dir, uninstall, open-panel, restart-pc, copy-details,
open-log-folder, minimise, exit. `installDir` (path, editable, freeBytes, neededBytes, maxLength, error) is the install
location; see "Install location" below.

### WebView2 Runtime missing: the window falls back, the worker downloads the Runtime afterwards

The Runtime ships with Windows 11. On earlier systems, Microsoft says other apps have usually installed the Evergreen
Runtime, but "some devices might not have the Runtime pre-installed", so an app should check first
(installer-ms-webview2-distribution-20261109). The shell checks `pv` under
`HKLM\SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-…}` and under HKCU, in both registry views (the 32-bit view of HKLM is
`WOW6432Node`). A missing value, an empty one or 0.0.0.0 counts as not installed. In that case the setup window is the native
one: a Tauri exe does not start without the Runtime.

The Panel needs the Runtime, so the elevated worker installs it after a successful engine run
(`tools/installer_webview2.h`, steps in `tools/installer_shell.cpp`):

- Installed: nothing is downloaded or run (`"webview2":"present"` in `install-result.json`).
- Missing: it downloads Microsoft's Evergreen Bootstrapper from the fixed link
  (`https://go.microsoft.com/fwlink/p/?LinkId=2124703`, HTTPS only, never a redirect to http, 8 MB cap, two attempts within one 90 s deadline that is checked between reads, a second attempt only if 30 s are left, and each WinHTTP wait (resolve, connect, send, receive: 10, 10, 15 and 15 s) is bounded, so the worst case is about 110 s) into the
  protected run directory, checks its Authenticode signature (`WinVerifyTrust`, and the organisation of the signing certificate
  must be exactly `Microsoft Corporation`), and runs `/silent /install` per machine. The hash is not pinned because Microsoft
  replaces the file behind the link; the signature is the check. The downloaded file is deleted afterwards.
- Whether it worked is read from the registry, not from the bootstrapper's exit code.
- A failure (offline, a proxy, a bad status, a signature that is not Microsoft's, the bootstrapper failing) never fails the
  install of the host. The events file gets a `webview2` phase with state `failed`, and `install-result.json` gets
  `"webview2":"missing","webview2_reason":"download_failed|signature_invalid|install_failed","webview2_code":<n>,
  "webview2_manual_url":"<the link>"`. The done page then tells the person the Runtime is missing and gives the link, and the
  Panel reports the missing Runtime when it starts (its native fallback, exit code 0x5352, which the service does not restart in a loop).

The installer used to carry the bootstrapper (1.9 MB, about 1.8 MB of the download). It no longer does. The Standalone
Installer for a fully offline machine stays the person's choice: it is far larger than the whole package.

### Silent mode

`/VERYSILENT` or `/SILENT` goes straight to `worker_main()` before any UI code. `/DIR=` is forwarded to the engine after the same
checks as the window's (see "Install location").

### Install location

A fresh install shows the folder on its first page: the default (`Program Files\Sunshine`), the free space of its drive, a field
the path can be typed in, and a Change button that opens the system folder picker (`IFileOpenDialog`, `FOS_PICKFOLDERS`), over the
setup window in both front ends. An upgrade or the maintenance page shows the folder of the installed copy and cannot change it: to
move, uninstall and install again. The folder travels to the elevated worker as `--dir "<path>"` and from there to the engine as
`/DIR="<path>"`; the default folder is never passed, so its behaviour (an older copy there, the previous folder of an update) is unchanged.

**Which folder.** What is typed is the folder as typed (spaces at the ends and one pair of quotes, which "Copy as path" adds, are
dropped). A folder picked in the picker gets `\Foundation Sunshine` appended, and the page shows the whole path: this is what Inno Setup
does after Browse (`AppendDefaultDirName`, authority `installer-inno-appenddefaultdirname-20261119`) and what NSIS does with the last
part of `InstallDir` (`installer-nsis-installdir-20261119`); Windows Installer's own browse dialog (`PathEdit`,
`installer-msi-pathedit-20261119`; WiX `WixUI_InstallDir`, `installer-wixui-installdir-20261119`) takes the selected path as it is, which
is what typing gives here. Nothing is appended to the default folder, to a folder already named `Foundation Sunshine`, or to an
existing Sunshine install. The root of a disk (`D:\`) gets it too, so it is never the install folder. After the append the page offers
"Use the selected folder itself" (`use-picked-dir` / the text button of the native window) for as long as the path is the picked one
with the name added; the folder picked is then checked like any other. A picked folder that has somebody's files in it is not
refused: the files are left alone, a folder inside it is used, and the page says so (`note: redirected`); only choosing that folder
itself is refused (rule 4), which protects the person's files from the uninstaller's cleanup.

The rules are `tools/installer_dir.h` (pure functions, `tests/unit/test_installer_dir.cpp`), the machine's answers are
`tools/installer_dir_win.h`. There are two kinds of answer. A **problem** refuses the folder: the page shows the first rule that
fails under the row and keeps Install and Customize off. A **warning** lets the folder through after the person confirmed it once.

Problems:

1. the text: a full drive path (no UNC, `\\?\`, relative or `..` part), parts Windows allows (no reserved name, no trailing space or dot),
   none of the characters the batch scripts, PATH and the engine's constants cannot carry (`! % ^ & ; , = ' ` { } $`); non-ASCII
   (Chinese and the like) is accepted and counted in UTF-16 units; not the root of a disk; at most `kMaxLength` characters
   (MAX_PATH minus the longest path of the package and a margin; `scripts/ci/check-install-path-budget.py` keeps that true);
2. the disk: a fixed disk of this PC (`GetDriveType`, and not a `subst` drive), NTFS or ReFS (FAT32 and exFAT have no
   permissions to protect the program with);
3. the place: not inside the Windows folder or a synced folder (OneDrive's folders, or a part named OneDrive, Dropbox, ...);
4. the folder itself: new, or empty, or an existing Sunshine folder (the registered one, one with the program and its
   uninstaller, or one that holds only the `config` folder an uninstall left behind with the person's settings); not a file, a junction or link (here or above), not unreadable;
5. room: `kNeededBytes` free on the drive.

Warnings (`installDir.warning`), the folder works and is protected itself, but sits where others can replace it from above:

- `parent`: an existing folder between the drive root and the install folder can be renamed, deleted or re-permissioned by an ordinary user, or has a non-system owner (see "Protection" below);
- `user`: inside a user's folder (profile, Desktop, Documents, Downloads, including a redirected one). `user` is named before `parent` when both apply.

A refused folder shows its reason only; a warning shows the risk (the service runs the programs as SYSTEM, so what others can swap from above
runs with those rights) and a button to the safe place (`use-safe-dir`): the default folder when the warning is on the system drive, otherwise
`<drive>:\Foundation Sunshine` directly on the drive (`installer_dir::safe_alternative`); the window checks that candidate with the same rules and offers it only when it has no problem or warning of its own (a drive root where ordinary users can delete folders would make it a risk again), otherwise there is no safe place to offer. Install stays on. Pressing it asks once: the host
sets `installDir.confirm`, the web page opens a dialog (`InstallerDirConfirm`) and the native window shows the same question as a page, with the
risk, the folder and the safe place; the safe place is the primary button, "Install anyway" counts down `kConfirmSeconds` (5, the countdown of the
other risky questions of the product) and Esc cancels. The host keeps the delay too: `confirm-dir-risk` is taken only while the question is open and
after the seconds, so a page that counts wrong cannot skip them. What was confirmed holds for that folder only: any other folder, or a refusal by the
worker, asks again. The install folder itself is made as protected as Program Files in every case.

The elevated worker checks the folder again (it never takes the shell's word), refuses a second place for an installed copy, and writes
`dir_error` into `install-result.json` when it refuses, which the window shows on the first page. A warning is passed to it as
`--dir-unsafe-ok` only after the confirmation; without that argument (and without `/ALLOWUNSAFEPARENT` in silent mode) the worker
refuses a warning like a problem, with `detail: needs_allowunsafeparent`.

The web page sends `set-dir` for the typed path (after a pause in typing, on Enter and when it loses focus), `choose-dir`, `use-picked-dir`,
`use-safe-dir`, `confirm-dir-risk` and `cancel-dir-risk`; every answer is a new snapshot with `installDir` (`error`, `warning`, `safePath`,
`pickedPath`, `note`, `confirm`, `confirmSeconds`). The native window has the same on its first page: an edit window (an owned pop-up, so that the
Direct2D surface does not paint over it; IME and the clipboard work as in any edit), the Change button, the two text buttons and the question page.

#### Protection

The service runs the programs of the install folder as SYSTEM. Program Files is protected on every level; a folder below the
root of another drive inherits "Authenticated Users: Modify" from that root, so any local user could replace `sunshine.exe` or
`sunshinesvc.exe` and be SYSTEM at the next start. The install folder outside Program Files is therefore given the same
protection (`kProtectedSddl`, and `HardenInstallDir` in the Inno script, which replaces the whole DACL again on every install and update, so
that nothing a parent left behind survives: a new folder gets an entry for the person who made it from the parent's CREATOR OWNER entry,
and "remove inheritance" does not touch that one): inheritance removed; SYSTEM, Administrators and NT SERVICE\TrustedInstaller full control; Users and ALL APPLICATION PACKAGES read
and execute; all of it for the folder and everything in it; the owner is Administrators (the owner can always change the
permissions). The worker creates the folder, and any missing folder above it, with that DACL from the first instant
(`CreateDirectory` with a security descriptor), and sets owner and DACL on an existing empty folder. An existing folder that is not the registered install but holds something (the `config` folder of an uninstalled copy, or files of someone else)
gets owner and permissions of everything in it reset by the engine before the install (`takeown /R`, `icacls /reset /T`), because the owner of a file can always change its permissions.

Protecting the folder is not enough when a folder above it can be renamed: the service path would then lead into someone
else's tree. That is the `parent` warning: an existing parent that ordinary users can rename, delete or re-permission (which is what a
folder made by a person under the root of a data drive is) is a risk the person is told about and has to confirm, and the safe place (directly on the drive, or
the default folder) has no such parent. A folder directly on the drive or a missing chain that the worker creates has no warning.

## Fixes in the same change

- **a) Start guide once.** 开启串流旅程 passes `--start-guide` (the whole first-run flow from its first step) only on a fresh
  install, and only while the host configuration (`<InstallDir>\config\sunshine.conf`) has neither
  `guide_finished = true` nor a `guide_closed` version: the keys foundation-ui `guideHost.js` writes when the person
  finishes or closes the guide. An upgrade or a maintenance run never passes the flag; the Panel's own `guideEntry`
  decides and shows what is new once per version. The keys live in the configuration, which an upgrade keeps. The
  rule is `installer_guide::start_guide()` in `installer_guide.h`, used by both front ends and covered by
  `tests/unit/test_installer_guide.cpp`.
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
   `scripts/build-setup-ui.ps1` runs `scripts/ci/prune-setup-ui.py` on `dist-installer` between the two: the pages
   import the whole foundation-ui stylesheet, which carries the clock components and their four fonts, and the
   installer page has no clock. The pruner removes those rules and font files and refuses to delete anything the
   remaining CSS, JavaScript or HTML still names (74,846 bytes of the current build).
2. Host repo: the CMake target `sunshine-installer-shell` as before.
3. `append_installer_payload.py <shell> <engine> <output> --ui foundation-setup-ui.exe`. There is no WebView2 bootstrapper in
   the package (see above). The shell and the setup UI have their COFF symbol tables removed first
   (`tools/pe_symbols.py`); the CodeView GUID stays, so the published symbol zip still matches a crash dump. The footer
   keeps the V1 keys first, so an older shell still finds the engine. Without `--ui`, the package is the native installer.

### CI (`.github/workflows/main.yml`, job Windows)

The workflow makes the same package. The setup UI comes from `scripts/fetch-setup-ui.ps1` when `scripts/installer-inputs.json`
has a `setupUi` entry for the committed Panel submodule commit and `scripts/ci/foundation-ui.sha`: the Panel repository's workflow
`setup-ui.yml` builds `foundation-setup-ui.exe` per Panel commit and foundation-ui commit and publishes it, with a build-provenance
attestation, to the release `setup-ui` (under 10 s to download and verify instead of about 3.5 min to build). The script checks the
pinned sha256 and runs `gh attestation verify --repo <repository> --signer-workflow <repository>/.github/workflows/setup-ui.yml
--source-digest <Panel commit> --source-ref refs/heads/main`; a file that fails either check fails the job. When there is no matching asset (no entry, a
submodule or foundation-ui bump that has no published build yet, a failed download), or when a manual run sets
`build_setup_ui_from_submodule`, `scripts/build-setup-ui.ps1` builds the setup UI from the committed Panel submodule instead.
To adopt a new build: run the Panel workflow (`setup-ui.yml`, optional `foundation_ui_sha`) for the new commits, then update
`setupUi` (`commit`, `foundationUi`, `asset`, `sha256`) together with the submodule bump.
`scripts/fetch-installer-inputs.ps1` downloads the Panel GUI bundle, pinned by tag and sha256 in
`scripts/installer-inputs.json` (the pinned Panel commit must equal the submodule, so a submodule bump fails the job until the
pin moves with it). ISCC writes `Sunshine-engine.exe`; the step "Assemble installer" wraps it with
`append_installer_payload.py --ui` and checks the footer hashes and that no `wv2` section is there. The step "Installer
WebView2 download and signature check" runs `--selftest-webview2 <dir>` of the finished installer: it downloads the
bootstrapper from Microsoft's link, checks that it is signed by Microsoft Corporation, deletes it, and checks that an
unsigned file (the installer itself) is refused. Nothing is installed. The decision of the worker (installed / missing and
downloaded / download fails) is `tests/unit/test_installer_webview2.cpp`, run with the native tests. Only the full installer name contains "Windows".
