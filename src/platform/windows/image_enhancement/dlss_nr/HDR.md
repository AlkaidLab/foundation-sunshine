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
start Moonlight or verify network delivery; the earlier live timeout still needs
a client retest and long-session validation.

`ProductionHlgConversionFirstEncodedPacket` runs the same production-path
placeholder transition and real NR/NVENC checks with HLG output. Both PQ and
HLG variants pass locally. A subsequent loopback Moonlight retest of the
placeholder fix received and decoded its first HEVC frame while the host
reported NR active, PQ output and active scene metadata. Long-session and
gameplay evaluation remain separate gates.

Live Moonlight HEVC/PQ testing reached NR feature creation but did not receive
the first video frame before the client timeout. End-to-end HDR streaming and
30-minute stability are therefore **not yet validated**. Keep this feature
experimental; passing the standalone filter test is not a streaming release gate.
