# Native HDR input

DLSS NR preserves the session's signal: SDR stays SDR, and native HDR capture
stays linear scRGB FP16 until the existing PQ/HLG conversion and encoder.
This does not turn SDR into HDR. If RTX HDR owns the single pre-encode slot,
RTX HDR takes priority and NR is inactive for that session.

The tested NVIDIA 310.8.0.0 runtime accepts FP16 resources but clamps values to
0..1, including at intensity zero. Passing captured scRGB directly loses signed
wide-gamut values and highlights. Resource-format support is not evidence of
native HDR model support.

`hdr_filter.*` therefore retains the original FP16 texture and presents an SDR
proxy to the model. A GPU pass normalizes around 203-nit white and compresses
highlights into a BGRA8 proxy. After NR (and optional optical flow on that same
proxy), the resolve pass decodes both the exact quantized proxy and its result,
bounds the linear residual to +/-0.25, rescales it, and adds it to the original
scRGB pixel. The original alpha and frame metadata survive. No intermediate
SDR frame is sent to the HDR encoder. Scene metadata analysis reads the resolved
frame through the existing conversion path.

This is HDR-preserving integration of an SDR model, not a claim that the model
itself understands HDR. An unchanged proxy produces a zero residual exactly;
enhancement may intentionally alter local brightness, colour and texture.
Temporal and visual quality still need real gameplay evaluation.

## Verification

- `pre_encode_filter_unit_tests`: WARP checks signed values, subnormals,
  1000/4000-nit highlights, alpha, metadata, odd dimensions, resize, caller
  context restoration, and unavailable-backend HDR passthrough.
- `frame_contract_unit_tests`: NR retains native PQ/HLG policy while requiring
  a private FP16 capture handoff; it does not become the synthetic HDR source.
- `dlssnr_pipeline_smoke <adapter> <runtime-sha256> --hdr --zero`: real NR through
  the production verified loader; requires pixel-exact unchanged HDR output.
- The same command with `--hdr --flow` enables NR and optical flow; requires
  changed output with retained HDR highlights over repeated 720p/1080p sessions.

The hardware smoke tests are explicit opt-in checks, not ordinary CI tests.
They do not prove Moonlight streaming, gameplay quality or a 30-minute session.

### End-to-end follow-up (2026-09-20)

Live native HDR testing exposed three remaining SDR-only gates: HTTP launch
attachment, the opened-display filter check, and the private texture handoff.
NR now survives all three; SDR-to-HDR filters still reject native HDR sources.
The handoff uses the resolved capture contract, excluding only the requirement
that its source already be detached.

The opt-in smoke test also accepts `--4k` (1080p/2160p resize). The real runtime
passed repeated native HDR processing at 3840x2160. This cost applies to capture
resolution: a 4K desktop streamed at 1080p still runs NR at 4K before downscaling.

`--shared` adds a producer D3D11 device, a keyed-mutex shared texture opened on
the consumer device, and a private copy made before releasing capture ownership.
The test reads the first processed frame back immediately, instead of checking
only a drained batch. Three 2160p first-frame readbacks completed in
137–146 ms on the local test GPU; the first 1080p initialization took 825 ms.
This does not reproduce the live timeout. It verifies shared-resource handoff
and NR progress, but does not exercise desktop duplication or NVENC submission.

The opt-in aggregate test `DlssNrHardware.NativeHdrFirstEncodedPacket` covers
NR -> synthetic P010 writes -> real HEVC NVENC submission without an explicit
caller Flush, CPU readback, or a second NR frame before the first packet. Set
`SUNSHINE_TEST_DLSSNR_ADAPTER` to the absolute adapter path and
`SUNSHINE_TEST_DLSSNR_SHA256` to the runtime's lowercase SHA-256 to run it.
Without both variables it skips. The local test produced an initial IDR and two
subsequent packets (about 1.2 seconds for the whole test, including initialization).
It uses minimal diagnostic P010 writes, not the production colour conversion;
desktop capture, the real conversion path and network delivery remain outside
its scope. No additional Flush was needed in this test.

The production-path follow-up reproduced a rejected first frame: Desktop
Duplication can return a nonblank cursor-only dummy before learning the capture
format. Its unknown semantics cannot satisfy the real NR contract. Conversion
now bypasses enhancement for these placeholders, retaining the normal startup
video path until a real capture is available.

`DlssNrHardware.ProductionConversionFirstEncodedPacket` tests this transition
through the production shared-texture handoff, HDR conversion/downscale and
NVENC code using synthetic pixels. `DlssNrHardware.DesktopCaptureFirstEncodedPacket`
additionally requires `SUNSHINE_TEST_DLSSNR_CAPTURE=1`: it captures the already
HDR desktop, encodes the initial frame, then requires a real frame to reach NR
active and produce another packet. No image or encoded bytes are saved or sent.
All three hardware tests passed locally after the placeholder fix. They do not
start Moonlight or verify network delivery; the separate client retest below
covers first-frame delivery.

