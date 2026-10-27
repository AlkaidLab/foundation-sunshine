# Native object capture to a dedicated MAT worker

This opt-in tool captures pre-mix object passes and hands owned PCM/XYZ/gain to
the dedicated native worker, then publishes complete IEC carrier cycles through
the private live pipe. The selected UUYC/plugin route uses
`sunshine-plugin-object-tap.dll` and `sunshine-plugin-mat-capture.exe`.
The shared object bridge and native worker have one production capture entry.

The private ABI is pinned to audioeng.dll 10.0.26100.9711, timestamp 0x0627570d,
image size 0x1ef000 and SHA256
`3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3`; the verified
builds live only in `src/spatial_audio/mat_compat.h`.
The launcher checks the explicit PID/creation time, original callback bytes,
system/native component identities and reviewed DLL hashes. This is an opt-in
development path, not a general Windows capture API.

The current bridge accepts one renderer, 48 kHz/480-frame full-span dynamic
objects and 7.1.4 static channels, with at most 20 complete inputs in total.
Static channels keep their AudioObjectType, including LFE; no XYZ is invented.
Unsupported static types, multiple sources for one bed channel (mixing is not
implemented), fragmented objects, other rates, missing inputs and multiple active renderers fail the session. No
resampling, inferred silence, catch-up or sample-file input is provided.

A paired successful plugin Process can return buffered headphone output with no
object Submit. Such output-only drains do not create MAT object input, advance
its sequence or retire objects. Zero-output missing-object passes, partial object
loss and capture record gaps still fail; no silence or timed frame is inferred.

Build with CMake, Visual Studio C++ tools and the Windows SDK. CMake fetches
MinHook at `c3fcafdc10146beb5919319d0683e44e3c30d537`:

```powershell
./tools/sonic-mat-live/build.ps1 -Output ./build/mat-capture-new
```

The build and CI share six CTests: capture options, object bridge, paired
callbacks, dynamic-only and 12 static + 2 dynamic IPC, and private pipe lifecycle.
The IPC tests run 1300 rounds with a FAKE worker and check exact type, PCM, gain,
position and EOS. CI passes `-TestsOnly`; it never attaches to audiodg, loads a
vendor encoder, starts a signal source or needs administrator privileges.

Preflight and Start first print one `COMPONENT` line each for `audioeng.dll` and
Dolby Access (version, SHA256, `supported`/`missing`/`unsupported`). Builds not
listed in `src/spatial_audio/mat_compat.h` end with exit code 3 before any hook or
elevation; `manage-live.ps1` reports them and Status shows `Unsupported`. Dolby
Access is located through its package family, so Store version folders and other
install drives work as long as the DLL contents are verified.

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
publisher, plugin tap and native worker. The manifest contains their required
content hashes; source trees, documentation and test executables are not runtime prerequisites.
Start deploys below Program Files and rejects untrusted owners, writable ACLs
or reparse points on the deployment path/files; it never repairs an existing ACL.
Each deployed copy must match its `build.json` hash (a build file replaced after
the manifest check is removed and rejected). Start runs its elevated preflight
and passes tap/worker hashes from the verified copies and manifest only, never
from the user-writable build directory.
Native attachment requires the operator's current authorization; running software
tests does not authorize it.

`spatial-probe.exe` is a separate endpoint inventory and explicit live-generated
object source for a separately authorized test. It does not change the default
endpoint. Enable MAT in Sunshine as described in
[MAT-HOST](../../docs/MAT-HOST.md).
For an explicitly authorized generated test, `--emit --bed-7.1.4 --objects 2`
requires the full static mask before stream activation and submits a distinct
live signal to each of the 12 bed channels plus two moving objects. It does not
substitute a smaller layout or replay a file.

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
