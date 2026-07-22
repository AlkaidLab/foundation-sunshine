# VDD Prerequisite Closure

## Problem

Sunshine exposes several features that require ZakoVDD, but most entry points do
not distinguish between these states:

- the VDD adapter driver is installed and healthy;
- the driver is installed but unavailable or incompatible;
- a VDD monitor is currently active;
- the driver is not installed at all.

The setup wizard currently recommends VDD even when the prerequisite is absent,
the desktop VDD page falls back to editable defaults when no driver configuration
exists, and stream startup can spend time retrying a control channel that cannot
exist. Users are left with a generic stream failure and no recovery path outside
the installer.

## Goals

1. Keep VDD as the recommended setup-wizard choice.
2. Detect the prerequisite before saving a guided setup or changing display
   topology.
3. Provide an install/repair entry point in the desktop GUI using the driver
   payload already shipped with Sunshine.
4. Verify the driver after installation and return the user to the interrupted
   operation.
5. Give standalone browser users an accurate status and a concrete host-side
   instruction.
6. Avoid silent driver installation, automatic reboot, or repair during an
   active stream.

## Non-goals

- A general-purpose Core driver-management task framework.
- Downloading a driver at runtime.
- Making the active VDD monitor synonymous with driver installation.
- Blocking advanced users from saving a configuration they intend to complete
  later.

## Status contract

The Core and desktop GUI expose the same compact status vocabulary. Some facts
may be unavailable to the Core's fast probe and remain empty.

```text
state:
  ready | degraded | not_installed | unhealthy | reboot_required |
  payload_missing | unsupported | unknown

facts:
  installed
  running
  control_available
  installed_version
  bundled_version
  version_match
  monitor_active
  problem_code
  status_text
```

`monitor_active` is independent of `installed` and `running`. UI actions are
derived from `state`; the backend does not need a second recommended-action
enum.

## Architecture

### Desktop management path

The Tauri control panel owns installation and repair. It reuses the existing
`bat_runner::run_elevated()` implementation and the bundled scripts:

- `install-vdd.bat --probe-only` for detailed, non-mutating status;
- `install-vdd.bat` for idempotent install/repair to the bundled compatible
  version;
- `uninstall-vdd.bat` for removal.

The Web UI embedded in the desktop shell receives a `window.vddDriver` bridge,
matching the existing virtual-mouse and ViGEm bridges. No privileged Core HTTP
action endpoint is added.

### Core runtime path

Core performs a fast, non-mutating native probe before VDD preparation. The
probe checks the adapter/control transport without creating a monitor. Core also
exposes `GET /api/vdd/status` so a standalone browser can explain the problem.

If a launch requires VDD and the driver is missing or unhealthy, startup fails
before topology is saved or modified. The launch response distinguishes the
stable `VDD_DRIVER_NOT_INSTALLED` and `VDD_DRIVER_UNAVAILABLE` errors and directs
the user to the desktop VDD page.

### UX policy

- Setup wizard: hard prerequisite gate. VDD remains selected; the primary action
  becomes **Install and continue** or **Repair and continue**.
- Advanced configuration: inline warning and recovery entry point, but saving is
  allowed for staged/remote configuration.
- Runtime: hard gate with an actionable error.
- Desktop VDD settings: unavailable states replace the editor with a status and
  repair panel; successful verification restores the editor.
- Tray create action: an unavailable prerequisite opens VDD settings instead of
  becoming a disabled dead end.
- Tray keep-enabled/headless-create actions and the remote VDD shortcut follow
  the same prerequisite policy. Disabling an already configured option remains
  available.

## Delivery

### Change 1: desktop closure

- Add `get_vdd_status`, `install_vdd_driver`, and a script-backed
  `uninstall_vdd_driver` Tauri command.
- Replace the legacy uninstall path that targets `ROOT\\iddsampledriver`.
- Add the VDD bridge to the injected Web UI API.
- Gate the desktop VDD editor on verified status.
- Refuse installation while a stream is active and warn before disrupting an
  active VDD monitor.

### Change 2: Core fast failure

- Add a fast VDD prerequisite probe and authenticated `GET /api/vdd/status`.
- Make VDD preparation return a typed result and propagate monitor-creation
  failure.
- Skip legacy-pipe retries when the adapter is absent.
- Return stable VDD launch errors and actionable hints.

### Change 3: Web entry points

- Add a small VDD-status composable that prefers `window.vddDriver` and falls
  back to `/api/vdd/status`.
- Gate the setup wizard and resume after successful verification.
- Add non-blocking warnings to output and direct-capture configuration.
- Add focused unit tests and localized user-facing strings.

All three changes ship in the same release.

## Implemented entry-point closure

Every path that can create or depend on a VDD now converges on the same status
contract:

- setup wizard: install/recheck gate, then resumes the current wizard step;
- standalone Web configuration: non-blocking status and host-side guidance;
- desktop VDD editor: unavailable states replace the editor with install/repair;
- desktop stream configuration: status, install/repair, and recheck actions;
- desktop tray create/keep/headless actions: preflight and route to VDD settings;
- Core tray HTTP actions: reject missing prerequisites before changing config;
- native tray and remote VDD shortcut: explain the prerequisite and open or
  notify the desktop management path;
- stream startup and the lowest-level monitor creation primitive: fail fast
  before topology changes or legacy-pipe retries.

The obsolete standalone `renderer/vdd` editor was removed so the desktop app
has one authoritative VDD management surface.

## Verification

The implementation is verified at five layers:

