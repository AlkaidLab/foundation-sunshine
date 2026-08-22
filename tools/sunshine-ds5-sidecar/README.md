# Sunshine DualSense sidecar

This optional Windows helper isolates Sunshine from the third-party
HIDMaestro runtime. It owns virtual DS5 devices and exposes the versioned
`SDS5` named-pipe protocol. The helper does not contain HIDMaestro binaries.

Build against the pinned upstream v1.6.1 runtime:

```powershell
dotnet build -c Release `
  -p:HIDMaestroCorePath=C:\path\to\HIDMaestro.Core.dll
```

Read-only capability probe:

```powershell
dotnet Sunshine.Ds5Sidecar.dll --probe
```

Deterministic four-channel layout and channel-isolation check (no elevation
or virtual device required):

```powershell
dotnet Sunshine.Ds5Sidecar.dll --self-check
```

The production process must be launched elevated and placed in the Sunshine
Job Object. The pipe accepts a single elevated client of the creating user;
non-elevated callers are rejected at connect time and dropped without
ending the sidecar.
Disconnecting the owning pipe disposes every device created by
that connection. Standard `dualsense` uses UMDF2; `dualsense-composite`
enables the USB composite HID/audio profile and authored haptics PCM.
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
The composite session monitors every Windows default render and capture role.
If Windows selects a HIDMaestro-backed virtual DualSense endpoint as a default,
the helper reports the policy violation and exits; Sunshine then performs its
single recovery attach in HID-only DS5 mode. This read-only fail-closed guard
avoids undocumented audio-policy writes and never changes a user's defaults.
