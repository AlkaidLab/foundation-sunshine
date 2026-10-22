# Native live MAT capture and streaming

Normal MAT preference is AUTO 6 locally, then **live codec 7 or Opus 0** before
ANNOUNCE. Finite playback 5 and dump 4 are explicit diagnostics only. AUTO never
opens a recorded file or chooses the finite profile. Unsupported phones keep
using Opus; there is no phone MAT decoder or object renderer in this branch.

The opt-in host source is the continuous output of the already-running Windows
native encoder Process wrapper. `sunshine-mat-capture.exe` attaches once to an
explicit audiodg PID and creation time, copies completed output through a fixed
shared ring, assembles exact 61,440-byte IEC cycles and serves a private local
pipe. It has no file-input, fixture-loop or PCM-encoding path. It does not
configure an endpoint, enable a privilege, restart a process/service or elevate
itself. A supported native MAT encoder must already be producing output.

The experimental tap is pinned to audioeng.dll 10.0.26100.9711, timestamp
0x0627570d, image size 0x1ef000, wrapper RVA 0x31e00, original prologue and SHA256
3936bffdc4aa365e74c9995359f2821aaf759a14155a180cfa95764962b9f1d3.
Both the collector and loaded tap verify the system image identity. Other builds
are unsupported. The wrapper ABI is private Windows implementation detail;
this is an opt-in development path, not a general Windows audio-capture API.

A DESCRIBE live request reserves one native source only after actual valid cycles
have been observed. Idle output is drained and discarded. PLAY starts delivery
with the next complete cycle whose first half was captured after PLAY; there is
no pre-session backlog. The pipe rejects remote clients, has an explicit owner /
SYSTEM / Administrators ACL and one instance, and its descriptor binds publisher
and engine PID/creation time. Sunshine also verifies both process images and the
configured producer path. A test-source descriptor is always rejected.

A four-cycle collector queue and four-packet sender window bound buffering.
Ring loss, encoder/format changes, corruption, consumer backpressure, source
stall, missed deadlines or owner exit terminate the stream. There is no drop,
concealment, replay catch-up or in-stream codec change. A clean explicit producer
stop may send END only after a complete cycle; failure/disconnect never means
successful EOS. Source PTS is a 48kHz **carrier clock**, not calibrated game PTS.
Game/video lip sync and downstream Atmos recognition still need real testing.

The `mat-live-v1` SDP profile carries the exact carrier/clock, random session
epoch, domain and crypto suite, with no fixed SHA or cycle count. ANNOUNCE and
ACK must match. Each 1,104-byte SMAT fragment is wrapped in a 1,152-byte MLIV
AES-128-GCM packet. RFC5869 HKDF-SHA256 derives a separate key from the launch
key plus live epoch/domain; the 32-byte header is authenticated AAD and the
nonce is `MATL || packetSequenceLE64`. The sequence continues over RTP16 wraps;
legacy RTP-sequence AES-CBC is bypassed only for this authenticated profile.
RTP/FEC operates on ciphertext; authentication precedes client reassembly and
all renderer callbacks. The network watchdog watches authenticated progress
and excludes the bounded hardware drain after verified END.

## Build and review

From the Sunshine root, clone the pinned MinHook dependency into a build-only
folder, then compile the separate MSVC helper and tap:

```powershell
git clone --branch v1.3.4 --depth 1 https://github.com/TsudaKageyu/minhook.git build/minhook-v1.3.4
# Required commit: c3fcafdc10146beb5919319d0683e44e3c30d537
./spatial-object-spike/mat-live/build-native.ps1 -Output ./build/mat-live-native-new
./spatial-object-spike/mat-live/verify.ps1 -Output ./build/mat-live-tests-new -Python <python-with-cryptography>
```

The helper defaults to read-only preflight:

```powershell
./build/mat-live-native-new/sunshine-mat-capture.exe --preflight <audiodg-PID> <exact-creation-FILETIME>
```

Actual capture requires a security context that Windows permits to open that
specific audio engine. Build success or preflight success does not prove native
MAT readiness. For an explicitly initiated developer capture, deploy the tap to
a location readable by audiodg, verify its SHA, and invoke the explicit attach
arguments (no automatic elevation or retries):

```text
sunshine-mat-capture --attach PID CREATION_TIME TAP_ABSOLUTE TAP_SHA256 \\.\pipe\SunshineMatLive-NAME Local\SunshineMatLiveStop-NAME
```