1. The installed-layout `install-vdd.bat --probe-only` smoke test validates
   payload discovery and the machine-readable status contract without changing
   the installed driver.
2. The dedicated `vdd_prerequisite_unit_tests` CTest covers every Core state and
   the usable-driver invariant without depending on the unrelated full test
   executable.
3. Web tests cover browser fallback, desktop bridge preference, UAC
   cancellation/retry, and the not-installed-to-ready transition; Tauri tests
   cover detailed status classification and tray behavior.
4. Production Web and desktop renderer builds, Tauri tests, and the
   complete Core `sunshine.exe` link validate all integration boundaries.
5. A source audit verifies every `CREATEMONITOR` call converges on the guarded
   `create_vdd_monitor()` primitive; the old independent VDD renderer is absent
   from the production output.

Real driver install/uninstall is intentionally not run as part of ordinary
verification because it changes the host display driver and topology. Those
release-candidate checks belong on a disposable clean Windows host; cancellation
and failure remain visible because the elevated runner propagates the real exit
code and the UI retains the interrupted selection. The RC verifier's
non-destructive validation mode and destructive opt-in guard are tested on the
development host. An explicitly approved local install/repair check is recorded
below; the clean-host install-from-absent and uninstall lifecycle remain RC work.

### Local installation validation (2026-07-22)

The development host was also used for an explicitly approved installation
check. Before any driver change, `scripts/vdd_rc_smoke.ps1 -ValidateOnly`
recorded `build/vdd_local_install/vdd-before.json` with these facts:

- installed version `100.0.16.6`;
- bundled version `100.0.16.4`;
- PnP status `OK`, problem code `0`;
- the desktop stream page classified the usable version mismatch as
  `degraded` and exposed **Install / Repair Driver**.
- the primary desktop VDD settings page kept its editor available in that
  usable state and exposed the same **Install / Repair Driver** action beside
  the version warning; a production renderer build and a real GUI pass verified
  the layout.

The check found and fixed a Windows-only probe defect before installation. Rust
was passing an embedded quoted batch path through `Command::arg`, which escaped
the quotes to literal `\"` characters for `cmd.exe`; every healthy installation
therefore appeared as `unknown` in the GUI. The probe now uses `raw_arg` with a
`call "..." --probe-only` command line. A Windows execution-level unit test runs
a real batch file from a path containing spaces, so a visually plausible but
non-executable quoting change cannot regress this path again.

The first real repair attempt was refused before UAC because a Moonlight stream
was active. This confirms the active-stream safety gate against the installed
Core. No driver files or display topology were changed by that attempt.

After the stream ended, the explicitly approved elevated repair completed
without rebooting Windows. The install log records removal of the old device,
publication as `oem4.inf`, recreation of the adapter, and the final `VDD adapter
is ready` check. Post-install evidence is stored in
`build/vdd_local_install/vdd-after-durable.json`:

- installed and bundled versions are both `100.0.16.4`;
- `ROOT\DISPLAY\0001` is present with PnP status `OK` and problem code `0`;
- the GUI returned to the editable VDD settings state;
- the persistent `VDDPATH` points to
  `C:\Program Files\Sunshine\config`, not the isolated test layout;
- the active-session count remained zero after repair.

The repair also exposed a display-topology edge case: Windows can reset the
desktop while the elevated script is running, causing the original Tauri IPC
response to be lost even though the driver is already healthy. The install UI
now runs an independent status-verification loop and completes as soon as it
observes `ready`, `running`, and an exact bundled-version match. Five focused
tests cover the verified invariant, normal completion, lost-IPC recovery,
immediate refusal, and timeout cleanup; the production renderer build passes.

## Acceptance criteria

- A fresh host can install VDD from the setup wizard inside the desktop GUI and
  continue without restarting the wizard.
- Cancelling UAC keeps the user's selection and reports cancellation/failure.
- Standalone Web UI shows host-side installation guidance rather than a dead
  control.
- The VDD settings editor never presents fake editable defaults for a missing
  prerequisite.
- Starting a VDD-dependent stream with no driver does not modify topology or
  wait through legacy-pipe retries.
- Healthy existing installations see no additional blocking UI.
- Install/repair is refused while streaming and never reboots Windows
  automatically.
- The script-backed uninstall path targets the current `Root\\ZakoVDD` package
  and reports its real result.

## Release-candidate clean-host checklist

Run these destructive checks only on a disposable Windows host:

From a repository checkout, start an elevated PowerShell session and run the
lifecycle verifier against the installed `scripts` directory. It refuses a
non-clean host and writes a JSON evidence record containing PnP and DriverStore
snapshots:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\vdd_rc_smoke.ps1 `
  -ScriptsDirectory "C:\Program Files\Sunshine\scripts" `
  -EvidencePath .\vdd-rc-evidence.json `
  -AllowDestructive
```

- [ ] With no ZakoVDD installed, select VDD in the desktop setup wizard, accept
  UAC, and verify the wizard resumes with the original selection.
- [ ] Repeat from a clean state, cancel UAC, and verify the selection remains
  while the cancellation/failure is shown.
- [ ] Disable or otherwise make the adapter unhealthy and verify setup, tray,
  shortcut, and stream entry points route to repair instead of retrying VDD.
- [ ] While a stream is active, verify install/repair and uninstall are refused.
- [ ] Uninstall from VDD settings and verify `Root\\ZakoVDD` is removed and the
  UI returns to `not_installed`; verify Windows is never rebooted automatically.
