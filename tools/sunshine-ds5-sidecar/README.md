# Sunshine virtual device host

This optional Windows helper isolates Sunshine from the third-party
HIDMaestro runtime. The shipping executable remains
`Sunshine.Ds5Sidecar.exe` for component-upgrade compatibility, while the
process now uses a `VirtualDeviceHostServer` and `DeviceRegistry` internally.
It owns virtual devices and exposes the versioned `SDS5` named-pipe protocol.
The optional component package includes patched HIDMaestro Core and its notices.

Build the compatible Foundation Core patch and the sidecar:

```powershell
scripts/build-ds5-sidecar.ps1
```

The version 1.4.1 component ZIP includes patched `HIDMaestro.Core.dll`, its
licenses, and the complete .NET runtime. The main Sunshine package contains only
the pinned ZIP download manifest. The Control Panel automatically updates older
installed components when streaming is idle, using the existing administrator
helper, verified staging and whole-component rollback. Optional USB/IP transport
installation remains part of explicit install/repair, not automatic updates.
The sidecar uses normal .NET loading and requires the v1.12.0 runtime and
device-ownership patch marker even for `--probe`. Sunshine validates the
installed component without replacing DLLs.
The upstream SDK API, assembly version and embedded resources are preserved; see
[`../hidmaestro-core-patch/README.md`](../hidmaestro-core-patch/README.md).

Read-only capability probe:

```powershell
dotnet Sunshine.Ds5Sidecar.dll --probe
```

Deterministic protocol ABI, microphone queue/runtime-contract, four-channel
layout, and channel-isolation checks (no elevation or virtual device required):

```powershell
dotnet Sunshine.Ds5Sidecar.dll --self-check
```

The production process must be launched elevated and placed in the Sunshine
Job Object. The pipe accepts a single elevated client of the creating user;
non-elevated callers are rejected at connect time and dropped without
ending the sidecar.
Input, touch, motion and battery updates submit their combined state directly
on receipt under the session state lock. They do not use a thread-pool timer:
even a 1 ms timer can run at roughly 15.6 ms intervals on Windows and reduce
continuous input to about 64 Hz. The sidecar preserves the incoming update
cadence; it does not synthesize repeated reports to inflate polling-rate tests.
The pipe reader reuses a receive buffer for headers and payloads, dispatching
complete frames as soon as they arrive. This removes separate header/body reads
and per-input payload allocations without adding a batching delay. Microphone
PCM retains its own copy because its queue outlives the receive buffer. The
device-free `--self-check` covers fragmented/packed frames, length validation,
EOF, cancellation and retained PCM ownership.
The reader uses one async state machine per pending frame. Standard axis
bindings are resolved once through the SDK at controller creation and update
the existing state dictionary in place. This avoids rebuilding the SDK's
stick/trigger lists and axis dictionary on every input update. `--self-check`
compares the cached mapping against the SDK's complete bundled profile catalog
and checks that steady-state axis updates allocate no managed objects.
The pinned .NET 10 runtime config sets
`System.Threading.ThreadPool.UnfairSemaphoreSpinLimit` to zero for this process.
Pool workers wait for a signal instead of spinning between short input bursts.
This adds no report-rate limiter or timer. Because it trades spinning for OS
wakeups, CPU and report-interval tests should be repeated when changing the
bundled runtime or enabling additional concurrent input/audio workloads.
Disconnecting the owning pipe disposes every device created by
that connection. Standard `dualsense` uses UMDF2; `dualsense-composite`
enables the USB composite HID/audio profile and authored haptics PCM.
HID-only attaches use `dualsense` with the native 273-byte descriptor and
Game Pad usage (0x05). HIDMaestro v1.12.0 no longer writes the incorrect global
GameInput mapping for JSON profiles and removes exact legacy SDK tables
(issue #1056), so the former Joystick usage override is unnecessary. Input
reports retain `alwaysArmed` for touch and motion data. Windows decides how
to expose the device; native Game Pad usage does not guarantee a WGI Gamepad.
The optional Genshin compatibility attach flag derives a third profile from
`dualsense-composite` at runtime. It preserves the Sony VID/PID, descriptors,
and four-channel layout while changing only the USB product string from
`DualSense Wireless Controller` to the launch-model `Wireless Controller`.
The runtime profile starts the USB speaker control unmuted at its declared
maximum and commits the active endpoint's 4-channel, available-speaker and
full-range masks as quadraphonic (`0x33`) through Core Audio. This matches the
three settings applied by completing Windows' speaker setup wizard as required
by Genshin.
The sidecar advertises this support through a protocol capability bit so an
older runtime cannot silently accept an ineffective setting.
The v1 wire contract reserves capability bits and message numbers for the
virtual microphone. Normal startup intentionally does not advertise them;
only the explicit development prototype can enable those capabilities.

The Phase 2 composite-profile microphone path is development-only and requires
both the exact HIDMaestro 1.12.0.0 runtime and an explicit opt-in. Its elevated
attach/PCM/flush/destroy smoke test is:

```powershell
dotnet Sunshine.Ds5Sidecar.dll --self-test microphone-prototype
```

For an end-to-end developer check, `microphone-capture` snapshots the active
capture endpoints, creates the prototype, opens only its newly added endpoint
through WASAPI, streams a 440 Hz signal for five seconds, and requires observed
host streaming, zero submit errors, captured frames, and non-zero PCM before it
passes:

```powershell
dotnet Sunshine.Ds5Sidecar.dll --self-test microphone-capture
```

This temporary path also enumerates the composite profile's HID and render
interfaces; it is not the final capture-only virtual microphone product.
The composite session monitors every Windows default render and capture role.
If Windows selects a HIDMaestro-backed virtual DualSense endpoint as a default,
the helper reports the policy violation and exits; Sunshine then performs its
single recovery attach in HID-only DS5 mode. This read-only fail-closed guard
avoids undocumented audio-policy writes and never changes a user's defaults.

The trigger effects and lightbar of an output report a game writes to the
virtual pad are read the way the hardware reads them: only when the report's
validity byte for that field is set. A game leaves the fields it is not
programming zero, and reading those zeros as an instruction would cancel an
effect the game just armed and strobe a held color. A validity byte the decoder
does not expose governs nothing, so the fields it would gate are read
unconditionally. The motor bytes are read unconditionally as well, because their
zeros can cancel a rumble that is still playing while gating them can drop a
stop that has no other way to arrive.
