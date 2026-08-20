# Sunshine DualSense sidecar

This optional Windows helper isolates Sunshine from the third-party
HIDMaestro runtime. It owns virtual DS5 devices and exposes the versioned
`SDS5` named-pipe protocol. The helper does not contain HIDMaestro binaries.

Build against the pinned upstream v1.6.2 runtime:

```powershell
dotnet build -c Release `
  -p:HIDMaestroCorePath=C:\path\to\HIDMaestro.Core.dll
```

Read-only capability probe:

```powershell
dotnet Sunshine.Ds5Sidecar.dll --probe
```

The production process must be launched elevated and placed in the Sunshine
Job Object. The pipe accepts a single elevated client of the creating user;
non-elevated callers are rejected at connect time and dropped without
ending the sidecar.
Disconnecting the owning pipe disposes every device created by
that connection. Standard `dualsense` uses UMDF2; `dualsense-composite`
enables the USB composite HID/audio profile and authored haptics PCM.
