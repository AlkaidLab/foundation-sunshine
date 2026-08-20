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
The composite session monitors the three Windows default render roles. If
Windows selects the HIDMaestro-backed virtual DualSense speaker as a default,
the helper reports the policy violation and exits; Sunshine then performs its
single recovery attach in HID-only DS5 mode. This read-only fail-closed guard
avoids undocumented audio-policy writes and never changes a user's defaults.