`ProductionHlgConversionFirstEncodedPacket` runs the same production-path
placeholder transition and real NR/NVENC checks with HLG output. Both PQ and
HLG variants pass locally. A subsequent loopback Moonlight retest of the
placeholder fix received and decoded its first HEVC frame while the host
reported NR active, PQ output and active scene metadata. This used official
Moonlight 6.1, 1920x1080 capture/output with 60 fps requested, and disabled motion
estimation.
It is a desktop loopback test, not evidence of 4K streaming performance or
gameplay quality. Long-session and gameplay evaluation remain separate gates.

The client retest ran for 29:13 on Moonlight's process clock (first decoded frame
at 00:06) before the user deliberately disconnected with the quit shortcut.
Moonlight reported 59.98 fps received/decoded/rendered, 0.00% network and jitter
drops, and host processing latency of 6.9/920.9/7.3 ms (min/max/mean). The peak's
cause was not isolated; these are whole-session host measurements, not the NR
model's incremental cost. The 56 host samples spanned 27.5 minutes with NR active,
PQ and scene metadata active, and private memory between 1369.20 and 1369.36 MiB.
The monitor started after streaming, so its interval differs from the client's.

The earlier first-frame timeout is resolved. This deliberately ended run is not
a completed 30-minute test; keep the feature experimental pending that gate and
real gameplay evaluation.

`ProductionUnavailableNrStillEncodesHdr` rejects the runtime pin without changing
installed files. It verifies production encoder initialization, the startup
placeholder and three subsequent HDR packets, plus NR `degraded` state with
`runtime_untrusted`. The factory already wraps a missing/rejected backend in an
identity fallback; a primary processing failure switches the same failover
wrapper to that fallback. Its input is the private capture handoff, never a
second read of the shared texture after releasing capture ownership. Invalid
capture contracts remain errors, as they cannot safely be interpreted as frames.

The paired-package retest exposed a separate input/output mismatch: an HDR
desktop was captured as scRGB FP16 while the client negotiated SDR HEVC. The NR
capture contract had been inferred from the output transfer, rejecting the real
frame before the backend could process it. NR now resolves its input domain
from each known SDR UNORM8 or linear scRGB FP16 frame, without changing the
client's output transfer or the private handoff requirement. Unknown domains
remain rejected, and the SDR-to-HDR filter retains its strict input contract.

`ProductionHdrCaptureToSdrFirstEncodedPacket` reproduces the failing conversion
before the fix and verifies the placeholder plus three real NR/NV12/NVENC
packets after it. `ProductionUnavailableNrStillEncodesHdrCaptureToSdr` checks
the same route with a rejected runtime pin. Both pass locally along with the
existing PQ, HLG and unavailable-backend HDR tests. This verifies frame delivery
through the production encoder, not HDR-to-SDR tone-mapping quality. The client
HDR negotiation and complete 30-minute paired-package stream remain separate
acceptance checks.


### Latest paired-package and throughput scope (2026-09-20)

The later Core 91080d54 / Panel 074ce83 paired package passed provenance and
file verification. An explicit HEVC Main10/PQ desktop session used 4K scRGB
capture and 1080p60 output. The user ended the client at 10:20; client statistics
reported 60 fps received/decoded/rendered and zero network/jitter drops. Twenty
host samples covered 570.39 seconds with NR, PQ, analysis and metadata active.
This is short desktop-stream evidence, not completed 30-minute stability or
real-game motion-quality acceptance. Those longer/visual checks were explicitly
cancelled for this round; their limitations remain.

A subsequent isolated RTX 5080 / driver 616.92 / runtime 310.8.0.0 test measured
4K BGRA8 NR at 68.78 fps over 1200 frames (14.539 ms/frame; GPU Evaluate mean
13.869 ms after ten warmup frames). Two and three independent processes reached
approximately 68.4 and 68.2 fps in aggregate, rather than increasing throughput.
The input was pre-uploaded, intensity was 1, optical flow was disabled, and only
the final output was read back. This excludes game rendering, HDR proxy passes,
capture and encoding. The historical ~44 ms GPU result was not reproduced;
its cause remains unisolated. Neither result is a fixed resolution/FPS gate.

### Review boundaries

Launch retains both enabled component references until RTSP resolves the final
wire format. PQ selects RTX HDR first; HLG/SDR can still select NR. RTSP releases
the unused reference. Actual `hdr_mode` reporting follows the encoder colorspace,
not the requested capture policy. A production regression covers an SDR source
with an HDR request and requires SDR output status and delivered NVENC packets.
That fallback retains 10-bit encoding with Rec.709; bit depth alone is not HDR.
Unknown FP16 colour semantics are still rejected: bypassing NR alone cannot
establish a correct downstream transfer function.

A short same-process test with two D3D11 devices and independent NR instances
passed interleaved processing, output readback, peer destruction and recreation.
Calls were serialized, as in the host; this is not simultaneous API-call safety
or multi-client streaming acceptance. No single-instance restriction is inferred
from the earlier unverified risk. The Panel already checks the VC++ runtime and
provides an explicit missing-runtime notice and download action.