`start-native.ps1 -Build <reviewed-build> -Output <fresh-metadata-directory>
-EnginePid <PID> -EngineCreated <creation-FILETIME>` runs only preflight. Add
`-Attach` explicitly to deploy immutable binaries to ProgramData and start the
publisher hidden, without elevation. It prints the two host config entries and
writes capture.json; `stop-native.ps1 -Metadata <capture.json>` signals that exact
publisher's stop event after checking its process identity. No installed host
config is changed. `NATIVE_MAT_READY` appears only after an actual complete native
cycle, not merely after hook installation.

Set Sunshine's `mat_live_pipe` to that exact pipe and `mat_live_producer` to the
absolute path of the running capture executable. Both default to empty. The
publisher continues draining between client sessions, with one active lease.
Signal the named stop event or Ctrl+C to stop; an incomplete final cycle fails
closed. The pinned tap remains loaded, its hook is disabled and original bytes
are checked on clean shutdown. There is no automatic attach/restart/rearm.

## Acceptance limits

### Source diagnosis and native-only loopback check

`spatial_probe --list --all` reads endpoint IDs, states, default roles, mix
formats and active endpoints' spatial object capacities. It never activates
inactive endpoints. `--endpoint <exact-ID>` selects an active endpoint explicitly
for the separately initiated `--emit` diagnostic; it does not change defaults.
Dynamic object capacity is not proof of MAT output. See Microsoft's
[endpoint states](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-state-xxx-constants)
and [spatial sound requirements](https://learn.microsoft.com/en-us/windows/win32/coreaudio/spatial-sound).

The publisher prints `OBSERVED` on shutdown even when no cycle completed:
wrapper calls, copied records/bytes, empty outputs, non-S_OK results, frame/byte
contract mismatches, missing IEC preambles, accepted halves, ring loss and last
call dimensions/result. Counters overlap; a second half normally has no IEC
preamble. `SOURCE_DIAGNOSIS pinned-wrapper-Process-not-invoked` applies to the
one pinned wrapper, not every possible Windows encoder implementation. Startup
skip counts are updated for every record, including probes with zero cycles.

A previously pinned, stopped tap can be rearmed **explicitly** using
`start-native.ps1 -Attach -Reattach -LoadedTap <exact-loaded-DLL-path>` with the
usual build/output/engine identity parameters. This checks its loaded full path,
file hash, original wrapper bytes and existing in-DLL running guard. Ordinary
`-Attach` still rejects an already loaded tap. No automatic retry is added.
Stop requests wait up to two seconds for an outstanding half-cycle to complete;
a stalled/truncated final cycle is a failure, never silence padding or clean END.
Account for this boundary drain when preparing a bounded probe.

The software verifier also builds `mat-live-check.exe` and tests its loopback
UDP/RTP/FEC adapter using separately labelled generated software fixtures. To
use the checker after a real publisher has reported `NATIVE_MAT_READY`:

```text
mat-live-check --pipe LIVE_PIPE --producer EXACT_PRODUCER --max-seconds 30 --drop-for-fec
```

It accepts only the native pipe and process identity contract. There is no
file/generated-source option. It uses the host's real live sender, ephemeral
GCM session keys, loopback UDP, 4+2 audio FEC and actual common-c AudioStream
decode path. Each received cycle and the whole stream must match source SHA256;
counts, carrier clock, authenticated END and every deliberately lost packet's
FEC recovery must match. Audio payloads are never written to disk. The timeout
fails the check; it does not convert cancellation into successful EOS. Stop the
publisher cleanly before it expires. The result reports actual RTP wrap status.

This tool does not open an audio sink or exercise a complete Sunshine RTSP/video
session. A loopback pass is source/transport bring-up evidence; supported HDMI
client playback, sustained full-session streaming and game A/V timing are
separate acceptance steps. Missing source, a file path or a half-cycle failure
cannot produce a successful result.

The software verifier uses generated carrier-shaped bytes, the actual host live
sender, actual common-c RTP/FEC queue and AudioStream receiver. Each long test
passes 78,000 data packets across RTP16 wrap; the independent Python cryptography
oracle checks the complete packet against HKDF/AES-GCM. It also covers tamper,
replay, gap, wrong identity/key, failure/EOS handling and 21 production-client
RTSP negotiation cases against a controlled peer. These are software tests, not
native capture, hardware playback or Dolby acceptance.

The older capability verifier retains finite recording tests as regression
coverage only; its normal network selection now checks live 7 / Opus 0. Private
captures, generated evidence, APKs, binaries, local keys and build dependencies
must remain outside public commits.

References: [RFC5869](https://www.rfc-editor.org/rfc/rfc5869),
[OpenSSL AEAD](https://docs.openssl.org/3.5/man3/EVP_EncryptInit/),
[Android Dolby MAT](https://developer.android.com/reference/android/media/AudioFormat#ENCODING_DOLBY_MAT).
