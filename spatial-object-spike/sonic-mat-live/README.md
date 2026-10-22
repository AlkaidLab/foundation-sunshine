# Sonic objects to native MAT

The user selected UUYC / Realtek spatial object input followed by an independent
MAT encoder. Host HDMI output is **not required for this route**. Sonic supplies
pre-HRTF object PCM and positions; it does not produce MAT bitstream. The separate
guarded Dolby encoder worker produces actual MAT. Already-mixed stereo PCM cannot
recover its original objects/positions through this adapter.

This opt-in path is separate from the passive `mat-live/capture.cpp` wrapper tap.
That tap observes an already running MAT encoder; its previous zero-cycle probe
cannot reject Sonic object capture. The earlier HDMI source prerequisite applied
to that passive experiment, not to the requested Sonic/re-encoding route.

## Current implementation

* `tap.cpp` observes the existing Sonic HRTF renderer Process, WriteMono (before
  gain/HRTF mixing) and successful SetEndOfStream. The actual device pass frame
  count, renderer's sample rate, object offset/count, cached XYZ/volume, call gain,
  original return status and EOS are copied to a fixed shared ring. No callback
  waits, logs, allocates, invokes the vendor encoder or performs pipe/network I/O.
* `bridge.h` consumes owned passes on the publisher thread. It applies the cached
  volume times call gain once, assigns new output IDs, and sends explicit create,
  update/end and final capture-close controls through the existing bounded IPC.
* `capture.cpp` starts the dedicated `StartMatStreamReceiver` worker inside the
  explicitly selected audiodg lifetime. Its native guard, exact component hashes,
  mapper/encoder ABI checks, strict S_OK and 30,720-byte Process contract remain.
  The output source joins two native blocks into complete 61,440-byte MAT cycles,
  including native bootstrap/drain output, with the first block's completion QPC.
* The existing private pipe / PLAY cutoff / four-cycle queue and mat-live-v1
  streaming apply. Private source kind 2 identifies Sonic re-encoding; its domain
  is the selected Sonic renderer. This does not alter the network profile, Android
  capability checks or the unsupported-device Opus fallback.

The first contract supports one observed Sonic renderer, 48 kHz/480-frame device
passes, up to 20 full-span dynamic objects. Static beds, other rates, nonzero
offsets, short/fragmented objects and multiple active object renderers stop with
diagnostics. Missing still-live objects are not guessed as silence or ended.
An empty pass advances only with the explicitly observed 480-frame device clock;
there is no QPC-derived sample clock. Selection of the first object-bearing
renderer is logged. This is not general per-application/endpoint isolation;
controlled native verification must rule out another object-bearing renderer.

## Guarded native boundary

The private ABI is pinned to the same audioeng SHA as the passive experiment:
3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3.
Offline Microsoft symbols and disassembly on this machine identify:

| Method | RVA | Information used |
| --- | --- | --- |
| MultiSourceHrtfRenderer::Process | 0xf7880 | Real pass, connection frame count |
| DynamicMultiSourceHrtfHost::WriteMono | 0x433f0 | Offset, PCM, frames, gain |
| DynamicMultiSourceHrtfHost::SetEndOfStream | 0x79e20 | Successful explicit EOS |

The renderer WAVEFORMATEX is copied at +0x44 (rate +0x48); its requested source
count is +0x22c. Host cached volume/id/type/XYZ remain +0x64/+0x68/+0x6c/+0x98.
WriteMonoInternal's internally generated prefix padding is not misidentified as
an application object write. The new outer WriteMono hook retains explicit offsets
and rejects unsupported spans. These facts still need native callback validation
on UUYC; offline analysis and generated own-process tests are not that validation.

The launcher pins PID + creation time and verifies system paths/file SHA and all
three original prologues. It never elevates itself, enables debug privilege,
restarts audiodg or changes endpoints/drivers/services. The tap is pinned, stops
on owner loss/stale heartbeat, drains admitted passes, disables hooks and checks
all saved bytes. The publisher independently rereads the three original entries.
Native codec and source timeout/failure do not produce a clean EOS or replay.
Ordinary attach rejects a loaded module. Explicit `--reattach`/`-Reattach` checks
the exact loaded path/hash and original bytes before using the in-DLL running
guards; it never retries automatically. DLL deployment names contain their hash.

## Build and test

```powershell
./spatial-object-spike/sonic-mat-live/build.ps1 -Output ./build/sonic-native-new
```

Own-process tests exercise the real hook callbacks against controlled stubs,
copy ownership, clock/span/gain/XYZ/EOS, and ten rejecting contracts. A second
test feeds 1,300 generated Sonic passes through the real IPC receiver's explicitly
FAKE codec worker, verifies 1,350 output blocks and PCM/XYZ/gain, and asserts no
vendor loads/host guard success. These generated diagnostics never enter the
native-only pipe/checker and are not evidence of native MAT encoding.

`run-once.ps1` is a prepared explicit native probe, not an instruction to run it
without the operator's current authorization. It checks the reviewed build and
engine identity, creates immutable ProgramData copies, starts an 8-second capture,
and submits a separate 10-second **live-generated** two-object ISpatialAudioClient
stream on an exact active endpoint ID. It does not read any recorded input or
change the default endpoint. On actual `NATIVE_MAT_READY` it starts the native-only
loopback checker with controlled ciphertext packet loss. Capture shutdown closes
the real object input and drains the native worker; all three processes and the
original engine lifetime are checked. A timeout or format mismatch fails the probe.

This probe's successful loopback would establish the actual object -> native
encoder -> client transport path. It would not establish real game capture,
full Sunshine RTSP/video streaming, A/V alignment, HDMI hardware playback or
Atmos recognition. The normal application source has no duration/count limit;
the optional 1..60-second bound exists for an explicitly initiated probe only.
