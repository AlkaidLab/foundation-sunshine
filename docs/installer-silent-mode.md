# Unattended installer dialog policy

The Inno engine and its installed uninstaller give every script-owned dialog an
explicit default in `/SILENT` and `/VERYSILENT` mode. Neither switch needs
`/SUPPRESSMSGBOXES` to handle these dialogs. Interactive calls retain their
existing text, buttons and default button. Headless callers must request silent
mode; absence of a desktop is not interpreted as permission to change data.

## Dialog inventory

Audit scope: `cmake/packaging/`, `tools/installer*`, all tracked `.iss`, `.iss.in`
and `.pas` files. The only Inno/Pascal script is
`cmake/packaging/sunshine.iss.in`. Its ten original message-box call sites are:

| Event / message key | Call sites | Silent answer | Result / reason |
| --- | ---: | --- | --- |
| VDD install failure / `MsgVDDInstallFailed` | 1 | OK; already skipped by `WizardSilent` | Preserve the existing log and failure result. |
| Service verification failure / `MsgServiceInstallFailed` | 1 | OK; already skipped by `WizardSilent` | Preserve files, log and exit code 100. |
| Stale legacy uninstall registration / `MsgOldNSISStaleRegistry` | 2 | No | Keep metadata; decline migration without consent. |
| Legacy registry cleanup failed / `MsgOldNSISCleanupFailed` | 2 | OK | Keep the existing installation failure result. |
| Run legacy uninstaller / `MsgOldNSISDetected` | 1 | No | Abort the new install rather than run an older remover that could delete data. |
| Legacy uninstall failed / `MsgOldNSISFailed` | 1 | OK | Keep the existing installation failure result. |
| Remove gamepad / `MsgUninstallGamepad` | 1 | No | Retain a driver that may serve other applications. |
| Delete application data / `MsgUninstallData` | 1 | No | Retain configuration, paired-client state and covers. Only Yes deletes them. |

`InstallerMessageBox` selects `UninstallSilent` only in the uninstaller, and
`WizardSilent` in Setup. Silent calls return the explicit default directly;
interactive calls use `SuppressibleMsgBox`, which the engine documents as
equivalent to `MsgBox` when suppression is inactive. No `TaskDialogMsgBox`,
`SuppressibleTaskDialogMsgBox`, `ShowModal` or custom-page creation call was found.
`InitializeWizard` styles the standard wizard controls.

Two other existing dialog mechanisms need no change:

- `tools/installer_shell.cpp`: the unsupported-OS `MessageBoxW` is already
  bypassed in silent and worker modes (exit 1150). The native interactive shell
  remains interactive by design.
- `cmake/packaging/windows_nsis.cmake`: both legacy `MessageBox` statements
  already specify `/SD IDNO`, preserving the gamepad and user data.

This policy covers application-script prompts. The Inno engine documents that
critical errors, pre-command-line failures and Windows elevation can still
display UI. Always use `/NORESTART` in automation. Inno's separate
`/SUPPRESSMSGBOXES` option remains available for engine-owned dialogs.

## Windows CI acceptance

`.github/actions/test-installer-silent/action.yml` runs
`scripts/ci/test-installer-silent.ps1` after the production package is assembled.
Both silent modes install the production `Sunshine-engine.exe` with only the
required application/assets/GUI components, then run the real `unins000.exe`
from that installation. Optional hardware drivers are deliberately excluded.
The engine test isolates the Inno code under test with an explicit destination;
the outer download shell does not forward that destination/component selection.
This check does not claim to exercise the outer shell or hardware drivers.

Each child must finish within 120 seconds and return zero. The check requires
the application, service wrapper, GUI and uninstaller to exist after install
and disappear after uninstall, with a further bounded 15-second self-deletion
wait. It verifies both installation registrations are removed and SHA-256
hashes of `config/sunshine.conf`, `config/apps.json`,
`config/sunshine_state.json` and a cover fixture stay unchanged. The service
created by the test is stopped before seeding fixtures to avoid first-run writes.
The outer workflow step has a 12-minute timeout. Logs and a JSON result summary
are uploaded even on failure. No process is terminated by the smoke script.

