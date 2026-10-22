audiodg resident object collector / local spike, 2026-10-17
=========================================================

DEFAULT STATEFUL COLLECTOR (2026-10-18)
-------------------------------------------------------------
Actual results and limits: STATEFUL-RESULT.txt. User ended the soak early after
305.657 seconds. A subsequent requested 20.125-second actual-game rotation test
passed 35 file switches; see ROTATION-RESULT.txt. No second long soak was run.
collector.exe, start-resident.ps1 and run-test.ps1 now default to --backend dolby:
stateful_tap.cpp -> engine_tap_stateful_v1.dll (ProgramData deployment).
This compact route captures raw update flags plus a loss epoch, NOT the diagnostic
640-byte trace and NOT the sanitizer hook. The audio callback still only performs
bounded copies and atomics; all state reconstruction and disk I/O run externally.
position_cache.h applies position ONLY for flag 0x4, volume ONLY for 0x2; creation
resets state, end removes it, and a missed block/unsupported reader invalidates
previous knowledge. An observed creation without position uses the API default
origin; late attachment without creation/position is UNKNOWN, represented by NaN.
Static channels retain their type and have implicit positions, not fabricated XYZ.
Cache is bounded to 4096 stream/object/type entries. Generation separates reuse.

Disk format v2 is INCOMPATIBLE with legacy analyze.py/audio_inventory.py:
  112-byte StateRecord header, then frames * 4 mono float32 samples (48 kHz here).
  First 64 bytes: Record, magic=0x324f424a; XYZ/volume are reconstructed state.
  +64 version u32, +68 propertyFlags u32, +72 positionState u32,
  +76 volumeKnown u32, +80 lossEpoch u64, +88 generation u64,
  +96 rawX f32, +100 rawY f32, +104 rawZ f32, +108 reserved u32.
  States: 0 unknown, 1 updated, 2 cached, 3 creation-default-origin,
          4 static-channel-implicit. Unknown XYZ/volume are NaNs, NOT zeroes.
  Identity includes the capture session, stream, object ID, type and generation.
  A stream pointer is NOT a game entity ID. XYZ are listener-relative, not world.

Files rotate at --chunk-mb (default 128), without unhooking or resetting the cache.
Read global-objects.bin, then global-objects-0001.bin etc in order. Sequence and
generation continue across files; each header already includes reconstructed state.
start-resident.ps1 uses one collector per engine lifetime up to the total quota;
ChunkMb controls files, NOT repeated attach/detach. Its legacy MaxMbPerSession
parameter is now an alias for ChunkMb. --max-mb bounds total capture storage.
No silent deletion/overwrite of captures and no automatic boot service are added.

Build (PowerShell, no collector currently running):
  .\build.ps1 -StatefulOnly
Run ELEVATED, on the tested Dolby endpoint:
  .\start-resident.ps1 -OutputRoot C:\captures\NEW-RUN -ChunkMb 128 -MaxTotalMb 2048
Stop by creating NEW-RUN\STOP or collector.exe --stop COLLECTOR_PID.
For a bounded test:
  .\run-test.ps1 -RunName NEW-RUN-NAME -NoSource -Seconds 600 -MaxMb 8192
Verify:
  python verify_stateful.py NEW-RUN-NAME --proof
  .\position_cache_test.exe
  .\cache_fixture_test.exe metadata-lifecycle01\property-trace.bin metadata-lifecycle01\position-state.csv
verify_stateful.py independently replays flags, validates every PCM sample and
sequence, checks cross-file cache continuity, and exports one audible moving object.
It requires successful cleanup, no queue loss, and zero unsupported metadata calls.
Proof WAV is a PCM16 preview of captured pre-volume/pre-call-gain mono samples;
the matching CSV retains both gains and coordinate state. It is NOT a final mix.
The 2048 MB default storage quota stops capture safely when reached; it does not
delete old data. At this game's full object rate, 10 minutes needs about 4.5 GB.
Increase MaxTotalMb deliberately for longer captures. A 10-minute soak is not an
hours/days endurance, all-games, endpoint-switching or end-to-end glitch guarantee.

