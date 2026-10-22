# Spatial audio source — M1/M2/M3

Opt-in components, not yet connected to Sunshine's default audio path. No installer,
service, self-elevation or automatic audiodg attachment is included here.

## Boundaries

* `EncodedAudioSource`: single-session start/read/stop; owned frames; explicit
  format, epoch, domain, sequence and 48 kHz **carrier** sample clock. This is not
  a calibrated source-media timestamp and does not remove the 200 ms reservoir.
* `MatReplaySource`: bounded-memory 61,440-byte carrier replay. Every original
  byte, including bootstrap, is preserved. It never opens an audio device.
* `capture_reader_api.h` / `capture_reader_msvc.cpp`: C exports, caller-owned
  buffers. Only MSVC sees the old MSVC `q4::Ring`. It duplicates supplied handles,
  validates identity/version/loss/sequence and copies before reclaiming a slot.
  The reader also maintains heartbeat while idle. Calls are single-consumer.
* `ObjectBroker`: incremental paired-event assembler -> per-domain cache/clock ->
  existing bounded FFmpeg bridge. File boundaries have no meaning to this layer.
  Domain end submits EOF controls without inventing another source Process.
  Selected-domain and explicit encoder/implementation exclusions are supported.
  These exclusions do not prove coexistence with the actual hooks in audiodg.
* `mat_stream_receiver.cpp`: new DLL, independent of the frozen native-tested DLL.
  Keeps native host/component/ABI guards and the dedicated codec worker. Replaces
  fixed diagnostic history with an eight-slot output ring and 64-bit statistics.
  The native branch is compiled, NOT native-tested in M1–M3.
* `StreamRingSource`: joins two native 30,720-byte Process blocks into a complete
  carrier cycle. Partial EOF and diagnostic formats fail; no fake MAT generation.

The GCC broker owns Boost/FFmpeg and the bridge. MSVC DLLs own their contexts and
maps. No STL, FFmpeg objects or allocator ownership cross the C ABI.

## Output ring v2

`stream_output_protocol.h` is an internal, same-machine POD contract. The input
ring remains the frozen IPC v1 layout. Output has eight reusable slots; each slot
has an 80-byte header and bounded payload. The mapping is 994,752 bytes on the
tested x64 GCC/MSVC ABI, independent of session duration. The larger payload bound
permits explicitly labelled **diagnostic call plans**; native payloads are bounded
to the original 30,720-byte request.

Each block carries format, epoch/domain, sequence, carrier sample cursor, phase,
source index, completion QPC, length and an FNV corruption checksum. The checksum
is not authentication. This is not a network protocol or a substitute for IPC ACLs.

The dedicated codec worker may wait at most 20 ms for native output capacity;
continued full output terminates the epoch, never overwrites or silently drops.
The explicitly unpaced FAKE test mode permits a 1 s wait for diagnostic file I/O.
Input capture callbacks never wait here. Zero duration/round limits mean an
EOF/cancellation/liveness-driven session, not disabled owner/heartbeat checks.
Integer overflow still stops the session rather than wrapping its sample clock.

No data after EOF is accepted. Cancellation, output corruption and codec failures
propagate to the one session; these modules never stop Sunshine's global queue.
Native output requires the frozen 30,720-byte Process contract. An odd number of
native Process blocks adds one silent drain block to close the final carrier
cycle; it never adds a source round or trims audio. This new native alignment
branch is unit-tested as a policy, not yet executed against the actual encoder.
If a native call blocks, the DLL retains its private live state and pins itself;
no forced thread termination or unsafe unload is attempted. This native timeout
path has not been revalidated against a real vendor call in this milestone.

## Build and verify (repository's existing Windows toolchains)

From `spatial-object-spike/mat-spike`, with fresh output directories:

```powershell
./build-spatial-broker.ps1 -Output NEW_BUILD
python verify_spatial_broker.py --build NEW_BUILD --output NEW_PROOF
```

The script uses the already installed x64 MSVC toolchain, UCRT GCC, Boost and
repository FFmpeg dependencies. It installs/downloads nothing. The test fixture
creates its own MSVC mapping and a hidden GCC child; the reader DLL runs inside
that child. Fake codec calls are made by the new MSVC DLL's own worker. Test files
are taps, not inputs between the reader, bridge and codec worker.

`broker_tool.cpp` and `capture_fixture_msvc.cpp` are diagnostic executables, NOT a
production service or an approved launcher for audiodg. Native capture deployment,
hook coexistence, real continuous encoding, bed channels, short/unknown-offset
objects, multiple-domain mixing, RTP and Android decoding remain separate work.
The supported bridge contract is full-span 480-frame dynamic objects, <=20 live.

Do not replace the pinned DLL in ProgramData with this new DLL without a newly
authorized native test. Keep the old capture/codec binaries and proof artifacts.