Run on an elevated, disposable Windows runner with no existing installation,
related service or running application:

```powershell
./scripts/ci/test-installer-silent.ps1 -Installer build/cpack_artifacts/Sunshine-engine.exe
```

Existing installation metadata, services or related processes cause refusal,
before invoking an installer. The existing uninstaller performs system-wide
cleanup; this smoke check must not be run on a shared workstation. The evidence
directory contains the launched PIDs and bounded process results; timeout
failure leaves teardown to the disposable runner.

## Design and review boundaries

Primary sources were read on 2026-11-18 (local date); record keys and exact
retrieval timestamps, content hashes and sizes are in
`docs/authority/source-index.tsv`:

- `installer-inno-suppressible-msgbox-20261118`,
  `installer-inno-silent-state-20261118`,
  `installer-inno-uninstall-state-20261118`,
  `installer-inno-is-uninstaller-20261118`: Description/prototypes justify the
  mode-aware wrapper and interactive behavior.
- `installer-inno-setup-cli-20261118` and
  `installer-inno-uninstall-cli-20261118`: silent switches, suppression limits,
  component selection, logs and restart control.
- `installer-inno-six-releases-20261118`: 6.7.4 (2026-10-28), including backports
  from the next major. This fix retains the existing CI compiler pin 6.7.3; no
  newer compiler feature is needed for these documented functions. Compiler
  migration and new major adoption are outside this repair.
- `installer-ms-start-process-20261118` (ArgumentList, PassThru),
  `installer-ms-wait-for-exit-20261118` (Int32 overload),
  `installer-ms-stop-service-20261118` (Description, NoWait): Windows-native
  process control and graceful service stop for bounded CI verification.
- `installer-github-composite-20261118` (runs.steps) and
  `installer-github-timeout-20261118` (jobs.job_id.steps.timeout-minutes):
  action composition, failure evidence and the outer step deadline.
- `installer-powershell-release-20261118`: current stable release metadata
  reports 7.7.0 (2026-11-10). The script uses facilities available since 7.0
  and the hosted runner's existing runtime; it does not install a toolchain.
- `installer-ms-parser-20261118` (Definition, Parameters and Returns): the
  queued Windows syntax check uses `Parser.ParseFile` without executing the
  installer smoke script on a shared machine. This is not runtime acceptance.

The approach uses the reference installer engine's documented suppression
primitive plus explicit mode detection, because that primitive alone still
needs `/SUPPRESSMSGBOXES`. It avoids replacing the installer UI or introducing
a separate automation protocol. No other implementation's source was used.
No superseded API is introduced; comparative implementation performance is
not verified. No network/media standards or emerging IETF work apply here.

Pascal Script is the engine's native extension language; PowerShell 7 and .NET
provide the Windows runner's process/service facilities. The change adds a
branch per rare installer dialog, no streaming hot path or public ABI. Copies,
allocations, syscalls, batching/offload, latency distributions, resource usage
and strongest-opponent ratios have not been measured; there is no performance
acceptance claim. The measurable CI liveness budget is a timeout, not a
performance result.

Implemented: ten explicit defaults, data-safe deletion consent, preserved
interactive call parameters, both-mode CI round trips and failure artifacts.
Deliberately excluded: compiler upgrades, driver acceptance, protocol/Adapter
changes, public interfaces and general uninstaller lifecycle redesign.
Windows source is changed; Windows build, install/uninstall execution,
interactive click-through and real-device behavior require Windows evidence.
Resource authorization and cross-tenant isolation are not verified. There is
no Linux/macOS/Android/iOS/HarmonyOS/Web Backend change; builds/runs on those
systems provide no evidence for the Windows behavior. This installer policy
does not introduce external protocol compatibility or an Adapter.