Explicit legacy choices remain --backend sonic, dolby-raw, metadata. These keep
their original 64-byte disk records; metadata adds the paired diagnostic sidecar.
Versioned DLLs are never overwritten while pinned; a rebuilt/changed DLL with a
different hash requires a new deployment filename in the loader/scripts, or a
normal audio-engine restart followed by a deliberately versioned deployment.
This is still a version-locked local spike, not an all-game production guarantee.

IMPORTANT CORRECTION FROM PROPERTY-CHAIN TRACE
--------------------------------------------
Positive result: POSITION-CHAIN-RESULT.txt and metadata-lifecycle01/proof-summary.json
now verify actual Nioh 3 dynamic audio WITH nonzero, changing coordinates.
Earlier Dolby reports recorded raw per-packet XYZ, without the update flags. These
are NOT sufficient to infer current object positions: flag 0x4 gates position
updates, 0x2 gates volume updates. A late attachment can receive only flag 0x2,
with zero-filled XYZ while the renderer retains an earlier position. Earlier
"XYZ zero" conclusions must be read as RAW FIELD observations, not proof that
the game supplied zero current positions or lacks spatial-audio support.
metadata_tap.cpp captures paired ingress/sanitized/submitted attributes and flags;
verify_metadata.py reconstructs known cached positions and explicitly marks unknown
initial positions. Stream creation must be captured to avoid that blind spot.

What this is
------------
An explicitly started, elevated collector plus an audiodg-only capture DLL.
No virtual device, no driver, no game injection, no live debugger or breakpoints.
MinHook 1.3.4 is vendored from https://github.com/TsudaKageyu/minhook (BSD license
in vendor/minhook-1.3.4/LICENSE.txt). Enable/disable briefly synchronizes threads;
the capture hot path never suspends a thread or waits for the consumer.

The hot path uses a preallocated 4096-slot (~33.8 MB) shared queue and bounded
copies. A single producer CAS attempt drops rather than waiting on contention.
External collector polls every 5 ms and performs all disk writes. Counters expose
full/busy drops, invalid reads, queue high water, and QPC copy-section timing.
These timings EXCLUDE hook entry/exit, original audio processing and all disk I/O;
they are not a hard realtime or whole-system incremental CPU guarantee.

Stop and failure behavior
-------------------------
--seconds, Ctrl+C, --stop COLLECTOR_PID, and disk quota request clean shutdown.
Management thread watches a duplicated SYNCHRONIZE process handle and heartbeat;
collector death/stall >5 seconds requests unhook independently of audio callbacks.
Stop clears the capture pointer, disables hooks, waits for active callbacks, then
unmaps the ring. Code/trampolines are intentionally PINNED until audiodg exits.
An inactive DLL still listed in audiodg does NOT mean recording remains active.
Do not force-restart audiodg while a game is running merely to unload these DLLs.

Version lock
------------
audioeng.dll 10.0.26100.9711, image 0x1ef000, timestamp 0x0627570d.
Microsoft PDB identity and prior full image SHA256 are documented in ../global/.
Both DLLs reject the wrong process/version/prologue. This is an undocumented
version-specific spike, not a supported Windows capture API or production release.

Sonic path
----------
DynamicMultiSourceHrtfHost::WriteMonoInternal RVA 0x2fdc8. Audio + host-cached
XYZ/volume before HRTF mixing. controlled04/analysis.json independently verifies
four 5-second objects from two ordinary native source processes, all 240000 source
frames each, waveform residual <4e-7 and XYZ error <3e-8, no drops/invalid reads.
Two extra 64-frame engine silence blocks were retained, not counted as source data.
An explicitly marked warm-up source was also captured separately.

