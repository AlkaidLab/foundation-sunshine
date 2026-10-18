# Foundation HIDMaestro Core patch

Sidecar component 1.4.1 uses HIDMaestro v1.12.0 with managed-code patches for
device-cleanup ownership and USB audio pacing. The v1.12.0 API, assembly/file versions and all
official embedded resources are preserved. The complete component includes Core,
runtime, licenses and notices; the Control Panel uses verified staging and
whole-component rollback. Sidecar checks the exact runtime version and
`FoundationCorePatch=device-ownership-1` before any SDK operation, even probing.

Upstream v1.12.0 replaces the old registry-isolation patch's GameInput/OEM/slot
changes. JSON profiles receive no GameInput table. The upstream ledger captures
and restores OEM names, preserves foreign OEM/axis settings and limits slot
cleanup to identities created by virtual devices. It also removes old GameInput
tables only when their entire contents match the legacy SDK template. We no
longer disable these upstream writers or cleanup paths.

`device-ownership.patch` retains the remaining stricter cleanup rules:

- Interface keys under shared ROOT/SWD enumerators require a positive SDK
  ownership record, or an interface instance snapshot captured before parent
  removal. Both launch cleanup and full cleanup keep this evidence local to
  the sweep. A missing devnode does not establish ownership.
- HID child ownership is captured before parent removal. The later orphan
  sweep requires that snapshot or a positively owned parent; an unknown or
  missing parent alone is insufficient.

The patch applies to the exact source commit and archive digest in
`upstream.json`. `scripts/build-hidmaestro-core-patch.ps1` extracts all embedded
resources from the separately hash-verified official release and rebuilds only
C# code. No upstream build targets, WDK or native-driver rebuild are needed.
Validation compares the public API, assembly identity and every embedded
resource against the v1.12.0 release. Registry regressions run in a redirected,
disposable HKCU hive and exercise JSON mappings, exact legacy-table removal,
foreign edits, OEM claim/release, interface cleanup and HID ownership evidence.
They do not deploy drivers or remove PnP devices.

The component ZIP digest binds the whole package. Installation records the
executable and Core digests for probing and rollback. Preserve the upstream MIT
license and notices when distributing it. Previously deleted calibration/axis
values cannot be reconstructed without a backup. Upstream's legacy mapping
cleanup cannot restore a Windows mapping an older SDK already deleted.

Before dropping the remaining patch, verify that upstream cleanup also rejects
unknown-parent HID children and shared interface keys without ownership proof.
Source: https://github.com/hifihedgehog/HIDMaestro/tree/v1.12.0

`usb-audio-pacing.patch` replaces the audio pump's 200-microsecond spin tail
with a full-deadline wait on its existing high-resolution timer. New work and
shutdown still interrupt the wait; timer failures use a blocking fallback.
Stream cursors, PCM handling and uncapped HID input remain unchanged.
