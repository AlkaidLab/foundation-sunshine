# Native object capture to a dedicated MAT worker

This opt-in tool captures pre-mix object passes and hands owned PCM/XYZ/gain to
the dedicated native worker, then publishes complete IEC carrier cycles through
the private live pipe. The selected UUYC/plugin route uses
`sunshine-plugin-object-tap.dll` and `sunshine-plugin-mat-capture.exe`.
The shared object bridge and native worker have one production capture entry.

The private ABI is pinned to audioeng.dll 10.0.26100.9711, timestamp 0x0627570d,
image size 0x1ef000 and SHA256
`3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3`.
The launcher checks the explicit PID/creation time, original callback bytes,
system/native component identities and reviewed DLL hashes. This is an opt-in
development path, not a general Windows capture API.

The current bridge accepts one renderer, 48 kHz/480-frame full-span dynamic
objects and at most 20 complete objects. Static beds, fragmented objects, other
rates, missing objects and multiple active renderers fail the session. No
resampling, inferred silence, catch-up or sample-file input is provided.

Build with MSVC 2022, Windows SDK and MinHook v1.3.4 at `build/minhook-v1.3.4`,
pinned to `c3fcafdc10146beb5919319d0683e44e3c30d537`:

```powershell
./tools/sonic-mat-live/build.ps1 -Output ./build/mat-capture-new
```

The build runs own-process callback/lifecycle and shared bridge tests, plus one
generated 1300-round IPC test using an explicitly FAKE worker. It compiles the real worker/tap but
does not attach them to audiodg or load a vendor encoder in those tests.

`manage-live.ps1` defaults to read-only `Preflight`. `Start` requires the exact
engine identity, reviewed build, fresh output and an explicit `-Continuous` or
`-Seconds 1..60`. `Status` and `Stop` take that run's `capture.json`, validate
publisher/engine identity and report observed readiness or verified restoration.
Start exposes the fixed local `SunshineMatLive-active` pipe. On first use, enable
MAT in the Sunshine UI, save and apply (restart Sunshine), then run Start.
Subsequent tool starts/restarts connect without copying any
configuration. Only one capture publisher can own this endpoint at a time.
The host verifies the elevated publisher, protected deployment location and
executable content hash before exchanging capture commands.
There is no UAC, endpoint change, installed-host edit, automatic restart or retry.
Preflight/Start require only `build.json` and its three runtime files: the plugin
publisher, plugin tap and native worker. Recorded source hashes are provenance;
source trees, documentation and test executables are not runtime prerequisites.
Start deploys below Program Files and rejects untrusted owners, writable ACLs
or reparse points on the deployment path/files; it never repairs an existing ACL.
Native attachment requires the operator's current authorization; running software
tests does not authorize it.

`spatial-probe.exe` is a separate endpoint inventory and explicit live-generated
object source for a separately authorized test. It does not change the default
endpoint. Enable MAT in Sunshine as described in
[MAT-HOST](../../docs/MAT-HOST.md).

Historical native reports and abandoned paths remain on the
[archive branch](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-history-20261023).
The separate Sonic HRTF capture entry/tap and its tests were archived at
[`d006e4b`, codex/mat-sonic-archive-20261023](https://github.com/AlkaidLab/foundation-sunshine/tree/codex/mat-sonic-archive-20261023).
The current build retains the shared bridge, native session and plugin regressions.
They do not establish current game compatibility, A/V timing or HDMI/Atmos output.

The paired recorder has eight fixed encoder bindings. Distinct empty domains
can run concurrently with the selected object domain; overlapping passes or
reset/destroy during a pass on the same encoder still fail. Producers reserve bounded queue
tickets and publish a completion marker after copying; the reader never skips an
unfinished ticket. QPC ordering is checked per domain, not across independent
threads. No callback waits, allocations or silent drops are introduced.
Only one object-bearing domain is encoded. The private capture IPC is version 5;
rebuild the tap and publisher together. This does not change the MAT wire protocol.