Dolby path
----------
dolby_tap.cpp bootstraps at MainPluginRenderer::Process RVA 0xfa3b0, obtains the
encoder interface, then verifies the Microsoft wrapper and its prologues:
  AsarEncoderWrapper<IAsarEncoder2>::SetAudioObject RVA 0x2fd90
  SetAudioObjectWithMetadataReader RVA 0x45be0
Only the basic-object ABI is decoded: stream identity, engine object ID, type,
XYZ, volume, mono float audio, frame count, global gain. Extended metadata calls
are COUNTED, not decoded. Renderer/encoder code is pinned to keep hook returns safe.
Offline disassembly evidence: dolby-offline-paths.txt, dolby-renderer-call.txt,
dolby-encoder-proxy.txt. These used CDB on an on-disk PE, NOT a live process.

Actual Nioh 3 / Dolby Atmos for Home Theater results so far
--------------------------------------------------------
Default endpoint F32D80U, Nioh3 PID 41424, audiodg PID 39292. Both endpoint format
and spatial setting visibly Dolby Atmos for Home Theater; capability maxDynamic=20.
Loaded DolbyMatEnc.dll 1.0.2403.20001 and DolbyMatEngine.dll 3.1.8.rel were recorded.
Game module audit: CaptureDllLoaded=False. No controlled source in game-only runs.

dolby-game01: 45 seconds with Sonic-specific hook, 0 calls. This does NOT prove
the absence of game objects; that hook is not on the Dolby processing path.
dolby-controlled01: native source activation failed SPTLAUDCLNT_E_NO_MORE_OBJECTS
(0x88890103). This is NOT a valid successful positive control. It does not identify
who reserved the object pool. No resource was forcibly reclaimed from the game.
dolby-game02/03: unknown encoder module was rejected, original bytes restored.
Diagnostics identified the additional Microsoft wrapper; PDB/disassembly verified
the ABI before it was allowed. These failed initialization runs are preserved.

dolby-game04: audio SETTINGS MENU observation, NOT a gameplay conclusion.
20.047 seconds, 73556 chunks, 35306880 object-frames, 145935104 bytes; 0 full/busy
drops and 0 invalid reads, all 37 slots have 1988 chunks of 480 frames at 48 kHz.
20 dynamic slots: all samples EXACTLY ZERO, XYZ always (0,0,0).
17 static slots: first 12 contain signal (7.1.4 labels), remaining 5 are silent.
Static XYZ fields zero are NOT moving source coordinates: positions are implicit
in AudioObjectType. No coordinates should be inferred from these waveform channels.
Copy timing total 17307.9 us, max 10.6 us, mean ~0.235 us/chunk; collector CPU
0.015625 s over 20.047 s. Whole audiodg CPU 0.890625 s is NOT incremental tap cost.
Both capture callbacks and renderer bootstrap were removed successfully. Original
wrapper bytes were compared inside the DLL; bootstrap bytes re-read externally.

