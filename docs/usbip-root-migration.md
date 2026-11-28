# Root-device migration boundaries

The root helper registers or reuses the controller matched by the original
signed driver. It does not install a modified INF or driver. Legacy cleanup
applies only to a separate `ROOT\USB\0000` node with the exact expected hardware
ID. With no such node, cleanup succeeds without a state change. The selected
controller must remain enabled, including when its instance ID is the old one.

`Ensure-RootDevice` returns exactly one structured result. Progress belongs on
the verbose stream: ordinary output before `return` also becomes part of the
function result (`ms-powershell-return-20261118`, Long description;
`ms-powershell-streams-20261118`, Success and Verbose streams). An array
containing progress text can cause the strict-mode caller to fail when reading
`RootPresent`. A warning from optional legacy cleanup is a separate condition
and does not by itself establish why installation returned a failure.

State changes retain the documented SetupAPI sequence
(`ms-setupapi-propchange-20261118`, StateChange and Scope). Parameter setup and
class installation now have separate failure diagnostics
(`ms-setupapi-classparams-20261118` and `ms-setupapi-classinstaller-20261118`,
Return value). Last-error values are meaningful only for the failed operation
(`ms-win32-getlasterror-20261118`, Return value and Remarks). An older combined
diagnostic could not distinguish the two calls. Required registration failures
and duplicate cleanup failures still propagate.

A clean Windows 11 run measured the two defects behind the observed error 203
(creation path, no controller present):

- `DiInstallDevice` was declared with five parameters. Its documented
  signature has six, with `DriverInfoData` between the device data and the
  flags (`ms-newdev-diinstalldevice-20261107`, Syntax); the call ended in an
  access violation.
- `SetupDiSetDeviceRegistryProperty` and `SetupDiGetDeviceRegistryProperty`
  were bound to the ANSI entry point, while the buffers are UTF-16
  (`ms-setupapi-setdeviceregistrypropertyw-20261107`). The hardware ID
  `ROOT\USBIP_WIN2\UDE` was stored as one-character IDs, no driver matched
  (`There are no compatible drivers for this device`), and `DiInstallDevice`
  failed with Win32 error 203.

Both declarations are corrected; the creation path then registers the
controller, loads the VHCI driver and is idempotent on a second run. The
regression test reads the production declarations, because the substituted
SetupAPI cannot see them.

The installer retains Windows PowerShell 5.1 and its existing C# interop. The
current documented PnPUtil device commands provide device state and package
management, but do not replace this helper's root-device registration
(`ms-pnputil-syntax-20261118`, /disable-device and /add-driver). The upstream
release metadata checked on 2026-11-18 still identifies v.0.9.8.1 as latest,
matching the pinned package (`usbip-win2-release-current-20261118`, tag_name and
published_at). No external implementation code was used for this change.
This installer correction does not change a wire protocol or a data hot path;
no throughput, latency, competing-implementation or hardware performance claim
is made. No new Backend or Adapter compatibility is introduced.

Run `scripts/packaging/test-usbip-root.ps1` with Windows PowerShell 5.1 or
PowerShell 7 for isolated regression coverage. The test loads production
functions without their command entry point and substitutes device inventory
and SetupAPI calls. It neither installs drivers nor changes device state.
Passing these tests establishes the control flow, not native SetupAPI or
physical-device behavior.