dolby-gameplay01: user confirmed return to a movable gameplay scene. Game had been
restarted as PID 39036; endpoint stayed F32D80U and capability maxDynamic=20. The
game-only before/after audits show Nioh3 as the only active non-system audio session,
CaptureDllLoaded=False. No source/emitter process was started during this run.
30.078 seconds, 110519 chunks, 53049120 object-frames, 219269696 raw bytes.
All 37 slots have 2987 x 480-frame blocks (~29.87 seconds each; bootstrap setup
misses the start before encoder hooks are ready, so this is NOT full source-frame
coverage from the game's stream start). No queue drops or invalid reads.
5 of the 20 dynamic slots have nonzero, distinct audio payloads. Their peaks are
0.02480790, 0.03810290, 0.01839293, 0.01246910, 0.01641596, with changing volume
metadata. Pairwise audio correlation is between -0.017514 and 0.005661; exported
audio SHA256s are all different. These can still be submixes, not individual game
emitters. Remaining 15 dynamic slots are silent. 12 of 17 static slots have signal.
Crucially ALL observed dynamic XYZ fields remain EXACTLY (0,0,0), including audible
slots. This proves nonzero dynamic-object audio capture, NOT useful moving source
coordinates. Other metadata/upstream source placement is not yet inspected.
Read gameplay-verification.json and objects.csv for the actual data and boundaries.
Copy section total 27484.9 us (~0.249 us/chunk), max 41.6 us. Whole audiodg CPU
1.578125 s is not incremental hook cost; collector CPU 0.234375 s (~0.78% one core).
Stop status=2/error=0, collector exit=0, original wrapper/bootstrap bytes restored.
Capture is stopped. Dolby setting and the user's running game are left unchanged.

dolby-motion01: repeat requested with human cues: initially still, then walk/turn
near collector second 10. Input itself was NOT recorded, so compliance/focus is
not independently established. Phase audit deliberately compares first captured
seconds 0-8 with 13-28, excluding the transition/reaction interval.
30.093 seconds, 110445 chunks, 53013600 object-frames, 219122880 bytes; no drops,
invalid reads or capture errors. Same game PID 39036 and audiodg PID 39292.
All 20 dynamic slots were silent with XYZ exactly zero in BOTH cued windows;
12 static slots had audio. This differs from the preceding gameplay capture's
5 audible dynamic slots. Therefore the repeat is NOT decisive evidence that
player movement cannot affect coordinates, nor that the title has no spatial
metadata. Current scene/focus/input and the upstream metadata path remain open.
motion-verification.json contains the per-phase counts; gameplay-verification.json
checks recording integrity. Collector exited 0 and original hook bytes restored.

Artifacts
---------
Each run stores global-objects.bin (64-byte header + float32 mono samples),
summary.json, backend.json, metrics.csv, plus endpoint/session/module audit logs.
analyze.py exports objects.csv, per-object IEEE float WAVs, and analysis.json.
Full 37-slot menu dump is ~146 MB, plus WAV exports; disk quotas stop without deleting
old captures. Do not treat concatenated WAVs as gapless when drops/gaps are present.
QPC/csv defines timing. Sample rate 48 kHz is verified for this endpoint/path only.
Source attribution is isolated active-session evidence, NOT automatic game-PID
mapping. Engine stream/object identifiers may be reused across lifetimes.

Run / stop (elevated PowerShell, existing scripts and prebuilt executables)
------------------------------------------------------------------------
  .\run-test.ps1 -RunName NEW_NAME -Seconds 20 -NoSource -Backend dolby
  .\start-resident.ps1 -OutputRoot C:\ABSOLUTE\NEW_FOLDER -Backend dolby
Default supervisor quota: 512 MB/session, 2048 MB total. It does not create a
Windows service, scheduled task, startup entry or boot persistence.
Supervisor stop: create an empty STOP file in OutputRoot, or Ctrl+C.
Single collector stop: .\collector.exe --stop COLLECTOR_PID
  python .\analyze.py .\NEW_NAME
  python .\analyze.py .\controlled04 --controlled
  .\build.ps1 -CollectorOnly   (does not rebuild pinned capture DLLs)
  .\build.ps1 -DolbyOnly      (rebuild Dolby DLL + collector, not Sonic DLL)
DLL deployment is C:\ProgramData\SpatialObjectSpike so LOCAL SERVICE can read it.
Never overwrite a pinned DLL; deployment hash mismatch fails closed. A changed
build needs a new deployment version name or a normal audio-engine restart.

Remaining limits
----------------
The later metadata lifecycle investigation verified actual Nioh 3 audio AND changing
coordinates. The old default Dolby format still lacks update flags and is not a
current-position protocol; use the metadata backend/decoder until those semantics
are merged into the compact resident protocol. See POSITION-CHAIN-RESULT.txt.
No generic arbitrary-game compatibility, non-basic metadata decoding, automatic
PID attribution, anti-cheat compatibility, multi-engine coverage, source-world
coordinates, Atmos bitstream decoder or Sunshine/Moonlight transport integration
is claimed. Supervisor restart/quota/crash behavior is implemented but has not
yet received full fault-injection/soak testing. No security settings were changed.
